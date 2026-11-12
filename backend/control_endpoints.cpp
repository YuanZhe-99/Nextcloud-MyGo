// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "namespace.h"
#include "switch_state.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>

namespace {
/// execute requires a successful Control query without exposing private
/// database details.
void execute(QSqlQuery &q) {
  if (!q.exec())
    throw std::runtime_error("INTERNAL_ERROR");
}
/// failure constructs a sanitized endpoint operation error.
QJsonObject failure(const QString &reason) {
  return {{"code", -32000},
          {"message", "Endpoint operation could not be completed"},
          {"data", QJsonObject{{"reasonCode", reason}}}};
}
/// roots reads the exact registered namespace, including paused folders, for
/// stale-result detection.
QJsonArray roots(QSqlDatabase db, const QString &account) {
  QSqlQuery q(db);
  q.prepare("SELECT id,remote_path,remote_identity FROM folders WHERE "
            "account_id=? ORDER BY id");
  q.addBindValue(account);
  execute(q);
  QJsonArray result;
  while (q.next())
    result.append(QJsonArray{q.value(0).toString(), q.value(1).toString(),
                             q.value(2).toString()});
  return result;
}
} // namespace

QUrl Control::verifySwitchTarget(QString account, QString endpoint) try {
  if (throttleRemaining(account) > 0)
    throw std::runtime_error("SERVER_THROTTLED");
  const auto base = SwitchState(db_).target(account, endpoint);
  QSqlQuery q(db_);
  q.prepare("SELECT uid,root_id,login_name,credential_ref FROM accounts WHERE "
            "id=? AND state='ready'");
  q.addBindValue(account);
  execute(q);
  if (!q.next())
    throw std::runtime_error("NOT_FOUND");
  const auto uid = q.value(0).toString(), root = q.value(1).toString(),
             login = q.value(2).toString(), credential = q.value(3).toString();
  q.finish();
  if (credential != credentialReference(account))
    throw std::runtime_error("AUTH_REQUIRED");
  const auto bindings = roots(db_, account);
  const auto validate = [&] {
    if (throttleRemaining(account) > 0)
      throw std::runtime_error("SERVER_THROTTLED");
    if (storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
      throw std::runtime_error("CANCELLED");
    if (SwitchState(db_).target(account, endpoint) != base ||
        roots(db_, account) != bindings)
      throw std::runtime_error("REVISION_CONFLICT");
  };
  validate();
  const auto ocs = readServerJson(base, "ocs/v2.php/cloud/user?format=json",
                                  login, credential)
                       .value("ocs")
                       .toObject();
  const auto code = ocs.value("meta").toObject().value("statuscode").toInt();
  if ((code != 100 && code != 200) ||
      ocs.value("data").toObject().value("id").toString() != uid)
    throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
  validate();
  syncCapabilities(readServerJson(
      base, "ocs/v2.php/cloud/capabilities?format=json", login, credential));
  validate();
  const auto home = readCollections(base, uid, login, credential, "/", false);
  if (home.front().identity != root || home.front().encrypted)
    throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
  if (!home.front().permissions.contains('G'))
    throw std::runtime_error("PERMISSION_DENIED");
  for (const auto &entry : bindings) {
    validate();
    const auto binding = entry.toArray();
    const auto found = readCollections(base, uid, login, credential,
                                       binding[1].toString(), false);
    if (found.front().identity != binding[2].toString() ||
        found.front().encrypted)
      throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
    if (!found.front().permissions.contains('G'))
      throw std::runtime_error("PERMISSION_DENIED");
  }
  validate();
  return base;
} catch (const std::exception &error) {
  metadataFailed(account, error);
  throw;
}

QJsonArray Control::endpointProjection() const {
  QSqlQuery q(db_);
  // Only ready accounts: an account being removed has no binding to show.
  q.prepare("SELECT e.id,e.account_id,e.name,e.base_url,e.state,e.kind FROM "
            "endpoints "
            "e JOIN accounts a ON a.id=e.account_id WHERE e.approved=1 AND "
            "a.state='ready' ORDER BY e.id");
  execute(q);
  QJsonArray result;
  while (q.next()) {
    const auto state = q.value(4).toString();
    const auto account = q.value(1).toString();
    QSqlQuery intent(db_);
    intent.prepare("SELECT phase FROM switch_intents WHERE account_id=?");
    intent.addBindValue(account);
    execute(intent);
    const bool blocked =
        intent.next() && intent.value(0).toString() == "blocked";
    result.append(
        QJsonObject{{"id", q.value(0).toString()},
                    {"accountId", q.value(1).toString()},
                    {"name", q.value(2).toString()},
                    {"url", q.value(3).toString()},
                    {"kind", q.value(5).toString()},
                    {"main", false},
                    {"approved", true},
                    {"verified", state == "verified"   ? "verified"
                                 : state == "rejected" ? "failed"
                                                       : "pending"},
                    {"blocked", state != "verified"},
                    {"reachable", state == "verified"},
                    {"inUse", SwitchState(db_).binding(account).endpoint ==
                                      q.value(0).toString() &&
                                  !blocked}});
    if (network_ && network_->accounts.contains(account)) {
      const auto &selection = network_->accounts.at(account);
      const auto id = q.value(0).toString();
      auto projection = result.last().toObject();
      const auto health =
          selection.policy.health(id, network_->clock.elapsed());
      projection.insert("reachable", state == "verified" && !health.failed);
      projection.insert("blocked", state != "verified" || health.failed);
      if (selection.networkBlocked && selection.active == id)
        projection.insert("inUse", false);
      result.replace(result.size() - 1, projection);
    }
  }
  return result;
}

void Control::stopForStorageFailure(std::source_location from) {
  // Name where and why admission closed: the file name, line and the
  // exception's fixed code only.
  QByteArray reason = "no exception";
  if (const auto current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception &error) {
      reason = QByteArray(error.what()).left(80);
    } catch (...) {
      reason = "unknown exception";
    }
  }
  const auto file = QByteArray(from.file_name());
  qCritical("SyncD stopped for a storage failure at %s:%u (%s)",
            file.mid(qMax(file.lastIndexOf('/'), file.lastIndexOf('\\')) + 1)
                .constData(),
            unsigned(from.line()), reason.constData());
  if (storageFailed_)
    return;
  storageFailed_ = true;
#ifdef SYNCD_WITH_ENGINE
  if (runtime_)
    runtime_->shutdown();
  else
    shutdown_ = true;
#else
  shutdown_ = true;
#endif
}

void Control::recoverEndpoints() {
  QSqlQuery q(db_);
  q.prepare("SELECT id,receipt FROM operations WHERE method IN "
            "('endpoints.verify','endpoints.stage','endpoints.approveOrigin')");
  execute(q);
  QStringList unfinished;
  while (q.next())
    if (QJsonDocument::fromJson(q.value(1).toByteArray())
            .object()
            .value("state") == "running")
      unfinished.append(q.value(0).toString());
  q.finish();
  for (const auto &operation : unfinished)
    operationFinished(operation, "OPERATION_INTERRUPTED");
  q.prepare("UPDATE endpoints SET state=CASE WHEN approved=1 THEN 'approved' "
            "ELSE 'rejected' END");
  execute(q);
}

QJsonObject Control::endpointMutation(const QString &method,
                                      const QJsonObject &params,
                                      const QByteArray &digest) {
  if (params.value("expectedRevision").toString() != QString::number(revision_))
    throw std::runtime_error("REVISION_CONFLICT");
  const bool stage = method == "endpoints.stage",
             approve = method == "endpoints.approveOrigin",
             remove = method == "endpoints.remove";
  const QStringList fields =
      stage ? QStringList{"accountId", "name", "baseUrl", "kind"}
            : QStringList{approve ? "stagingId" : "endpointId"};
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        !fields.contains(it.key()))
      throw std::runtime_error("INVALID_PARAMS");
  QString id = params.value(approve ? "stagingId" : "endpointId").toString(),
          account, name, url, kind;
  QSqlQuery q(db_);
  if (method == "endpoints.verify") {
    q.prepare("SELECT id FROM accounts WHERE endpoint_id=? AND state='ready'");
    q.addBindValue(id);
    execute(q);
    const auto canonical = q.next() ? q.value(0).toString() : QString{};
    q.finish();
    if (!canonical.isEmpty()) {
      if (throttleRemaining(canonical) > 0)
        throw std::runtime_error("SERVER_THROTTLED");
      q.prepare("SELECT phase FROM switch_intents WHERE account_id=?");
      q.addBindValue(canonical);
      execute(q);
      const bool switching = q.next() && q.value(0) != "blocked";
      q.finish();
      if (switching || reauthenticating(canonical) ||
          endpointChecks_.contains(canonical))
        throw std::runtime_error("QUIESCE_STALLED");
      if (endpointChecks_.size() >= 4)
        throw std::runtime_error("RATE_LIMITED");
      const auto operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
      const QJsonObject receipt{{"operationId", operation},
                                {"state", "running"},
                                {"result", QJsonObject{}}};
      accountChange([&] {
        q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
                  "VALUES(?,?,?,?,'endpoints.verify')");
        q.addBindValue(params.value("operationKey").toString());
        q.addBindValue(operation);
        q.addBindValue(digest);
        q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
        execute(q);
      });
      endpointChecks_[canonical] = operation;
      const auto generation = network_ ? network_->generation : 0;
      QTimer::singleShot(
          0, &networkTasks_, [this, canonical, id, operation, generation] {
            QString reason;
            try {
              verifySwitchTarget(canonical, id);
              if (network_ && generation != network_->generation)
                throw std::runtime_error("REVISION_CONFLICT");
            } catch (const std::exception &e) {
              const QString value = QString::fromUtf8(e.what());
              const QStringList known{"AUTH_REQUIRED",
                                      "CREDENTIALS_LOCKED",
                                      "TLS_ERROR",
                                      "ENDPOINT_UNTRUSTED",
                                      "ENDPOINT_IDENTITY_MISMATCH",
                                      "NETWORK_UNAVAILABLE",
                                      "PERMISSION_DENIED",
                                      "REMOTE_LOCKED",
                                      "SERVER_THROTTLED",
                                      "INVALID_SERVER_RESPONSE",
                                      "REVISION_CONFLICT",
                                      "REMOTE_ROOT_UNAVAILABLE",
                                      "UNSUPPORTED_CAPABILITY",
                                      "UNSUPPORTED_FOLDER_MODE",
                                      "CANCELLED",
                                      "NOT_FOUND"};
              reason = known.contains(value) ? value
                                             : QStringLiteral("INTERNAL_ERROR");
            }
            endpointChecks_.erase(canonical);
            try {
              accountChange([&] {
                if (reason.isEmpty())
                  reviewEndpoint(canonical, id);
                else if (reason == "TLS_ERROR" ||
                         reason == "ENDPOINT_UNTRUSTED" ||
                         reason == "ENDPOINT_IDENTITY_MISMATCH")
                  quarantineEndpoint(canonical, id, reason);
                const QJsonObject receipt{
                    {"operationId", operation},
                    {"state", reason.isEmpty() ? "succeeded" : "failed"},
                    {"result", QJsonObject{{"endpointId", id}}},
                    {"error", reason.isEmpty() ? QJsonValue(QJsonValue::Null)
                                               : QJsonValue(failure(reason))}};
                QSqlQuery q(db_);
                q.prepare("UPDATE operations SET receipt=? WHERE id=?");
                q.addBindValue(
                    QJsonDocument(receipt).toJson(QJsonDocument::Compact));
                q.addBindValue(operation);
                execute(q);
              });
            } catch (...) {
              stopForStorageFailure();
            }
          });
      return receipt;
    }
  }
  if (!remove) {
    q.prepare("SELECT COUNT(*) FROM endpoints WHERE state IN "
              "('probing','verifying')");
    execute(q);
    q.next();
    if (q.value(0).toInt() >= 4)
      throw std::runtime_error("RATE_LIMITED");
    q.finish();
  }
  if (stage) {
    account = params.value("accountId").toString();
    name = params.value("name").toString().trimmed();
    // Protocol 1.9: how the user reaches this address, for its icon and
    // name; it never affects trust or scoring.
    kind = params.value("kind").toString(QStringLiteral("internet"));
    if ((params.contains("kind") && !params.value("kind").isString()) ||
        (kind != "internet" && kind != "home" && kind != "vpn"))
      throw std::runtime_error("INVALID_PARAMS");
    if (name.isEmpty() || name.size() > 128)
      throw std::runtime_error("INVALID_PARAMS");
    url = canonicalServer(params.value("baseUrl").toString()).toString();
    q.prepare("SELECT base_url FROM accounts WHERE id=? AND state='ready'");
    q.addBindValue(account);
    execute(q);
    if (!q.next())
      throw std::runtime_error("NOT_FOUND");
    if (q.value(0).toString() == url)
      throw std::runtime_error("INVALID_PARAMS");
    q.finish();
    q.prepare(
        "SELECT COUNT(*),SUM(base_url=?) FROM endpoints WHERE account_id=? AND "
        "(approved=1 OR state IN ('probing','verifying') OR (state!='rejected' "
        "AND expires>?))");
    q.addBindValue(url);
    q.addBindValue(account);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    execute(q);
    q.next();
    if (q.value(0).toInt() >= 7)
      throw std::runtime_error("RATE_LIMITED");
    if (q.value(1).toInt())
      throw std::runtime_error("INVALID_PARAMS");
    q.finish();
    id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  } else {
    q.prepare("SELECT e.account_id,e.approved,e.state,e.expires FROM endpoints "
              "e JOIN accounts a ON a.id=e.account_id WHERE e.id=? AND "
              "a.state='ready'");
    q.addBindValue(id);
    execute(q);
    if (!q.next())
      throw std::runtime_error("NOT_FOUND");
    account = q.value(0).toString();
    const auto state = q.value(2).toString();
    if (state == "probing" || state == "verifying")
      throw std::runtime_error("QUIESCE_STALLED");
    if (approve &&
        (state != "staged" ||
         QDateTime::fromString(q.value(3).toString(), Qt::ISODateWithMs) <=
             QDateTime::currentDateTimeUtc()))
      throw std::runtime_error("REVISION_CONFLICT");
    if (!approve && !remove && !q.value(1).toBool())
      throw std::runtime_error("ENDPOINT_UNTRUSTED");
    q.finish();
    if (remove) {
      q.prepare("SELECT 1 FROM endpoint_bindings WHERE account_id=? AND "
                "endpoint_id=? UNION ALL SELECT 1 FROM switch_intents WHERE "
                "account_id=? AND target_id=? LIMIT 1");
      q.addBindValue(account);
      q.addBindValue(id);
      q.addBindValue(account);
      q.addBindValue(id);
      execute(q);
      if (q.next())
        throw std::runtime_error("QUIESCE_STALLED");
      q.finish();
    }
  }
  bool switching = SwitchState(db_).gated(account);
  if (!remove && throttleRemaining(account) > 0)
    throw std::runtime_error("SERVER_THROTTLED");
  if (method == "endpoints.verify") {
    q.prepare("SELECT phase FROM switch_intents WHERE account_id=?");
    q.addBindValue(account);
    execute(q);
    switching = q.next() && q.value(0) != "blocked";
    q.finish();
  }
  if (switching || reauthenticating(account) ||
      endpointChecks_.contains(account))
    throw std::runtime_error("QUIESCE_STALLED");
  if (!remove) {
    q.prepare("SELECT COUNT(*) FROM endpoints WHERE account_id=? AND state IN "
              "('probing','verifying')");
    q.addBindValue(account);
    execute(q);
    q.next();
    if (q.value(0).toInt() >= 2)
      throw std::runtime_error("RATE_LIMITED");
    q.finish();
  }
  const auto operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QJsonObject receipt{{"operationId", operation},
                            {"state", remove ? "succeeded" : "running"},
                            {"result", QJsonObject{}}};
  accountChange([&] {
    if (stage) {
      q.prepare("DELETE FROM endpoints WHERE account_id=? AND approved=0 AND "
                "state NOT IN ('probing','verifying') AND (state='rejected' OR "
                "expires<=?)");
      q.addBindValue(account);
      q.addBindValue(
          QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
      execute(q);
      q.prepare("INSERT INTO "
                "endpoints(id,account_id,name,base_url,state,expires,operation_"
                "id,kind) VALUES(?,?,?,?,'probing',?,?,?)");
      q.addBindValue(id);
      q.addBindValue(account);
      q.addBindValue(name);
      q.addBindValue(url);
      q.addBindValue(QDateTime::currentDateTimeUtc().addSecs(600).toString(
          Qt::ISODateWithMs));
      q.addBindValue(operation);
      q.addBindValue(kind);
      execute(q);
    } else if (remove) {
      q.prepare("DELETE FROM endpoints WHERE id=?");
      q.addBindValue(id);
      execute(q);
    } else {
      q.prepare("UPDATE endpoints SET "
                "approved=1,state='verifying',operation_id=? WHERE id=?");
      q.addBindValue(operation);
      q.addBindValue(id);
      execute(q);
    }
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,?,?,?)");
    q.addBindValue(params.value("operationKey").toString());
    q.addBindValue(operation);
    q.addBindValue(digest);
    q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    q.addBindValue(method);
    execute(q);
  });
  if (!remove)
    QTimer::singleShot(0, &networkTasks_, [this, id, operation, stage] {
      probeEndpoint(id, operation, !stage);
    });
  return receipt;
}

void Control::probeEndpoint(const QString &id, const QString &operation,
                            bool authenticated) {
  QString reason;
  QJsonObject result;
  QString account;
  QJsonArray bindings;
  try {
    QSqlQuery q(db_);
    q.prepare("SELECT "
              "e.account_id,e.base_url,e.approved,a.base_url,a.uid,a.root_id,a."
              "login_name,a.credential_ref FROM endpoints e JOIN accounts a ON "
              "a.id=e.account_id WHERE e.id=? AND e.operation_id=? AND "
              "a.state='ready'");
    q.addBindValue(id);
    q.addBindValue(operation);
    execute(q);
    if (!q.next())
      throw std::runtime_error("NOT_FOUND");
    account = q.value(0).toString();
    if (throttleRemaining(account) > 0)
      throw std::runtime_error("SERVER_THROTTLED");
    const auto base = canonicalServer(q.value(1).toString()),
               main = canonicalServer(q.value(3).toString());
    const bool approved = q.value(2).toBool();
    const auto uid = q.value(4).toString(), root = q.value(5).toString(),
               login = q.value(6).toString(),
               credential = q.value(7).toString();
    q.finish();
    if (shutdown_ || !shutdownOperation_.isEmpty())
      throw std::runtime_error("CANCELLED");
    if (!authenticated) {
      const auto status = readServerJson(base, "status.php", {}, {}, &result);
      if (!status.value("installed").toBool() ||
          status.value("maintenance").toBool(true) ||
          status.value("needsDbUpgrade").toBool(true) ||
          !status.value("versionstring").isString())
        throw std::runtime_error("INVALID_SERVER_RESPONSE");
      bool same = false;
      try {
        same = readServerJson(main, "status.php").value("versionstring") ==
               status.value("versionstring");
      } catch (const std::exception &error) {
        metadataFailed(account, error);
        if (throttleRemaining(account) > 0)
          throw;
      }
      result.insert("id", id);
      result.insert("url", base.toString());
      result.insert("sameVersion", same);
    } else {
      if (!approved || credential != credentialReference(account))
        throw std::runtime_error("ENDPOINT_UNTRUSTED");
      bindings = roots(db_, account);
      const auto ocs = readServerJson(base, "ocs/v2.php/cloud/user?format=json",
                                      login, credential)
                           .value("ocs")
                           .toObject();
      const int code = ocs.value("meta").toObject().value("statuscode").toInt();
      if ((code != 100 && code != 200) ||
          ocs.value("data").toObject().value("id").toString() != uid)
        throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
      if (storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
        throw std::runtime_error("CANCELLED");
      const auto home =
          readCollections(base, uid, login, credential, "/", false);
      if (home.front().identity != root || home.front().encrypted)
        throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
      for (const auto &entry : bindings) {
        if (throttleRemaining(account) > 0)
          throw std::runtime_error("SERVER_THROTTLED");
        if (shutdown_ || !shutdownOperation_.isEmpty())
          throw std::runtime_error("CANCELLED");
        const auto binding = entry.toArray();
        const auto found = readCollections(base, uid, login, credential,
                                           binding[1].toString(), false);
        if (found.front().identity != binding[2].toString() ||
            found.front().encrypted)
          throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
      }
      if (roots(db_, account) != bindings)
        throw std::runtime_error("REVISION_CONFLICT");
      result = {{"endpointId", id}};
    }
  } catch (const std::exception &e) {
    try {
      metadataFailed(account, e);
    } catch (...) {
      stopForStorageFailure();
      return;
    }
    reason = QString::fromUtf8(e.what());
    const QStringList known{"NOT_FOUND",           "CANCELLED",
                            "AUTH_REQUIRED",       "TLS_ERROR",
                            "ENDPOINT_UNTRUSTED",  "ENDPOINT_IDENTITY_MISMATCH",
                            "NETWORK_UNAVAILABLE", "INVALID_SERVER_RESPONSE",
                            "PERMISSION_DENIED",   "REMOTE_LOCKED",
                            "SERVER_THROTTLED",    "CREDENTIALS_LOCKED",
                            "REVISION_CONFLICT",   "REMOTE_ROOT_UNAVAILABLE"};
    if (!known.contains(reason))
      reason = "INTERNAL_ERROR";
  }
  try {
    accountChange([&] {
      QSqlQuery q(db_);
      q.prepare("UPDATE endpoints SET state=? WHERE id=? AND operation_id=?");
      const bool constraint = reason == "PERMISSION_DENIED" ||
                              reason == "REMOTE_LOCKED" ||
                              reason == "SERVER_THROTTLED";
      q.addBindValue(reason.isEmpty() ? authenticated ? "verified" : "staged"
                     : authenticated && constraint ? "approved"
                                                   : "rejected");
      q.addBindValue(id);
      q.addBindValue(operation);
      execute(q);
      QJsonObject receipt{
          {"operationId", operation},
          {"state", reason.isEmpty() ? "succeeded" : "failed"},
          {"result", reason.isEmpty() ? result : QJsonObject{}}};
      if (!reason.isEmpty())
        receipt.insert("error", failure(reason));
      q.prepare("UPDATE operations SET receipt=? WHERE id=?");
      q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
      q.addBindValue(operation);
      execute(q);
      if (reason.isEmpty() && authenticated)
        reviewEndpoint(account, id);
      else if (authenticated &&
               (reason == "TLS_ERROR" || reason == "ENDPOINT_UNTRUSTED" ||
                reason == "ENDPOINT_IDENTITY_MISMATCH"))
        quarantineEndpoint(account, id, reason);
    });
  } catch (...) {
    stopForStorageFailure();
  }
}
