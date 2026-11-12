// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "namespace.h"
#include "switch_state.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace {
/// execute requires durable switch SQL success without exposing database
/// details.
void execute(QSqlQuery &q) {
  if (!q.exec())
    throw std::runtime_error("INTERNAL_ERROR");
}
} // namespace

void Control::recoverSwitches() {
#ifdef SYNCD_WITH_ENGINE
  QSqlQuery q(db_);
  q.prepare("SELECT s.account_id,o.receipt FROM switch_intents s JOIN "
            "operations o ON o.id=s.operation_id WHERE s.phase='blocked'");
  execute(q);
  QStringList interrupted;
  while (q.next()) {
    const auto value =
        QJsonDocument::fromJson(q.value(1).toByteArray()).object();
    if (value.value("error").toObject().value("data").toObject().value(
            "reasonCode") == "OPERATION_INTERRUPTED")
      interrupted.append(q.value(0).toString());
  }
  q.finish();
  for (const auto &account : interrupted) {
    if (storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
      return;
    try {
      const auto binding = SwitchState(db_).binding(account);
      const QJsonObject params{
          {"accountId", account},
          {"endpointId", binding.endpoint},
          {"expectedRevision", QString::number(revision_)},
          {"operationKey", QUuid::createUuid().toString(QUuid::WithoutBraces)}};
      switchMutation(params,
                     QCryptographicHash::hash(
                         QJsonDocument(params).toJson(QJsonDocument::Compact),
                         QCryptographicHash::Sha256));
    } catch (...) {
      // An unavailable lease/credential remains visibly blocked. Never repair
      // or omit a mapped root.
    }
  }
#endif
}
namespace {
/// receipt represents accepted, reconciled or failed work without claiming
/// file-operation rollback.
QJsonObject receipt(const QString &operation, const QString &reason = {},
                    bool complete = false) {
  QJsonObject result{{"operationId", operation},
                     {"state", reason.isEmpty()
                                   ? complete ? "succeeded" : "running"
                                   : "failed"},
                     {"result", QJsonObject{}}};
  if (!reason.isEmpty())
    result.insert("error",
                  QJsonObject{{"code", -32000},
                              {"message", "Switch could not be completed"},
                              {"data", QJsonObject{{"reasonCode", reason}}}});
  return result;
}
/// saveReceipt updates the operation inside the caller's binding/outbox
/// transaction.
void saveReceipt(QSqlDatabase db, const QJsonObject &value) {
  QSqlQuery q(db);
  q.prepare("UPDATE operations SET receipt=? WHERE id=?");
  q.addBindValue(QJsonDocument(value).toJson(QJsonDocument::Compact));
  q.addBindValue(value.value("operationId").toString());
  execute(q);
  if (q.numRowsAffected() != 1)
    throw std::runtime_error("INTERNAL_ERROR");
}
} // namespace

QJsonObject Control::switchMutation(const QJsonObject &params,
                                    const QByteArray &digest,
                                    bool credentialReload, const QString &cause,
                                    bool fixedPolicy) {
#ifndef SYNCD_WITH_ENGINE
  Q_UNUSED(params);
  Q_UNUSED(digest);
  Q_UNUSED(credentialReload);
  Q_UNUSED(cause);
  Q_UNUSED(fixedPolicy);
  throw std::runtime_error("UNSUPPORTED_CAPABILITY");
#else
  if (params.value("expectedRevision").toString() != QString::number(revision_))
    throw std::runtime_error("REVISION_CONFLICT");
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        it.key() != "accountId" && it.key() != "endpointId")
      throw std::runtime_error("INVALID_PARAMS");
  const auto account = params.value("accountId").toString(),
             target = params.value("endpointId").toString();
  SwitchState store(db_);
  const auto current = store.binding(account);
  store.target(account, target);
  if (throttleRemaining(account) > 0)
    throw std::runtime_error("SERVER_THROTTLED");
  const bool recovering = store.gated(account);
  if (endpointChecks_.contains(account))
    throw std::runtime_error("QUIESCE_STALLED");
  QSqlQuery q(db_);
  q.prepare("SELECT reason FROM endpoint_quarantine WHERE account_id=? AND "
            "endpoint_id=?");
  q.addBindValue(account);
  q.addBindValue(target);
  execute(q);
  if (q.next())
    throw std::runtime_error(q.value(0).toString().toStdString());
  q.finish();
  if (!credentialReload) {
    q.prepare("SELECT 1 FROM reauthentication WHERE account_id=? AND phase IN "
              "('waiting','prepared','installed','discarding')");
    q.addBindValue(account);
    execute(q);
    if (q.next())
      throw std::runtime_error("QUIESCE_STALLED");
    q.finish();
  }
  q.prepare("SELECT 1 FROM endpoints WHERE account_id=? AND state IN "
            "('probing','verifying') LIMIT 1");
  q.addBindValue(account);
  execute(q);
  if (q.next())
    throw std::runtime_error("QUIESCE_STALLED");
  q.finish();
  q.prepare("SELECT id FROM folders WHERE account_id=?");
  q.addBindValue(account);
  execute(q);
  QStringList folders;
  while (q.next())
    folders.append(q.value(0).toString());
  q.finish();
  // A missing runtime must acquire and validate its original root, never be
  // silently excluded.
  for (const auto &id : folders)
    if (!runtime_->contains(id))
      attachFolder(id);
  const auto operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto accepted = receipt(operation);
  qint64 epoch = 0;
  accountChange([&] {
    epoch = store.begin(account, target, operation, current.epoch);
    QSqlQuery policy(db_);
    policy.prepare("SELECT revision FROM endpoint_policies WHERE account_id=?");
    policy.addBindValue(account);
    execute(policy);
    const auto policyRevision =
        policy.next() ? policy.value(0).toLongLong() : 0;
    policy.finish();
    policy.prepare(
        "INSERT OR REPLACE INTO switch_context VALUES(?,?,?,?,?,?,?,?)");
    policy.addBindValue(account);
    policy.addBindValue(operation);
    policy.addBindValue(epoch);
    policy.addBindValue(current.endpoint);
    policy.addBindValue(QString::number(network_ ? network_->generation : 0));
    policy.addBindValue(policyRevision);
    policy.addBindValue(current.epoch);
    policy.addBindValue(cause);
    execute(policy);
    if (fixedPolicy) {
      policy.prepare("INSERT INTO pending_policies VALUES(?,?,?)");
      policy.addBindValue(account);
      policy.addBindValue(operation);
      policy.addBindValue(target);
      execute(policy);
    }
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,?,?,?)");
    q.addBindValue(params.value("operationKey").toString());
    q.addBindValue(operation);
    q.addBindValue(digest);
    q.addBindValue(QJsonDocument(accepted).toJson(QJsonDocument::Compact));
    q.addBindValue(fixedPolicy ? QStringLiteral("endpoints.setPolicy")
                               : QStringLiteral("endpoints.requestSwitch"));
    execute(q);
  });
  const bool cancelling = cause == "failover" || cause == "recovery";
  runtime_->quiesceAccount(account, cancelling);
  QTimer::singleShot(
      0, &networkTasks_, [this, account, epoch, recovering, cancelling] {
        advanceSwitch(account, epoch,
                      QDeadlineTimer(cancelling ? 120000 : 30000),
                      recovering || cancelling);
      });
  return accepted;
#endif
}

void Control::advanceSwitch(const QString &account, qint64 epoch,
                            QDeadlineTimer deadline, bool recovering) {
#ifdef SYNCD_WITH_ENGINE
  QString operation, target;
  // A switch the user asked for ends visibly: switched, or switchFailed with
  // its reason in the history, inside the caller's transaction (protocol
  // 1.10). Notes: regression for a request that vanished when its drain
  // window expired during a long upload (B14 step 6).
  const auto refused = [&](const QString &reason) {
    QSqlQuery context(db_);
    context.prepare("SELECT from_id,cause FROM switch_context WHERE "
                    "account_id=? AND epoch=?");
    context.addBindValue(account);
    context.addBindValue(epoch);
    execute(context);
    if (context.next() && context.value(1) == "manual")
      recordActivity(account, {}, "switchFailed", 1, context.value(0).toString(),
                     target, reason);
  };
  try {
    QSqlQuery q(db_);
    q.prepare("SELECT operation_id,target_id FROM switch_intents WHERE "
              "account_id=? AND target_epoch=? AND phase='prepared'");
    q.addBindValue(account);
    q.addBindValue(epoch);
    execute(q);
    if (!q.next())
      return; // superseded/stale callbacks never touch a new operation
    operation = q.value(0).toString();
    target = q.value(1).toString();
    q.finish();
    q.prepare("SELECT 1 FROM accounts WHERE id=? AND state='ready'");
    q.addBindValue(account);
    execute(q);
    if (!q.next())
      return; // An account being removed: releaseAccount owns the intent.
    q.finish();
    if (storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
      throw std::runtime_error("CANCELLED");
    if (throttleRemaining(account) > 0)
      throw std::runtime_error("SERVER_THROTTLED");
    switchEvidence(account, epoch);
    if (!runtime_->quiesceAccount(account, false)) {
      if (deadline.hasExpired()) {
        accountChange([&] {
          if (recovering)
            SwitchState(db_).block(account, epoch);
          else
            SwitchState(db_).defer(account, epoch);
          saveReceipt(db_, receipt(operation, "QUIESCE_STALLED"));
          refused(QStringLiteral("QUIESCE_STALLED"));
          q.prepare("DELETE FROM pending_policies WHERE operation_id=?");
          q.addBindValue(operation);
          execute(q);
        });
        if (!recovering)
          runtime_->resumeAccount(account);
        return;
      }
      QTimer::singleShot(25, &networkTasks_,
                         [this, account, epoch, deadline, recovering] {
                           advanceSwitch(account, epoch, deadline, recovering);
                         });
      return;
    }
    const auto drainage = runtime_->retireAccount(account);
    if (drainage == "QUIESCE_STALLED" && !deadline.hasExpired()) {
      QTimer::singleShot(25, &networkTasks_,
                         [this, account, epoch, deadline, recovering] {
                           advanceSwitch(account, epoch, deadline, recovering);
                         });
      return;
    }
    if (!drainage.isEmpty())
      throw std::runtime_error(drainage.toStdString());
    const auto server = verifySwitchTarget(account, target);
    const auto admission = runtime_->rebindError(account, server);
    if (!admission.isEmpty())
      throw std::runtime_error(admission.toStdString());
    if (storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
      throw std::runtime_error("CANCELLED");
    switchEvidence(account, epoch);
    accountChange([&] {
      SwitchState(db_).commit(account, epoch);
      q.prepare("UPDATE endpoints SET state='verified' WHERE id=? AND "
                "account_id=? AND approved=1");
      q.addBindValue(target);
      q.addBindValue(account);
      execute(q);
      q.prepare(
          "UPDATE folders SET status='offline',reason='' WHERE account_id=?");
      q.addBindValue(account);
      execute(q);
      // Old boot/epoch deletion authorizations cannot approve new endpoint
      // propagation.
      q.prepare("DELETE FROM decisions WHERE folder_id IN (SELECT id FROM "
                "folders WHERE account_id=?)");
      q.addBindValue(account);
      execute(q);
    });
    q.prepare("SELECT login_name FROM accounts WHERE id=? AND state='ready'");
    q.addBindValue(account);
    execute(q);
    if (!q.next())
      throw std::runtime_error("NOT_FOUND");
    const auto login = q.value(0).toString();
    q.finish();
    runtime_->rebindAccount(account, server, quint64(epoch), login);
    accountChange([&] {
      q.prepare("SELECT from_id,cause FROM switch_context WHERE account_id=? "
                "AND epoch=?");
      q.addBindValue(account);
      q.addBindValue(epoch);
      execute(q);
      if (!q.next())
        throw std::runtime_error("INTERNAL_ERROR");
      const auto from = q.value(0).toString(), cause = q.value(1).toString();
      q.finish();
      if (from != target)
        recordActivity(account, {}, "switched", 1, from, target,
                       cause == "preference" ? QStringLiteral("faster")
                       : cause == "failover" ? QStringLiteral("unreachable")
                       : cause == "manual"   ? QStringLiteral("userChoice")
                                             : QStringLiteral("verified"));
    });
    if (!SwitchState(db_).gated(account)) {
      accountChange([&] {
        completePolicy(operation);
        saveReceipt(db_, receipt(operation, {}, true));
      });
    }
    if (network_) {
      refreshNetwork(account);
      auto &state = network_->accounts.at(account);
      state.policy.activated(target, network_->clock.elapsed());
      state.networkBlocked = false;
      state.policy.reviewed(target);
      state.authFailed.clear();
      state.authAlternate.clear();
    }
    runtime_->credentialsRenewed(account); // All target roots and the installed
                                           // credential were freshly verified.
    q.prepare(
        "SELECT cause FROM switch_context WHERE account_id=? AND epoch=?");
    q.addBindValue(account);
    q.addBindValue(epoch);
    execute(q);
    if (q.next() && q.value(0) != "credentialReload")
      runtime_->endpointVerified(account);
    q.finish();
    runtime_->resumeAccount(account);
  } catch (const std::exception &e) {
    try {
      metadataFailed(account, e);
    } catch (...) {
      stopForStorageFailure();
      return;
    }
    if (storageFailed_ || operation.isEmpty()) {
      stopForStorageFailure();
      return;
    }
    const QString reason = QString::fromUtf8(e.what());
    if (reason == "REMOTE_LOCKED" || reason == "SERVER_THROTTLED") {
      // Keep this exact verified-intent target/epoch parked. A business
      // constraint must not turn into previous/alternate-address failover.
      // Admission stays closed; user pause and all journals remain untouched.
      const auto *metadata = dynamic_cast<const MetadataError *>(&e);
      const auto delay =
          reason == "SERVER_THROTTLED"              ? throttleRemaining(account)
          : metadata && metadata->retryAfterMs != 0 ? metadata->retryAfterMs
                                                    : 60000;
      try {
        accountChange([&] {
          QSqlQuery q(db_);
          q.prepare(
              "UPDATE folders SET status='error',reason=? WHERE account_id=?");
          q.addBindValue(reason);
          q.addBindValue(account);
          execute(q);
        });
        if (delay >= 0)
          QTimer::singleShot(
              std::min<qint64>(std::max<qint64>(delay, 1), 2147483647),
              &networkTasks_, [this, account, epoch, recovering] {
                advanceSwitch(account, epoch, QDeadlineTimer(120000),
                              recovering);
              });
      } catch (...) {
        stopForStorageFailure();
      }
      return;
    }
    const QStringList known{"CANCELLED",
                            "QUIESCE_STALLED",
                            "JOURNAL_ERROR",
                            "FOLDER_ROOT_CHANGED",
                            "AUTH_REQUIRED",
                            "CREDENTIALS_LOCKED",
                            "TLS_ERROR",
                            "ENDPOINT_UNTRUSTED",
                            "ENDPOINT_IDENTITY_MISMATCH",
                            "NETWORK_UNAVAILABLE",
                            "PERMISSION_DENIED",
                            "REMOTE_LOCKED",
                            "SERVER_THROTTLED",
                            "INVALID_SERVER_RESPONSE",
                            "REMOTE_ROOT_UNAVAILABLE",
                            "REVISION_CONFLICT",
                            "RESUME_CONTEXT_UNSUPPORTED",
                            "UNSUPPORTED_CAPABILITY",
                            "UNSUPPORTED_FOLDER_MODE"};
    try {
      accountChange([&] {
        // Commit may have succeeded before runtime construction failed: never
        // roll it back.
        if (SwitchState(db_).gated(account))
          SwitchState(db_).block(account, epoch);
        saveReceipt(db_, receipt(operation, known.contains(reason)
                                                ? reason
                                                : "INTERNAL_ERROR"));
        refused(known.contains(reason) ? reason
                                       : QStringLiteral("INTERNAL_ERROR"));
        if (reason == "TLS_ERROR" || reason == "ENDPOINT_IDENTITY_MISMATCH" ||
            reason == "ENDPOINT_UNTRUSTED") {
          QSqlQuery rejected(db_);
          rejected.prepare("UPDATE endpoints SET state='rejected' WHERE "
                           "account_id=? AND id=?");
          rejected.addBindValue(account);
          rejected.addBindValue(target);
          execute(rejected);
          quarantineEndpoint(account, target, reason);
        }
        QSqlQuery q(db_);
        q.prepare(
            "UPDATE folders SET status='error',reason=? WHERE account_id=?");
        q.addBindValue(known.contains(reason) ? reason : "INTERNAL_ERROR");
        q.addBindValue(account);
        execute(q);
      });
      switchFailed(account, epoch,
                   known.contains(reason) ? reason
                                          : QStringLiteral("INTERNAL_ERROR"));
    } catch (...) {
      stopForStorageFailure();
    }
  }
#else
  Q_UNUSED(account);
  Q_UNUSED(epoch);
  Q_UNUSED(deadline);
  Q_UNUSED(recovering);
#endif
}
