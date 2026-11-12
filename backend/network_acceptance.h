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
void fileSafetyTransferAcceptance();
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
/// cleanupOwnedUser removes the second server user a failed multi-account
/// run left behind, named only by its own reservation.
void cleanupOwnedUser(const QString &root);
