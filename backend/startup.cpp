// SPDX-License-Identifier: GPL-3.0-only
#include "startup.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace {
constexpr qint64 crashWindowMs = 600000;
constexpr qsizetype stopFileLimit = 32768;
constexpr qsizetype stopReceiptLimit = 32;
/// milliseconds rejects malformed or noncanonical persistent clock samples.
bool milliseconds(const QJsonValue &value, qint64 &out) {
  if (!value.isString())
    return false;
  bool ok = false;
  out = value.toString().toLongLong(&ok);
  return ok && out >= 0 && QString::number(out) == value.toString();
}
/// ReadDatabase releases a named read-only SQLite connection on every exit
/// path.
struct ReadDatabase {
  QSqlDatabase db;
  /// ReadDatabase reserves a unique connection without creating a database.
  explicit ReadDatabase(const QString &path)
      : db(QSqlDatabase::addDatabase(
            "QSQLITE", QUuid::createUuid().toString(QUuid::WithoutBraces))) {
    db.setDatabaseName(path);
    db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
  }
  /// ~ReadDatabase closes and unregisters the connection after queries die.
  ~ReadDatabase() {
    const auto name = db.connectionName();
    db.close();
    db = {};
    QSqlDatabase::removeDatabase(name);
  }
};
/// schema verifies the whole database before trusting its migration version.
int schema(QSqlDatabase db) {
  QSqlQuery q(db);
  if (!q.exec("PRAGMA integrity_check") || !q.next() ||
      q.value(0).toString() != "ok" || q.next())
    throw std::runtime_error("Control database integrity check failed");
  if (!q.exec("PRAGMA user_version") || !q.next())
    throw std::runtime_error("Cannot read Control database version");
  return q.value(0).toInt();
}
} // namespace

Startup::Startup(const QString &directory, qint64 monotonicMs)
    : lock_(directory + "/daemon.lock"), path_(directory + "/startup.json"),
      stopPath_(directory + "/diagnostic-stop.json"),
      run_(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      started_(monotonicMs) {
  lock_.setStaleLockTime(0);
  if (!lock_.tryLock(0))
    throw std::runtime_error("SyncD profile is already owned");
  if (monotonicMs < 0) {
    decision_.diagnostic = true;
    return;
  }
  QFile file(path_);
  if (file.exists()) {
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
      decision_.diagnostic = true;
      return;
    }
    QJsonParseError error;
    const auto bytes = file.read(4097);
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    const auto object = doc.object();
    const auto entries = object.value("crashes");
    qint64 previous = 0;
    if (file.error() != QFileDevice::NoError || bytes.size() > 4096 ||
        error.error != QJsonParseError::NoError || !doc.isObject() ||
        doc.toJson(QJsonDocument::Compact) != bytes || object.size() != 5 ||
        object.value("version") != 1 || !object.value("running").isBool() ||
        !object.value("stopped").isBool() ||
        !milliseconds(object.value("started"), previous) ||
        !entries.isArray() || entries.toArray().size() > 6) {
      decision_.diagnostic = true;
      return;
    }
    for (const auto &entry : entries.toArray()) {
      qint64 at = 0;
      if (!milliseconds(entry, at)) {
        decision_.diagnostic = true;
        return;
      }
      // A reboot or monotonic discontinuity cannot erase recent crash evidence.
      if (previous > monotonicMs || at > monotonicMs ||
          monotonicMs - at < crashWindowMs)
        crashes_.append(entry);
    }
    decision_.priorUnclean = object.value("running").toBool();
    decision_.automaticRestartSuppressed = object.value("stopped").toBool();
    stopped_ = decision_.automaticRestartSuppressed;
    if (previous > monotonicMs) {
      // Re-anchor retained evidence conservatively after a reboot.
      for (qsizetype i = 0; i < crashes_.size(); ++i)
        crashes_[i] = QString::number(monotonicMs);
    }
    if (decision_.priorUnclean) {
      if (crashes_.size() == 6)
        crashes_.removeFirst();
      crashes_.append(QString::number(monotonicMs));
    }
  }
  decision_.crashes = static_cast<int>(crashes_.size());
  decision_.diagnostic = decision_.crashes > 5;
  if (decision_.priorUnclean && !decision_.diagnostic) {
    constexpr qint64 delays[] = {2000, 5000, 15000, 60000};
    decision_.delayMs = delays[std::min(decision_.crashes - 1, 3)];
  }
  file.close();
  writable_ = true;
  if (!save(true)) {
    writable_ = false;
    decision_.diagnostic = true;
  }
}

bool Startup::engineAllowed(qint64 monotonicMs) const {
  return !decision_.diagnostic && !finished_ && !stopped_ &&
         monotonicMs >= started_ && monotonicMs - started_ >= decision_.delayMs;
}

bool Startup::save(bool running) {
  if (!writable_)
    return false;
  const QJsonObject object{{"version", 1},
                           {"running", running},
                           {"stopped", stopped_},
                           {"started", QString::number(started_)},
                           {"crashes", crashes_}};
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  QSaveFile file(path_);
  file.setDirectWriteFallback(false);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
         file.commit();
}

bool Startup::recordStopIntent(bool stopped) {
  if (finished_)
    return false;
  stopped_ = stopped;
  if (save(true))
    return true;
  decision_.diagnostic = true;
  return false;
}

bool Startup::finishClean() {
  if (finished_)
    return true;
  if (!save(false)) {
    decision_.diagnostic = true;
    return false;
  }
  finished_ = true;
  return true;
}

QJsonArray Startup::stopRecords() const {
  QFile file(stopPath_);
  if (!file.exists())
    return {};
  if (!file.open(QIODevice::ReadOnly))
    throw std::runtime_error("INTERNAL_ERROR");
  const auto bytes = file.read(stopFileLimit + 1);
  if (file.error() != QFileDevice::NoError || bytes.size() > stopFileLimit)
    throw std::runtime_error("INTERNAL_ERROR");
  // Reject nesting before parsing; strings may contain escaped punctuation.
  // The largest accepted shape is root/receipts/entry/receipt/result (depth 5).
  int depth = 0;
  bool quoted = false, escaped = false;
  for (const auto ch : bytes) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == '"')
        quoted = false;
    } else if (ch == '"') {
      quoted = true;
    } else if (ch == '{' || ch == '[') {
      if (++depth > 5)
        throw std::runtime_error("INTERNAL_ERROR");
    } else if ((ch == '}' || ch == ']') && --depth < 0) {
      throw std::runtime_error("INTERNAL_ERROR");
    }
  }
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(bytes, &error);
  const auto envelope = document.object();
  if (error.error != QJsonParseError::NoError || !document.isObject() ||
      document.toJson(QJsonDocument::Compact) != bytes)
    throw std::runtime_error("INTERNAL_ERROR");
  QJsonArray records;
  if (envelope.value("version") == 1 && envelope.size() == 5) {
    // Existing accepted single-receipt files remain readable until an append
    // atomically publishes version 2, or a complete healthy import deletes it.
    auto record = envelope;
    record.remove("version");
    records.append(record);
  } else if (envelope.value("version") == 2 && envelope.size() == 2 &&
             envelope.value("receipts").isArray()) {
    records = envelope.value("receipts").toArray();
  } else {
    throw std::runtime_error("INTERNAL_ERROR");
  }
  if (records.isEmpty() || records.size() > stopReceiptLimit)
    throw std::runtime_error("INTERNAL_ERROR");
  const auto validId = [](const QJsonValue &value) {
    const auto text = value.toString();
    return !QUuid(text).isNull() &&
           QUuid(text).toString(QUuid::WithoutBraces) == text;
  };
  QSet<QString> keys, operations;
  for (const auto &value : records) {
    const auto record = value.toObject();
    const auto receipt = record.value("receipt").toObject();
    const auto key = record.value("key").toString();
    const auto operation = receipt.value("operationId").toString();
    const auto digest = record.value("digest").toString().toLatin1();
    if (!value.isObject() || record.size() != 4 ||
        !validId(record.value("key")) || !validId(record.value("run")) ||
        digest.size() != 64 || QByteArray::fromHex(digest).toHex() != digest ||
        receipt.size() != 3 || !validId(receipt.value("operationId")) ||
        receipt.value("state") != "succeeded" ||
        !receipt.value("result").isObject() ||
        !receipt.value("result").toObject().isEmpty() || keys.contains(key) ||
        operations.contains(operation))
      throw std::runtime_error("INTERNAL_ERROR");
    keys.insert(key);
    operations.insert(operation);
  }
  return records;
}

QJsonObject Startup::diagnosticStop(const QJsonObject &params,
                                    bool &acceptedNow) {
  acceptedNow = false;
  const auto key = params.value("operationKey").toString();
  if (QUuid(key).isNull() || QUuid(key).toString(QUuid::WithoutBraces) != key)
    throw std::runtime_error("INVALID_PARAMS");
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        it.key() != "reason")
      throw std::runtime_error("INVALID_PARAMS");
  const auto digest = QCryptographicHash::hash(QByteArray("daemon.shutdown\n") +
                                                   QJsonDocument(params).toJson(
                                                       QJsonDocument::Compact),
                                               QCryptographicHash::Sha256)
                          .toHex();
  auto records = stopRecords();
  for (const auto &value : records) {
    const auto prior = value.toObject();
    if (prior.value("key") != key)
      continue;
    if (prior.value("digest").toString().toLatin1() != digest)
      throw std::runtime_error("OPERATION_KEY_CONFLICT");
    // An old terminal receipt never commands a new daemon to exit (INV-11/12).
    acceptedNow = prior.value("run") == run_;
    return prior.value("receipt").toObject();
  }
  if (records.size() == stopReceiptLimit)
    throw std::runtime_error("QUIESCE_STALLED");
  if (!recordStopIntent())
    throw std::runtime_error("INTERNAL_ERROR");
  const QJsonObject receipt{
      {"operationId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
      {"state", "succeeded"},
      {"result", QJsonObject{}}};
  const QJsonObject record{{"key", key},
                           {"digest", QString::fromLatin1(digest)},
                           {"run", run_},
                           {"receipt", receipt}};
  records.append(record);
  const auto bytes =
      QJsonDocument(QJsonObject{{"version", 2}, {"receipts", records}})
          .toJson(QJsonDocument::Compact);
  QSaveFile file(stopPath_);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit())
    throw std::runtime_error("INTERNAL_ERROR");
  acceptedNow = true;
  return receipt;
}

QJsonObject Startup::diagnosticOperation(const QString &operation) const {
  for (const auto &value : stopRecords()) {
    const auto receipt = value.toObject().value("receipt").toObject();
    if (receipt.value("operationId") == operation)
      return receipt;
  }
  throw std::runtime_error("NOT_FOUND");
}

void Startup::importDiagnosticStop(QSqlDatabase db) {
  const auto records = stopRecords();
  if (records.isEmpty())
    return;
  if (!db.transaction())
    throw std::runtime_error("INTERNAL_ERROR");
  try {
    QSqlQuery query(db);
    for (const auto &value : records) {
      const auto record = value.toObject();
      const auto receipt = record.value("receipt").toObject();
      const auto digest =
          QByteArray::fromHex(record.value("digest").toString().toLatin1());
      query.prepare(
          "SELECT digest,receipt,method,id FROM operations WHERE key=?");
      query.addBindValue(record.value("key").toString());
      if (!query.exec())
        throw std::runtime_error("INTERNAL_ERROR");
      if (query.next()) {
        if (query.value(0).toByteArray() != digest ||
            QJsonDocument::fromJson(query.value(1).toByteArray()).object() !=
                receipt ||
            query.value(2).toString() != "daemon.shutdown" ||
            query.value(3).toString() !=
                receipt.value("operationId").toString())
          throw std::runtime_error("INTERNAL_ERROR");
      } else {
        query.finish();
        query.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
                      "VALUES(?,?,?,?, 'daemon.shutdown')");
        query.addBindValue(record.value("key").toString());
        query.addBindValue(receipt.value("operationId").toString());
        query.addBindValue(digest);
        query.addBindValue(
            QJsonDocument(receipt).toJson(QJsonDocument::Compact));
        if (!query.exec())
          throw std::runtime_error("INTERNAL_ERROR");
      }
      query.finish();
    }
    if (!db.commit())
      throw std::runtime_error("INTERNAL_ERROR");
  } catch (...) {
    db.rollback();
    throw;
  }
  // A delete failure leaves every receipt in both locations. Retrying validates
  // all matching rows in one transaction before attempting deletion again.
  QFile::remove(stopPath_);
}

QString backupControlBeforeMigration(const QString &path, int targetSchema) {
  if (!QFileInfo::exists(path))
    return {};
  ReadDatabase source(path);
  if (!source.db.open())
    throw std::runtime_error("Cannot inspect Control database");
  const int before = schema(source.db);
  if (before > targetSchema || before < 0)
    throw std::runtime_error("Unsupported Control database version");
  if (before == targetSchema)
    return {};
  const auto backup = path + ".pre-schema-" + QString::number(targetSchema) +
                      "." + QUuid::createUuid().toString(QUuid::WithoutBraces) +
                      ".sqlite";
  const auto pending = backup + ".pending";
  try {
    {
      QSqlQuery q(source.db);
      q.prepare("VACUUM main INTO ?");
      q.addBindValue(pending);
      if (!q.exec())
        throw std::runtime_error("Cannot back up Control database");
    }
    {
      ReadDatabase copy(pending);
      if (!copy.db.open() || schema(copy.db) != before)
        throw std::runtime_error("Control database backup verification failed");
    }
    if (!QFile::rename(pending, backup))
      throw std::runtime_error("Cannot publish Control database backup");
  } catch (...) {
    // Only this invocation's unpublished backup is disposable, never the
    // source.
    QFile::remove(pending);
    throw;
  }
  return backup;
}
