// SPDX-License-Identifier: GPL-3.0-only
#include "request_lifetime.h"
#include "namespace.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <limits>

/// State retains counters and monotonic server constraints for every manager of
/// one adapter, even when the original observer has been destroyed.
struct RequestLifetime::State : Counts {
  QElapsedTimer elapsed;
  std::function<qint64()> clock;
  qint64 lockedUntil = 0, throttleUntil = 0;
  /// now reads the injected monotonic clock or this observer's elapsed clock.
  qint64 now() const { return clock ? clock() : elapsed.elapsed(); }
  /// remaining never turns an oversized valid server delay into an early retry.
  qint64 remaining(qint64 deadline) const {
    return deadline == std::numeric_limits<qint64>::max()
               ? -1
               : qMax<qint64>(0, deadline - now());
  }
};

namespace {
/// ThrottledReply rejects a new request locally without a socket, HTTP status,
/// origin-denial evidence or a second server Retry-After observation.
class ThrottledReply final : public QNetworkReply {
public:
  /// ThrottledReply defers completion until the caller can install observers.
  ThrottledReply(const QNetworkRequest &request, QObject *parent)
      : QNetworkReply(parent) {
    setRequest(request);
    setUrl(request.url());
    open(QIODevice::ReadOnly);
    QTimer::singleShot(0, this, [this] { finish(ContentAccessDenied); });
  }
  /// abort settles this transport-free request exactly once.
  void abort() override { finish(OperationCanceledError); }

protected:
  /// readData exposes no manufactured server body.
  qint64 readData(char *, qint64) override { return -1; }

private:
  /// finish reports only a local sanitized rejection or cancellation.
  void finish(NetworkError error) {
    if (isFinished())
      return;
    setError(error, "Server request admission is paused");
    setFinished(true);
    const QPointer guard(this);
    Q_EMIT errorOccurred(error);
    if (guard)
      Q_EMIT finished();
  }
};
} // namespace

RequestLifetime::RequestLifetime(std::function<qint64()> clock)
    : state_(std::make_shared<State>()) {
  state_->clock = std::move(clock);
  state_->elapsed.start();
}

RequestLifetime::Counts RequestLifetime::counts() const { return *state_; }

QString RequestLifetime::constraintSince(const Counts &baseline,
                                         bool success) const {
  if (state_->throttleFailures > baseline.throttleFailures)
    return "SERVER_THROTTLED";
  if (state_->permissionFailures > baseline.permissionFailures)
    return "PERMISSION_DENIED";
  if (!success && state_->lockFailures > baseline.lockFailures)
    return "REMOTE_LOCKED";
  return {};
}

qint64 RequestLifetime::remainingServerRetry(const QString &reason) const {
  return state_->remaining(reason == "REMOTE_LOCKED" ? state_->lockedUntil
                                                     : state_->throttleUntil);
}
bool RequestLifetime::throttled() const {
  return remainingServerRetry("SERVER_THROTTLED") != 0;
}
QNetworkReply *RequestLifetime::denyThrottled(const QNetworkRequest &request,
                                              QObject *parent) const {
  return throttled() ? new ThrottledReply(request, parent) : nullptr;
}

bool RequestLifetime::transportFailure(QNetworkReply::NetworkError error) {
  switch (error) {
  case QNetworkReply::ConnectionRefusedError:
  case QNetworkReply::RemoteHostClosedError:
  case QNetworkReply::HostNotFoundError:
  case QNetworkReply::TimeoutError:
  case QNetworkReply::TemporaryNetworkFailureError:
  case QNetworkReply::NetworkSessionFailedError:
  case QNetworkReply::ProxyConnectionRefusedError:
  case QNetworkReply::ProxyConnectionClosedError:
  case QNetworkReply::ProxyNotFoundError:
  case QNetworkReply::ProxyTimeoutError:
    return true;
  default:
    return false;
  }
}

void RequestLifetime::observe(QNetworkReply *reply, bool mutating,
                              bool contentTransfer,
                              std::shared_ptr<const quint64> generation) {
  ++state_->created;
  ++state_->active;
  ++state_->alive;
  if (contentTransfer) {
    if (mutating)
      ++state_->activeUploads;
    else
      ++state_->activeDownloads;
  }
  auto settled = std::make_shared<bool>(false);
  const auto startedGeneration = generation ? *generation : 0;
  const auto finish = [counts = state_, settled, reply, mutating,
                       contentTransfer, generation, startedGeneration,
                       constrained = constrained] {
    if (*settled)
      return;
    *settled = true;
    --counts->active;
    if (contentTransfer) {
      if (mutating)
        --counts->activeUploads;
      else
        --counts->activeDownloads;
    }
    const auto status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() == QNetworkReply::NoError && status >= 200 &&
        status < 300 && (!generation || *generation == startedGeneration))
      ++counts->currentNetworkSuccesses;
    if (mutating && (reply->error() != QNetworkReply::NoError || status < 200 ||
                     status >= 300))
      ++counts->uncertainWrites;
    if (status == 403 || status == 423 || status == 429) {
      // Network changes invalidate health evidence, not a server constraint on
      // this immutable binding. Epoch replacement requires reply drainage.
      // This observer is installed before upstream job slots. In particular,
      // the shared 429 gate must exist before a slot can enqueue a retry.
      try {
        checkMetadataReply(*reply, status, QDateTime::currentDateTimeUtc());
      } catch (const MetadataError &error) {
        const auto reason = QString::fromUtf8(error.what());
        qint64 delay = 0;
        if (reason == "PERMISSION_DENIED")
          ++counts->permissionFailures;
        else if (reason == "REMOTE_LOCKED" || reason == "SERVER_THROTTLED") {
          if (reason == "REMOTE_LOCKED")
            ++counts->lockFailures;
          else
            ++counts->throttleFailures;
          auto &until = reason == "REMOTE_LOCKED" ? counts->lockedUntil
                                                  : counts->throttleUntil;
          const auto now = counts->now();
          const auto duration = error.retryAfterMs;
          const auto maximum = std::numeric_limits<qint64>::max();
          until = duration < 0
                      ? maximum
                      : qMax(until, now + qMin(duration, maximum - now));
          delay = counts->remaining(until);
        } else
          return; // TLS and other hard failures retain their existing domain.
        if (constrained) {
          try {
            constrained(reason, delay);
          } catch (...) {
            // No coordination failure may escape through a Qt signal or leave
            // this account silently issuing requests without a durable gate.
            qFatal("Server constraint observer failed");
          }
        }
      }
    }
  };
  QObject::connect(reply, &QNetworkReply::finished, reply, finish);
  QObject::connect(reply, &QObject::destroyed,
                   [counts = state_, settled, mutating, contentTransfer] {
                     --counts->alive;
                     if (!*settled) {
                       *settled = true;
                       --counts->active;
                       if (contentTransfer) {
                         if (mutating)
                           --counts->activeUploads;
                         else
                           --counts->activeDownloads;
                       }
                       if (mutating)
                         ++counts->uncertainWrites;
                     }
                   });
  if (reply->isFinished())
    finish();
}
