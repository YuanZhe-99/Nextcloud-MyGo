// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "engine.h"
#include "scheduler.h"
#include "watcher.h"
#include <QElapsedTimer>
#include <QTimer>
#include <map>

/// EngineRuntime joins the serial scheduler to real libsync cycles on one
/// control thread. Notes: account/folder identities are supplied by Control;
/// this layer never invents or changes them.
class EngineRuntime : public QObject {
public:
  /// EngineRuntime starts a monotonic scheduling clock without starting any
  /// unregistered work.
  EngineRuntime();
  /// ~EngineRuntime requires shutdown completion before releasing root leases
  /// and journals.
  ~EngineRuntime() override;
  /// add acquires a root and queues full recovery discovery for a validated
  /// durable mapping.
  void add(const QString &folder, const QString &account,
           const EngineAdapter::Binding &binding);
  /// remove releases an idle mapping without deleting files or its journal.
  bool remove(const QString &folder);
  /// wake coalesces an external trigger; it never overrides pause, trust or
  /// backoff.
  void wake(const QString &folder, Scheduler::Reason reason);
  /// block updates one independent blocker and requests cancellation of an
  /// affected active cycle.
  void block(const QString &folder, const QString &reason, bool blocked);
  /// authorize unblocks only the deletion gate and queues rediscovery with a
  /// one-use expiring digest.
  void authorize(const QString &folder, const QString &digest,
                 qint64 remainingMilliseconds);
  /// pause freezes admission and cancels current work; resume preserves
  /// per-folder blockers.
  void pause(bool value);
  /// hold freezes admission for an update without cancelling the running
  /// cycle, which finishes naturally; independent of the user's pause.
  void hold(bool value);
  /// shutdown freezes admission and reports stopped only after the active
  /// adapter finishes teardown.
  void shutdown();
  /// quiesceAccount freezes only switch admission, preserving every user/safety
  /// blocker; failure-driven switches may request upstream cancellation.
  bool quiesceAccount(const QString &account, bool cancelActive);
  /// retireAccount requires account-wide idle engines and checks actual
  /// resource drainage and journal commits while retaining all root leases.
  QString retireAccount(const QString &account);
  /// rebindAccount installs freshly verified bindings under the switch gate;
  /// partial construction failure leaves admission frozen for controlled
  /// recovery. Side effects: drops the folders' retry backoff from failures
  /// on the previous address; account throttles and blockers stay.
  void rebindAccount(const QString &account, const QUrl &server, quint64 epoch,
                     const QString &login = {});
  /// blockAccount changes one independent account blocker without overriding
  /// other safety or user pauses.
  void blockAccount(const QString &account, const QString &reason,
                    bool blocked);
  /// throttleAccount extends server Retry-After for every current/future folder
  /// without permitting manual wakes or endpoint rebinding to evade it (N18).
  /// Side effects: asks an active account cycle to cancel through upstream.
  void throttleAccount(const QString &account, qint64 delayMs);
  /// serverThrottled propagates observed metadata/business delays before
  /// normalization so Control durably gates probes and alternate endpoints.
  /// The handler must fail closed if persistence fails; exceptions are fatal.
  std::function<void(const QString &, qint64)> serverThrottled;
  /// credentialsRenewed clears only credential failures after verified
  /// replacement under closed admission.
  void credentialsRenewed(const QString &account);
  /// endpointVerified clears only old-endpoint safety blockers after drained,
  /// full-root new-epoch activation.
  void endpointVerified(const QString &account);
  /// networkChanged invalidates old-request health qualification without
  /// altering immutable endpoint epochs.
  void networkChanged(quint64 generation) { *networkGeneration_ = generation; }
  /// rebindError validates every retired root and resume context before Control
  /// publishes a target.
  QString rebindError(const QString &account, const QUrl &server) const;
  /// resumeAccount queues full reconciliation and clears only switchInProgress.
  void resumeAccount(const QString &account);
  /// idle indicates that no adapter holds an active engine/discovery cycle.
  bool idle() const;
  /// contains reports whether a durable mapping has an attached runtime.
  bool contains(const QString &folder) const {
    return folders_.contains(folder);
  }
  /// running stays true through cancellation and teardown of the nominated
  /// cycle.
  bool running(const QString &folder) const {
    return active_ && active_->folder == folder;
  }
  /// started publishes cycle admission before upstream discovery begins.
  std::function<void(const QString &)> started;
  /// requests exposes safe request counters for acceptance and diagnostics.
  EngineAdapter::Requests requests(const QString &folder) const;
  /// Progress is what the running cycle of a folder does now.
  struct Progress {
    QString phase; // checking, uploading or downloading
    quint64 filesDone = 0;
    bool operator==(const Progress &) const = default;
  };
  /// progress reports the running cycle of a folder, or nothing when idle.
  std::optional<Progress> progress(const QString &folder) const;
  /// progressed is called at most once a second while a running cycle's
  /// progress changes, so its owner can publish it.
  std::function<void()> progressed;
  /// quiescent reports, without closing anything, that a folder runs neither a
  /// cycle nor an on-demand request; for acceptance and diagnostics.
  bool quiescent(const QString &folder) const {
    return folders_.at(folder).adapter->quiescent();
  }
  /// blockers exposes scheduler admission evidence without paths, credentials
  /// or journal access.
  QSet<QString> blockers(const QString &folder) const {
    return scheduler_.blockers(folder);
  }
  /// cycle reports sanitized engine completion; durable projection updates
  /// belong to Control.
  std::function<void(const QString &, const EngineAdapter::Result &)> cycle;
  /// stopped notifies the owner that destroying the runtime no longer
  /// interrupts an active cycle.
  std::function<void()> stopped;

private:
  /// Folder retains one journal/runtime across cycles and bounded retry/poll
  /// metadata.
  struct Folder {
    QString account;
    std::unique_ptr<EngineAdapter> adapter;
    std::unique_ptr<RootWatcher> watcher;
    qint64 pollAt = 0, auditAt = 0;
    unsigned failures = 0;
  };
  /// tick polls conservatively and admits only one eligible full-discovery
  /// cycle.
  void tick();
  /// finishShutdown waits for real network-context destruction after all engine
  /// cycles stop, retaining root leases until the process owner can safely
  /// exit.
  void finishShutdown();
  /// throttleObserved coordinates durable account throttling before applying
  /// scheduler cancellation; no exception may escape a Qt callback (N18).
  void throttleObserved(const QString &account, qint64 delayMs);
  Scheduler scheduler_;
  std::map<QString, Folder> folders_;
  std::optional<Scheduler::Run> active_;
  std::optional<Progress> published_;
  qint64 progressAt_ = 0;
  QElapsedTimer clock_;
  QTimer timer_;
  std::shared_ptr<quint64> networkGeneration_ = std::make_shared<quint64>(0);
  bool stopping_ = false;
  bool paused_ = false, held_ = false;
  bool shutdownFinished_ = false;
  qint64 shutdownStarted_ = 0;
};
