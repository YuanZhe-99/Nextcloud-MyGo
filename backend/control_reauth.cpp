// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "namespace.h"
#include "switch_state.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
/// execute requires successful reauthentication persistence without exposing
/// database details.
void execute(QSqlQuery &q) {
  if (!q.exec())
    throw std::runtime_error("INTERNAL_ERROR");
}
/// uuid creates one independent login or operation identity.
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
/// failure returns a stable machine-readable reason without credential
/// references.
QJsonObject failure(const QString &reason) {
  return {{"code", -32000},
          {"message", "Sign-in could not be completed"},
          {"data", QJsonObject{{"reasonCode", reason}}}};
}
} // namespace

QString Control::reauthReference(const QString &account, const QString &session,
                                 bool previous) const {
  if (QUuid(session).isNull() ||
      QUuid(session).toString(QUuid::WithoutBraces) != session)
    throw std::runtime_error("INTERNAL_ERROR");
  return credentialReference(account) + "/reauthentication/" + session +
         (previous ? "/previous" : "/candidate");
}

bool Control::reauthenticating(const QString &account) const {
  QSqlQuery q(db_);
  q.prepare("SELECT 1 FROM reauthentication WHERE account_id=? AND phase IN "
            "('waiting','prepared','installed','discarding')");
  q.addBindValue(account);
  execute(q);
  return q.next();
}

void Control::resumeReauthentication() {
  QSqlQuery q(db_);
  q.prepare("SELECT account_id,session_id FROM reauthentication WHERE "
            "phase='installed'");
  execute(q);
  QList<QPair<QString, QString>> installed;
  while (q.next())
    installed.append({q.value(0).toString(), q.value(1).toString()});
  q.finish();
  for (const auto &entry : installed) {
    try {
      reloadCredentials(entry.first);
      cleanupReauthentication(entry.first, entry.second);
    } catch (...) {
      stopForStorageFailure();
      return;
    }
  }
}

QJsonObject Control::reauthenticate(const QJsonObject &params,
                                    const QByteArray &digest) {
  if (params.value("expectedRevision").toString() != QString::number(revision_))
    throw std::runtime_error("REVISION_CONFLICT");
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        it.key() != "accountId")
      throw std::runtime_error("INVALID_PARAMS");
  const auto account = params.value("accountId").toString();
  if (throttleRemaining(account) > 0)
    throw std::runtime_error("SERVER_THROTTLED");
  if (endpointChecks_.contains(account))
    throw std::runtime_error("QUIESCE_STALLED");
  QSqlQuery q(db_);
  q.prepare("SELECT a.base_url,a.login_name,a.credential_ref FROM accounts a "
            "WHERE a.id=? AND "
            "a.state='ready'");
  q.addBindValue(account);
  execute(q);
  if (!q.next())
    throw std::runtime_error("NOT_FOUND");
  const auto base = canonicalServer(q.value(0).toString());
  const auto previousLogin = q.value(1).toString();
  if (q.value(2).toString() != credentialReference(account))
    throw std::runtime_error("AUTH_REQUIRED");
  q.finish();
  q.prepare(
      "SELECT 1 FROM reauthentication WHERE account_id=? AND phase IN "
      "('waiting','prepared','installed','discarding') UNION ALL SELECT 1 FROM "
      "endpoints WHERE account_id=? AND state IN ('probing','verifying') "
      "UNION ALL SELECT 1 FROM switch_intents WHERE account_id=? AND "
      "phase!='blocked' LIMIT 1");
  q.addBindValue(account);
  q.addBindValue(account);
  q.addBindValue(account);
  execute(q);
  if (q.next())
    throw std::runtime_error("QUIESCE_STALLED");
  q.finish();
  std::erase_if(logins_, [](const auto &entry) {
    const auto state = entry.second->status();
    return state == "failed" || state == "cancelled" || state == "succeeded";
  });
  const auto session = uuid(), operation = uuid();
  const QJsonObject receipt{{"operationId", operation},
                            {"state", "running"},
                            {"result", QJsonObject{}}};
  accountChange([&] {
    q.prepare("INSERT INTO "
              "reauthentication(account_id,session_id,operation_id,phase,"
              "previous_login) VALUES(?,?,?,'waiting',?) ON "
              "CONFLICT(account_id) DO UPDATE SET "
              "session_id=excluded.session_id,operation_id=excluded.operation_"
              "id,phase='waiting',login_name='',previous_login=excluded."
              "previous_login,reason='',previous_present=1,retired=0");
    q.addBindValue(account);
    q.addBindValue(session);
    q.addBindValue(operation);
    q.addBindValue(previousLogin);
    execute(q);
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,?,?,'accounts.reauthenticate')");
    q.addBindValue(params.value("operationKey").toString());
    q.addBindValue(operation);
    q.addBindValue(digest);
    q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    execute(q);
  });
#ifdef SYNCD_WITH_ENGINE
  runtime_->blockAccount(account, "reauthInProgress", true);
#endif
  auto flow = std::make_shared<LoginFlow>(base, session, account);
  flow->saveCredential = [this, account, session](const QString &user,
                                                  const QString &password) {
    try {
      QSqlQuery q(db_);
      q.prepare("SELECT 1 FROM reauthentication WHERE account_id=? AND "
                "session_id=? AND phase='waiting'");
      q.addBindValue(account);
      q.addBindValue(session);
      execute(q);
      if (!q.next())
        return false;
      q.finish();
      writeCredential(reauthReference(account, session), user, password);
      accountChange([&] {
        q.prepare("UPDATE reauthentication SET login_name=? WHERE account_id=? "
                  "AND session_id=?");
        q.addBindValue(user);
        q.addBindValue(account);
        q.addBindValue(session);
        execute(q);
      });
      return true;
    } catch (...) {
      return false;
    }
  };
  flow->changed = [this](LoginFlow &value) { return reauthChanged(value); };
  flow->throttled = [this, account](qint64 delay) {
    try {
      throttleAccount(account, delay);
      return true;
    } catch (...) {
      return false;
    }
  };
  flow->verified = [this](LoginFlow &value) {
    const auto session = value.id;
    QTimer::singleShot(0, &networkTasks_, [this, session] {
      installReauthentication(session, QDeadlineTimer(120000));
    });
  };
  auto pointer = flow.get();
  logins_.emplace(session, std::move(flow));
  QTimer::singleShot(0, pointer, [pointer] { pointer->start(); });
  return receipt;
}

bool Control::reauthChanged(LoginFlow &flow) {
  try {
    QSqlQuery q(db_);
    q.prepare(
        "SELECT operation_id,phase,previous_login,previous_present,retired "
        "FROM reauthentication WHERE account_id=? AND session_id=?");
    q.addBindValue(flow.accountId);
    q.addBindValue(flow.id);
    execute(q);
    if (!q.next())
      return false;
    const auto operation = q.value(0).toString(), phase = q.value(1).toString(),
               previousLogin = q.value(2).toString();
    const bool previousPresent = q.value(3).toBool(),
               retired = q.value(4).toBool();
    q.finish();
    const bool failed =
        flow.status() == "failed" || flow.status() == "cancelled";
    if (failed && phase == "installed") {
      stopForStorageFailure();
      return false;
    }
    if (failed) {
      if (phase == "prepared") {
        if (previousPresent)
          writeCredential(
              credentialReference(flow.accountId), previousLogin,
              readCredential(reauthReference(flow.accountId, flow.id, true)));
        else
          removeCredential(credentialReference(flow.accountId));
      }
      accountChange([&] {
        q.prepare("UPDATE reauthentication SET phase='discarding',reason=? "
                  "WHERE account_id=? AND session_id=?");
        q.addBindValue(flow.reason());
        q.addBindValue(flow.accountId);
        q.addBindValue(flow.id);
        execute(q);
      });
      removeCredential(reauthReference(flow.accountId, flow.id));
      removeCredential(reauthReference(flow.accountId, flow.id, true));
    }
    QJsonObject receipt{
        {"operationId", operation},
        {"state", failed ? "failed" : "succeeded"},
        {"result",
         QJsonObject{{"id", flow.id},
                     {"expires", flow.expires.toString(Qt::ISODateWithMs)}}}};
    if (failed) {
      receipt.remove("result");
      receipt.insert("error", failure(flow.reason()));
    }
    accountChange([&] {
      if (failed) {
        q.prepare("UPDATE reauthentication SET phase='failed',reason=? WHERE "
                  "account_id=? AND session_id=?");
        q.addBindValue(flow.reason());
        q.addBindValue(flow.accountId);
        q.addBindValue(flow.id);
        execute(q);
      }
      q.prepare("UPDATE operations SET receipt=? WHERE id=?");
      q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
      q.addBindValue(operation);
      execute(q);
      if (failed && cancellations_.contains(flow.id)) {
        const auto cancel = cancellations_.at(flow.id);
        QJsonObject done{{"operationId", cancel},
                         {"state", "succeeded"},
                         {"result", QJsonObject{}}};
        if (flow.reason() == "APP_PASSWORD_REVOCATION_UNCONFIRMED") {
          done.insert("state", "failed");
          done.insert("error", failure(flow.reason()));
        }
        q.prepare("UPDATE operations SET receipt=? WHERE id=?");
        q.addBindValue(QJsonDocument(done).toJson(QJsonDocument::Compact));
        q.addBindValue(cancel);
        execute(q);
      }
    });
    if (failed) {
      cancellations_.erase(flow.id);
#ifdef SYNCD_WITH_ENGINE
      runtime_->blockAccount(flow.accountId, "reauthInProgress", false);
      if (retired)
        reloadCredentials(flow.accountId);
      else if (!SwitchState(db_).gated(flow.accountId))
        runtime_->resumeAccount(flow.accountId);
#endif
    }
    return true;
  } catch (...) {
    stopForStorageFailure();
    return false;
  }
}

void Control::installReauthentication(const QString &session,
                                      QDeadlineTimer deadline) {
  auto found = logins_.find(session);
  if (found == logins_.end() || found->second->status() != "verifying")
    return;
  // readCollections pumps Qt events: cancellation may replace and erase this
  // session from logins_.
  const auto lifetime = found->second;
  auto &flow = *lifetime;
  try {
    if (throttleRemaining(flow.accountId) > 0)
      throw std::runtime_error("SERVER_THROTTLED");
    QSqlQuery q(db_);
    q.prepare("SELECT a.uid,a.root_id,a.base_url,r.previous_login FROM "
              "accounts a JOIN reauthentication r ON r.account_id=a.id WHERE "
              "r.session_id=? AND r.phase='waiting' AND a.state='ready'");
    q.addBindValue(session);
    execute(q);
    if (!q.next())
      throw std::runtime_error("REVISION_CONFLICT");
    const auto uid = q.value(0).toString(), root = q.value(1).toString(),
               previousLogin = q.value(3).toString();
    const auto base = canonicalServer(q.value(2).toString());
    q.finish();
    if (uid != flow.uid || root != flow.rootId)
      throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
    if (storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
      throw std::runtime_error("CANCELLED");
#ifdef SYNCD_WITH_ENGINE
    if (!runtime_->quiesceAccount(flow.accountId, true)) {
      if (deadline.hasExpired())
        throw std::runtime_error("QUIESCE_STALLED");
      QTimer::singleShot(25, &networkTasks_, [this, session, deadline] {
        installReauthentication(session, deadline);
      });
      return;
    }
#endif
    q.prepare("SELECT remote_path,remote_identity FROM folders WHERE "
              "account_id=? ORDER BY id");
    q.addBindValue(flow.accountId);
    execute(q);
    QJsonArray roots;
    while (q.next())
      roots.append(QJsonArray{q.value(0).toString(), q.value(1).toString()});
    q.finish();
    for (const auto &entry : roots) {
      if (throttleRemaining(flow.accountId) > 0)
        throw std::runtime_error("SERVER_THROTTLED");
      const auto tuple = entry.toArray();
      const auto remote = readCollections(
          base, uid, flow.loginName, reauthReference(flow.accountId, session),
          tuple[0].toString(), false);
      if (flow.status() != "verifying")
        return;
      if (remote.front().identity != tuple[1].toString() ||
          remote.front().encrypted)
        throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
      if (storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
        throw std::runtime_error("CANCELLED");
    }
    if (throttleRemaining(flow.accountId) > 0)
      throw std::runtime_error("SERVER_THROTTLED");
#ifdef SYNCD_WITH_ENGINE
    accountChange([&] {
      q.prepare("UPDATE reauthentication SET retired=1 WHERE account_id=? AND "
                "session_id=? AND phase='waiting'");
      q.addBindValue(flow.accountId);
      q.addBindValue(session);
      execute(q);
    });
    const auto reason = runtime_->retireAccount(flow.accountId);
    if (reason == "QUIESCE_STALLED" && !deadline.hasExpired()) {
      QTimer::singleShot(25, &networkTasks_, [this, session, deadline] {
        installReauthentication(session, deadline);
      });
      return;
    }
    if (!reason.isEmpty())
      throw std::runtime_error(reason.toStdString());
#endif
    // Keep a durable rollback source before touching the stable slot. An
    // already-revoked old password is still backed up.
    const auto previous = findCredential(credentialReference(flow.accountId));
    if (previous)
      writeCredential(reauthReference(flow.accountId, session, true),
                      previousLogin, *previous);
    accountChange([&] {
      q.prepare(
          "UPDATE reauthentication SET phase='prepared',previous_present=? "
          "WHERE account_id=? AND session_id=? AND phase='waiting'");
      q.addBindValue(previous.has_value());
      q.addBindValue(flow.accountId);
      q.addBindValue(session);
      execute(q);
      if (q.numRowsAffected() != 1)
        throw std::runtime_error("REVISION_CONFLICT");
    });
    writeCredential(credentialReference(flow.accountId), flow.loginName,
                    readCredential(reauthReference(flow.accountId, session)));
    accountChange([&] {
      q.prepare(
          "UPDATE accounts SET login_name=? WHERE id=? AND state='ready'");
      q.addBindValue(flow.loginName);
      q.addBindValue(flow.accountId);
      execute(q);
      q.prepare("UPDATE reauthentication SET phase='installed' WHERE "
                "account_id=? AND session_id=? AND phase='prepared'");
      q.addBindValue(flow.accountId);
      q.addBindValue(session);
      execute(q);
      if (q.numRowsAffected() != 1)
        throw std::runtime_error("REVISION_CONFLICT");
    });
    flow.accept(true); // Ownership is now durable; a later error must never
                       // revoke the installed password.
    if (flow.status() != "succeeded")
      return;
    reloadCredentials(flow.accountId);
    cleanupReauthentication(flow.accountId, session);
  } catch (const std::exception &error) {
    try {
      metadataFailed(flow.accountId, error);
    } catch (...) {
      // Failed throttle persistence must not discard/revoke the candidate or
      // manufacture a successful busy receipt.
      stopForStorageFailure();
      return;
    }
    if (storageFailed_)
      return;
    if (flow.status() == "succeeded") {
      stopForStorageFailure();
      return;
    }
    if (flow.status() != "verifying")
      return; // Cancellation/replacement owns cleanup after a nested DAV wait.
    const QString reason = QString::fromUtf8(error.what());
    if ((reason == "SERVER_THROTTLED" || reason == "REMOTE_LOCKED") &&
        flow.status() == "verifying") {
      const auto *metadata = dynamic_cast<const MetadataError *>(&error);
      const auto delay =
          reason == "SERVER_THROTTLED" ? throttleRemaining(flow.accountId)
          : metadata && metadata->retryAfterMs != 0 ? metadata->retryAfterMs
                                                    : 60000;
      try {
        accountChange([&] {
          QSqlQuery q(db_);
          q.prepare("UPDATE reauthentication SET reason=? WHERE account_id=? "
                    "AND session_id=? AND phase='waiting'");
          q.addBindValue(reason);
          q.addBindValue(flow.accountId);
          q.addBindValue(session);
          execute(q);
        });
        if (delay >= 0 && delay != std::numeric_limits<qint64>::max())
          QTimer::singleShot(
              std::min<qint64>(std::max<qint64>(delay, 1), 2147483647),
              &networkTasks_, [this, session] {
                installReauthentication(session, QDeadlineTimer(120000));
              });
      } catch (...) {
        stopForStorageFailure();
      }
      return; // Keep the candidate and its durable cleanup ownership intact.
    }
    const QStringList known{"REVISION_CONFLICT",
                            "AUTH_REQUIRED",
                            "CREDENTIALS_LOCKED",
                            "ENDPOINT_IDENTITY_MISMATCH",
                            "TLS_ERROR",
                            "ENDPOINT_UNTRUSTED",
                            "REMOTE_ROOT_UNAVAILABLE",
                            "NETWORK_UNAVAILABLE",
                            "PERMISSION_DENIED",
                            "REMOTE_LOCKED",
                            "SERVER_THROTTLED",
                            "INVALID_SERVER_RESPONSE",
                            "QUIESCE_STALLED",
                            "JOURNAL_ERROR",
                            "FOLDER_ROOT_CHANGED",
                            "CANCELLED"};
    flow.reject(known.contains(reason) ? reason : "INTERNAL_ERROR");
  }
}

void Control::reloadCredentials(const QString &account) {
#ifdef SYNCD_WITH_ENGINE
  const auto remaining = throttleRemaining(account);
  if (remaining > 0) {
    if (remaining != std::numeric_limits<qint64>::max())
      QTimer::singleShot(std::min<qint64>(remaining, 2147483647),
                         &networkTasks_, [this, account] {
                           try {
                             reloadCredentials(account);
                           } catch (...) {
                             stopForStorageFailure();
                           }
                         });
    return;
  }
  try {
    QSqlQuery q(db_);
    q.prepare("SELECT phase FROM switch_intents WHERE account_id=?");
    q.addBindValue(account);
    execute(q);
    const auto phase = q.next() ? q.value(0).toString() : QString{};
    q.finish();
    if (phase != "prepared" && phase != "reconciling") {
      const auto binding = SwitchState(db_).binding(account);
      const QJsonObject params{
          {"accountId", account},
          {"endpointId", binding.endpoint},
          {"operationKey", uuid()},
          {"expectedRevision", QString::number(revision_)}};
      switchMutation(params,
                     QCryptographicHash::hash(
                         QJsonDocument(params).toJson(QJsonDocument::Compact),
                         QCryptographicHash::Sha256),
                     true, "credentialReload");
    }
    runtime_->credentialsRenewed(account);
    runtime_->blockAccount(account, "reauthInProgress", false);
  } catch (const std::exception &error) {
    const QString reason = QString::fromUtf8(error.what());
    const QStringList unavailable{"AUTH_REQUIRED", "CREDENTIALS_LOCKED",
                                  "LOCAL_ROOT_UNAVAILABLE",
                                  "FOLDER_ROOT_CHANGED", "JOURNAL_ERROR"};
    if (!unavailable.contains(reason))
      throw;
    // Credential installation is committed. An unavailable root must not undo
    // it or create a startup loop.
    accountChange([&] {
      QSqlQuery q(db_);
      q.prepare(
          "UPDATE folders SET status='error',reason=? WHERE account_id=?");
      q.addBindValue(reason);
      q.addBindValue(account);
      execute(q);
    });
  }
#else
  Q_UNUSED(account);
#endif
}

void Control::cleanupReauthentication(const QString &account,
                                      const QString &session) {
  try {
    if (storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
      return;
    const auto remaining = throttleRemaining(account);
    if (remaining > 0) {
      if (remaining != std::numeric_limits<qint64>::max())
        QTimer::singleShot(std::min<qint64>(remaining, 2147483647),
                           &networkTasks_, [this, account, session] {
                             cleanupReauthentication(account, session);
                           });
      return; // Installed and previous slots stay durably reserved.
    }
    QSqlQuery q(db_);
    q.prepare("SELECT a.base_url,r.previous_login FROM accounts a JOIN "
              "reauthentication r ON r.account_id=a.id WHERE a.id=? AND "
              "r.session_id=? AND r.phase='installed'");
    q.addBindValue(account);
    q.addBindValue(session);
    execute(q);
    if (!q.next())
      return;
    const auto base = canonicalServer(q.value(0).toString());
    const auto login = q.value(1).toString();
    q.finish();
    const auto previous =
        findCredential(reauthReference(account, session, true));
    if (!previous) {
      finishReauthentication(account, session, false);
      return;
    }
    // A server may return the existing credential: never revoke the installed
    // slot itself.
    if (*previous == readCredential(credentialReference(account))) {
      finishReauthentication(account, session, true);
      return;
    }
    QNetworkRequest request(
        base.resolved(QUrl("ocs/v2.php/core/apppassword?format=json")));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("OCS-APIRequest", "true");
    request.setRawHeader(
        "Authorization",
        "Basic " + (login.toUtf8() + ':' + previous->toUtf8()).toBase64());
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
        [this, manager, reply, body, account, session] {
          body->append(reply->readAll());
          const auto code =
              reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                  .toInt();
          if (code == 429 || code == 423) {
            try {
              checkMetadataReply(*reply, 200, QDateTime::currentDateTimeUtc());
            } catch (const MetadataError &error) {
              manager->deleteLater();
              if (QString::fromUtf8(error.what()) != "SERVER_THROTTLED" &&
                  QString::fromUtf8(error.what()) != "REMOTE_LOCKED") {
                finishReauthentication(account, session, false);
                return; // TLS/identity failures never become throttle retries.
              }
              try {
                metadataFailed(account, error);
                const auto delay = code == 429 ? throttleRemaining(account)
                                               : error.retryAfterMs;
                if (delay >= 0 && delay != std::numeric_limits<qint64>::max())
                  QTimer::singleShot(
                      std::min<qint64>(std::max<qint64>(delay, 60000),
                                       2147483647),
                      &networkTasks_, [this, account, session] {
                        cleanupReauthentication(account, session);
                      });
              } catch (...) {
                stopForStorageFailure();
              }
              return; // A transient reply never releases credential ownership.
            }
          }
          const bool revoked = body->size() <= 1048576 && code == 200 &&
                               reply->error() == QNetworkReply::NoError &&
                               QJsonDocument::fromJson(*body)
                                       .object()
                                       .value("ocs")
                                       .toObject()
                                       .value("meta")
                                       .toObject()
                                       .value("status") == "ok";
          manager->deleteLater();
          finishReauthentication(account, session, revoked);
        });
  } catch (...) {
    if (!storageFailed_)
      finishReauthentication(account, session, false);
  }
}

void Control::finishReauthentication(const QString &account,
                                     const QString &session, bool revoked) {
  try {
    QSqlQuery current(db_);
    current.prepare("SELECT 1 FROM reauthentication WHERE account_id=? AND "
                    "session_id=? AND phase='installed'");
    current.addBindValue(account);
    current.addBindValue(session);
    execute(current);
    if (!current.next())
      return;
    current.finish();
    removeCredential(reauthReference(account, session));
    removeCredential(reauthReference(account, session, true));
    accountChange([&] {
      QSqlQuery q(db_);
      q.prepare("UPDATE reauthentication SET phase='succeeded',reason=? WHERE "
                "account_id=? AND session_id=? AND phase='installed'");
      q.addBindValue(
          revoked ? QStringLiteral("")
                  : QStringLiteral("APP_PASSWORD_REVOCATION_UNCONFIRMED"));
      q.addBindValue(account);
      q.addBindValue(session);
      execute(q);
    });
  } catch (...) {
    stopForStorageFailure();
  }
}

void Control::recoverReauthentication() {
  QSqlQuery q(db_);
  q.prepare("SELECT "
            "account_id,session_id,phase,previous_login,operation_id,previous_"
            "present,reason FROM reauthentication WHERE phase IN "
            "('waiting','prepared','discarding')");
  execute(q);
  QList<QStringList> interrupted;
  while (q.next())
    interrupted.append({q.value(0).toString(), q.value(1).toString(),
                        q.value(2).toString(), q.value(3).toString(),
                        q.value(4).toString(), q.value(5).toString(),
                        q.value(6).toString()});
  q.finish();
  for (const auto &row : interrupted) {
    if (row[2] == "prepared") {
      if (row[5].toInt())
        writeCredential(credentialReference(row[0]), row[3],
                        readCredential(reauthReference(row[0], row[1], true)));
      else
        removeCredential(credentialReference(row[0]));
    }
    const bool candidate =
        findCredential(reauthReference(row[0], row[1])).has_value();
    const QString reason = row[2] == "discarding" && !row[6].isEmpty() ? row[6]
                           : candidate ? "APP_PASSWORD_REVOCATION_UNCONFIRMED"
                                       : "LOGIN_SESSION_EXPIRED";
    accountChange([&] {
      q.prepare("UPDATE reauthentication SET phase='discarding',reason=? WHERE "
                "account_id=? AND session_id=?");
      q.addBindValue(reason);
      q.addBindValue(row[0]);
      q.addBindValue(row[1]);
      execute(q);
    });
    removeCredential(reauthReference(row[0], row[1]));
    removeCredential(reauthReference(row[0], row[1], true));
    accountChange([&] {
      q.prepare("UPDATE reauthentication SET phase='failed',reason=? WHERE "
                "account_id=? AND session_id=?");
      q.addBindValue(reason);
      q.addBindValue(row[0]);
      q.addBindValue(row[1]);
      execute(q);
      QJsonObject receipt{{"operationId", row[4]},
                          {"state", "failed"},
                          {"error", failure(reason)}};
      q.prepare("UPDATE operations SET receipt=? WHERE id=?");
      q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
      q.addBindValue(row[4]);
      execute(q);
    });
  }
}
