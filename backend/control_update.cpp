// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>

namespace {
/// execute requires SQL success without exposing database details to IPC.
void execute(QSqlQuery &q) {
  if (!q.exec())
    throw std::runtime_error("INTERNAL_ERROR");
}
/// run executes one fixed statement, failing like execute.
void run(QSqlQuery &q, const char *sql) {
  if (!q.exec(QString::fromLatin1(sql)))
    throw std::runtime_error("INTERNAL_ERROR");
}
/// receipt builds an update operation receipt with an optional stable reason.
QJsonObject receipt(const QString &operation, const QString &state,
                    const QString &reason = {}) {
  QJsonObject value{{"operationId", operation}, {"state", state}};
  if (reason.isEmpty())
    value.insert("result", QJsonObject{{"ready", state == "succeeded"}});
  else
    value.insert("error",
                 QJsonObject{{"code", -32000},
                             {"message", "Request could not be completed"},
                             {"data", QJsonObject{{"reasonCode", reason}}}});
  return value;
}
} // namespace

void Control::publishState() {
  const auto previous = seq_;
  if (!db_.transaction())
    throw std::runtime_error("INTERNAL_ERROR");
  try {
    ++seq_;
    QSqlQuery q(db_);
    q.prepare("UPDATE settings SET seq=? WHERE id=1");
    q.addBindValue(seq_);
    execute(q);
    q.prepare("INSERT INTO outbox VALUES(?,?)");
    q.addBindValue(seq_);
    q.addBindValue(QJsonDocument(projection()).toJson(QJsonDocument::Compact));
    execute(q);
    if (!db_.commit())
      throw std::runtime_error("INTERNAL_ERROR");
  } catch (...) {
    db_.rollback();
    seq_ = previous;
    throw;
  }
}

QJsonObject Control::startupReport() const {
  QJsonArray reasons;
  for (const auto &reason : selfCheckReasons_)
    reasons.append(reason);
  return {{"previousVersion", previousVersion_},
          {"previousSchema", previousSchema_},
          {"schema", schemaVersion},
          {"selfCheck", !selfChecked_                 ? "pending"
                        : selfCheckReasons_.isEmpty() ? "passed"
                                                      : "failed"},
          {"reasonCodes", reasons}};
}

void Control::selfCheck() {
  QStringList reasons;
  QSqlQuery q(db_);
  // The migrated database itself.
  if (!q.exec("PRAGMA quick_check") || !q.next() ||
      q.value(0).toString() != "ok")
    reasons.append("STATE_UNAVAILABLE");
  q.finish();
  // Every ready account can still reach its own credential.
  run(q, "SELECT id FROM accounts WHERE state='ready'");
  QStringList accounts;
  while (q.next())
    accounts.append(q.value(0).toString());
  q.finish();
  for (const auto &account : accounts) {
    try {
      if (!findCredential(credentialReference(account)))
        reasons.append("AUTH_REQUIRED");
    } catch (...) {
      reasons.append("CREDENTIALS_LOCKED");
    }
  }
  // Every mapping attached again, or says why not.
  run(q, "SELECT reason FROM folders WHERE status='error'");
  while (q.next())
    reasons.append(q.value(0).toString());
  q.finish();
  reasons.removeDuplicates();
  reasons.removeAll(QString{});
  selfCheckReasons_ = reasons;
  selfChecked_ = true;
  publishState();
}

QJsonObject Control::prepareUpdate(const QJsonObject &params,
                                   const QByteArray &digest) {
  if (params.value("expectedRevision").toString() != QString::number(revision_))
    throw std::runtime_error("REVISION_CONFLICT");
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        it.key() != "targetVersion")
      throw std::runtime_error("INVALID_PARAMS");
  const auto target = params.value("targetVersion").toString();
  if (!params.value("targetVersion").isString() ||
      !QRegularExpression("^[0-9A-Za-z][0-9A-Za-z.+-]{0,63}$")
           .match(target)
           .hasMatch())
    throw std::runtime_error("INVALID_PARAMS");
  if (!updateOperation_.isEmpty())
    throw std::runtime_error("QUIESCE_STALLED");
  // A deletion waiting for the user postpones the update: installing must not
  // make that decision for them.
  QSqlQuery q(db_);
  run(q, "SELECT 1 FROM decisions LIMIT 1");
  if (q.next())
    throw std::runtime_error("DECISION_PENDING");
  q.finish();
  // Work that holds credentials or an address choice in flight finishes
  // first: a switch, a renewal, an address check, a sign-in or a removal.
  run(q, "SELECT 1 FROM switch_intents WHERE phase!='blocked' UNION ALL "
         "SELECT 1 FROM reauthentication WHERE phase IN "
         "('waiting','prepared','installed','discarding') UNION ALL SELECT "
         "1 FROM accounts WHERE state!='ready' UNION ALL SELECT 1 FROM "
         "endpoints WHERE state IN ('probing','verifying') LIMIT 1");
  if (q.next() || !endpointChecks_.empty())
    throw std::runtime_error("QUIESCE_STALLED");
  q.finish();
  for (const auto &[id, flow] : logins_) {
    const auto status = flow->status();
    if (status != "failed" && status != "cancelled" && status != "succeeded")
      throw std::runtime_error("QUIESCE_STALLED");
  }
  const auto operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto accepted = receipt(operation, "running");
  accountChange([&] {
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,?,?,'daemon.prepareUpdate')");
    q.addBindValue(params.value("operationKey").toString());
    q.addBindValue(operation);
    q.addBindValue(digest);
    q.addBindValue(QJsonDocument(accepted).toJson(QJsonDocument::Compact));
    execute(q);
  });
  updateOperation_ = operation;
#ifdef SYNCD_WITH_ENGINE
  // No new cycle starts; a running one finishes on its own (§16.2).
  runtime_->hold(true);
#endif
  QTimer::singleShot(0, &networkTasks_, [this, operation] {
    advanceUpdate(operation, QDeadlineTimer(120000));
  });
  return accepted;
}

void Control::advanceUpdate(const QString &operation, QDeadlineTimer deadline) {
  if (updateOperation_ != operation || storageFailed_ || shutdown_)
    return;
  try {
    bool quiet = true;
#ifdef SYNCD_WITH_ENGINE
    QSqlQuery q(db_);
    run(q, "SELECT id FROM folders");
    while (q.next()) {
      const auto id = q.value(0).toString();
      quiet = quiet && !runtime_->running(id) &&
              (!runtime_->contains(id) || runtime_->quiescent(id));
    }
    q.finish();
#endif
    if (!quiet) {
      if (deadline.hasExpired()) {
        finishUpdate(operation, "QUIESCE_STALLED");
        return;
      }
      QTimer::singleShot(100, &networkTasks_, [this, operation, deadline] {
        advanceUpdate(operation, deadline);
      });
      return;
    }
    // A consistent copy of Control at the proven safe point; only the newest
    // one is kept (journals are closed between cycles and stay in place).
    const QDir dir(directory_);
    for (const auto &old :
         dir.entryList({"control.pre-update.*.sqlite"}, QDir::Files))
      QFile::remove(dir.filePath(old));
    const auto backup = dir.filePath(
        "control.pre-update." +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + ".sqlite");
    QSqlQuery vacuum(db_);
    vacuum.prepare("VACUUM main INTO ?");
    vacuum.addBindValue(backup);
    if (!vacuum.exec()) {
      QFile::remove(backup);
      finishUpdate(operation, "STATE_UNAVAILABLE");
      return;
    }
    finishUpdate(operation, {});
    // Held for the installer; an update that never comes releases the hold.
    QTimer::singleShot(600000, &networkTasks_, [this, operation] {
      if (updateOperation_ == operation && !shutdown_ &&
          shutdownOperation_.isEmpty())
        releaseUpdate();
    });
  } catch (...) {
    stopForStorageFailure();
  }
}

void Control::finishUpdate(const QString &operation, const QString &reason) {
  accountChange([&] {
    QSqlQuery q(db_);
    q.prepare("UPDATE operations SET receipt=? WHERE id=?");
    q.addBindValue(
        QJsonDocument(receipt(operation,
                              reason.isEmpty() ? "succeeded" : "failed",
                              reason))
            .toJson(QJsonDocument::Compact));
    q.addBindValue(operation);
    execute(q);
  });
  if (!reason.isEmpty())
    releaseUpdate();
}

void Control::releaseUpdate() {
  updateOperation_.clear();
#ifdef SYNCD_WITH_ENGINE
  runtime_->hold(false);
#endif
}
