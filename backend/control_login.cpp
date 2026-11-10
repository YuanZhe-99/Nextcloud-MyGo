#include "control.h"
#include "switch_state.h"
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>

namespace {
/// execute requires SQL completion before any success can be published.
void execute(QSqlQuery &q) {
  if (!q.exec())
    throw std::runtime_error("Account database operation failed");
}
/// error returns only a stable reason, never remote content or a credential
/// reference.
QJsonObject error(const QString &reason) {
  return {{"code", -32000},
          {"message", "Request could not be completed"},
          {"data", QJsonObject{{"reasonCode", reason}}}};
}
/// uuid creates independent entity and operation identities.
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
} // namespace

QString Control::credentialReference(const QString &account) const {
  if (QUuid(account).isNull() ||
      QUuid(account).toString(QUuid::WithoutBraces) != account)
    throw std::runtime_error("Invalid stored account identity");
  return "Nextcloud-MyGo/" + profileKey_ + '/' + account + "/app-password";
}

void Control::accountChange(const std::function<void()> &change) {
  const auto revision = revision_, seq = seq_;
  if (!db_.transaction())
    throw std::runtime_error("Cannot begin account transaction");
  try {
    change();
    ++revision_;
    ++seq_;
    QSqlQuery q(db_);
    q.prepare("UPDATE settings SET revision=?,seq=? WHERE id=1");
    q.addBindValue(revision_);
    q.addBindValue(seq_);
    execute(q);
    q.prepare("INSERT INTO outbox VALUES(?,?)");
    q.addBindValue(seq_);
    q.addBindValue(QJsonDocument(projection()).toJson(QJsonDocument::Compact));
    execute(q);
    if (!db_.commit())
      throw std::runtime_error("Cannot commit account transaction");
  } catch (...) {
    db_.rollback();
    revision_ = revision;
    seq_ = seq;
    throw;
  }
}

void Control::recoverAccounts() {
  QSqlQuery q(db_);
  q.prepare("SELECT id,credential_ref,operation_id,state FROM accounts WHERE "
            "state!='ready'");
  execute(q);
  QStringList abandoned;
  QStringList removals;
  QStringList unrevoked;
  while (q.next()) {
    const auto id = q.value(0).toString(), reference = credentialReference(id);
    if (q.value(1).toString() != reference)
      throw std::runtime_error("Invalid stored credential reference");
    // An interrupted login that had already acquired a password cannot prove
    // its revocation: say so instead of reporting a plain expiry.
    if (q.value(3).toString() == "creating" && findCredential(reference))
      unrevoked.append(q.value(2).toString());
    removeCredential(reference);
    abandoned.append(id);
    if (q.value(3).toString() == "removing")
      removals.append(q.value(2).toString());
  }
  q.finish();
  q.prepare("SELECT id,receipt,method FROM operations WHERE method IN "
            "('accounts.login.start','accounts.login.cancel')");
  execute(q);
  QList<QPair<QString, QJsonObject>> expired;
  while (q.next()) {
    auto receipt = QJsonDocument::fromJson(q.value(1).toByteArray()).object();
    if (q.value(2).toString() == "accounts.login.cancel" &&
        receipt.value("state") != "running")
      continue;
    if (receipt.value("state") == "failed")
      continue;
    receipt.insert("state", "failed");
    receipt.remove("result");
    receipt.insert("error", error(unrevoked.contains(q.value(0).toString())
                                      ? "APP_PASSWORD_REVOCATION_UNCONFIRMED"
                                      : "LOGIN_SESSION_EXPIRED"));
    expired.append({q.value(0).toString(), receipt});
  }
  q.finish();
  if (abandoned.isEmpty() && expired.isEmpty())
    return;
  accountChange([&] {
    for (const auto &id : abandoned) {
      q.prepare("DELETE FROM accounts WHERE id=?");
      q.addBindValue(id);
      execute(q);
    }
    for (const auto &id : removals) {
      q.prepare("SELECT receipt FROM operations WHERE id=?");
      q.addBindValue(id);
      execute(q);
      if (!q.next())
        throw std::runtime_error("Missing removal operation");
      auto receipt = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
      q.finish();
      if (receipt.value("state") != "running")
        continue;
      receipt.insert("state", "succeeded");
      receipt.insert("result",
                     QJsonObject{{"serverRevocationConfirmed", false}});
      q.prepare("UPDATE operations SET receipt=? WHERE id=?");
      q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
      q.addBindValue(id);
      execute(q);
    }
    for (const auto &item : expired) {
      q.prepare("UPDATE operations SET receipt=? WHERE id=?");
      q.addBindValue(QJsonDocument(item.second).toJson(QJsonDocument::Compact));
      q.addBindValue(item.first);
      execute(q);
    }
  });
}

QJsonObject Control::loginReceipt(QJsonObject receipt) const {
  const auto result = receipt.value("result").toObject();
  auto it = logins_.find(result.value("id").toString());
  if (it != logins_.end() && receipt.value("state") == "succeeded") {
    auto hydrated = result;
    hydrated.insert("loginURL", it->second->loginUrl);
    receipt.insert("result", hydrated);
  }
  return receipt;
}

QJsonObject Control::loginStatus(const QString &id) const {
  QSqlQuery renewal(db_);
  renewal.prepare("SELECT account_id,phase,reason FROM reauthentication WHERE "
                  "session_id=?");
  renewal.addBindValue(id);
  execute(renewal);
  if (renewal.next()) {
    const auto phase = renewal.value(1).toString();
    if (phase == "succeeded" || phase == "failed")
      return {{"state", phase == "failed" &&
                                renewal.value(2).toString() == "LOGIN_CANCELLED"
                            ? QStringLiteral("cancelled")
                            : phase},
              {"accountId",
               phase == "succeeded" ? renewal.value(0).toString() : QString{}},
              {"reasonCode", renewal.value(2).toString()}};
    if (phase == "installed")
      return {{"state", "verifying"}};
  }
  renewal.finish();
  auto it = logins_.find(id);
  if (it != logins_.end()) {
    const auto &flow = *it->second;
    return {{"state", flow.status()},
            {"accountId",
             flow.status() == "succeeded" ? flow.accountId : QString()},
            {"reasonCode", flow.reason()}};
  }
  QSqlQuery q(db_);
  q.prepare("SELECT id FROM accounts WHERE session_id=? AND state='ready'");
  q.addBindValue(id);
  execute(q);
  if (q.next())
    return {{"state", "succeeded"}, {"accountId", q.value(0).toString()}};
  return {{"state", "failed"}, {"reasonCode", "LOGIN_SESSION_EXPIRED"}};
}

bool Control::loginChanged(LoginFlow &flow) {
  try {
    QSqlQuery q(db_);
    q.prepare(
        "SELECT operation_id FROM accounts WHERE id=? AND state='creating'");
    q.addBindValue(flow.accountId);
    execute(q);
    if (!q.next())
      return false;
    const auto operation = q.value(0).toString();
    q.finish();
    QJsonObject receipt{
        {"operationId", operation},
        {"state", "succeeded"},
        {"result",
         QJsonObject{{"id", flow.id},
                     {"expires", flow.expires.toString(Qt::ISODateWithMs)}}}};
    const bool failed =
        flow.status() == "failed" || flow.status() == "cancelled";
    const auto cancellation = cancellations_.find(flow.id);
    if (failed) {
      // The reservation stays durable until removal succeeds, including vault
      // failures.
      removeCredential(credentialReference(flow.accountId));
      receipt.remove("result");
      receipt.insert("state", "failed");
      receipt.insert("error", error(flow.reason()));
    }
    accountChange([&] {
      if (flow.status() == "succeeded") {
        q.prepare(
            "UPDATE accounts SET state='ready',uid=?,display_name=?,root_id=? "
            "WHERE id=? AND state='creating'");
        q.addBindValue(flow.uid);
        q.addBindValue(flow.displayName);
        q.addBindValue(flow.rootId);
        q.addBindValue(flow.accountId);
        execute(q);
      } else if (failed) {
        q.prepare("DELETE FROM accounts WHERE id=? AND state='creating'");
        q.addBindValue(flow.accountId);
        execute(q);
      }
      q.prepare("UPDATE operations SET receipt=? WHERE id=?");
      q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
      q.addBindValue(operation);
      execute(q);
      if (failed && cancellation != cancellations_.end()) {
        QJsonObject cancelled{{"operationId", cancellation->second},
                              {"state", "succeeded"},
                              {"result", QJsonObject{}}};
        if (flow.reason() == "APP_PASSWORD_REVOCATION_UNCONFIRMED") {
          cancelled.insert("state", "failed");
          cancelled.insert("error", error(flow.reason()));
        }
        q.prepare("UPDATE operations SET receipt=? WHERE id=?");
        q.addBindValue(QJsonDocument(cancelled).toJson(QJsonDocument::Compact));
        q.addBindValue(cancellation->second);
        execute(q);
      }
    });
    if (failed && cancellation != cancellations_.end())
      cancellations_.erase(cancellation);
    return true;
  } catch (...) {
    return false;
  }
}

QJsonObject Control::loginMutation(const QString &method,
                                   const QJsonObject &params,
                                   const QByteArray &digest) {
  const auto field = method == "accounts.login.start"
                         ? "canonicalBaseUrl"
                         : (method == "accounts.login.cancel" ? "loginSessionId"
                                                              : "accountId");
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        it.key() != field)
      return error("INVALID_PARAMS");
  if (!params.value(field).isString() ||
      params.value(field).toString().isEmpty())
    return error("INVALID_PARAMS");
  QSqlQuery q(db_);
  QUrl base;
  QString session, account;
  if (method == "accounts.login.start") {
    try {
      base = canonicalServer(params.value(field).toString());
    } catch (...) {
      return error("INVALID_PARAMS");
    }
    q.prepare("SELECT COUNT(*) FROM accounts");
    execute(q);
    q.next();
    if (q.value(0).toInt() != 0)
      return error("UNSUPPORTED_CAPABILITY");
    q.finish();
    // Terminal memory-only sessions can be discarded; persistent operation
    // receipts retain deduplication.
    std::erase_if(logins_, [](const auto &item) {
      const auto s = item.second->status();
      return s == "failed" || s == "cancelled";
    });
    if (logins_.size() >= 4)
      return error("RATE_LIMITED");
    account = uuid();
    session = uuid();
  } else if (method == "accounts.login.cancel") {
    session = params.value(field).toString();
    if (!logins_.contains(session))
      return error("NOT_FOUND");
    if (logins_.at(session)->status() == "succeeded")
      return error("REVISION_CONFLICT");
    if (cancellations_.contains(session))
      return error("QUIESCE_STALLED");
  } else {
    if (params.value("expectedRevision").toString() !=
        QString::number(revision_))
      return error("REVISION_CONFLICT");
    account = params.value(field).toString();
    if (endpointChecks_.contains(account))
      return error("QUIESCE_STALLED");
    q.prepare("SELECT 1 FROM reauthentication WHERE account_id=? AND phase IN "
              "('waiting','prepared','installed','discarding')");
    q.addBindValue(account);
    execute(q);
    if (q.next())
      return error("QUIESCE_STALLED");
    q.finish();
    if (SwitchState(db_).gated(account))
      return error("QUIESCE_STALLED");
    q.prepare("SELECT COUNT(*) FROM endpoints WHERE account_id=? AND state IN "
              "('probing','verifying')");
    q.addBindValue(account);
    execute(q);
    q.next();
    if (q.value(0).toInt())
      return error("QUIESCE_STALLED");
    q.finish();
    q.prepare("SELECT COUNT(*) FROM folders WHERE account_id=?");
    q.addBindValue(account);
    execute(q);
    q.next();
    if (q.value(0).toInt())
      return error("QUIESCE_STALLED");
    q.finish();
    q.prepare("SELECT id FROM accounts WHERE id=? AND state='ready'");
    q.addBindValue(account);
    execute(q);
    if (!q.next())
      return error("NOT_FOUND");
    q.finish();
  }
  const auto operation = uuid();
  const bool cancelFinished = method == "accounts.login.cancel" &&
                              (logins_.at(session)->status() == "failed" ||
                               logins_.at(session)->status() == "cancelled");
  QJsonObject receipt{{"operationId", operation},
                      {"state", cancelFinished ? "succeeded" : "running"},
                      {"result", QJsonObject{}}};
  if (cancelFinished &&
      logins_.at(session)->reason() == "APP_PASSWORD_REVOCATION_UNCONFIRMED") {
    receipt.insert("state", "failed");
    receipt.insert("error", error("APP_PASSWORD_REVOCATION_UNCONFIRMED"));
  }
  accountChange([&] {
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,?,?,?)");
    q.addBindValue(params.value("operationKey").toString());
    q.addBindValue(operation);
    q.addBindValue(digest);
    q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    q.addBindValue(method);
    execute(q);
    if (method == "accounts.login.start") {
      q.prepare("INSERT INTO "
                "accounts(id,session_id,operation_id,endpoint_id,base_url,"
                "credential_ref,state) VALUES(?,?,?,?,?,?,'creating')");
      q.addBindValue(account);
      q.addBindValue(session);
      q.addBindValue(operation);
      q.addBindValue(uuid());
      q.addBindValue(base.toString(QUrl::FullyEncoded));
      q.addBindValue(credentialReference(account));
      execute(q);
    } else if (method == "accounts.remove") {
      q.prepare(
          "UPDATE accounts SET state='removing',operation_id=? WHERE id=?");
      q.addBindValue(operation);
      q.addBindValue(account);
      execute(q);
    }
  });
  if (method == "accounts.login.start") {
    auto flow = std::make_shared<LoginFlow>(base, session, account);
    flow->saveCredential = [this, account](const QString &user,
                                           const QString &password) {
      try {
        QSqlQuery q(db_);
        q.prepare(
            "UPDATE accounts SET login_name=? WHERE id=? AND state='creating'");
        q.addBindValue(user);
        q.addBindValue(account);
        execute(q);
        if (q.numRowsAffected() != 1)
          return false;
        writeCredential(credentialReference(account), user, password);
        return true;
      } catch (...) {
        return false;
      }
    };
    flow->changed = [this](LoginFlow &flow) { return loginChanged(flow); };
    flow->throttled = [this, account](qint64 delay) {
      try {
        throttleAccount(account, delay);
        return true;
      } catch (...) {
        return false;
      }
    };
    auto pointer = flow.get();
    logins_.emplace(session, std::move(flow));
    QTimer::singleShot(0, pointer, [pointer] { pointer->start(); });
  } else if (method == "accounts.login.cancel") {
    if (!cancelFinished) {
      cancellations_.emplace(session, operation);
      logins_.at(session)->cancel();
    }
  } else
    QTimer::singleShot(0, &networkTasks_, [this, account, operation] {
      removeAccount(account, operation);
    });
  return receipt;
}

void Control::removeAccount(const QString &account, const QString &operation) {
  try {
    QSqlQuery q(db_);
    q.prepare("SELECT base_url,login_name FROM accounts WHERE id=? AND "
              "state='removing'");
    q.addBindValue(account);
    execute(q);
    if (!q.next())
      return;
    const auto base = canonicalServer(q.value(0).toString());
    auto secret = readCredential(credentialReference(account)).toUtf8();
    QNetworkRequest request(
        base.resolved(QUrl("ocs/v2.php/core/apppassword?format=json")));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    request.setRawHeader("OCS-APIRequest", "true");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader(
        "Authorization",
        "Basic " + (q.value(1).toString().toUtf8() + ':' + secret).toBase64());
    secret.fill(0);
    auto manager = new QNetworkAccessManager(&networkTasks_);
    auto reply = manager->sendCustomRequest(request, "DELETE");
    reply->setReadBufferSize(1048577);
    auto body = std::make_shared<QByteArray>();
    QObject::connect(reply, &QIODevice::readyRead, &networkTasks_,
                     [reply, body] {
                       body->append(reply->readAll());
                       if (body->size() > 1048576)
                         reply->abort();
                     });
    QObject::connect(reply, &QNetworkReply::finished, &networkTasks_,
                     [this, manager, reply, body, account, operation] {
                       body->append(reply->readAll());
                       const auto meta = QJsonDocument::fromJson(*body)
                                             .object()
                                             .value("ocs")
                                             .toObject()
                                             .value("meta")
                                             .toObject();
                       const bool revoked =
                           body->size() <= 1048576 &&
                           reply->error() == QNetworkReply::NoError &&
                           reply->attribute(
                                    QNetworkRequest::HttpStatusCodeAttribute)
                                   .toInt() == 200 &&
                           meta.value("status").toString() == "ok";
                       manager->deleteLater();
                       finishRemoval(account, operation, revoked);
                     });
  } catch (...) {
    finishRemoval(account, operation, false);
  }
}

void Control::finishRemoval(const QString &account, const QString &operation,
                            bool revoked) {
  QJsonObject receipt{
      {"operationId", operation},
      {"state", "succeeded"},
      {"result", QJsonObject{{"serverRevocationConfirmed", revoked}}}};
  try {
    removeCredential(credentialReference(account));
    accountChange([&] {
      QSqlQuery q(db_);
      q.prepare("DELETE FROM accounts WHERE id=? AND state='removing'");
      q.addBindValue(account);
      execute(q);
      q.prepare("UPDATE operations SET receipt=? WHERE id=?");
      q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
      q.addBindValue(operation);
      execute(q);
    });
    std::erase_if(logins_, [&](const auto &item) {
      return item.second->accountId == account;
    });
  } catch (...) {
    receipt.insert("state", "failed");
    receipt.insert("error", error("CREDENTIALS_LOCKED"));
    try {
      accountChange([&] {
        QSqlQuery q(db_);
        q.prepare("UPDATE operations SET receipt=? WHERE id=?");
        q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
        q.addBindValue(operation);
        execute(q);
      });
    } catch (...) {
    }
  }
}
