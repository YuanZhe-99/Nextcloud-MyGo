// SPDX-License-Identifier: GPL-3.0-only
#include "network_tunnel.h"
#include <QNetworkProxy>

namespace {
constexpr qint64 headerLimit = 8192, bufferLimit = 65536;
// Unthrottled by default: faults come from explicit stall points and cuts.
qint64 bytesPerTick = bufferLimit;
qint64 stallAfter = 0;
bool stallPrimary = true;
quint64 stallCarried[2] = {0, 0}; // upstream, downstream
constexpr quint64 smallRelay = 1048576;
QString target;
constexpr int tickMilliseconds = 10;
QList<NetworkTunnel *> tunnels;
} // namespace

NetworkTunnel::NetworkTunnel(QTcpSocket *client, QUrl primary, QUrl alternate,
                             std::function<bool(bool)> failed,
                             std::function<bool(bool)> held,
                             bool withholdReplies)
    : QObject(client->parent()), client_(client), primary_(std::move(primary)),
      alternate_(std::move(alternate)), failed_(std::move(failed)),
      held_(std::move(held)), withholdReplies_(withholdReplies) {
  client_->setParent(this);
  client_->setReadBufferSize(bufferLimit);
  remote_.setReadBufferSize(bufferLimit);
  remote_.setProxy(QNetworkProxy::NoProxy);
  QObject::connect(client_, &QTcpSocket::readyRead, this, [this] {
    if (!connecting_)
      handshake();
  });
  QObject::connect(client_, &QTcpSocket::disconnected, this,
                   [this] { close(); });
  QObject::connect(&remote_, &QTcpSocket::disconnected, this,
                   [this] { remoteEnded_ = true; });
  QObject::connect(client_, &QTcpSocket::errorOccurred, this,
                   [this](auto) { close(); });
  QObject::connect(&remote_, &QTcpSocket::errorOccurred, this,
                   [this](auto error) {
                     if (error == QAbstractSocket::RemoteHostClosedError)
                       remoteEnded_ = true;
                     else
                       close();
                   });
  QObject::connect(&remote_, &QTcpSocket::connected, this, [this] {
    if (closed_)
      return;
    if (failed_(primaryOrigin_)) {
      close();
      return;
    }
    client_->write("HTTP/1.1 200 Connection Established\r\n\r\n");
    ready_ = true;
  });
  timer_.setInterval(tickMilliseconds);
  QObject::connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
  timer_.start();
  QTimer::singleShot(10000, this, [this] {
    if (!ready_)
      close();
  });
  if (client_->bytesAvailable())
    handshake();
  tunnels.append(this);
}

NetworkTunnel::~NetworkTunnel() { tunnels.removeOne(this); }

void NetworkTunnel::setTarget(const QString &host) { target = host; }

void NetworkTunnel::setStallAfter(qint64 bytes, bool primary) {
  stallAfter = qMax<qint64>(0, bytes);
  stallPrimary = primary;
  stallCarried[0] = stallCarried[1] = 0;
}

void NetworkTunnel::setThroughput(qint64 bytes) {
  bytesPerTick = qBound<qint64>(1, bytes, bufferLimit);
}

NetworkTunnel::Progress NetworkTunnel::heldProgress() {
  Progress progress;
  for (const auto *tunnel : tunnels) {
    if (tunnel->closed_ || !tunnel->ready_ ||
        tunnel->upstreamBytes_ <= quint64(bufferLimit) || !tunnel->held_ ||
        !tunnel->held_(tunnel->primaryOrigin_))
      continue;
    ++progress.count;
    progress.maxUpstreamBytes =
        qMax(progress.maxUpstreamBytes, tunnel->upstreamBytes_);
    progress.clientPending += tunnel->client_->bytesAvailable();
    progress.remotePending += tunnel->remote_.bytesAvailable();
    progress.upstreamQueued += tunnel->remote_.bytesToWrite();
  }
  return progress;
}

void NetworkTunnel::handshake() {
  header_.append(client_->read(headerLimit + 1 - header_.size()));
  if (header_.size() > headerLimit) {
    close();
    return;
  }
  const auto end = header_.indexOf("\r\n\r\n");
  if (end < 0)
    return;
  const auto line = header_.first(header_.indexOf("\r\n")).split(' ');
  if (line.size() != 3 || line[0] != "CONNECT" || line[2] != "HTTP/1.1" ||
      end + 4 != header_.size()) {
    close();
    return;
  }
  const auto authority = [](QUrl url) {
    url.setPort(url.port(443));
    return url.authority(QUrl::FullyEncoded).toUtf8();
  };
  QUrl destination;
  if (line[1].compare(authority(primary_), Qt::CaseInsensitive) == 0) {
    primaryOrigin_ = true;
    destination = primary_;
  } else if (line[1].compare(authority(alternate_), Qt::CaseInsensitive) == 0) {
    destination = alternate_;
  } else {
    close();
    return;
  }
  header_.clear();
  connecting_ = true;
  if (failed_(primaryOrigin_)) {
    close();
    return;
  }
  remote_.connectToHost(target.isEmpty() ? destination.host() : target,
                        quint16(destination.port(443)));
}

void NetworkTunnel::tick() {
  if (closed_)
    return;
  if (connecting_ && failed_(primaryOrigin_)) {
    close();
    return;
  }
  if (!ready_)
    return;
  // Only content-sized relays stall, so probes and small requests stay
  // healthy; the bytes they carry past their first MiB count together, so a
  // transfer split over several connections stalls at the same offset.
  const auto stalls = [&](quint64 own) {
    return stallAfter && primaryOrigin_ == stallPrimary && own >= smallRelay;
  };
  const auto allowance = [&](quint64 own, int way) {
    if (!stalls(own))
      return bytesPerTick;
    return qBound<qint64>(0, stallAfter - qint64(stallCarried[way]),
                          bytesPerTick);
  };
  if (!remoteEnded_ && remote_.bytesToWrite() < bufferLimit &&
      client_->bytesAvailable() && allowance(upstreamBytes_, 0) > 0) {
    const auto bytes = client_->read(qMin(allowance(upstreamBytes_, 0),
                                          bufferLimit - remote_.bytesToWrite()));
    if (stalls(upstreamBytes_))
      stallCarried[0] += quint64(bytes.size());
    upstreamBytes_ += quint64(bytes.size());
    remote_.write(bytes);
    // The client speaking again after the server's first flight is the end of
    // its handshake and the request: everything the server says next is the
    // reply this relay exists to lose.
    withheld_ = withheld_ || (withholdReplies_ && replied_ && !bytes.isEmpty());
  }
  const bool hold = withheld_ || (upstreamBytes_ > quint64(bufferLimit) &&
                                  held_ && held_(primaryOrigin_));
  if (!hold && client_->bytesToWrite() < bufferLimit &&
      remote_.bytesAvailable() && allowance(downstreamBytes_, 1) > 0) {
    const auto bytes = remote_.read(qMin(allowance(downstreamBytes_, 1),
                                         bufferLimit - client_->bytesToWrite()));
    if (stalls(downstreamBytes_))
      stallCarried[1] += quint64(bytes.size());
    downstreamBytes_ += quint64(bytes.size());
    client_->write(bytes);
    replied_ = replied_ || !bytes.isEmpty();
  }
  if (remoteEnded_ && remote_.bytesAvailable() == 0 &&
      client_->bytesToWrite() == 0)
    close();
}

void NetworkTunnel::close() {
  if (closed_)
    return;
  closed_ = true;
  timer_.stop();
  client_->abort();
  remote_.abort();
  deleteLater();
}
