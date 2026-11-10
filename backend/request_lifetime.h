// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QNetworkReply>
#include <functional>
#include <memory>

/// RequestLifetime observes actual reply completion and destruction on the
/// control thread. Completion alone never proves drained references
/// (INV-05/06/08).
class RequestLifetime {
public:
  /// Counts contains no URLs, headers, credentials or payloads; uncertainty is
  /// monotonic and is not erased by another successful request.
  struct Counts {
    quint64 created = 0, active = 0, alive = 0, uncertainWrites = 0;
    quint64 activeUploads = 0, activeDownloads = 0;
    quint64 currentNetworkSuccesses = 0;
    quint64 permissionFailures = 0, lockFailures = 0, throttleFailures = 0;
  };
  /// RequestLifetime uses a monotonic millisecond clock; tests may supply one
  /// without changing HTTP-date interpretation at the real reply boundary.
  explicit RequestLifetime(std::function<qint64()> clock = {});
  /// observe attaches lifetime observers before the caller can consume the
  /// reply. Inputs: mutating identifies requests that may change server state.
  /// Notes: generation qualifies health success only, never server constraints;
  /// the owning adapter drains replies before replacing its immutable epoch.
  void observe(QNetworkReply *reply, bool mutating,
               bool contentTransfer = false,
               std::shared_ptr<const quint64> generation = {});
  /// transportFailure excludes cancellation, TLS, permissions and HTTP errors
  /// from endpoint soft-failure evidence.
  static bool transportFailure(QNetworkReply::NetworkError error);
  /// counts returns observed state, including replies that finished but still
  /// exist.
  Counts counts() const;
  /// constraintSince selects only this cycle's typed failure. A successfully
  /// recovered upstream lock retry is not a failed cycle; older counters never
  /// poison a new baseline.
  QString constraintSince(const Counts &baseline, bool success) const;
  /// throttled reports the shared manager gate, including indefinite deadlines.
  bool throttled() const;
  /// remainingServerRetry returns monotonic milliseconds for REMOTE_LOCKED or
  /// SERVER_THROTTLED; zero means expired/absent and -1 means no automatic
  /// retry.
  qint64 remainingServerRetry(const QString &reason) const;
  /// denyThrottled returns a transport-free asynchronous local rejection, or
  /// nullptr if requests are admitted. It never manufactures an HTTP response.
  QNetworkReply *denyThrottled(const QNetworkRequest &request,
                               QObject *parent) const;
  /// constrained is copied into each reply observer and runs after its local
  /// gate is installed, before later finished handlers. Owners must guard their
  /// own lifetime and queue any upstream cancellation or durable coordination.
  std::function<void(const QString &, qint64)> constrained;

private:
  /// State survives the observing owner until every reply has settled.
  struct State;
  std::shared_ptr<State> state_;
};
