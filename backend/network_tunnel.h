// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <functional>

/// NetworkTunnel is an acceptance-only bounded CONNECT relay; TLS remains
/// end-to-end with the real server. Notes: only the two supplied authorities
/// are admitted; no decrypted payload, headers or credentials are logged.
class NetworkTunnel final : public QObject {
public:
  /// NetworkTunnel owns one accepted socket and limits raw-byte throughput so a
  /// real active fault is observable. An optional hold suppresses responses
  /// only after substantial upstream traffic, leaving metadata probes
  /// independent.
  /// Inputs: withholdReplies makes this relay deliver the client's request
  /// but never its reply: downstream stops once the client sends again after
  /// the server's first flight (TLS 1.3: its Finished, then the request).
  /// Notes: with a four-flight handshake the request is never sent, so a
  /// fixture waiting for the server-side effect fails instead of passing.
  NetworkTunnel(QTcpSocket *client, QUrl primary, QUrl alternate,
                std::function<bool(bool)> failed,
                std::function<bool(bool)> held = {},
                bool withholdReplies = false);
  /// ~NetworkTunnel removes this relay from process-local diagnostic
  /// observation.
  ~NetworkTunnel() override;
  /// Progress aggregates opaque counters of currently held relays, never their
  /// authorities, encrypted bytes or credentials.
  struct Progress {
    quint64 count = 0, maxUpstreamBytes = 0;
    qint64 clientPending = 0, remotePending = 0, upstreamQueued = 0;
  };
  /// heldProgress exposes forwarded and buffered counts to diagnose stalled
  /// uploads; encrypted byte counts never prove HTTP success. Notes: like relay
  /// callbacks, observations run only on the control thread.
  static Progress heldProgress();
  /// setThroughput changes how many bytes every relay forwards per 10 ms tick
  /// in each direction, bounded by the relay buffer. Notes: the default keeps
  /// small transfers observable; large chunk fixtures raise it.
  static void setThroughput(qint64 bytes);
  /// setStallAfter makes every relay of one origin (the primary by default)
  /// stop forwarding in a direction once it has carried this many bytes that
  /// way (counted over its relays past their first MiB), until cleared with 0.
  /// Notes: a larger transfer then stays unfinished with positive bytes for as
  /// long as a fixture needs, without throttling; requests below 1 MiB and the
  /// other origin are unaffected.
  static void setStallAfter(qint64 bytes, bool primary = true);
  /// setTarget makes every relay connect to this host instead of resolving the
  /// admitted authority; the port still comes from the authority. Notes: Qt
  /// never proxies loopback destinations, so an isolated loopback server is
  /// addressed through reserved names that only this relay maps back.
  static void setTarget(const QString &host);

private:
  /// handshake admits an exact approved CONNECT authority, without interpreting
  /// encrypted traffic.
  void handshake();
  /// tick forwards bounded encrypted bytes or breaks both ends when this
  /// origin's fault becomes active.
  void tick();
  /// close ends this relay once and defers only its own QObject destruction.
  void close();
  QTcpSocket *client_;
  QTcpSocket remote_;
  QTimer timer_;
  QUrl primary_, alternate_;
  std::function<bool(bool)> failed_;
  std::function<bool(bool)> held_;
  QByteArray header_;
  quint64 upstreamBytes_ = 0, downstreamBytes_ = 0;
  bool primaryOrigin_ = false, connecting_ = false, ready_ = false,
       closed_ = false, remoteEnded_ = false, withholdReplies_ = false,
       replied_ = false, withheld_ = false;
};
