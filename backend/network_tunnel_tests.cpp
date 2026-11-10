// SPDX-License-Identifier: GPL-3.0-only
#include "network_tunnel.h"
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QNetworkProxy>
#include <QPointer>
#include <QTcpServer>
#include <cstdio>
#include <stdexcept>

namespace {
/// require retains relay assertions in optimized fixture builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
/// wait drives actual local sockets with a finite asynchronous deadline.
void wait(const std::function<bool()> &condition) {
  QEventLoop loop;
  QTimer poll, deadline;
  bool done = false;
  poll.setInterval(5);
  deadline.setSingleShot(true);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (condition()) {
      done = true;
      loop.quit();
    }
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  deadline.start(5000);
  loop.exec();
  require(done, "owned relay socket deadline");
}
/// relay verifies authority admission, bounded bidirectional forwarding, active
/// faults and recipient destruction; it is not a DAV/TLS acceptance fixture.
void relay() {
  QTcpServer destination, listener;
  require(destination.listen(QHostAddress::LocalHost, 0) &&
              listener.listen(QHostAddress::LocalHost, 0),
          "owned relay listener unavailable");
  int accepted = 0;
  qint64 received = 0;
  QObject::connect(&destination, &QTcpServer::newConnection, &destination, [&] {
    while (auto *socket = destination.nextPendingConnection()) {
      ++accepted;
      QObject::connect(socket, &QTcpSocket::readyRead, socket,
                       [socket, &received] {
                         const auto bytes = socket->readAll();
                         received += bytes.size();
                         socket->write(bytes);
                       });
      QObject::connect(socket, &QTcpSocket::disconnected, socket,
                       &QObject::deleteLater);
    }
  });
  QPointer<NetworkTunnel> witness;
  bool failed = false, held = false;
  const QUrl approved(
      "https://127.0.0.1:" + QString::number(destination.serverPort()) + '/');
  QObject::connect(&listener, &QTcpServer::newConnection, &listener, [&] {
    while (auto *socket = listener.nextPendingConnection())
      witness = new NetworkTunnel(
          socket, approved, QUrl("https://other.example.invalid/"),
          [&](bool) { return failed; }, [&](bool) { return held; });
  });
  for (const auto &header :
       {QByteArray("CONNECT unknown.example.invalid:443 HTTP/1.1\r\n\r\n"),
        QByteArray("GET / HTTP/1.1\r\n\r\n"), QByteArray(8193, 'x')}) {
    QTcpSocket client;
    client.setProxy(QNetworkProxy::NoProxy);
    client.connectToHost(QHostAddress::LocalHost, listener.serverPort());
    wait([&] { return client.state() == QAbstractSocket::ConnectedState; });
    client.write(header);
    wait([&] { return client.state() == QAbstractSocket::UnconnectedState; });
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(accepted == 0 && !witness, "unapproved relay request connected "
                                       "upstream or retained its recipient");
  }
  QTcpSocket client;
  client.setProxy(QNetworkProxy::NoProxy);
  client.connectToHost(QHostAddress::LocalHost, listener.serverPort());
  wait([&] { return client.state() == QAbstractSocket::ConnectedState; });
  client.write(
      "CONNECT 127.0.0.1:" + QByteArray::number(destination.serverPort()) +
      " HTTP/1.1\r\n\r\n");
  QByteArray reply;
  wait([&] {
    reply += client.readAll();
    return reply.contains("\r\n\r\n");
  });
  require(reply == "HTTP/1.1 200 Connection Established\r\n\r\n" &&
              accepted == 1,
          "approved CONNECT admission drifted");
  const QByteArray payload(100 * 1024, 'p');
  client.write(payload);
  reply.clear();
  wait([&] {
    reply += client.readAll();
    return reply.size() >= payload.size();
  });
  require(reply == payload, "bounded relay corrupted opaque bytes");
  held = true;
  client.write(payload);
  wait([&] { return received == 2 * payload.size(); });
  require(client.bytesAvailable() == 0,
          "held response escaped before acknowledgement fault");
  held = false;
  reply.clear();
  wait([&] {
    reply += client.readAll();
    return reply.size() >= payload.size();
  });
  require(reply == payload, "released held response corrupted opaque bytes");
  client.write(payload);
  failed = true;
  wait([&] { return client.state() == QAbstractSocket::UnconnectedState; });
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  require(!witness, "failed active relay retained its callback recipient");
}
} // namespace

/// main checks only the acceptance relay's actual local socket contracts.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    relay();
    std::puts("PASS bounded CONNECT authority, opaque forwarding, active fault "
              "and relay teardown");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
