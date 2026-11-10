// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "namespace.h"
#include "root.h"
#include "switch_state.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>

namespace {
/// execute requires SQL success without returning database details to IPC.
void execute(QSqlQuery &q) {
  if (!q.exec())
    throw std::runtime_error("INTERNAL_ERROR");
}
/// overlaps compares canonical collection/local paths including ancestor
/// relationships.
bool overlaps(const QString &a, const QString &b,
              Qt::CaseSensitivity sensitivity) {
  return a.compare(b, sensitivity) == 0 || a == "/" || b == "/" ||
         a.startsWith(b + '/', sensitivity) ||
         b.startsWith(a + '/', sensitivity);
}
/// failure builds a sanitized terminal operation error.
QJsonObject failure(const QString &reason) {
  return {{"code", -32000},
          {"message", "Request could not be completed"},
          {"data", QJsonObject{{"reasonCode", reason}}}};
}
#ifdef SYNCD_WITH_ENGINE
/// attachmentReason preserves recognized root/runtime blockers without exposing
/// exception details. Returns: empty for unexpected or Control-storage
/// failures, which must stop admission instead of masquerading as a root
/// problem (INV-10).
QString attachmentReason(const std::exception &error) {
  const auto reason = QString::fromUtf8(error.what());
  const QStringList known{"AUTH_REQUIRED",
                          "CREDENTIALS_LOCKED",
                          "JOURNAL_ERROR",
                          "UNSUPPORTED_FOLDER_MODE",
                          "LOCAL_ROOT_UNAVAILABLE",
                          "FOLDER_ROOT_CHANGED",
                          "FOLDER_OWNED",
                          "ENDPOINT_UNTRUSTED",
                          "ENDPOINT_IDENTITY_MISMATCH",
                          "TLS_ERROR",
                          "RESUME_CONTEXT_UNSUPPORTED"};
  return known.contains(reason) ? reason : QString{};
}
#endif
} // namespace

QJsonArray Control::folderProjection() const {
  QJsonArray result;
  QSqlQuery q(db_);
  q.prepare("SELECT f.id,f.account_id,f.local_path,f.remote_path,f.paused,CASE "
            "WHEN r.phase IN ('waiting','prepared','discarding') THEN "
            "'waitingSignIn' ELSE f.status END,f.reason FROM "
            "folders f LEFT JOIN reauthentication r ON "
            "r.account_id=f.account_id ORDER BY f.id");
  execute(q);
  while (q.next()) {
    const auto id = q.value(0).toString();
    auto status = q.value(5).toString();
    if (status != "syncing" && (paused_ || q.value(4).toBool()))
      status = "paused";
    result.append(
        QJsonObject{{"id", id},
                    {"accountId", q.value(1).toString()},
                    {"name", QFileInfo(q.value(2).toString()).fileName()},
                    {"localPath", q.value(2).toString()},
                    {"remotePath", q.value(3).toString()},
                    {"userPaused", q.value(4).toBool()},
                    {"state", status},
                    {"reasonCode", q.value(6).toString()},
                    {"phase", "checking"}});
  }
  return result;
}

QJsonValue Control::folderRead(const QString &method,
                               const QJsonObject &params) try {
  if (method == "folders.list" && !params.value("remote").toBool())
    return folderProjection();
#ifndef SYNCD_WITH_ENGINE
  throw std::runtime_error("UNSUPPORTED_CAPABILITY");
#else
  const auto account = params.value("accountId").toString();
  if (throttleRemaining(account) > 0)
    throw std::runtime_error("SERVER_THROTTLED");
  if (reauthenticating(account))
    throw std::runtime_error("QUIESCE_STALLED");
  if (SwitchState(db_).gated(account))
    throw std::runtime_error("QUIESCE_STALLED");
  QSqlQuery q(db_);
  q.prepare("SELECT base_url,uid,login_name,credential_ref FROM accounts WHERE "
            "id=? AND state='ready'");
  q.addBindValue(account);
  execute(q);
  if (!q.next())
    throw std::runtime_error("NOT_FOUND");
  const auto base = SwitchState(db_).binding(account).server;
  const auto uid = q.value(1).toString(), login = q.value(2).toString(),
             credential = q.value(3).toString();
  q.finish();
  if (credential != credentialReference(account))
    throw std::runtime_error("AUTH_REQUIRED");
  if (method == "folders.list") {
    const auto list = readCollections(base, uid, login, credential, "/", true);
    QJsonArray result;
    for (const auto &entry : list)
      if (entry.path != "/")
        result.append(
            QJsonObject{{"path", entry.path}, {"encrypted", entry.encrypted}});
    return result;
  }
  if (!params.value("localPath").isString() ||
      !params.value("remotePath").isString())
    throw std::runtime_error("INVALID_PARAMS");
  const QFileInfo local(params.value("localPath").toString());
  if (!local.isAbsolute())
    throw std::runtime_error("INVALID_PARAMS");
  const auto path =
      QDir::cleanPath(QDir::fromNativeSeparators(local.absoluteFilePath()));
  QString identity;
  if (!local.exists())
    throw std::runtime_error("LOCAL_ROOT_UNAVAILABLE");
  {
    RootLease lease(path);
    identity = lease.identity();
  }
  const auto remote = remotePath(params.value("remotePath").toString());
  q.prepare("SELECT account_id,local_path,remote_path FROM folders");
  execute(q);
  while (q.next())
    if (overlaps(path, q.value(1).toString(), Qt::CaseInsensitive) ||
        (account == q.value(0).toString() &&
         overlaps(remote, q.value(2).toString(), Qt::CaseSensitive)))
      throw std::runtime_error("FOLDER_OWNED");
  q.finish();
  const auto roots =
      readCollections(base, uid, login, credential, remote, false);
  if (roots.front().encrypted)
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  std::erase_if(inspections_, [](const auto &item) {
    return item.second.expiry.hasExpired();
  });
  if (inspections_.size() >= 32)
    throw std::runtime_error("RATE_LIMITED");
  const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
  inspections_.emplace(token, Inspection{account, path, remote, identity,
                                         roots.front().identity});
  return QJsonObject{{"inspectToken", token},
                     {"localExists", local.exists()},
                     {"mergeExisting", local.isDir() && !QDir(path).isEmpty()},
                     {"expiresInSeconds", 300}};
#endif
} catch (const std::exception &error) {
  metadataFailed(params.value("accountId").toString(), error);
  throw;
}

void Control::operationFinished(const QString &id, const QString &reason) {
  accountChange([&] {
    QJsonObject receipt{{"operationId", id},
                        {"state", reason.isEmpty() ? "succeeded" : "failed"},
                        {"result", QJsonObject{}}};
    if (!reason.isEmpty())
      receipt.insert("error", failure(reason));
    QSqlQuery q(db_);
    q.prepare("UPDATE operations SET receipt=? WHERE id=?");
    q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    q.addBindValue(id);
    execute(q);
  });
}

void Control::attachFolder(const QString &id) {
#ifdef SYNCD_WITH_ENGINE
  QSqlQuery q(db_);
  q.prepare("SELECT "
            "f.account_id,a.base_url,a.login_name,a.uid,a.credential_ref,f."
            "local_path,f.remote_path,f.journal_path,f.local_identity,f.remote_"
            "identity,f.paused FROM folders f JOIN accounts a ON "
            "a.id=f.account_id WHERE f.id=? AND a.state='ready'");
  q.addBindValue(id);
  execute(q);
  if (!q.next())
    throw std::runtime_error("NOT_FOUND");
  const auto account = q.value(0).toString();
  EngineAdapter::Binding binding{canonicalServer(q.value(1).toString()),
                                 q.value(2).toString(),
                                 q.value(3).toString(),
                                 q.value(4).toString(),
                                 q.value(5).toString(),
                                 q.value(6).toString(),
                                 q.value(7).toString(),
                                 QCoreApplication::applicationDirPath() +
                                     "/sync-exclude.lst",
                                 q.value(8).toString(),
                                 q.value(9).toString()};
  const bool paused = q.value(10).toBool();
  q.finish();
  if (binding.credentialReference != credentialReference(account) ||
      binding.journalPath != directory_ + "/journals/" + id + ".sqlite")
    throw std::runtime_error("INVALID_PARAMS");
  const auto committed = SwitchState(db_).binding(account);
  binding.server = committed.server;
  binding.epoch = quint64(committed.epoch);
  if (!QFileInfo(binding.localPath).isDir())
    throw std::runtime_error("LOCAL_ROOT_UNAVAILABLE");
  runtime_->add(id, account, binding);
  runtime_->block(id, "userPaused", paused);
  runtime_->block(id, "switchInProgress", SwitchState(db_).gated(account));
  runtime_->block(id, "reauthInProgress", reauthenticating(account));
#else
  Q_UNUSED(id);
#endif
}

void Control::restoreFolders() {
  QSqlQuery cleanup(db_);
  cleanup.prepare("DELETE FROM decisions WHERE boot!=?");
  cleanup.addBindValue(boot_);
  execute(cleanup);
  cleanup.prepare("SELECT id,receipt FROM operations WHERE method IN "
                  "('daemon.shutdown','diagnostics.export')");
  execute(cleanup);
  QStringList interrupted;
  while (cleanup.next())
    if (QJsonDocument::fromJson(cleanup.value(1).toByteArray())
            .object()
            .value("state") == "running")
      interrupted.append(cleanup.value(0).toString());
  cleanup.finish();
  for (const auto &id : interrupted)
    operationFinished(id, "OPERATION_INTERRUPTED");
#ifdef SYNCD_WITH_ENGINE
  runtime_ = std::make_unique<EngineRuntime>();
  for (const auto &[account, until] : accountThrottleUntil_) {
    const auto remaining = throttleRemaining(account);
    if (remaining > 0)
      runtime_->throttleAccount(account, until < 0 ? -1 : remaining);
  }
  runtime_->pause(paused_);
  runtime_->stopped = [this] {
    if (storageFailed_) {
      shutdown_ = true;
      return;
    }
    if (!shutdownOperation_.isEmpty()) {
      operationFinished(shutdownOperation_);
      shutdownReady_ = true;
      // Let the connected caller observe terminal completion; an absent caller
      // cannot prevent exit.
      QTimer::singleShot(5000, &networkTasks_, [this] { shutdown_ = true; });
    }
  };
  runtime_->started = [this](const QString &id) {
    accountChange([&] {
      QSqlQuery q(db_);
      q.prepare("UPDATE folders SET status='syncing',reason='' WHERE id=?");
      q.addBindValue(id);
      execute(q);
    });
  };
  runtime_->cycle = [this](const QString &id,
                           const EngineAdapter::Result &result) {
    cycleFinished(id, result);
  };
  runtime_->serverThrottled = [this](const QString &account, qint64 delayMs) {
    throttleAccount(account, delayMs);
  };
  QSqlQuery q(db_);
  q.prepare("UPDATE folders SET status='offline',reason='' WHERE "
            "status='syncing' OR status='upToDate'");
  execute(q);
  QTimer::singleShot(0, &networkTasks_, [this] {
    try {
      QSqlQuery q(db_);
      q.prepare("SELECT id FROM folders");
      execute(q);
      QStringList ids;
      while (q.next())
        ids.append(q.value(0).toString());
      q.finish();
      for (const auto &id : ids) {
        if (runtime_->contains(id))
          continue;
        try {
          attachFolder(id);
        } catch (const std::exception &error) {
          const auto reason = attachmentReason(error);
          if (reason.isEmpty())
            throw;
          accountChange([&] {
            q.prepare("UPDATE folders SET status='error',reason=? WHERE id=?");
            q.addBindValue(reason);
            q.addBindValue(id);
            execute(q);
          });
        }
      }
      recoverSwitches();
      resumeReauthentication();
    } catch (...) {
      qCritical("SyncD stopped because folder restoration could not complete");
      stopForStorageFailure();
    }
  });
#endif
}

#ifdef SYNCD_WITH_ENGINE
void Control::cycleFinished(const QString &id,
                            const EngineAdapter::Result &result) {
  try {
    QSqlQuery binding(db_);
    binding.prepare(
        "SELECT COALESCE(b.epoch,0),COALESCE(s.phase,''),f.account_id FROM "
        "folders f LEFT "
        "JOIN endpoint_bindings b ON b.account_id=f.account_id LEFT JOIN "
        "switch_intents s ON s.account_id=f.account_id WHERE f.id=?");
    binding.addBindValue(id);
    execute(binding);
    if (!binding.next()) {
      if (binding.lastError().isValid())
        throw std::runtime_error("INTERNAL_ERROR");
      return;
    }
    if (binding.value(0).toULongLong() != result.epoch ||
        binding.value(1).toString() == "blocked")
      return;
    const auto account = binding.value(2).toString();
    QSqlQuery renewal(db_);
    renewal.prepare("SELECT phase FROM reauthentication WHERE account_id=?");
    renewal.addBindValue(account);
    execute(renewal);
    if (renewal.next() &&
        (renewal.value(0) == "waiting" || renewal.value(0) == "prepared" ||
         renewal.value(0) == "discarding"))
      return;
    renewal.finish();
    binding.finish();
    accountChange([&] {
      QSqlQuery q(db_);
      q.prepare("UPDATE folders SET status=?,reason=?,last_sync=CASE WHEN ? "
                "THEN ? ELSE last_sync END WHERE id=?");
      q.addBindValue(result.success ? "upToDate"
                     : result.reason == "DELETION_CONFIRMATION_REQUIRED"
                         ? "needsReview"
                         : "error");
      // A default QString binds as SQL NULL; successful cycles have no
      // reason.
      q.addBindValue(result.reason.isNull() ? QStringLiteral("")
                                            : result.reason);
      q.addBindValue(result.success);
      q.addBindValue(
          QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
      q.addBindValue(id);
      execute(q);
      recordActivity(account, id, "uploaded", result.uploaded);
      recordActivity(account, id, "downloaded", result.downloaded);
      recordActivity(account, id, "deletedRemote", result.deletedRemote);
      if (!result.proposalDigest.isEmpty()) {
        q.prepare("INSERT OR REPLACE INTO "
                  "decisions(id,folder_id,digest,boot,expires,local_count,"
                  "remote_count,baseline) VALUES(?,?,?,?,?,?,?,?)");
        q.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
        q.addBindValue(id);
        q.addBindValue(result.proposalDigest);
        q.addBindValue(boot_);
        q.addBindValue(QDateTime::currentDateTimeUtc().addSecs(600).toString(
            Qt::ISODateWithMs));
        q.addBindValue(qint64(result.localDeletes));
        q.addBindValue(qint64(result.remoteDeletes));
        q.addBindValue(qint64(result.baseline));
        execute(q);
      }
      if (result.success) {
        q.prepare("SELECT operation_id FROM switch_intents WHERE account_id=? "
                  "AND target_epoch=? AND phase='reconciling'");
        q.addBindValue(account);
        q.addBindValue(qint64(result.epoch));
        execute(q);
        const auto operation = q.next() ? q.value(0).toString() : QString{};
        q.finish();
        SwitchState store(db_);
        if (!operation.isEmpty() &&
            store.reconciled(account, qint64(result.epoch), id) &&
            !store.gated(account)) {
          completePolicy(operation);
          const QJsonObject receipt{{"operationId", operation},
                                    {"state", "succeeded"},
                                    {"result", QJsonObject{}}};
          q.prepare("UPDATE operations SET receipt=? WHERE id=?");
          q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
          q.addBindValue(operation);
          execute(q);
        }
      }
    });
    endpointResult(account, result.success, result.reason, result.epoch,
                   result.networkGeneration);
  } catch (...) {
    // Completion is delivered only after the engine and journal writer are
    // gone. A failed Control transaction must not masquerade as a live stalled
    // sync.
    qCritical("SyncD stopped because cycle completion could not be persisted");
    stopForStorageFailure();
  }
}
#endif

QJsonArray Control::decisionProjection() const {
  QJsonArray result;
  QSqlQuery q(db_);
  q.prepare("SELECT "
            "d.id,f.account_id,d.folder_id,d.digest,d.local_count,d.remote_"
            "count,d.expires FROM decisions d JOIN folders f ON "
            "f.id=d.folder_id ORDER BY d.id");
  execute(q);
  while (q.next())
    result.append(QJsonObject{
        {"id", q.value(0).toString()},
        {"kind", "bulkDeletions"},
        {"accountId", q.value(1).toString()},
        {"folderId", q.value(2).toString()},
        {"digest", q.value(3).toString()},
        {"count", q.value(4).toLongLong() + q.value(5).toLongLong()},
        {"localDeletes", q.value(4).toLongLong()},
        {"remoteDeletes", q.value(5).toLongLong()},
        {"expires", q.value(6).toString()}});
  return result;
}

QJsonObject Control::decisionMutation(const QJsonObject &params,
                                      const QByteArray &digest) {
#ifndef SYNCD_WITH_ENGINE
  Q_UNUSED(params);
  Q_UNUSED(digest);
  throw std::runtime_error("UNSUPPORTED_CAPABILITY");
#else
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        it.key() != "decisionId" && it.key() != "proposalDigest" &&
        it.key() != "action")
      throw std::runtime_error("INVALID_PARAMS");
  const auto id = params.value("decisionId").toString(),
             action = params.value("action").toString();
  if (action != "approveDeletion" && action != "later")
    throw std::runtime_error("UNSUPPORTED_CAPABILITY");
  QSqlQuery q(db_);
  q.prepare("SELECT folder_id,digest,boot,expires FROM decisions WHERE id=?");
  q.addBindValue(id);
  execute(q);
  if (!q.next())
    throw std::runtime_error("NOT_FOUND");
  const auto folder = q.value(0).toString(), proposal = q.value(1).toString();
  const auto remaining = QDateTime::currentDateTimeUtc().msecsTo(
      QDateTime::fromString(q.value(3).toString(), Qt::ISODateWithMs));
  if (q.value(2).toString() != boot_ || remaining <= 0 ||
      params.value("proposalDigest").toString() != proposal)
    throw std::runtime_error("REVISION_CONFLICT");
  q.finish();
  if (!runtime_->contains(folder))
    throw std::runtime_error("LOCAL_ROOT_UNAVAILABLE");
  const auto operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QJsonObject receipt{{"operationId", operation},
                            {"state", "succeeded"},
                            {"result", QJsonObject{}}};
  accountChange([&] {
    if (action == "approveDeletion") {
      q.prepare("DELETE FROM decisions WHERE id=?");
      q.addBindValue(id);
      execute(q);
    }
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,?,?,'decisions.resolve')");
    q.addBindValue(params.value("operationKey").toString());
    q.addBindValue(operation);
    q.addBindValue(digest);
    q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    execute(q);
  });
  if (action == "approveDeletion")
    runtime_->authorize(folder, proposal, remaining);
  return receipt;
#endif
}

QJsonObject Control::folderMutation(const QString &method,
                                    const QJsonObject &params,
                                    const QByteArray &digest) {
#ifndef SYNCD_WITH_ENGINE
  Q_UNUSED(method);
  Q_UNUSED(params);
  Q_UNUSED(digest);
  throw std::runtime_error("UNSUPPORTED_CAPABILITY");
#else
  if (params.value("expectedRevision").toString() != QString::number(revision_))
    throw std::runtime_error("REVISION_CONFLICT");
  const auto field = method == "folders.add" ? "inspectToken" : "folderId";
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        it.key() != field)
      throw std::runtime_error("INVALID_PARAMS");
  const auto operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QString id = params.value("folderId").toString();
  Inspection inspection;
  if (method == "folders.add") {
    const auto it = inspections_.find(params.value("inspectToken").toString());
    if (it == inspections_.end() || it->second.expiry.hasExpired())
      throw std::runtime_error("REVISION_CONFLICT");
    inspection = it->second;
    if (reauthenticating(inspection.account))
      throw std::runtime_error("QUIESCE_STALLED");
    if (SwitchState(db_).gated(inspection.account))
      throw std::runtime_error("QUIESCE_STALLED");
    const auto rechecked =
        folderRead("folders.inspect", {{"accountId", inspection.account},
                                       {"localPath", inspection.local},
                                       {"remotePath", inspection.remote}})
            .toObject();
    const auto fresh =
        inspections_.at(rechecked.value("inspectToken").toString());
    inspections_.erase(rechecked.value("inspectToken").toString());
    if (fresh.localIdentity != inspection.localIdentity ||
        fresh.remoteIdentity != inspection.remoteIdentity)
      throw std::runtime_error("FOLDER_ROOT_CHANGED");
    {
      RootLease lease(inspection.local);
      inspection.localIdentity = lease.identity();
    }
    if (!QDir().mkpath(directory_ + "/journals"))
      throw std::runtime_error("LOCAL_ROOT_UNAVAILABLE");
    id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  } else {
    QSqlQuery q(db_);
    q.prepare("SELECT paused,account_id FROM folders WHERE id=?");
    q.addBindValue(id);
    execute(q);
    if (!q.next())
      throw std::runtime_error("NOT_FOUND");
    if (method == "folders.remove" &&
        (SwitchState(db_).gated(q.value(1).toString()) ||
         reauthenticating(q.value(1).toString())))
      throw std::runtime_error("QUIESCE_STALLED");
    if (method == "folders.syncNow" && (paused_ || q.value(0).toBool()))
      throw std::runtime_error("PAUSED");
    if (method == "folders.syncNow" &&
        throttleRemaining(q.value(1).toString()) > 0)
      throw std::runtime_error("SERVER_THROTTLED");
    if (method == "folders.remove" && runtime_->running(id))
      throw std::runtime_error("QUIESCE_STALLED");
  }
  const QJsonObject receipt{{"operationId", operation},
                            {"state", "succeeded"},
                            {"result", QJsonObject{{"folderId", id}}}};
  accountChange([&] {
    QSqlQuery q(db_);
    if (method == "folders.add") {
      q.prepare("INSERT INTO "
                "folders(id,account_id,local_path,remote_path,local_identity,"
                "remote_identity,journal_path) VALUES(?,?,?,?,?,?,?)");
      q.addBindValue(id);
      q.addBindValue(inspection.account);
      q.addBindValue(inspection.local);
      q.addBindValue(inspection.remote);
      q.addBindValue(inspection.localIdentity);
      q.addBindValue(inspection.remoteIdentity);
      q.addBindValue(directory_ + "/journals/" + id + ".sqlite");
      execute(q);
      q.prepare("UPDATE endpoints SET state='approved' WHERE account_id=? AND "
                "state='verified'");
      q.addBindValue(inspection.account);
      execute(q);
    } else if (method == "folders.remove") {
      q.prepare("UPDATE endpoints SET state='approved' WHERE state='verified' "
                "AND account_id=(SELECT account_id FROM folders WHERE id=?)");
      q.addBindValue(id);
      execute(q);
      q.prepare("DELETE FROM folders WHERE id=?");
      q.addBindValue(id);
      execute(q);
    } else if (method == "folders.pause" || method == "folders.resume") {
      q.prepare("UPDATE folders SET paused=? WHERE id=?");
      q.addBindValue(method == "folders.pause");
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
  if (method == "folders.add") {
    inspections_.erase(params.value("inspectToken").toString());
    try {
      try {
        attachFolder(id);
      } catch (const std::exception &error) {
        const auto reason = attachmentReason(error);
        if (reason.isEmpty())
          throw;
        accountChange([&] {
          QSqlQuery q(db_);
          q.prepare("UPDATE folders SET status='error',reason=? WHERE id=?");
          q.addBindValue(reason);
          q.addBindValue(id);
          execute(q);
        });
      }
    } catch (...) {
      stopForStorageFailure();
      throw;
    }
  } else if (method == "folders.remove") {
    runtime_->remove(id);
  } else {
    if (!runtime_->contains(id))
      attachFolder(id);
    if (method == "folders.syncNow")
      runtime_->wake(id, Scheduler::Reason::User);
    else
      runtime_->block(id, "userPaused", method == "folders.pause");
  }
  return receipt;
#endif
}
