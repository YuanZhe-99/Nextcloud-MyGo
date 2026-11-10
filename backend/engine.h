// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>
#include <memory>

/// EngineAdapter hosts one immutable single-endpoint libsync runtime on the Qt
/// control thread. Notes: callers must validate folder ownership and namespace
/// before construction (INV-01/02/03).
class EngineAdapter : public QObject {
public:
  /// Binding carries durable identities; the journal path is independent of the
  /// endpoint URL.
  struct Binding {
    QUrl server;
    QString login, uid, credentialReference, localPath, remotePath, journalPath,
        excludePath;
    QString localIdentity, remoteIdentity;
    quint64 epoch = 0;
    std::shared_ptr<quint64> networkGeneration;
  };
  /// Result reports sanitized completion only after teardown; it is not a
  /// switch-quiescence proof.
  struct Result {
    bool success = false;
    bool followUp = false;
    QString reason;
    QString proposalDigest;
    quint64 localDeletes = 0, remoteDeletes = 0, baseline = 0;
    quint64 epoch = 0;
    quint64 uploaded = 0, downloaded = 0, deletedRemote = 0;
    quint64 networkGeneration = 0;
    /// retryAfterMs is an internal remaining monotonic delay, not an IPC field;
    /// zero is absent/expired and -1 forbids an earlier automatic retry.
    qint64 retryAfterMs = 0;
  };
  /// Requests counts creation, active/retained replies, live managers and
  /// unconfirmed writes without recording paths, headers, credentials or
  /// payloads.
  struct Requests {
    quint64 total = 0, discovery = 0, downloads = 0, writes = 0;
    quint64 active = 0, alive = 0, managers = 0, accounts = 0,
            uncertainWrites = 0;
    quint64 denied = 0, authenticationFailures = 0, tlsFailures = 0;
    quint64 transportFailures = 0, successes = 0;
    quint64 activeUploads = 0, activeDownloads = 0;
    quint64 uploadBytes = 0, downloadBytes = 0;
    quint64 permissionFailures = 0, lockFailures = 0, throttleFailures = 0;
  };
  /// EngineAdapter reads the stable vault reference and prepares a private
  /// upstream account.
  explicit EngineAdapter(const Binding &binding);
  /// ~EngineAdapter requires the cycle to have completed; active teardown is a
  /// lifecycle error.
  ~EngineAdapter() override;
  /// start queues a full upstream discovery using the same journal for every
  /// cycle.
  void start();
  /// stop asks upstream to cancel; completion remains asynchronous and retains
  /// dirty work.
  void stop();
  /// active remains true through engine and deferred-discovery teardown.
  bool active() const;
  /// epoch identifies the immutable binding used for this adapter's cycle
  /// results.
  quint64 epoch() const;
  /// networkGeneration identifies the network captured when this cycle began,
  /// not when it completed.
  quint64 networkGeneration() const;
  /// requests returns cumulative request-creation counters, not transferred
  /// byte measurements.
  Requests requests() const;
  /// Retirement reports actual resource destruction and commit status
  /// separately from uncertainty; it is not proof that remote writes have been
  /// reconciled.
  struct Retirement {
    bool networkDrained = false;
    bool journalCommitted = false;
    quint64 uncertainWrites = 0;
  };
  /// retire permanently closes an idle binding, retains its root lease, and
  /// observes deferred account/QNAM/reply destruction before reporting
  /// drainage.
  Retirement retire();
  /// rebind creates a new immutable epoch after retirement while retaining the
  /// root lease, credential reference and journal. The control layer must first
  /// reverify the explicitly approved target and all account folder identities.
  /// Side effects: uses upstream full-rediscovery invalidation, never journal
  /// repair.
  void rebind(const QUrl &server, quint64 epoch, const QString &login = {});
  /// rebindError checks retired local identity, journal integrity and
  /// cross-origin resume admission without writes.
  QString rebindError(const QUrl &server) const;
  /// authorize permits exactly one recomputed matching deletion proposal;
  /// bindings and expiry are enforced by Control.
  void authorize(const QString &digest, qint64 remainingMilliseconds);
  /// completed runs on the control thread with no password, raw server error or
  /// request URL.
  std::function<void(const Result &)> completed;
  /// constrained queues observed server constraints after request-local gating;
  /// runtime account throttling must coordinate persistence before
  /// cancellation. No raw HTTP data or credential crosses this boundary.
  std::function<void(const QString &, qint64)> constrained;

private:
  /// createAccount constructs only the private network/credential context for
  /// the current binding.
  void createAccount();
  /// State keeps upstream implementation headers out of the domain and IPC
  /// boundaries.
  struct State;
  std::unique_ptr<State> state_;
};
