// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "switch_state.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>

QJsonObject Control::diagnosticsProjection() const {
  const auto daemon =
      projection().value("snapshot").toObject().value("daemon").toObject();
  const bool available = daemon.value("stateAvailable").toBool(true);
  const auto startupState =
      daemon.value("startupState").toString(QStringLiteral("normal"));
  if (!available)
    return {{"version", SYNCD_VERSION},
            {"protocol", "1.8"},
            {"stateAvailable", false},
            {"startupState", startupState},
            {"reasonCode", daemon.value("reasonCode").toString()},
            {"allPaused", true},
            {"networkGeneration",
             QString::number(network_ ? network_->generation : 0)}};
  QJsonArray folders;
  QSqlQuery q(db_);
  if (!q.exec(
          "SELECT f.id,f.account_id,f.status,f.reason,f.paused FROM folders f "
          "JOIN accounts a ON a.id=f.account_id WHERE a.state='ready' "
          "ORDER BY f.id"))
    throw std::runtime_error("INTERNAL_ERROR");
  while (q.next()) {
    const auto folder = q.value(0).toString(), account = q.value(1).toString();
    QJsonObject item{
        {"folderId", folder},
        {"accountId", account},
        {"epoch", QString::number(SwitchState(db_).binding(account).epoch)}};
    item.insert("state", q.value(2).toString());
    item.insert("reasonCode", q.value(3).toString());
    item.insert("userPaused", q.value(4).toBool());
#ifdef SYNCD_WITH_ENGINE
    if (runtime_->contains(folder)) {
      const auto counts = runtime_->requests(folder);
      QJsonArray blockers;
      for (const auto &reason : runtime_->blockers(folder))
        blockers.append(reason);
      item.insert("blockers", blockers);
      item.insert("activeUploads", QString::number(counts.activeUploads));
      item.insert("activeDownloads", QString::number(counts.activeDownloads));
      item.insert("activeRequests", QString::number(counts.active));
      item.insert("retainedReplies", QString::number(counts.alive));
      item.insert("uncertainWrites", QString::number(counts.uncertainWrites));
      item.insert("requests", QString::number(counts.total));
      item.insert("transportFailures",
                  QString::number(counts.transportFailures));
      item.insert("uploadBytes", QString::number(counts.uploadBytes));
      item.insert("downloadBytes", QString::number(counts.downloadBytes));
    }
#endif
    folders.append(item);
  }
  return {{"version", SYNCD_VERSION},
          {"allPaused", paused_},
          {"protocol", "1.8"},
          {"stateAvailable", true},
          {"startupState", startupState},
          {"reasonCode", ""},
          {"networkGeneration",
           QString::number(network_ ? network_->generation : 0)},
          {"folders", folders}};
}

QJsonObject Control::exportDiagnostics(const QJsonObject &params,
                                       const QByteArray &digest) {
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        it.key() != "options")
      throw std::runtime_error("INVALID_PARAMS");
  if (params.contains("options") &&
      (!params.value("options").isObject() ||
       !params.value("options").toObject().isEmpty()))
    throw std::runtime_error("INVALID_PARAMS");
  const auto operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QJsonObject receipt{{"operationId", operation},
                      {"state", "running"},
                      {"result", QJsonObject{}}};
  accountChange([&] {
    QSqlQuery q(db_);
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,?,?,'diagnostics.export')");
    q.addBindValue(params.value("operationKey").toString());
    q.addBindValue(operation);
    q.addBindValue(digest);
    q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    if (!q.exec())
      throw std::runtime_error("INTERNAL_ERROR");
  });
  try {
    const auto directory = directory_ + "/diagnostics";
    if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink() ||
        QFileInfo(directory).isJunction())
      throw std::runtime_error("INTERNAL_ERROR");
    const auto path = directory + '/' + operation + ".json";
    const auto body =
        QJsonDocument(
            QJsonObject{{"schemaVersion", 1},
                        {"createdAt", QDateTime::currentDateTimeUtc().toString(
                                          Qt::ISODateWithMs)},
                        {"runtime", diagnosticsProjection()}})
            .toJson(QJsonDocument::Indented);
    if (body.size() > 1048576)
      throw std::runtime_error("INTERNAL_ERROR");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size() ||
        !file.commit())
      throw std::runtime_error("INTERNAL_ERROR");
    receipt.insert("state", "succeeded");
    receipt.insert("result",
                   QJsonObject{{"path", QDir::toNativeSeparators(path)},
                               {"format", "application/json"}});
    const auto files = QDir(directory).entryInfoList(
        {"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Time);
    int retained = 1;
    for (const auto &entry : files) {
      if (entry.absoluteFilePath() == path)
        continue;
      const auto id = entry.completeBaseName();
      if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id)
        continue;
      // A UUID-shaped filename alone does not establish ownership (INV-11).
      // Include interrupted exports: their files may have committed before the
      // terminal operation receipt could be persisted.
      QSqlQuery owned(db_);
      owned.prepare("SELECT 1 FROM operations WHERE id=? AND "
                    "method='diagnostics.export'");
      owned.addBindValue(id);
      if (!owned.exec())
        throw std::runtime_error("INTERNAL_ERROR");
      if (!owned.next())
        continue;
      if (++retained > 5 && !QFile::remove(entry.absoluteFilePath()))
        throw std::runtime_error("INTERNAL_ERROR");
    }
  } catch (...) {
    receipt.insert("state", "failed");
    receipt.remove("result");
    receipt.insert(
        "error",
        QJsonObject{{"code", -32000},
                    {"message", "Support bundle could not be written"},
                    {"data", QJsonObject{{"reasonCode", "INTERNAL_ERROR"}}}});
  }
  accountChange([&] {
    QSqlQuery q(db_);
    q.prepare("UPDATE operations SET receipt=? WHERE id=?");
    q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    q.addBindValue(operation);
    if (!q.exec())
      throw std::runtime_error("INTERNAL_ERROR");
  });
  return receipt;
}
