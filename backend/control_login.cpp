#include "control.h"
#include "namespace.h"
#include "platform/windows/ondemand.h"
#include "switch_state.h"
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>
#include <limits>
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
  QStringList held;
  while (q.next()) {
    const auto id = q.value(0).toString(), reference = credentialReference(id);
    if (q.value(1).toString() != reference)
      throw std::runtime_error("Invalid stored credential reference");
    // The previous process recorded that this acquired password's DELETE was
    // only delayed by the server and that none was in flight: it can still be
    // sent once, so the slot and the reservation stay until its outcome.
    QSqlQuery record(db_);
    record.prepare("SELECT 1 FROM held_revocations WHERE account_id=?");
    record.addBindValue(id);
    execute(record);
    if (q.value(3).toString() == "creating" && record.next() &&
        findCredential(reference)) {
      held.append(id);
      continue;
    }
    // An interrupted login that had already acquired a password cannot prove
    // its revocation: say so instead of reporting a plain expiry.
    if (q.value(3).toString() == "creating" && findCredential(reference))
      unrevoked.append(q.value(2).toString());
    if (q.value(3).toString() == "removing") {
      // A removal interrupted while releasing the account: its mappings,
      // on-demand registrations and renewal slots go too; files and journals
      // stay.
#ifdef SYNCD_WITH_ENGINE
      record.prepare("SELECT id,local_path FROM folders WHERE account_id=? "
                     "AND mode='onDemand'");
      record.addBindValue(id);
      execute(record);
      while (record.next())
        OnDemandRoot::unregister(
            record.value(1).toString(),
            OnDemandRoot::Identity{id, record.value(0).toString(), {}});
#endif
      record.prepare("SELECT session_id FROM reauthentication WHERE "
                     "account_id=?");
      record.addBindValue(id);
      execute(record);
      if (record.next()) {
        removeCredential(reauthReference(id, record.value(0).toString()));
        removeCredential(reauthReference(id, record.value(0).toString(), true));
      }
      removals.append(q.value(2).toString());
    }
    removeCredential(reference);
    abandoned.append(id);
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
  // No earlier than the shared 60-second floor: a lock delay is not stored,
  // and a restart must not turn it into an immediate request.
  for (const auto &id : held)
    QTimer::singleShot(60000, &networkTasks_,
                       [this, id] { retireHeldPassword(id); });
  if (abandoned.isEmpty() && expired.isEmpty())
    return;
  accountChange([&] {
    for (const auto &id : abandoned) {
      q.prepare("DELETE FROM folders WHERE account_id=?");
      q.addBindValue(id);
      execute(q);
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
    // Several accounts are allowed; the same server identity and user twice
    // is refused once the new login has proven who it is (ACCOUNT_EXISTS).
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
    // Removal is never refused for a busy account: a running sign-in, switch,
    // probe or cycle is stopped by releaseAccount before anything is removed.
    account = params.value(field).toString();
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
    // The same user on the same server (its DAV root identity names the
    // instance) is one account, whichever address the login used.
    flow->verified = [this](LoginFlow &flow) {
      QSqlQuery q(db_);
      q.prepare("SELECT 1 FROM accounts WHERE state='ready' AND uid=? AND "
                "root_id=? AND id!=?");
      q.addBindValue(flow.uid);
      q.addBindValue(flow.rootId);
      q.addBindValue(flow.accountId);
      if (!q.exec())
        flow.reject("INTERNAL_ERROR");
      else if (q.next())
        flow.reject("ACCOUNT_EXISTS");
      else
        flow.accept();
    };
    flow->throttled = [this, account](qint64 delay) {
      try {
        throttleAccount(account, delay);
        return true;
      } catch (...) {
        return false;
      }
    };
    flow->held = [this, account](bool value) {
      QSqlQuery q(db_);
      q.prepare(value ? "INSERT OR IGNORE INTO held_revocations VALUES(?)"
                      : "DELETE FROM held_revocations WHERE account_id=?");
      q.addBindValue(account);
      return q.exec();
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
      releaseAccount(account, operation, QDeadlineTimer(120000));
    });
  return receipt;
}

void Control::retireHeldPassword(const QString &account) {
  try {
    if (storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
      return; // Still recorded as held: the next boot decides again.
    const auto remaining = throttleRemaining(account);
    if (remaining == std::numeric_limits<qint64>::max()) {
      finishHeldPassword(account, false); // No automatic retry is allowed.
      return;
    }
    if (remaining > 0) {
      QTimer::singleShot(std::min<qint64>(remaining, 2147483647),
                         &networkTasks_,
                         [this, account] { retireHeldPassword(account); });
      return;
    }
    QSqlQuery q(db_);
    q.prepare("SELECT a.base_url,a.login_name FROM accounts a JOIN "
              "held_revocations h ON h.account_id=a.id WHERE a.id=? AND "
              "a.state='creating'");
    q.addBindValue(account);
    execute(q);
    if (!q.next())
      return;
    const auto base = canonicalServer(q.value(0).toString());
    const auto login = q.value(1).toString();
    q.finish();
    auto secret = readCredential(credentialReference(account)).toUtf8();
    QNetworkRequest request(
        base.resolved(QUrl("ocs/v2.php/core/apppassword?format=json")));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    request.setRawHeader("OCS-APIRequest", "true");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization",
                         "Basic " + (login.toUtf8() + ':' + secret).toBase64());
    secret.fill(0);
    // Once sent, the outcome is unknown until answered: forget "only delayed"
    // first, so no later boot can send a second DELETE.
    q.prepare("DELETE FROM held_revocations WHERE account_id=?");
    q.addBindValue(account);
    execute(q);
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
    QObject::connect(
        reply, &QNetworkReply::finished, &networkTasks_,
        [this, manager, reply, body, account] {
          body->append(reply->readAll());
          manager->deleteLater();
          const auto code =
              reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                  .toInt();
          if (code == 429 || code == 423) {
            // Refused, so the password still exists and nothing is uncertain:
            // record it as held again and wait out the new delay.
            try {
              checkMetadataReply(*reply, 200, QDateTime::currentDateTimeUtc());
            } catch (const MetadataError &error) {
              const QString reason = QString::fromUtf8(error.what());
              try {
                if ((reason == "SERVER_THROTTLED" ||
                     reason == "REMOTE_LOCKED") &&
                    error.retryAfterMs >= 0) {
                  metadataFailed(account, error);
                  QSqlQuery again(db_);
                  again.prepare(
                      "INSERT OR IGNORE INTO held_revocations VALUES(?)");
                  again.addBindValue(account);
                  execute(again);
                  QTimer::singleShot(
                      std::min<qint64>(
                          std::max<qint64>(error.retryAfterMs, 60000),
                          2147483647),
                      &networkTasks_,
                      [this, account] { retireHeldPassword(account); });
                  return;
                }
              } catch (...) {
              }
            } catch (...) {
            }
            finishHeldPassword(account, false);
            return;
          }
          finishHeldPassword(account,
                             body->size() <= 1048576 &&
                                 reply->error() == QNetworkReply::NoError &&
                                 code == 200 &&
                                 QJsonDocument::fromJson(*body)
                                         .object()
                                         .value("ocs")
                                         .toObject()
                                         .value("meta")
                                         .toObject()
                                         .value("status") == "ok");
        });
  } catch (...) {
    finishHeldPassword(account, false);
  }
}

void Control::finishHeldPassword(const QString &account, bool revoked) {
  try {
    QSqlQuery q(db_);
    q.prepare(
        "SELECT operation_id FROM accounts WHERE id=? AND state='creating'");
    q.addBindValue(account);
    execute(q);
    if (!q.next())
      return;
    const auto operation = q.value(0).toString();
    q.finish();
    removeCredential(credentialReference(account));
    accountChange([&] {
      q.prepare("DELETE FROM accounts WHERE id=? AND state='creating'");
      q.addBindValue(account);
      execute(q);
      // The session ended with the previous process either way; only an
      // unconfirmed revocation needs the user's attention.
      const QJsonObject receipt{
          {"operationId", operation},
          {"state", "failed"},
          {"error", error(revoked ? "LOGIN_SESSION_EXPIRED"
                                  : "APP_PASSWORD_REVOCATION_UNCONFIRMED")}};
      q.prepare("UPDATE operations SET receipt=? WHERE id=?");
      q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
      q.addBindValue(operation);
      execute(q);
    });
  } catch (...) {
    stopForStorageFailure();
  }
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
