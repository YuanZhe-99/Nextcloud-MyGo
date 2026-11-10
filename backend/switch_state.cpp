// SPDX-License-Identifier: GPL-3.0-only
#include "switch_state.h"
#include "login.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
/// execute fails closed without exposing database contents.
void execute(QSqlQuery &q) {
  if (!q.exec())
    throw std::runtime_error("INTERNAL_ERROR");
}
/// identities freezes all mapped roots, including user-paused folders.
QByteArray identities(QSqlDatabase db, const QString &account) {
  QSqlQuery q(db);
  q.prepare(
      "SELECT "
      "id,local_identity,remote_path,remote_identity,journal_path,local_path "
      "FROM folders WHERE account_id=? ORDER BY id");
  q.addBindValue(account);
  execute(q);
  QJsonArray rows;
  while (q.next())
    rows.append(QJsonArray{q.value(0).toString(), q.value(1).toString(),
                           q.value(2).toString(), q.value(3).toString(),
                           q.value(4).toString(), q.value(5).toString()});
  if (q.lastError().isValid())
    throw std::runtime_error("INTERNAL_ERROR");
  return QJsonDocument(rows).toJson(QJsonDocument::Compact);
}
/// approved resolves origin permission afresh rather than trusting persisted
/// target text.
QUrl approved(QSqlDatabase db, const QString &account,
              const QString &endpoint) {
  QSqlQuery q(db);
  q.prepare("SELECT base_url FROM accounts WHERE id=? AND endpoint_id=? AND "
            "state='ready' UNION ALL SELECT e.base_url FROM endpoints e JOIN "
            "accounts a ON a.id=e.account_id WHERE a.id=? AND a.state='ready' "
            "AND e.id=? AND e.approved=1");
  q.addBindValue(account);
  q.addBindValue(endpoint);
  q.addBindValue(account);
  q.addBindValue(endpoint);
  execute(q);
  if (!q.next())
    throw std::runtime_error("ENDPOINT_UNTRUSTED");
  const auto server = canonicalServer(q.value(0).toString());
  if (q.next())
    throw std::runtime_error("ENDPOINT_UNTRUSTED");
  if (q.lastError().isValid())
    throw std::runtime_error("INTERNAL_ERROR");
  return server;
}
} // namespace

bool SwitchState::createSchema(QSqlDatabase db) {
  QSqlQuery q(db);
  return q.exec("CREATE TABLE IF NOT EXISTS endpoint_bindings(account_id TEXT "
                "PRIMARY KEY REFERENCES accounts(id) ON DELETE "
                "CASCADE,endpoint_id TEXT NOT NULL,epoch INTEGER NOT NULL "
                "CHECK(epoch>=0),allocated_epoch INTEGER NOT NULL "
                "CHECK(allocated_epoch>=epoch))") &&
         q.exec("CREATE TABLE IF NOT EXISTS switch_intents(account_id TEXT "
                "PRIMARY KEY REFERENCES accounts(id) ON DELETE "
                "CASCADE,operation_id TEXT UNIQUE NOT NULL,target_id TEXT NOT "
                "NULL,target_epoch INTEGER NOT NULL "
                "CHECK(target_epoch>0),phase TEXT NOT NULL CHECK(phase IN "
                "('prepared','reconciling','blocked')),roots BLOB NOT "
                "NULL,pending BLOB NOT NULL)");
}

SwitchState::Binding SwitchState::binding(const QString &account) const {
  QSqlQuery q(db_);
  q.prepare("SELECT COALESCE(b.endpoint_id,a.endpoint_id),COALESCE(b.epoch,0) "
            "FROM accounts a LEFT JOIN endpoint_bindings b ON "
            "b.account_id=a.id WHERE a.id=? AND a.state='ready'");
  q.addBindValue(account);
  execute(q);
  if (!q.next())
    throw std::runtime_error("NOT_FOUND");
  const auto endpoint = q.value(0).toString();
  const auto epoch = q.value(1).toLongLong();
  if (epoch < 0)
    throw std::runtime_error("INTERNAL_ERROR");
  q.finish();
  return {endpoint, approved(db_, account, endpoint), epoch};
}

qint64 SwitchState::begin(const QString &account, const QString &target,
                          const QString &operation, qint64 expectedEpoch) {
  const auto current = binding(account);
  if (current.epoch != expectedEpoch)
    throw std::runtime_error("REVISION_CONFLICT");
  approved(db_, account, target);
  if (operation.isEmpty())
    throw std::runtime_error("INVALID_PARAMS");
  QSqlQuery q(db_);
  q.prepare("SELECT phase FROM switch_intents WHERE account_id=?");
  q.addBindValue(account);
  execute(q);
  if (q.next() && q.value(0).toString() != "blocked")
    throw std::runtime_error("QUIESCE_STALLED");
  q.finish();
  q.prepare("INSERT OR IGNORE INTO endpoint_bindings VALUES(?,?,?,?)");
  q.addBindValue(account);
  q.addBindValue(current.endpoint);
  q.addBindValue(current.epoch);
  q.addBindValue(current.epoch);
  execute(q);
  q.prepare("SELECT allocated_epoch FROM endpoint_bindings WHERE account_id=?");
  q.addBindValue(account);
  execute(q);
  if (!q.next())
    throw std::runtime_error("INTERNAL_ERROR");
  const auto allocated = q.value(0).toLongLong();
  if (allocated < current.epoch ||
      allocated == std::numeric_limits<qint64>::max())
    throw std::runtime_error("INTERNAL_ERROR");
  const auto epoch = allocated + 1;
  q.finish();
  const auto roots = identities(db_, account);
  QJsonArray pending;
  for (const auto &root : QJsonDocument::fromJson(roots).array())
    pending.append(root.toArray().at(0));
  q.prepare(
      "UPDATE endpoint_bindings SET allocated_epoch=? WHERE account_id=?");
  q.addBindValue(epoch);
  q.addBindValue(account);
  execute(q);
  q.prepare("INSERT INTO switch_intents VALUES(?,?,?,?,'prepared',?,?) ON "
            "CONFLICT(account_id) DO UPDATE SET "
            "operation_id=excluded.operation_id,target_id=excluded.target_id,"
            "target_epoch=excluded.target_epoch,phase=excluded.phase,roots="
            "excluded.roots,pending=excluded.pending");
  q.addBindValue(account);
  q.addBindValue(operation);
  q.addBindValue(target);
  q.addBindValue(epoch);
  q.addBindValue(roots);
  q.addBindValue(QJsonDocument(pending).toJson(QJsonDocument::Compact));
  execute(q);
  return epoch;
}

void SwitchState::commit(const QString &account, qint64 epoch) {
  QSqlQuery q(db_);
  q.prepare("SELECT target_id,roots FROM switch_intents WHERE account_id=? AND "
            "target_epoch=? AND phase='prepared'");
  q.addBindValue(account);
  q.addBindValue(epoch);
  execute(q);
  if (!q.next())
    throw std::runtime_error("REVISION_CONFLICT");
  const auto target = q.value(0).toString();
  const auto frozen = q.value(1).toByteArray();
  q.finish();
  approved(db_, account, target);
  if (identities(db_, account) != frozen)
    throw std::runtime_error("REVISION_CONFLICT");
  q.prepare("UPDATE endpoint_bindings SET endpoint_id=?,epoch=? WHERE "
            "account_id=? AND allocated_epoch=? AND epoch<?");
  q.addBindValue(target);
  q.addBindValue(epoch);
  q.addBindValue(account);
  q.addBindValue(epoch);
  q.addBindValue(epoch);
  execute(q);
  if (q.numRowsAffected() != 1)
    throw std::runtime_error("REVISION_CONFLICT");
  q.prepare(
      frozen == "[]"
          ? "DELETE FROM switch_intents WHERE account_id=?"
          : "UPDATE switch_intents SET phase='reconciling' WHERE account_id=?");
  q.addBindValue(account);
  execute(q);
}

bool SwitchState::reconciled(const QString &account, qint64 epoch,
                             const QString &folder) {
  QSqlQuery q(db_);
  q.prepare("SELECT pending FROM switch_intents WHERE account_id=? AND "
            "target_epoch=? AND phase='reconciling'");
  q.addBindValue(account);
  q.addBindValue(epoch);
  execute(q);
  if (!q.next())
    return false;
  QJsonParseError error;
  const auto document =
      QJsonDocument::fromJson(q.value(0).toByteArray(), &error);
  if (error.error != QJsonParseError::NoError || !document.isArray())
    throw std::runtime_error("INTERNAL_ERROR");
  auto pending = document.array();
  q.finish();
  const auto index =
      std::find(pending.begin(), pending.end(), QJsonValue(folder));
  if (index == pending.end())
    return false;
  pending.erase(index);
  if (pending.isEmpty()) {
    q.prepare("DELETE FROM switch_intents WHERE account_id=?");
    q.addBindValue(account);
  } else {
    q.prepare("UPDATE switch_intents SET pending=? WHERE account_id=?");
    q.addBindValue(QJsonDocument(pending).toJson(QJsonDocument::Compact));
    q.addBindValue(account);
  }
  execute(q);
  return true;
}

bool SwitchState::gated(const QString &account) const {
  QSqlQuery q(db_);
  q.prepare("SELECT 1 FROM switch_intents WHERE account_id=?");
  q.addBindValue(account);
  execute(q);
  return q.next();
}

QUrl SwitchState::target(const QString &account,
                         const QString &endpoint) const {
  return approved(db_, account, endpoint);
}

void SwitchState::block(const QString &account, qint64 epoch) {
  QSqlQuery q(db_);
  q.prepare("UPDATE switch_intents SET phase='blocked' WHERE account_id=? AND "
            "target_epoch=?");
  q.addBindValue(account);
  q.addBindValue(epoch);
  execute(q);
  if (q.numRowsAffected() != 1)
    throw std::runtime_error("REVISION_CONFLICT");
}

void SwitchState::defer(const QString &account, qint64 epoch) {
  QSqlQuery q(db_);
  q.prepare("DELETE FROM switch_intents WHERE account_id=? AND target_epoch=? "
            "AND phase='prepared'");
  q.addBindValue(account);
  q.addBindValue(epoch);
  execute(q);
  if (q.numRowsAffected() != 1)
    throw std::runtime_error("REVISION_CONFLICT");
}

QStringList SwitchState::interrupt() {
  QSqlQuery q(db_);
  q.prepare("SELECT operation_id FROM switch_intents WHERE phase!='blocked'");
  execute(q);
  QStringList operations;
  while (q.next())
    operations.append(q.value(0).toString());
  if (q.lastError().isValid())
    throw std::runtime_error("INTERNAL_ERROR");
  q.finish();
  if (!q.exec(
          "UPDATE switch_intents SET phase='blocked' WHERE phase!='blocked'"))
    throw std::runtime_error("INTERNAL_ERROR");
  return operations;
}
