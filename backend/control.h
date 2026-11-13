#pragma once
#include "endpoint_policy.h"
#include "login.h"
#include "network_monitor.h"
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>
#include <map>
#include <memory>
#include <source_location>
class Startup;
#ifdef SYNCD_WITH_ENGINE
#include "engine_runtime.h"
#endif

/// Control owns durable settings, operation receipts and the transactional
/// event outbox. Notes: all calls and the SQLite connection stay on the Qt
/// control thread (INV-12).
class Control {
public:
  /// schemaVersion is the Control database version this build writes; the
  /// `PRAGMA user_version=` statement in control.cpp must name the same.
  static constexpr int schemaVersion = 11;
  /// protocolMinor is the protocol 1.x minor version this build speaks.
  static constexpr int protocolMinor = 10;
  /// protocolVersion is the "1.<minor>" text Diagnostics and the startup
  /// report show; derived from protocolMinor so no copy can drift.
  static QString protocolVersion() {
    return QStringLiteral("1.%1").arg(protocolMinor);
  }
  /// StartupMode selects immediate fixtures, deferred admission or DB-less IPC.
  enum class StartupMode {
    Immediate,
    Deferred,
    Backoff,
    Diagnostic,
    Stopped,
    DatabaseUnavailable
  };
  /// Control opens a private profile database and fails closed on migration or
  /// integrity errors. Deferred modes never recover credentials or attach
  /// roots.
  explicit Control(const QString &path,
                   StartupMode mode = StartupMode::Immediate,
                   Startup *startup = nullptr);
  /// activate admits recovery only after IPC exists and the startup gate
  /// passes. Notes: diagnostic/stopped/unavailable instances cannot activate
  /// (INV-10/14).
  bool activate(qint64 monotonicMs = 0);
  /// ~Control closes its own database connection.
  ~Control();
  /// dispatch validates a negotiated RPC request and returns its reply, or null
  /// for a notification.
  QJsonValue dispatch(const QJsonValue &request, bool &negotiated,
                      bool &subscribed);
  /// projection returns a consistent full snapshot with the current boot and
  /// cursor.
  QJsonObject projection() const;
  /// sequence returns the committed event cursor.
  qint64 sequence() const { return seq_; }
  /// shutdownRequested follows a durable user shutdown or fail-closed storage
  /// termination.
  bool shutdownRequested() const { return shutdown_; }
  /// cleanShutdownReady identifies an explicitly completed stop, not storage
  /// failure.
  bool cleanShutdownReady() const { return shutdownReady_ && !storageFailed_; }
#ifdef SYNCD_WITH_ENGINE
  /// cycleFinished commits a torn-down engine's result and proposal together
  /// (INV-10/11). Side effects: closes admission and requests exit if durable
  /// completion fails.
  void cycleFinished(const QString &folder,
                     const EngineAdapter::Result &result);
#endif

private:
  /// StartupTestAccess observes absence of side-effecting runtime owners while
  /// gated.
  friend struct StartupTestAccess;
  /// closeDatabase unregisters even a partially initialized Qt SQL connection.
  void closeDatabase();
  /// startupDiagnostics returns only sanitized process admission evidence.
  QJsonObject startupDiagnostics() const;
  /// startupDispatch serves bounded diagnostics/shutdown without accessing DB
  /// state.
  QJsonValue startupDispatch(const QString &method, const QJsonObject &params);
  /// ReauthTestAccess injects borrowed credentials only in the isolated
  /// lifetime regression executable.
  friend struct ReauthTestAccess;
  /// NetworkTestAccess drives actual policy/IPC boundaries without a production
  /// fault-injection switch.
  friend struct NetworkTestAccess;
  /// ConstraintTestAccess drives persisted throttle restoration with a
  /// deterministic UTC/monotonic clock, without a production bypass.
  friend struct ConstraintTestAccess;
  /// startNetwork starts monotonic network evaluation after durable mappings
  /// are restored.
  void startNetwork();
  /// networkChanged invalidates all outstanding health and switch evidence on
  /// the control thread.
  void networkChanged(bool online);
  /// evaluateNetwork runs bounded probes and submits eligible proposals to the
  /// switch coordinator.
  void evaluateNetwork();
  /// refreshNetwork imports current consent, policy, binding and root
  /// identities without reusing stale evidence. Notes: an endpoint's kind
  /// sets its default priority (home 90, vpn 70, internet 50; PLAN §8.4),
  /// never its trust.
  void refreshNetwork(const QString &account);
  /// networkOutcome consumes only a matching generation/revision-bound health
  /// result.
  void networkOutcome(const QString &account,
                      const EndpointPolicy::Ticket &ticket,
                      const QString &reason, qint64 rtt);
  /// throttleAccount commits an account-wide server deadline before gating
  /// requests/sync; failed persistence stops admission (N18, INV-10/13/14).
  void throttleAccount(const QString &account, qint64 delayMs);
  /// restoreAccountConstraints restores validated durable deadlines before any
  /// credential, network or folder recovery. Rollback before receipt UTC
  /// retains the full original delay; runtime deadlines use only monotonic
  /// time.
  void restoreAccountConstraints(qint64 utcNow = -1, qint64 monotonicNow = -1);
  /// throttleRemaining checks server admission using the same monotonic clock;
  /// explicit times are supplied only by deterministic contract tests.
  qint64 throttleRemaining(const QString &account, qint64 now = -1) const;
  /// metadataFailed preserves typed Retry-After before a caller sanitizes the
  /// reason or catches a generic runtime_error.
  void metadataFailed(const QString &account, const std::exception &error);
  /// reviewEndpoint releases persisted security quarantine only following
  /// explicit successful full verification.
  void reviewEndpoint(const QString &account, const QString &endpoint);
  /// quarantineEndpoint persists a hard security failure and closes only the
  /// affected active account admission. Notes: runs inside the caller's Control
  /// transaction; time and network changes cannot clear it (INV-04/14).
  void quarantineEndpoint(const QString &account, const QString &endpoint,
                          const QString &reason);
  /// policyMutation reserves fixed activation or commits automatic mode,
  /// retaining idempotent receipts.
  QJsonObject policyMutation(const QJsonObject &params,
                             const QByteArray &digest);
  /// completePolicy commits a pending fixed policy inside the successful
  /// reconciliation transaction.
  void completePolicy(const QString &operation);
  /// switchEvidence checks the frozen network, policy and binding inputs before
  /// publication.
  void switchEvidence(const QString &account, qint64 epoch) const;
  /// switchFailed schedules at most one fresh previous-binding recovery, never
  /// successful old receipts.
  void switchFailed(const QString &account, qint64 epoch,
                    const QString &reason);
  /// endpointResult observes matching business success/failure and blocks
  /// failed reconciliation before fallback.
  void endpointResult(const QString &account, bool success,
                      const QString &reason, quint64 epoch, quint64 generation);
  /// diagnosticsProjection exposes only sanitized aggregate runtime evidence,
  /// never paths or credentials.
  QJsonObject diagnosticsProjection() const;
  /// exportDiagnostics reserves idempotent work and atomically writes only
  /// sanitized runtime support data.
  QJsonObject exportDiagnostics(const QJsonObject &params,
                                const QByteArray &digest);
  /// activityProjection returns bounded cursor-filtered aggregate history
  /// without file names or paths.
  QJsonArray activityProjection(const QJsonObject &params = {}) const;
  /// recordActivity appends one confirmed aggregate inside the caller's Control
  /// transaction.
  void recordActivity(const QString &account, const QString &folder,
                      const QString &kind, quint64 count,
                      const QString &from = {}, const QString &to = {},
                      const QString &reason = {});
  /// NetworkNode tracks transient full-root verification and next bounded probe
  /// time.
  struct NetworkNode {
    EndpointPolicy::Config config;
    QUrl server;
    QByteArray identity;
    qint64 next = 0;
    bool verified = false, inFlight = false;
  };
  /// NetworkAccount separates selection history from persistent configuration
  /// and engine lifetime.
  struct NetworkAccount {
    EndpointPolicy policy;
    std::map<QString, NetworkNode> nodes;
    QString active, mode, fixed;
    QString authFailed, authAlternate;
    quint64 policyRevision = 0;
    qint64 businessSuccess = -30000, nextSwitch = 0;
    int probes = 0;
    bool networkBlocked = false;
    std::map<QString, quint64> businessCounts;
    qint64 epoch = -1;
  };
  /// NetworkState owns monotonic evaluation and native notification lifetime.
  struct NetworkState {
    QElapsedTimer clock;
    QTimer timer;
    std::unique_ptr<NetworkMonitor> monitor;
    std::map<QString, NetworkAccount> accounts;
    quint64 generation = 0;
    int probes = 0;
    bool online = true, evaluating = false;
  };
  std::unique_ptr<NetworkState> network_;
  QElapsedTimer constraintClock_;
  // A negative deadline is explicitly indefinite, never a short default.
  std::map<QString, qint64> accountThrottleUntil_;
  std::map<QString, QString> endpointChecks_;
  /// reauthenticate reserves a replacement login without changing the existing
  /// account identity.
  QJsonObject reauthenticate(const QJsonObject &params,
                             const QByteArray &digest);
  /// reauthReference derives a reserved candidate/previous vault key from
  /// validated entity IDs.
  QString reauthReference(const QString &account, const QString &session,
                          bool previous = false) const;
  /// reauthChanged publishes browser/session transitions without exposing
  /// either credential.
  bool reauthChanged(LoginFlow &flow);
  /// installReauthentication verifies all roots and drains engines before
  /// replacing the stable vault slot.
  void installReauthentication(const QString &session, QDeadlineTimer deadline);
  /// cleanupReauthentication revokes the prior app password and removes only
  /// reserved temporary slots.
  void cleanupReauthentication(const QString &account, const QString &session);
  /// finishReauthentication persists credential cleanup and its explicit
  /// revocation warning.
  void finishReauthentication(const QString &account, const QString &session,
                              bool revoked);
  /// recoverReauthentication rolls back uncommitted vault replacement or
  /// retains a committed new credential.
  void recoverReauthentication();
  /// resumeReauthentication reloads committed replacements before
  /// asynchronously cleaning prior credentials.
  void resumeReauthentication();
  /// reauthenticating reports the independent durable credential-replacement
  /// gate.
  bool reauthenticating(const QString &account) const;
  /// reloadCredentials queues fresh same-origin verification/rebinding without
  /// clearing user pauses, then cleans up the given installed renewal.
  /// Inputs: session is empty when a retired, failed renewal only rebinds;
  /// retryMs is the wait before the next attempt after a refusal.
  /// Side effects: a refusal (a fixed reason code such as QUIESCE_STALLED or
  /// a quarantine reason) marks the account's folders and retries after
  /// retryMs, doubling to 60 s; the installed phase and credential stay.
  /// Notes: only Control storage failures (INTERNAL_ERROR or a free-text
  /// database error) propagate, and only they may stop SyncD.
  void reloadCredentials(const QString &account, const QString &session = {},
                         qint64 retryMs = 1000);
  /// switchMutation reserves one account-wide manual activation and its receipt
  /// (INV-06/12).
  QJsonObject switchMutation(const QJsonObject &params,
                             const QByteArray &digest,
                             bool credentialReload = false,
                             const QString &cause = QStringLiteral("manual"),
                             bool fixedPolicy = false);
  /// advanceSwitch waits for observable drainage before fresh verification and
  /// gated publication.
  void advanceSwitch(const QString &account, qint64 epoch,
                     QDeadlineTimer deadline, bool recovering);
  /// verifySwitchTarget checks current origin consent, UID, home and all
  /// registered roots without writes.
  QUrl verifySwitchTarget(QString account, QString endpoint);
  /// probeAllowed rechecks durable consent and the exact in-flight context
  /// before a credentialed probe step.
  bool probeAllowed(const QString &account,
                    const EndpointPolicy::Ticket &ticket);
  /// recoverSwitches coordinates only interrupted operations after original
  /// root leases are reacquired.
  void recoverSwitches();
  /// stopForStorageFailure freezes admission and exits only after active engine
  /// teardown.
  /// Notes: logs, for the support record, the calling file and line and the
  /// stable code of the exception being handled, never data.
  void stopForStorageFailure(
      std::source_location from = std::source_location::current());
  /// endpointMutation reserves staged origin consent and read-only verification
  /// before network work.
  QJsonObject endpointMutation(const QString &method, const QJsonObject &params,
                               const QByteArray &digest);
  /// probeEndpoint executes only a durably reserved probe and commits its
  /// terminal receipt.
  void probeEndpoint(const QString &id, const QString &operation,
                     bool authenticated);
  /// endpointProjection returns approved origins with verified and
  /// committed-use projections.
  QJsonArray endpointProjection() const;
  /// recoverEndpoints invalidates interrupted probes and restart-stale
  /// verification evidence.
  void recoverEndpoints();
  /// mutate commits a setting, operation receipt and event atomically before
  /// acknowledging.
  QJsonObject mutate(const QString &method, const QJsonObject &params);
  /// loginMutation durably reserves login/removal work before any
  /// credential-store side effect.
  QJsonObject loginMutation(const QString &method, const QJsonObject &params,
                            const QByteArray &digest);
  /// loginChanged commits a login transition and its outbox projection
  /// together.
  bool loginChanged(LoginFlow &flow);
  /// loginStatus resolves the session independently of the operation that
  /// created it.
  QJsonObject loginStatus(const QString &id) const;
  /// loginReceipt adds a memory-only browser URL to a persisted operation
  /// result.
  QJsonObject loginReceipt(QJsonObject receipt) const;
  /// accountChange wraps an account/operation update with its revision and
  /// outbox transaction.
  void accountChange(const std::function<void()> &change);
  /// publishEvent appends the projection at seq_ to the outbox inside the
  /// caller's transaction and drops the rows older than the latest 100.
  /// Notes: `events.resume` always asks for a fresh bootstrap, so no older row
  /// is read again; unbounded, every state change kept a full projection and
  /// the Control database grew about 2 MB a minute in the soak (2026-11-13).
  void publishEvent(QSqlQuery &q);
  /// recoverAccounts removes interrupted account creations/removals without
  /// forgetting vault entries.
  void recoverAccounts();
  /// credentialReference derives an owned vault key without trusting arbitrary
  /// persisted key names.
  QString credentialReference(const QString &account) const;
  /// releaseAccount stops everything a removed account still runs, then
  /// removes its mappings and hands over to removeAccount.
  /// Side effects: cancels its sign-ins, blocks and waits out its engines,
  /// ends on-demand registrations, fails an unfinished switch and deletes
  /// mapping rows; local files and engine journals stay untouched.
  /// Notes: retries on the control thread until idle; an on-demand
  /// registration that cannot end by the deadline fails the removal and keeps
  /// the account.
  void releaseAccount(const QString &account, const QString &operation,
                      QDeadlineTimer deadline);
  /// removeAccount attempts remote app-password revocation, then removes only
  /// owned local account state.
  void removeAccount(const QString &account, const QString &operation);
  /// retireHeldPassword sends, once and only after the restored server delay,
  /// the DELETE a previous process had to leave held for an interrupted login.
  /// Notes: the durable record is removed before the request leaves, so an
  /// unanswered DELETE is never sent again by a later boot (N18).
  void retireHeldPassword(const QString &account);
  /// finishHeldPassword removes the interrupted login's local state and says
  /// whether the server confirmed the revocation.
  void finishHeldPassword(const QString &account, bool revoked);
  /// finishRemoval records local completion and whether server-side revocation
  /// was confirmed.
  void finishRemoval(const QString &account, const QString &operation,
                     bool revoked);
  /// folderRead inspects/revalidates roots and lists remote metadata without
  /// changing sync state.
  QJsonValue folderRead(const QString &method, const QJsonObject &params);
  /// folderMutation atomically persists a mapping or pause before applying
  /// runtime side effects.
  QJsonObject folderMutation(const QString &method, const QJsonObject &params,
                             const QByteArray &digest);
  /// folderProjection reads durable mappings with truthful current-cycle
  /// status.
  QJsonArray folderProjection() const;
  /// decisionProjection exposes pending deletion summaries, never authorization
  /// or credentials.
  QJsonArray decisionProjection() const;
  /// decisionMutation validates an unexpired boot-bound proposal before
  /// authorizing one rediscovery.
  QJsonObject decisionMutation(const QJsonObject &params,
                               const QByteArray &digest);
  /// restoreFolders attaches durable mappings to one runtime; failed roots
  /// remain blocked.
  void restoreFolders();
  /// attachFolder revalidates a persisted root and registers it with the serial
  /// engine runtime.
  void attachFolder(const QString &id);
  /// publishState commits a new snapshot event without a configuration edit.
  void publishState();
  /// startupReport says what this start migrated from and whether its
  /// self-check passed, for the installer after an update (PLAN §16.2).
  QJsonObject startupReport() const;
  /// selfCheck verifies the migrated database, every account's credential
  /// and every mapping's attachment once restoration ran, then publishes.
  void selfCheck();
  /// prepareUpdate reserves the update safe point: no new cycle starts and
  /// nothing that holds credentials or an address choice is in flight.
  /// Notes: refuses DECISION_PENDING while a deletion waits for the user and
  /// QUIESCE_STALLED while a switch, renewal, check or sign-in runs.
  QJsonObject prepareUpdate(const QJsonObject &params,
                            const QByteArray &digest);
  /// advanceUpdate waits for running work to end naturally, then backs up
  /// Control and reports the safe point; the hold lasts ten minutes.
  void advanceUpdate(const QString &operation, QDeadlineTimer deadline);
  /// finishUpdate records the safe point or why it was not reached.
  void finishUpdate(const QString &operation, const QString &reason);
  /// releaseUpdate lets sync admission continue when no update follows.
  void releaseUpdate();
  /// operationFinished durably records asynchronous completion and publishes
  /// its snapshot.
  void operationFinished(const QString &id, const QString &reason = {});
  /// Inspection binds a short-lived token to exact roots, identities and
  /// account revision.
  struct Inspection {
    QString account, local, remote, localIdentity, remoteIdentity;
    QDeadlineTimer expiry{300000};
  };
  QSqlDatabase db_;
  Startup *startup_ = nullptr;
  QString startupState_;
  bool stateAvailable_ = false, activationAllowed_ = false;
  bool publishActivation_ = false;
  QString boot_;
  qint64 seq_ = 0;
  qint64 revision_ = 0;
  bool paused_ = false;
  bool shutdown_ = false;
  bool storageFailed_ = false;
  bool shutdownReady_ = false;
  QString profileKey_;
  // Nested metadata checks retain a local lease even when a terminal session is
  // replaced.
  std::map<QString, std::shared_ptr<LoginFlow>> logins_;
  std::map<QString, QString> cancellations_;
  QObject networkTasks_;
  std::map<QString, Inspection> inspections_;
  QString directory_, shutdownOperation_;
  bool dispatching_ = false;
  // The update safe point being held, if any (in memory: a new start is the
  // new version).
  QString updateOperation_;
  // What this start migrated from, and its self-check (startupReport).
  QString previousVersion_;
  int previousSchema_ = 0;
  bool selfChecked_ = false;
  QStringList selfCheckReasons_;
#ifdef SYNCD_WITH_ENGINE
  std::unique_ptr<EngineRuntime> runtime_;
#endif
};
