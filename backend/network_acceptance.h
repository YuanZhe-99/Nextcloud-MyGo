// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>

class Control;

/// TransferFaults are the owned relay's process-local fault controls. Notes:
/// none of them tells the daemon anything; it sees only what a real network
/// would show it.
struct TransferFaults {
  /// failPrimary refuses and breaks every tunnel to the primary origin.
  std::function<void(bool)> failPrimary;
  /// ready requires full verification and fresh health on both origins.
  std::function<bool()> ready;
  /// holdResponse suppresses replies on primary tunnels that carried a large
  /// request body.
  std::function<void(bool)> holdResponse;
  /// withhold routes primary-origin requests whose path ends with the suffix
  /// through a relay that delivers them and loses the reply; empty stops it.
  std::function<void(const QString &)> withhold;
  /// withheld counts the requests routed that way since the last withhold.
  std::function<quint64()> withheld;
  /// onTargetRequest observes each request about to be created for the
  /// alternate origin, with its path, before anything is sent.
  std::function<void(const std::function<void(const QString &)> &)>
      onTargetRequest;
  /// stall holds primary transfers past this many bytes in the out-of-process
  /// relay of sleep-resume (its NetworkTunnel::setStallAfter); 0 clears it.
  std::function<void(qint64)> stall;
  /// sleep has the out-of-process relay suspend this whole process for about
  /// the given milliseconds while it drops every connection and refuses the
  /// primary from then on. Returns: how long the relay held the process
  /// suspended, and the longest pause a control-thread timer and an
  /// independent thread of this process both saw.
  std::function<std::pair<qint64, qint64>(qint64)> sleep;
};

/// SwitchFacts are the durable identities a switch must never change
/// (INV-05/08): read from Control storage, never from the engine journal.
struct SwitchFacts {
  QString endpoint, phase, credential;
  qint64 epoch = 0;
  QStringList folders; // id, journal path, local and remote identity per row
  QStringList interrupted; // receipts of switch operations that ended failed
};
/// switchFacts reads the committed binding, any intent phase, the credential
/// reference, every mapping row and the switch receipts of one account.
SwitchFacts switchFacts(Control &c, const QString &account);

/// selectTransferCase limits network-active (upload, download,
/// unacknowledged-upload) or network-uncertain (move, delete, final-move) to
/// one case; empty runs them all.
void selectTransferCase(const QString &name);
/// networkAcceptance runs one named real-network scenario against the approved
/// origins in NEXTCLOUD_TEST_URL / NEXTCLOUD_TEST_ALTERNATE_URL.
void networkAcceptance(const QString &scenario);
/// chunkedTransferAcceptance selects the interrupted chunk-session case.
void chunkedTransferAcceptance();
/// uncertainTransferAcceptance selects the lost-reply cases: a chunk session's
/// final MOVE, and MOVE and DELETE of existing entries.
void uncertainTransferAcceptance();
/// largeTransferAcceptance selects the interrupted chunk session of a file
/// above 4 GiB.
void largeTransferAcceptance();
/// fileSafetyTransferAcceptance selects the fault-free rename, move, deletion
/// review and missing-remote-root cases.
/// Inputs: bulk selects only the 100 server deletions on a 510-file baseline,
/// which run as a case of their own (file-safety-bulk) to keep both in budget.
void fileSafetyTransferAcceptance(bool bulk);
/// sleepResumeAcceptance selects the simulated sleep: the whole process is
/// suspended during a held upload and wakes where only the alternate answers.
void sleepResumeAcceptance();
/// sleepRelay runs the out-of-process relay of sleep-resume for the parent
/// process with this ID: it prints its loopback port, then answers "stall N"
/// and "sleep N" lines from standard input until it ends. Returns: the exit
/// code. Side effects: suspends and always resumes that parent process.
int sleepRelay(quint32 parent);
/// networkTransferAcceptance breaks real transfers through the owned relay and
/// requires automatic mapped-root recovery.
void networkTransferAcceptance(Control &c, const QString &root,
                               const QString &account, const QString &primary,
                               const QString &alternate,
                               const QString &credential,
                               const TransferFaults &faults);
/// switchKillAcceptance drives a real switch to one persisted stage and ends
/// this process there without any cleanup, as a kill would (R03).
void switchKillAcceptance(Control &c, const QString &root,
                          const QString &account, const QString &primary,
                          const QString &alternate, const QString &credential,
                          const QString &stage, const TransferFaults &faults);
/// switchRecoverAcceptance checks what a restarted daemon made of the stage
/// the previous process died in (INV-05/06/08, R03, N21).
void switchRecoverAcceptance(Control &c, const QString &root,
                             const QString &stage);
/// cleanupNetworkContent removes only the manifest-owned remote collection
/// before native credential deletion.
void cleanupNetworkContent(const QString &root, const QString &credential);

/// AcceptanceCalls are the harness's production-path helpers a scenario in
/// another file uses: an RPC, waiting for its receipt, a bounded wait with a
/// named step, a borrowed-credential login, and the committed binding.
struct AcceptanceCalls {
  std::function<QJsonObject(const QString &, const QJsonObject &)> call;
  std::function<QJsonObject(const QJsonObject &)> settled;
  std::function<void(const std::function<bool()> &, const char *)> wait;
  /// login adds an account with a fixture user's own credentials and
  /// returns its terminal login status; its vault slot is in the manifest.
  std::function<QJsonObject(const QString &, const QString &)> login;
  /// binding is the committed endpoint and epoch of an account.
  std::function<std::pair<QString, qint64>(const QString &)> binding;
  /// stable is true once no switch of the account is in progress.
  std::function<bool(const QString &)> stable;
};
/// multiAccountAcceptance runs two accounts with two folders each on one
/// daemon against the disposable server (PLAN §2.1, R05): the same user twice
/// is refused, each account keeps its own addresses, binding and epoch, a
/// switch of one never pauses the other's folders, and removing one leaves
/// the other syncing. Side effects: one second server user, removed again.
void multiAccountAcceptance(Control &c, const QString &root,
                            const QString &first, const QString &target,
                            const QUrl &base, const AcceptanceCalls &calls);
/// switchQuotaAcceptance asks for a switch while an upload of an owned user
/// is held unfinished past the drain window, then lets it fill the user's
/// quota (B14 step 6): the request ends visibly, as a switchFailed entry with
/// QUIESCE_STALLED and an unchanged binding and policy, and a second request
/// at a safe point while the quota refuses uploads switches.
/// Side effects: one owned server user, removed again.
void switchQuotaAcceptance(Control &c, const QString &root, const QUrl &base,
                           const AcceptanceCalls &calls);
/// cleanupOwnedUser removes the second server user a failed multi-account
/// or switch-quota run left behind, named only by its own reservation.
void cleanupOwnedUser(const QString &root);
/// davRequest sends one bounded request as the named fixture user to an
/// approved origin and returns its status; the body comes back in body.
/// Notes: never replays a request; credentials stay in memory.
int davRequest(const QUrl &base, const QByteArray &method, const QString &path,
               const QString &user, const QString &password,
               const QByteArray &content = {}, QByteArray *body = nullptr,
               const QByteArray &type = {});
/// home is a fixture user's DAV files root, relative to the origin.
QString home(const QString &uid);

/// SwitchingControls are what a switching scenario may do to the daemon and
/// its network: the same notifications and faults a real network produces,
/// plus aging the dwell, which the switching contracts prove on a caller
/// clock (PLAN §8.5), so a real run need not wait 120 s for it.
struct SwitchingControls {
  AcceptanceCalls calls;
  QString account, primary, target, credential;
  QUrl base, alternate;
  /// failPrimary/failAlternate refuse and break every tunnel to that origin.
  std::function<void(bool)> failPrimary, failAlternate;
  /// refuseAlternate refuses alternate-origin requests whose path contains
  /// this text (empty stops it).
  std::function<void(const QString &)> refuseAlternate;
  /// onTargetRequest observes each request about to be created for the
  /// alternate origin, with its path, on the control thread.
  std::function<void(const std::function<void(const QString &)> &)>
      onTargetRequest;
  /// changed delivers a network-change notification.
  std::function<void()> changed;
  /// ready requires full verification and fresh health on both origins.
  std::function<bool()> ready;
  /// failed reports observed failure or quarantine of one endpoint.
  std::function<bool(const QString &)> failed;
  /// fresh reports a verified endpoint with fresh repeated successes.
  std::function<bool(const QString &)> fresh;
  /// age makes the active address count as serving for this many more
  /// milliseconds and the switch cooldown as that much older.
  std::function<void(qint64)> age;
  /// facts reads the durable switch identities of the account.
  std::function<SwitchFacts()> facts;
};
/// SwitchingFixture owns a switching scenario's loopback TLS-terminating
/// proxies and their throwaway certificates (test process only).
struct SwitchingFixture;
/// switchingScenario says whether the name is a switching scenario, and
/// kind is the alternate's endpoint kind it needs.
bool switchingScenario(const QString &scenario, QString *kind = nullptr);
/// switchingFixture creates the scenario's proxies before any sign-in and
/// routes their host names to them through route (port 0 removes a route).
/// Side effects: a throwaway CA and certificates under root, trusted only
/// in this process; no trust store, hosts file or proxy setting changes.
std::shared_ptr<SwitchingFixture>
switchingFixture(const QString &scenario, const QString &root,
                 const std::function<void(const QString &, quint16)> &route);
/// switchingAcceptance runs one switching scenario on the signed-in account
/// with its approved alternate (N03–N06, N08–N11, N13–N20, N22).
void switchingAcceptance(const QString &scenario, SwitchingFixture *fixture,
                         const SwitchingControls &controls);
/// ownedUser creates a second user of the disposable server with a random
/// password and returns its name and password. Side effects: reserves
/// owned-user.json first, so the run and the parent's cleanup remove it.
std::pair<QString, QString> ownedUser(const QString &root, const QUrl &base);
/// removeOwnedUser deletes the owned user and its reservation.
void removeOwnedUser(const QString &root, const QUrl &base,
                     const QString &user);

/// SoakControls are what the accelerated soak may do to the daemon and its
/// network: the same faults and notifications a real network produces, plus
/// running the daemon's timers faster (sync poll, probe interval, circuit
/// cooldown, switch cooldown and dwell, daily audit). Every threshold, window
/// and safety check stays the daemon's own.
struct SoakControls {
  AcceptanceCalls calls;
  QString account, primary, target;
  QUrl base, alternate;
  /// failPrimary/failAlternate refuse and break every tunnel to that origin.
  std::function<void(bool)> failPrimary, failAlternate;
  /// changed delivers a network-change notification.
  std::function<void()> changed;
  /// accelerate makes ms of real time count factor times as long for every
  /// pending probe, circuit cooldown, switch cooldown and dwell.
  std::function<void(qint64 ms, int factor)> accelerate;
  /// wake queues a remote poll (or the daily audit) of one folder, as its own
  /// timer would.
  std::function<void(const QString &folder, bool audit)> wake;
  /// observe calls back on the control thread when a folder's cycle starts
  /// (true) and when it ends (false, with the files the engine uploaded).
  std::function<void(
      const std::function<void(const QString &, bool, quint64)> &)>
      observe;
  /// verify completes a sign-in or renewal session with a user's own
  /// credentials, as the browser would.
  std::function<void(const QString &session, const QString &user,
                     const QString &password)>
      verify;
  /// ready requires full verification and fresh health on both origins of an
  /// account.
  std::function<bool(const QString &)> ready;
  /// facts reads the durable switch identities of an account.
  std::function<SwitchFacts(const QString &)> facts;
  /// explain describes an account's probe state when a wait fails.
  std::function<QString(const QString &)> explain;
};
/// soakAcceptance runs the accelerated soak (acceptance R21, PLAN §23.3):
/// two accounts with two folders each, a repeating mix of local and remote
/// changes, large chunked uploads, address faults and flaps, renewals and UI
/// connections over the real pipe, for SYNCD_SOAK_SECONDS. Inputs: resume is
/// the restarted daemon's half on the same profile. Side effects: one owned
/// server user and the reserved collection; soak.json and soak-result.json in
/// the profile.
void soakAcceptance(Control &c, const QString &root, const SoakControls &k,
                    bool resume);

/// dataSafetyAcceptance runs one named data-safety case against the
/// disposable server (PLAN §20.2, §20.4): interrupted-change (S05), watcher
/// (S09, S10), root-denied (S13), maintenance and partial-listing (S14), names
/// (S17), excluded (S18, S21), revocation (R07, R22), and the first half of the
/// restart cases
/// journal (S16), root-replaced (S15) and discovery-kill (R02), which end
/// this process with exit code 75.
/// Inputs: cut refuses (true) or restores (false) every connection to both
/// addresses: a plain network loss with nowhere to fail over to.
void dataSafetyAcceptance(Control &c, const QString &root,
                          const QString &account, const QString &name,
                          const AcceptanceCalls &calls,
                          const std::function<void(bool)> &cut);
/// dataSafetyBeforeRestart changes what the restarted daemon of a restart
/// case finds, before it opens its profile: a corrupt journal (S16) or a
/// replaced root directory (S15); nothing for discovery-kill (R02).
void dataSafetyBeforeRestart(const QString &root, const QString &name);
/// dataSafetyRecover checks what the restarted daemon made of it.
void dataSafetyRecover(Control &c, const QString &root, const QString &name,
                       const AcceptanceCalls &calls);
