// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>
#include <memory>
#include <utility>

namespace OCC {
class SyncFileItem;
}

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
    /// onDemand serves the folder as Windows Cloud Files placeholders; the
    /// folder and account IDs then name its sync root.
    bool onDemand = false;
    /// convertExisting makes the content a former full-file folder already
    /// has in-sync placeholders before the first on-demand cycle
    /// (OnDemandRoot::convertExisting); only with onDemand.
    bool convertExisting = false;
    QString folderId, accountId;
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
    /// groups names what a held deletion proposal covers: up to eight
    /// top-level entries with their file counts, as compact JSON
    /// [{"name","files","loose"}] (protocol 1.9).
    QByteArray groups;
    /// conflicts lists the conflict copies upstream kept, as compact JSON
    /// [[copy, original]] of folder-relative paths, after a finished cycle.
    QByteArray conflicts;
    /// deletedLocal counts entries removed on this PC because the server
    /// removed them; renamedRemote and renamedLocal count renames or moves
    /// applied on the server and on this PC (protocol 1.10).
    quint64 deletedLocal = 0, renamedRemote = 0, renamedLocal = 0;
    /// restarted marks a cycle upstream ended after a complete discovery that
    /// found a local removal in an on-demand folder, so that the follow-up
    /// discovers again before anything is removed on the server
    /// (SyncEngine::shouldRestartSync, patch 0020). It also marks a cycle
    /// whose only failures were new placeholders that another process's
    /// directory listing of an on-demand folder had created first, for the
    /// same server files (Explorer opened on a folder while its first cycle
    /// ran; OnDemandRoot::holds). It is no failure: success and reason stay
    /// false and empty, followUp is set.
    bool restarted = false;
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
  /// Side effects: an on-demand binding registers and connects its sync root;
  /// with convertExisting it then converts the folder's existing content for
  /// at most conversionBudgetMs of file work on this (control) thread.
  /// Notes: throws a sanitized reason code; nothing of a failed construction
  /// stays connected (the registration may).
  explicit EngineAdapter(const Binding &binding);
  /// conversionBudgetMs bounds the file part of a conversion pass; files left
  /// are made placeholders by the first cycle without a download.
  /// Notes: about 2,000 small files fit on the ARM64 test machine (10,000
  /// took 13.4 s in all, 2026-11-14).
  static constexpr int conversionBudgetMs = 3000;
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
  /// closeRequests refuses new hydration and placeholder requests of an
  /// on-demand folder until the next cycle or rebinding reconnects it.
  void closeRequests();
  /// quiescent is true when no cycle and no on-demand request of this epoch is
  /// running; a running hydration is a transfer the switch waits for (§2.3).
  bool quiescent() const;
  /// availability reads an on-demand folder's root pin (protocol 1.12):
  /// "alwaysLocal", "onlineOnly" or "onDemand" (unspecified or inherited);
  /// "" for a full-file folder or a root closed for a switch.
  QString availability() const;
  /// setAvailability pins an on-demand folder's root, as upstream's folder
  /// availability does; the next cycle's full local discovery downloads or
  /// frees the files that follow it.
  /// Inputs: "alwaysLocal", "onlineOnly" or "onDemand" (Unspecified: what is
  /// downloaded stays, new files arrive online-only).
  /// Returns: false when there is no open root or Cloud Files refused.
  bool setAvailability(const QString &value);
  /// epoch identifies the immutable binding used for this adapter's cycle
  /// results.
  quint64 epoch() const;
  /// networkGeneration identifies the network captured when this cycle began,
  /// not when it completed.
  quint64 networkGeneration() const;
  /// requests returns cumulative request-creation counters, not transferred
  /// byte measurements.
  Requests requests() const;
  /// filesDone counts the files the current cycle completed so far; there is
  /// no total while discovery runs.
  quint64 filesDone() const;
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
  /// Failure is how one failed item of a cycle counts toward
  /// Result::restarted: a new placeholder another process's listing may have
  /// created first, an item that fails nothing (a kept conflict copy, an
  /// ignored file), or any other failure.
  enum class Failure { none, placeholder, other };
  /// failure classifies one item upstream completed without success.
  /// Notes: only a new, downloaded virtual file of an on-demand folder that
  /// failed with a normal error is a placeholder; the caller skips
  /// successful items.
  static Failure failure(const OCC::SyncFileItem &item, bool onDemand);
  /// Populated is a failed new placeholder: its folder-relative path and the
  /// server file ID it was created for.
  using Populated = std::pair<QString, QByteArray>;
  /// excused decides whether a finished cycle's failure is only the race with
  /// another process's directory listing (Result::restarted).
  /// Inputs: the reason reached so far, whether the cycle was stopped, whether
  /// any item failed otherwise, whether the previous cycle was excused this
  /// way, the failed placeholders, and holds (OnDemandRoot::holds).
  /// Returns: true only for a plain SYNC_FAILED, not stopped, with at least one
  /// failed placeholder, every one held for its own file ID, no other
  /// failure, and the previous cycle not excused; the caller keeps the
  /// result as the next cycle's excusedBefore, so a failure that repeats is
  /// reported and a later one can be excused again.
  static bool
  excused(const QString &reason, bool stopping, bool otherFailure,
          bool excusedBefore, const QList<Populated> &populated,
          const std::function<bool(const QString &, const QByteArray &)> &holds);
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
  /// connectOnDemand connects an on-demand folder's sync root to the current
  /// account and journal; a no-op for full-file folders.
  void connectOnDemand();
  /// State keeps upstream implementation headers out of the domain and IPC
  /// boundaries.
  struct State;
  std::unique_ptr<State> state_;
};
