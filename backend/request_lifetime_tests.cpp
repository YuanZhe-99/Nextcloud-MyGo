// SPDX-License-Identifier: GPL-3.0-only
#include "request_lifetime.h"
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QPointer>
#include <QTimer>
#include <cstdio>
#include <stdexcept>

namespace {
/// Reply emits actual Qt reply signals without sending a network request.
class Reply final : public QNetworkReply {
public:
  /// retryAfter supplies only the standard server delay header.
  void retryAfter(const QByteArray &value) {
    setRawHeader("Retry-After", value);
  }
  /// finish supplies a transport/HTTP outcome; it deliberately does not destroy
  /// the reply.
  void finish(int status, NetworkError error = NoError) {
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    setError(error, {});
    setFinished(true);
    Q_EMIT finished();
  }
  /// abort models an interrupted request whose server-side outcome is unknown.
  void abort() override { finish(0, OperationCanceledError); }

protected:
  /// readData exposes no payload to lifetime accounting.
  qint64 readData(char *, qint64) override { return -1; }
};
/// require retains contract checks in optimized builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// outcomes checks completion versus destruction, unknown writes and duplicate
/// signals (INV-05/08).
void outcomes() {
  for (const bool mutating : {false, true}) {
    for (const int status : {0, 200, 204, 302, 401, 500}) {
      RequestLifetime lifetime;
      auto reply = std::make_unique<Reply>();
      lifetime.observe(reply.get(), mutating);
      require(lifetime.counts().active == 1 && lifetime.counts().alive == 1,
              "request not tracked");
      reply->finish(status);
      reply->finish(status);
      const auto uncertain =
          mutating && (status < 200 || status >= 300) ? 1u : 0u;
      require(lifetime.counts().active == 0 && lifetime.counts().alive == 1 &&
                  lifetime.counts().uncertainWrites == uncertain,
              "finished reply falsely drained or duplicate outcome applied");
      reply.reset();
      require(lifetime.counts().alive == 0 &&
                  lifetime.counts().uncertainWrites == uncertain,
              "destruction lost uncertainty or retained a reply");
    }
  }
}
/// cancellation checks uncertain success headers, destruction without finished,
/// deferred deletion and an observer owner that expires before its replies.
void cancellation() {
  RequestLifetime lifetime;
  auto pending = std::make_unique<Reply>();
  lifetime.observe(pending.get(), true);
  pending.reset();
  require(lifetime.counts().uncertainWrites == 1 &&
              lifetime.counts().active == 0,
          "unreported destruction lost a write");
  auto *cancelled = new Reply;
  lifetime.observe(cancelled, true);
  cancelled->finish(200, QNetworkReply::RemoteHostClosedError);
  cancelled->deleteLater();
  require(lifetime.counts().alive == 1 &&
              lifetime.counts().uncertainWrites == 2,
          "headers hid failed transport");
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  require(lifetime.counts().alive == 0,
          "deferred destruction was not observed");
  auto success = std::make_unique<Reply>();
  lifetime.observe(success.get(), true);
  success->finish(204);
  require(lifetime.counts().uncertainWrites == 2,
          "later success erased uncertainty");
  auto late = std::make_unique<Reply>();
  {
    RequestLifetime temporary;
    temporary.observe(late.get(), true);
  }
  late->abort();
}
/// transfers counts actual unfinished content replies across duplicate finish,
/// destruction and cancellation (INV-05/09).
void transfers() {
  RequestLifetime lifetime;
  auto upload = std::make_unique<Reply>();
  auto download = std::make_unique<Reply>();
  lifetime.observe(upload.get(), true, true);
  lifetime.observe(download.get(), false, true);
  require(lifetime.counts().activeUploads == 1 &&
              lifetime.counts().activeDownloads == 1,
          "content reply was not observed as active");
  upload->finish(204);
  upload->finish(204);
  require(lifetime.counts().activeUploads == 0,
          "duplicate finish underflowed upload observation");
  download.reset();
  require(lifetime.counts().activeDownloads == 0,
          "unreported content destruction retained active download");
  auto interrupted = std::make_unique<Reply>();
  lifetime.observe(interrupted.get(), true, true);
  interrupted->abort();
  require(lifetime.counts().activeUploads == 0 &&
              lifetime.counts().uncertainWrites == 1,
          "cancelled upload lost uncertainty or stayed active");
}
/// generations rejects old-network business evidence even when its reply
/// succeeds after the event (INV-06).
void generations() {
  RequestLifetime lifetime;
  auto generation = std::make_shared<quint64>(1);
  auto old = std::make_unique<Reply>();
  lifetime.observe(old.get(), false, false, generation);
  ++*generation;
  old->finish(200);
  require(lifetime.counts().currentNetworkSuccesses == 0,
          "old-network success qualified fresh health");
  auto fresh = std::make_unique<Reply>();
  lifetime.observe(fresh.get(), false, false, generation);
  fresh->finish(200);
  fresh->finish(200);
  require(lifetime.counts().currentNetworkSuccesses == 1,
          "fresh business success was lost or counted twice");
}
/// constraints preserves real reply status domains and monotonic Retry-After
/// semantics before upstream finished handlers can submit another request
/// (INV-07/13/14, PLAN N18). These are synthetic reply contracts, not DAV
/// acceptance.
void constraints() {
  bool classified = true;
  for (const auto status : {403, 423, 429}) {
    qint64 now = 100;
    RequestLifetime lifetime([&] { return now; });
    int notifications = 0;
    QString reason;
    qint64 delay = 0;
    lifetime.constrained = [&](const QString &value, qint64 retry) {
      ++notifications;
      reason = value;
      delay = retry;
    };
    Reply reply;
    const auto baseline = lifetime.counts();
    reply.retryAfter("120");
    lifetime.observe(&reply, true);
    bool gateInstalled = false;
    QObject::connect(&reply, &QNetworkReply::finished, &reply,
                     [&] { gateInstalled = lifetime.throttled(); });
    reply.finish(status, QNetworkReply::ContentAccessDenied);
    reply.finish(status, QNetworkReply::ContentAccessDenied);
    const auto counts = lifetime.counts();
    if (notifications != 1 || counts.permissionFailures != (status == 403) ||
        counts.lockFailures != (status == 423) ||
        counts.throttleFailures != (status == 429)) {
      std::fprintf(stderr, "HTTP %d constraint was lost or counted twice\n",
                   status);
      classified = false;
      continue;
    }
    require(reason == (status == 403   ? "PERMISSION_DENIED"
                       : status == 423 ? "REMOTE_LOCKED"
                                       : "SERVER_THROTTLED") &&
                delay == (status == 403 ? 0 : 120000) &&
                gateInstalled == (status == 429),
            "HTTP constraint scope or immediate throttle gate was lost");
    require(lifetime.constraintSince(baseline, false) == reason &&
                lifetime.constraintSince(lifetime.counts(), false).isEmpty() &&
                lifetime.constraintSince(baseline, true) ==
                    (status == 423 ? QString{} : reason),
            "cycle baseline or successful upstream lock recovery was lost");
    if (status == 403)
      continue;
    now += 5000;
    require(lifetime.remainingServerRetry(reason) == 115000,
            "server deadline did not use the controllable monotonic clock");
    now += 115000;
    require(lifetime.remainingServerRetry(reason) == 0 && !lifetime.throttled(),
            "expired finite server gate did not release");
  }
  require(classified, "typed HTTP constraints were not preserved");
  for (const auto status : {423, 429}) {
    for (const auto &header :
         {QByteArray{}, QByteArray("1"), QByteArray("604800"),
          QByteArray("604801"), QByteArray("999999999999999999999999")}) {
      qint64 now = 0;
      RequestLifetime lifetime([&] { return now; });
      Reply reply;
      reply.retryAfter(header);
      lifetime.observe(&reply, false);
      reply.finish(status);
      const auto reason = status == 423 ? "REMOTE_LOCKED" : "SERVER_THROTTLED";
      const auto expected = header.isEmpty() || header == "1" ? 60000
                            : header == "604800"              ? 604800000
                                                              : -1;
      require(lifetime.remainingServerRetry(reason) == expected,
              "server retry floor, maximum or indefinite delay changed");
      now += 604800001;
      require(lifetime.remainingServerRetry(reason) == (expected < 0 ? -1 : 0),
              "long valid server deadline became an earlier retry");
    }
  }
}

/// constraintLifetimes retains binding-level server constraints across network
/// changes while rejecting stale health success, duplicate signals and unsafe
/// owner callbacks (INV-06/07/14, N18).
void constraintLifetimes() {
  bool retained = true;
  for (const auto status : {429, 403, 423}) {
    qint64 now = 100;
    RequestLifetime lifetime([&] { return now; });
    int notifications = 0;
    QString reason;
    qint64 delay = 0;
    lifetime.constrained = [&](const QString &value, qint64 retry) {
      ++notifications;
      reason = value;
      delay = retry;
    };
    auto generation = std::make_shared<quint64>(1);
    Reply old, oldSuccess;
    old.retryAfter("120");
    const auto baseline = lifetime.counts();
    lifetime.observe(&old, false, false, generation);
    lifetime.observe(&oldSuccess, false, false, generation);
    ++*generation;
    now = 5000;
    old.finish(status, QNetworkReply::ContentAccessDenied);
    oldSuccess.finish(200);
    if (notifications != 1 || lifetime.throttled() != (status == 429)) {
      std::fprintf(stderr, "HTTP %d constraint vanished after network change\n",
                   status);
      retained = false;
      continue;
    }
    const auto expected = status == 403   ? "PERMISSION_DENIED"
                          : status == 423 ? "REMOTE_LOCKED"
                                          : "SERVER_THROTTLED";
    require(
        reason == expected && delay == (status == 403 ? 0 : 120000) &&
            lifetime.constraintSince(baseline, false) == expected &&
            lifetime.counts().currentNetworkSuccesses == 0,
        "network change lost the constraint domain or qualified stale health");
    now += 5000;
    old.finish(status, QNetworkReply::ContentAccessDenied);
    require(
        notifications == 1 &&
            (status == 403 || lifetime.remainingServerRetry(reason) == 115000),
        "duplicate stale-network finish extended the original server deadline");
    Reply fresh;
    lifetime.observe(&fresh, false, false, generation);
    fresh.finish(status);
    require(notifications == 2 && lifetime.throttled() == (status == 429) &&
                lifetime.counts().currentNetworkSuccesses == 0,
            "current-network constraint did not reach the shared gate");
    const auto before = lifetime.counts();
    Reply tls;
    lifetime.observe(&tls, false);
    tls.finish(status, QNetworkReply::SslHandshakeFailedError);
    Reply cancelled;
    lifetime.observe(&cancelled, true);
    cancelled.abort();
    require(
        notifications == 2 &&
            lifetime.counts().permissionFailures == before.permissionFailures &&
            lifetime.counts().lockFailures == before.lockFailures &&
            lifetime.counts().throttleFailures == before.throttleFailures &&
            lifetime.counts().uncertainWrites == 1,
        "TLS or cancellation changed server constraints or write uncertainty");
  }
  require(retained,
          "network generation discarded binding-level server constraints");
  auto late = std::make_unique<Reply>();
  auto owner = std::make_unique<QObject>();
  bool called = false;
  {
    RequestLifetime temporary;
    temporary.constrained = [guard = QPointer(owner.get()),
                             &called](const QString &, qint64) {
      if (guard)
        QTimer::singleShot(0, guard, [&called] { called = true; });
    };
    temporary.observe(late.get(), false);
  }
  late->finish(429);
  owner.reset();
  QCoreApplication::processEvents();
  require(!called, "destroyed constraint owner received a queued callback");
  late.reset();
  late = std::make_unique<Reply>();
  {
    auto expired = std::make_unique<QObject>();
    RequestLifetime temporary;
    temporary.constrained = [guard = QPointer(expired.get()),
                             &called](const QString &, qint64) {
      if (guard)
        called = true;
    };
    temporary.observe(late.get(), false);
    temporary.constrained = {};
  }
  late->finish(429);
  require(!called,
          "reply dereferenced a constraint owner that died before finish");
}

/// extendedConstraints keeps the latest required server deadline across replies
/// and verifies that each observer retains its installed callback (N18).
void extendedConstraints() {
  qint64 now = 0;
  RequestLifetime lifetime([&] { return now; });
  int observed = 0;
  lifetime.constrained = [&](const QString &, qint64) { ++observed; };
  Reply first;
  first.retryAfter("120");
  lifetime.observe(&first, false);
  lifetime.constrained = {};
  first.finish(429);
  require(observed == 1,
          "reply observer did not retain its installed callback");
  now = 5000;
  Reply shorter;
  shorter.retryAfter("60");
  lifetime.observe(&shorter, false);
  shorter.finish(429);
  require(lifetime.remainingServerRetry("SERVER_THROTTLED") == 115000,
          "later reply shortened an existing server deadline");
  Reply indefinite;
  indefinite.retryAfter("604801");
  lifetime.observe(&indefinite, false);
  indefinite.finish(429);
  now += 700000000;
  Reply finite;
  finite.retryAfter("60");
  lifetime.observe(&finite, false);
  finite.finish(429);
  require(lifetime.remainingServerRetry("SERVER_THROTTLED") == -1 &&
              lifetime.throttled(),
          "finite retry overwrote an indefinite server constraint");
}

/// gatedRequests checks actual local rejection replies without creating a
/// transport, manufacturing server status, or extending a server deadline
/// (N18).
void gatedRequests() {
  qint64 now = 0;
  RequestLifetime lifetime([&] { return now; });
  QObject owner;
  const QNetworkRequest request(QUrl("https://approved.example.invalid/"));
  require(!lifetime.denyThrottled(request, &owner),
          "unconstrained request was locally rejected");
  Reply throttle;
  lifetime.observe(&throttle, false);
  throttle.retryAfter("120");
  throttle.finish(429);
  auto *denied = lifetime.denyThrottled(request, &owner);
  require(denied && !denied->isFinished(),
          "server gate admitted a new request or completed before observation");
  lifetime.observe(denied, false);
  QCoreApplication::processEvents();
  require(denied->isFinished() &&
              denied->error() == QNetworkReply::ContentAccessDenied &&
              !denied->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                   .isValid() &&
              lifetime.counts().throttleFailures == 1 &&
              lifetime.remainingServerRetry("SERVER_THROTTLED") == 120000,
          "local server gate manufactured HTTP evidence or extended retry");
  delete denied;
  auto *released = lifetime.denyThrottled(request, &owner);
  lifetime.observe(released, false);
  QObject::connect(released, &QNetworkReply::errorOccurred, &owner,
                   [released](auto) { delete released; });
  QCoreApplication::processEvents();
  require(lifetime.counts().active == 0 &&
              lifetime.counts().throttleFailures == 1,
          "local rejection recipient destruction retained an active reply");
  now = 120000;
  require(!lifetime.denyThrottled(request, &owner),
          "expired server gate still rejected an admitted request");
}
} // namespace
/// main tests lifetime semantics independently of real-server transfer
/// acceptance.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    outcomes();
    cancellation();
    transfers();
    generations();
    constraints();
    constraintLifetimes();
    extendedConstraints();
    gatedRequests();
    for (auto error :
         {QNetworkReply::RemoteHostClosedError,
          QNetworkReply::HostNotFoundError, QNetworkReply::TimeoutError,
          QNetworkReply::ProxyTimeoutError})
      require(RequestLifetime::transportFailure(error),
              "transport failure did not contribute to endpoint recovery");
    for (auto error :
         {QNetworkReply::NoError, QNetworkReply::OperationCanceledError,
          QNetworkReply::SslHandshakeFailedError,
          QNetworkReply::AuthenticationRequiredError,
          QNetworkReply::ContentAccessDenied,
          QNetworkReply::InternalServerError})
      require(!RequestLifetime::transportFailure(error),
              "nontransport error incorrectly triggered soft failover");
    std::puts("PASS request lifetime and uncertain outcomes");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
