// SPDX-License-Identifier: GPL-3.0-only
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "control.h"
#include "startup.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>
#include <stdexcept>

/// StartupTestAccess observes actual runtime/credential/network owners, not a
/// simulated admission flag (PLAN §14.1/R02; INV-01/03/10).
struct StartupTestAccess {
  /// inactive requires the absence of any owner that could attach roots or
  /// auth.
  static bool inactive(const Control &control) {
    bool result = !control.network_ && control.logins_.empty();
#ifdef SYNCD_WITH_ENGINE
    result = result && !control.runtime_;
#endif
    return result;
  }
};

namespace {
/// require retains contract assertions in optimized builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// write creates a disposable malformed or inaccessible fixture.
void write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
          "fixture write failed");
}
/// read obtains exact original evidence for preservation checks.
QByteArray read(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "fixture read failed");
  return file.readAll();
}
/// request invokes the real Control dispatcher with a negotiated peer.
QJsonObject request(Control &control, const QString &method,
                    const QJsonObject &params = {}) {
  bool negotiated = true, subscribed = false;
  return control
      .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                            {"id", 1},
                            {"method", method},
                            {"params", params}},
                negotiated, subscribed)
      .toObject();
}
/// reason extracts only a stable safe RPC refusal.
QString reason(const QJsonObject &reply) {
  return reply.value("error")
      .toObject()
      .value("data")
      .toObject()
      .value("reasonCode")
      .toString();
}
/// unavailable checks successful diagnostic bootstrap without false-empty state
/// or a subscription to static diagnostic state (INV-07/10).
void unavailable(Control &control, const QString &state) {
  bool negotiated = false, subscribed = false;
  const auto hello =
      control
          .dispatch(
              QJsonObject{{"jsonrpc", "2.0"},
                          {"id", 1},
                          {"method", "session.hello"},
                          {"params", QJsonObject{{"protocol",
                                                  QJsonObject{{"major", 1}}}}}},
              negotiated, subscribed)
          .toObject();
  require(negotiated && !hello.contains("error") &&
              hello.value("result")
                      .toObject()
                      .value("protocol")
                      .toObject()
                      .value("minor") == 9,
          "diagnostic hello failed");
  subscribed = true;
  const auto reply = control
                         .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                                               {"id", 2},
                                               {"method", "state.bootstrap"}},
                                   negotiated, subscribed)
                         .toObject();
  const auto snapshot =
      reply.value("result").toObject().value("snapshot").toObject();
  const auto daemon = snapshot.value("daemon").toObject();
  require(!reply.contains("error") && subscribed == (state == "backoff") &&
              snapshot.size() == 1 && daemon.value("stateAvailable") == false &&
              daemon.value("startupState") == state,
          "unavailable bootstrap implied empty state or subscribed");
  for (const auto &key : {"accounts", "folders", "endpoints", "networks",
                          "switches", "activity", "decisions"})
    require(!snapshot.contains(key),
            "unavailable snapshot exposed a false-empty array");
  const auto capabilities = daemon.value("capabilities").toObject();
  for (auto it = capabilities.begin(); it != capabilities.end(); ++it)
    require(it.value() == false, "diagnostic startup advertised a capability");
  const auto diagnostic =
      request(control, "diagnostics.snapshot").value("result").toObject();
  require(diagnostic.value("stateAvailable") == false &&
              diagnostic.value("startupState") == state &&
              diagnostic.value("reasonCode") == "STATE_UNAVAILABLE" &&
              !diagnostic.contains("folders"),
          "diagnostics implied known folder state");
  if (state != "backoff")
    require(reason(request(control, "operations.get",
                           {{"operationId", QUuid::createUuid().toString(
                                                QUuid::WithoutBraces)}})) ==
                "STATE_UNAVAILABLE",
            "unavailable Control DB falsely proved an old operation absent");
  for (const auto &method :
       {"accounts.list", "folders.list", "folders.inspect", "endpoints.list",
        "decisions.list", "activity.list", "accounts.login.status",
        "settings.update", "accounts.login.start", "folders.syncNow",
        "endpoints.verify", "diagnostics.export"})
    require(reason(request(control, method)) == "STATE_UNAVAILABLE",
            "state-dependent RPC escaped the startup gate");
  require(StartupTestAccess::inactive(control),
          "startup gate created an auth/network/engine owner");
}
/// crashGate connects six unclean lifetimes to the actual Control admission
/// path.
void crashGate() {
  QTemporaryDir root;
  for (int i = 0; i < 6; ++i) {
    Startup startup(root.path(), 1000000 + i * 1000);
  }
  Startup startup(root.path(), 1006000);
  const auto mode = startup.decision().diagnostic
                        ? Control::StartupMode::Diagnostic
                        : Control::StartupMode::Backoff;
  Control control(root.filePath("control.sqlite"), mode, &startup);
  unavailable(control, "diagnostic");
  require(!control.activate(9000000000LL) &&
              !QFile::exists(root.filePath("control.sqlite")),
          "crash storm opened a database or activated later");
  QCoreApplication::processEvents();
  require(StartupTestAccess::inactive(control),
          "queued crash recovery started side effects");
}
/// deferred starts ordinary recovery only after the controllable admission
/// clock.
void deferred() {
  QTemporaryDir root;
  {
    Startup crashed(root.path(), 1000);
  }
  Startup startup(root.path(), 2000);
  Control control(root.filePath("control.sqlite"),
                  Control::StartupMode::Backoff, &startup);
  unavailable(control, "backoff");
  const auto before = control.sequence();
  require(!control.activate(3999), "deferred startup ignored crash backoff");
  QCoreApplication::processEvents();
  require(StartupTestAccess::inactive(control),
          "deferred initialization queued engine/network work");
  require(control.activate(4000),
          "deferred startup never admitted healthy recovery");
  const auto daemon = control.projection()
                          .value("snapshot")
                          .toObject()
                          .value("daemon")
                          .toObject();
  require(daemon.value("stateAvailable") == true &&
              daemon.value("startupState") == "normal",
          "activation did not publish known normal state");
  require(control.sequence() == before + 1 && control.activate(4000) &&
              control.sequence() == before + 1,
          "availability did not advance exactly one sequence");
  {
    const auto name = QUuid::createUuid().toString();
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", name);
      db.setDatabaseName(root.filePath("control.sqlite"));
      db.setConnectOptions("QSQLITE_OPEN_READONLY");
      require(db.open(), "availability evidence connection failed");
      QSqlQuery query(db);
      require(
          query.exec("SELECT seq,payload FROM outbox ORDER BY seq") &&
              query.next() && query.value(0).toLongLong() == before + 1 &&
              QJsonDocument::fromJson(query.value(1).toByteArray())
                      .object()
                      .value("snapshot")
                      .toObject()
                      .value("daemon")
                      .toObject()
                      .value("stateAvailable") == true &&
              !query.next(),
          "normal availability was not published once in the durable outbox");
      query.finish();
      db.close();
    }
    QSqlDatabase::removeDatabase(name);
  }
  bool negotiated = true, subscribed = false;
  control.dispatch(
      QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "state.bootstrap"}},
      negotiated, subscribed);
  require(subscribed, "normal bootstrap did not subscribe");
}
/// corruptDatabase exercises the production constructor-failure fallback while
/// preserving exact original bytes and authenticated-dispatch readability.
void corruptDatabase() {
  QTemporaryDir root;
  Startup startup(root.path(), 1000);
  const auto path = root.filePath("control.sqlite");
  const auto journal = root.filePath("journal.sqlite");
  write(path, "corrupt original Control database");
  write(journal, "preserved journal evidence");
  const auto original = read(path), originalJournal = read(journal);
  bool refused = false;
  try {
    Control invalid(path, Control::StartupMode::Deferred, &startup);
  } catch (...) {
    refused = true;
  }
  require(refused, "corrupt database unexpectedly initialized");
  Control diagnostic(path, Control::StartupMode::DatabaseUnavailable, &startup);
  unavailable(diagnostic, "databaseUnavailable");
  require(!diagnostic.activate(10000000) && read(path) == original &&
              read(journal) == originalJournal,
          "database fallback modified original bytes or journal");
}

/// immediateAndPause preserves fixture sequence behavior and the user's pause
/// independently from an explicit initial launch clearing stop intent (INV-14).
void immediateAndPause() {
  QTemporaryDir root;
  const auto path = root.filePath("control.sqlite");
  {
    Control immediate(path);
    require(immediate.sequence() == 0 &&
                immediate.projection()
                        .value("snapshot")
                        .toObject()
                        .value("daemon")
                        .toObject()
                        .value("stateAvailable") == true,
            "default Control fixture behavior is no longer immediate");
    const auto result = request(
        immediate, "settings.update",
        {{"operationKey", QUuid::createUuid().toString(QUuid::WithoutBraces)},
         {"expectedRevision", "0"},
         {"allPaused", true}});
    require(!result.contains("error") && immediate.sequence() == 1,
            "default fixture mutation sequence changed");
  }
  {
    Startup stopped(root.path(), 1000);
    require(stopped.recordStopIntent() && stopped.finishClean(),
            "explicit clean-stop fixture failed");
  }
  Startup startup(root.path(), 2000);
  require(startup.decision().automaticRestartSuppressed &&
              startup.recordStopIntent(false),
          "explicit initial launch could not clear its prior stop intent");
  Control deferred(path, Control::StartupMode::Deferred, &startup);
  require(deferred.activate(2000) && deferred.projection()
                                             .value("snapshot")
                                             .toObject()
                                             .value("daemon")
                                             .toObject()
                                             .value("allPaused") == true,
          "explicit initial start silently cleared the user's global pause");
}
/// constructorCleanup rejects a valid SQLite file with an invalid application
/// schema after connection creation, without leaking its named Qt connection.
void constructorCleanup() {
  QTemporaryDir root;
  const auto name = QUuid::createUuid().toString();
  const auto path = root.filePath("control.sqlite");
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(path);
    require(db.open(), "cleanup fixture open failed");
    QSqlQuery query(db);
    require(query.exec("CREATE TABLE settings(broken INTEGER)") &&
                query.exec("PRAGMA user_version=7"),
            "cleanup fixture schema failed");
    query.finish();
    db.close();
  }
  QSqlDatabase::removeDatabase(name);
  auto before = QSqlDatabase::connectionNames();
  before.sort();
  bool refused = false;
  try {
    Control invalid(path, Control::StartupMode::Deferred);
  } catch (...) {
    refused = true;
  }
  auto after = QSqlDatabase::connectionNames();
  after.sort();
  require(refused && before == after,
          "throwing Control constructor leaked a Qt SQL connection");
}
/// stopParams supplies one explicit idempotent stop request, never a new
/// method.
QJsonObject stopParams() {
  return {{"operationKey", QUuid::createUuid().toString(QUuid::WithoutBraces)},
          {"reason", "user"}};
}
/// sequentialCorruptStops keeps a later explicit diagnostic boot stoppable
/// while old-key retries remain historical and cannot stop that boot
/// (INV-11/12/14).
void sequentialCorruptStops() {
  QTemporaryDir root;
  const auto path = root.filePath("control.sqlite");
  const auto params = stopParams();
  QJsonObject prior;
  write(path, "preserved corrupt Control database");
  for (int boot = 0; boot < 2; ++boot) {
    Startup startup(root.path(), 1000 + boot * 1000);
    if (startup.decision().automaticRestartSuppressed)
      require(startup.recordStopIntent(false),
              "explicit diagnostic start failed");
    std::unique_ptr<Control> control;
    try {
      control = std::make_unique<Control>(path, Control::StartupMode::Deferred,
                                          &startup);
    } catch (...) {
      control = std::make_unique<Control>(
          path, Control::StartupMode::DatabaseUnavailable, &startup);
    }
    if (boot) {
      require(request(*control, "daemon.shutdown", params).value("result") ==
                      prior &&
                  !control->shutdownRequested(),
              "old key stopped the next corrupt-DB boot");
      require(request(*control, "operations.get",
                      {{"operationId", prior.value("operationId")}})
                      .value("result") == prior,
              "prior corrupt-DB stop became unqueryable");
    }
    const auto next =
        request(*control, "daemon.shutdown", boot ? stopParams() : params);
    require(next.value("result").toObject().value("state") == "succeeded" &&
                control->shutdownRequested(),
            "second corrupt-DB diagnostic boot refused a fresh shutdown key");
    if (!boot)
      prior = next.value("result").toObject();
    control.reset();
    require(startup.finishClean(),
            "sequential diagnostic stop did not finish cleanly");
    require(read(path) == "preserved corrupt Control database",
            "diagnostic stop rewrote corrupt Control bytes");
  }
}
/// shutdownReceipt checks durable deduplication, lost-reply lookup, no
/// cross-boot replay, append-only bounded history and import into healthy
/// normal history (INV-11/12/14).
void shutdownReceipt() {
  QTemporaryDir root;
  const auto params = stopParams();
  const auto nextParams = stopParams();
  QJsonObject receipt, nextReceipt;
  {
    Startup startup(root.path(), 1000);
    Control control(root.filePath("control.sqlite"),
                    Control::StartupMode::Diagnostic, &startup);
    receipt =
        request(control, "daemon.shutdown", params).value("result").toObject();
    require(receipt.value("state") == "succeeded" &&
                control.shutdownRequested(),
            "diagnostic shutdown was not accepted durably");
    require(request(control, "daemon.shutdown", params).value("result") ==
                receipt,
            "same-key shutdown retry changed its receipt");
    const auto stored =
        QJsonDocument::fromJson(read(root.filePath("startup.json"))).object();
    require(stored.value("stopped") == true,
            "stop intent was not written before ack");
    require(startup.finishClean(), "diagnostic clean finish failed");
  }
  {
    Startup startup(root.path(), 2000);
    Control control(root.filePath("control.sqlite"),
                    Control::StartupMode::Diagnostic, &startup);
    require(request(control, "daemon.shutdown", params).value("result") ==
                    receipt &&
                !control.shutdownRequested(),
            "old receipt replay shut down a new boot");
    require(request(control, "operations.get",
                    {{"operationId", receipt.value("operationId")}})
                    .value("result") == receipt,
            "lost shutdown reply could not be recovered");
    auto changed = params;
    changed.insert("reason", "different");
    require(reason(request(control, "daemon.shutdown", changed)) ==
                "OPERATION_KEY_CONFLICT",
            "changed input reused an accepted operation key");
    nextReceipt = request(control, "daemon.shutdown", nextParams)
                      .value("result")
                      .toObject();
    require(nextReceipt.value("state") == "succeeded" &&
                nextReceipt.value("operationId") !=
                    receipt.value("operationId") &&
                control.shutdownRequested(),
            "next boot did not accept its own diagnostic stop");
    require(request(control, "operations.get",
                    {{"operationId", receipt.value("operationId")}})
                    .value("result") == receipt,
            "appending a diagnostic stop lost an old receipt");
    require(startup.finishClean(), "retry-only clean finish failed");
  }
  {
    Startup startup(root.path(), 3000);
    require(startup.recordStopIntent(false),
            "explicit initial start could not clear stopped intent");
    Control healthy(root.filePath("control.sqlite"),
                    Control::StartupMode::Deferred, &startup);
    require(!QFile::exists(root.filePath("diagnostic-stop.json")),
            "healthy database did not retire the fully imported receipt file");
    require(request(healthy, "operations.get",
                    {{"operationId", receipt.value("operationId")}})
                    .value("result") == receipt,
            "healthy history lost the imported diagnostic receipt");
    require(request(healthy, "operations.get",
                    {{"operationId", nextReceipt.value("operationId")}})
                    .value("result") == nextReceipt,
            "healthy history did not import every diagnostic receipt");
    require(healthy.activate(3000),
            "healthy imported startup did not activate");
    require(request(healthy, "daemon.shutdown", params).value("result") ==
                    receipt &&
                !healthy.shutdownRequested(),
            "imported receipt replayed a shutdown");
    require(request(healthy, "daemon.shutdown", nextParams).value("result") ==
                    nextReceipt &&
                !healthy.shutdownRequested(),
            "second imported receipt replayed a shutdown");
  }
}
/// receiptCapacity preserves all 32 accepted keys across boots, allowing
/// retries at capacity while refusing only fresh work without eviction
/// (INV-11/12/14).
void receiptCapacity() {
  QTemporaryDir root;
  QList<QJsonObject> params, receipts;
  for (int i = 0; i < 32; ++i) {
    Startup startup(root.path(), 1000 + i * 1000);
    if (startup.decision().automaticRestartSuppressed)
      require(startup.recordStopIntent(false),
              "capacity explicit start failed");
    {
      Control control(root.filePath("control.sqlite"),
                      Control::StartupMode::DatabaseUnavailable, &startup);
      params.append(stopParams());
      const auto receipt = request(control, "daemon.shutdown", params.last())
                               .value("result")
                               .toObject();
      require(receipt.value("state") == "succeeded" &&
                  control.shutdownRequested(),
              "diagnostic receipt capacity was less than 32");
      receipts.append(receipt);
    }
    require(startup.finishClean(), "capacity clean finish failed");
  }
  const auto path = root.filePath("diagnostic-stop.json");
  const auto original = read(path);
  const auto stored = QJsonDocument::fromJson(original).object();
  require(stored.value("version") == 2 &&
              stored.value("receipts").toArray().size() == 32 &&
              original.size() <= 32768,
          "receipt file did not honor bounded schema");
  Startup startup(root.path(), 9000000000LL);
  require(startup.recordStopIntent(false), "full-file explicit start failed");
  Control control(root.filePath("control.sqlite"),
                  Control::StartupMode::DatabaseUnavailable, &startup);
  for (int i = 0; i < 32; ++i) {
    require(request(control, "daemon.shutdown", params[i]).value("result") ==
                    receipts[i] &&
                !control.shutdownRequested(),
            "retained old key replay stopped a later boot");
    require(request(control, "operations.get",
                    {{"operationId", receipts[i].value("operationId")}})
                    .value("result") == receipts[i],
            "accepted receipt was evicted or became unqueryable");
  }
  auto conflict = params.first();
  conflict.insert("reason", "changed");
  require(reason(request(control, "daemon.shutdown", conflict)) ==
              "OPERATION_KEY_CONFLICT",
          "full history hid a same-key digest conflict");
  require(reason(request(control, "daemon.shutdown", stopParams())) ==
                  "QUIESCE_STALLED" &&
              !control.shutdownRequested() && read(path) == original,
          "full receipt history was overwritten or new work falsely accepted");
  require(QJsonDocument::fromJson(read(root.filePath("startup.json")))
                  .object()
                  .value("stopped") == false,
          "capacity refusal changed the user's stopped intent");
}

/// receiptImport imports all records atomically, rolls back partial conflicts,
/// and safely retries after a real Win32 file-delete sharing failure
/// (INV-11/12).
void receiptImport() {
  QTemporaryDir root;
  Startup startup(root.path(), 1000);
  bool accepted = false;
  const auto first = startup.diagnosticStop(stopParams(), accepted);
  const auto second = startup.diagnosticStop(stopParams(), accepted);
  const auto path = root.filePath("diagnostic-stop.json");
  const auto original = read(path);
  const auto records =
      QJsonDocument::fromJson(original).object().value("receipts").toArray();
  const auto name = QUuid::createUuid().toString();
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(root.filePath("import.sqlite"));
    require(db.open(), "receipt import fixture open failed");
    QSqlQuery query(db);
    require(query.exec("CREATE TABLE operations(key TEXT PRIMARY KEY,id TEXT "
                       "UNIQUE NOT NULL,digest BLOB NOT NULL,receipt BLOB NOT "
                       "NULL,method TEXT NOT NULL)"),
            "receipt import fixture schema failed");
    query.prepare("INSERT INTO operations VALUES(?,?,?,?, 'daemon.shutdown')");
    query.addBindValue(records[1].toObject().value("key").toString());
    query.addBindValue(second.value("operationId").toString());
    query.addBindValue(QByteArray("conflicting digest"));
    query.addBindValue(QJsonDocument(second).toJson(QJsonDocument::Compact));
    require(query.exec(), "receipt conflict fixture insert failed");
    query.finish();
    bool refused = false;
    try {
      startup.importDiagnosticStop(db);
    } catch (...) {
      refused = true;
    }
    require(
        refused && read(path) == original &&
            query.exec("SELECT COUNT(*) FROM operations") && query.next() &&
            query.value(0).toInt() == 1,
        "multi-receipt import committed partially or discarded source history");
    query.finish();
    query.prepare("SELECT 1 FROM operations WHERE id=?");
    query.addBindValue(first.value("operationId").toString());
    require(query.exec() && !query.next(),
            "failed import retained an earlier inserted receipt");
    query.finish();
    for (int mismatch = 0; mismatch < 2; ++mismatch) {
      query.prepare("UPDATE operations SET digest=?,method=?,id=?");
      query.addBindValue(QByteArray::fromHex(
          records[1].toObject().value("digest").toString().toLatin1()));
      query.addBindValue(mismatch == 0 ? QStringLiteral("settings.update")
                                       : QStringLiteral("daemon.shutdown"));
      query.addBindValue(
          mismatch == 0 ? second.value("operationId").toString()
                        : QUuid::createUuid().toString(QUuid::WithoutBraces));
      require(query.exec(), "receipt metadata conflict fixture failed");
      query.finish();
      refused = false;
      try {
        startup.importDiagnosticStop(db);
      } catch (...) {
        refused = true;
      }
      require(refused && read(path) == original &&
                  query.exec("SELECT COUNT(*) FROM operations") &&
                  query.next() && query.value(0).toInt() == 1,
              "metadata mismatch discarded accepted receipt history");
      query.finish();
    }
    require(query.exec("DELETE FROM operations"),
            "receipt conflict fixture cleanup failed");
    query.finish();
    const auto held = CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(held != INVALID_HANDLE_VALUE,
            "receipt delete-failure fixture open failed");
    try {
      {
        Control diagnostic(root.filePath("control.sqlite"),
                           Control::StartupMode::DatabaseUnavailable, &startup);
        require(
            reason(request(diagnostic, "daemon.shutdown", stopParams())) ==
                    "INTERNAL_ERROR" &&
                !diagnostic.shutdownRequested() && read(path) == original,
            "failed atomic append overwrote history or acknowledged new work");
        require(request(diagnostic, "operations.get",
                        {{"operationId", first.value("operationId")}})
                        .value("result") == first,
                "failed append made an accepted prior receipt unqueryable");
      }
      startup.importDiagnosticStop(db);
      require(read(path) == original,
              "delete-denied import discarded receipt bytes");
      startup.importDiagnosticStop(db);
      require(read(path) == original &&
                  query.exec("SELECT COUNT(*) FROM operations") &&
                  query.next() && query.value(0).toInt() == 2,
              "delete-failure retry duplicated or lost imported receipts");
      query.finish();
    } catch (...) {
      CloseHandle(held);
      throw;
    }
    CloseHandle(held);
    startup.importDiagnosticStop(db);
    require(!QFile::exists(path),
            "retry after delete denial did not retire the fully imported file");
    require(query.exec("SELECT COUNT(*) FROM operations") && query.next() &&
                query.value(0).toInt() == 2,
            "final import retry changed retained normal history");
    query.finish();
    db.close();
  }
  QSqlDatabase::removeDatabase(name);
}

/// receiptValidation rejects malformed bounded history without overwrite and
/// preserves accepted legacy single-receipt files during upgrade (INV-10/12).
void receiptValidation() {
  QTemporaryDir seed;
  const auto originalParams = stopParams();
  QJsonObject envelope, originalReceipt;
  {
    Startup startup(seed.path(), 1000);
    bool accepted = false;
    originalReceipt = startup.diagnosticStop(originalParams, accepted);
    envelope =
        QJsonDocument::fromJson(read(seed.filePath("diagnostic-stop.json")))
            .object();
    require(startup.finishClean(), "validation seed did not finish cleanly");
  }
  const auto record = envelope.value("receipts").toArray().first().toObject();
  auto legacy = record;
  legacy.insert("version", 1);
  const auto legacyBytes = QJsonDocument(legacy).toJson(QJsonDocument::Compact);
  write(seed.filePath("diagnostic-stop.json"), legacyBytes);
  {
    Startup startup(seed.path(), 2000);
    require(startup.recordStopIntent(false), "legacy explicit start failed");
    bool accepted = true;
    require(startup.diagnosticStop(originalParams, accepted) ==
                    originalReceipt &&
                !accepted &&
                read(seed.filePath("diagnostic-stop.json")) == legacyBytes,
            "legacy receipt retry changed history or replayed its old boot");
    const auto next = startup.diagnosticStop(stopParams(), accepted);
    require(
        accepted && next.value("state") == "succeeded" &&
            startup.diagnosticOperation(
                originalReceipt.value("operationId").toString()) ==
                originalReceipt &&
            QJsonDocument::fromJson(read(seed.filePath("diagnostic-stop.json")))
                    .object()
                    .value("receipts")
                    .toArray()
                    .size() == 2,
        "legacy upgrade lost accepted history");
  }
  QList<QByteArray> bad{"not JSON", QByteArray(32769, 'x'),
                        QByteArray("{\"version\":2,\"receipts\":") +
                            QByteArray(64, '[') + "0" + QByteArray(64, ']') +
                            "}"};
  const auto appendBad = [&](const QJsonObject &value) {
    bad.append(QJsonDocument(value).toJson(QJsonDocument::Compact));
  };
  auto invalid = envelope;
  invalid.insert("receipts", QJsonObject{});
  appendBad(invalid);
  invalid = envelope;
  invalid.insert("unknown", true);
  appendBad(invalid);
  invalid = envelope;
  invalid.insert("receipts", QJsonArray{});
  appendBad(invalid);
  invalid = envelope;
  invalid.insert("receipts", QJsonArray{QJsonValue::Null});
  appendBad(invalid);
  for (const auto &key : {"key", "digest", "run"}) {
    auto item = record;
    item.insert(key, key == QStringLiteral("digest")
                         ? QJsonValue(QString(64, 'G'))
                         : QJsonValue(123));
    invalid = envelope;
    invalid.insert("receipts", QJsonArray{item});
    appendBad(invalid);
  }
  auto item = record;
  item.insert("unknown", false);
  invalid = envelope;
  invalid.insert("receipts", QJsonArray{item});
  appendBad(invalid);
  auto receipt = originalReceipt;
  receipt.insert("state", "running");
  item = record;
  item.insert("receipt", receipt);
  invalid = envelope;
  invalid.insert("receipts", QJsonArray{item});
  appendBad(invalid);
  receipt = originalReceipt;
  receipt.insert("result", QJsonArray{});
  item = record;
  item.insert("receipt", receipt);
  invalid = envelope;
  invalid.insert("receipts", QJsonArray{item});
  appendBad(invalid);
  item = record;
  item.insert("key", QUuid::createUuid().toString(QUuid::WithoutBraces));
  invalid = envelope;
  invalid.insert("receipts", QJsonArray{record, item});
  appendBad(invalid);
  receipt = originalReceipt;
  receipt.insert("operationId",
                 QUuid::createUuid().toString(QUuid::WithoutBraces));
  item = record;
  item.insert("receipt", receipt);
  invalid = envelope;
  invalid.insert("receipts", QJsonArray{record, item});
  appendBad(invalid);
  QJsonArray overflow;
  for (int i = 0; i < 33; ++i)
    overflow.append(record);
  invalid = envelope;
  invalid.insert("receipts", overflow);
  appendBad(invalid);
  auto duplicate = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
  duplicate.replace("\"version\":2", "\"version\":2,\"version\":2");
  bad.append(duplicate);
  for (const auto &bytes : bad) {
    QTemporaryDir root;
    Startup startup(root.path(), 1000);
    const auto path = root.filePath("diagnostic-stop.json");
    write(path, bytes);
    Control control(root.filePath("control.sqlite"),
                    Control::StartupMode::DatabaseUnavailable, &startup);
    require(reason(request(control, "daemon.shutdown", stopParams())) ==
                    "INTERNAL_ERROR" &&
                !control.shutdownRequested() && read(path) == bytes,
            "invalid receipt file was repaired, overwritten or acknowledged");
  }
}
/// persistenceFailure refuses acknowledgement/exit when either durable boundary
/// fails, while preserving malformed receipt bytes (INV-10/12).
void persistenceFailure() {
  for (int fixture = 0; fixture < 3; ++fixture) {
    QTemporaryDir root;
    Startup startup(root.path(), 1000);
    if (fixture == 0) {
      require(QFile::rename(root.filePath("startup.json"),
                            root.filePath("startup.original")) &&
                  QDir().mkdir(root.filePath("startup.json")),
              "ledger failure fixture failed");
    } else if (fixture == 1) {
      require(QDir().mkdir(root.filePath("diagnostic-stop.json")),
              "receipt failure fixture failed");
    } else {
      write(root.filePath("diagnostic-stop.json"), "original damaged receipt");
    }
    Control control(root.filePath("control.sqlite"),
                    Control::StartupMode::DatabaseUnavailable, &startup);
    const auto reply = request(control, "daemon.shutdown", stopParams());
    require(reason(reply) == "INTERNAL_ERROR" && !control.shutdownRequested() &&
                StartupTestAccess::inactive(control),
            "failed persistence was acknowledged or started side effects");
    if (fixture == 2)
      require(read(root.filePath("diagnostic-stop.json")) ==
                  "original damaged receipt",
              "damaged receipt was replaced");
  }
}
} // namespace

/// main runs connected Control startup contracts without content fixtures.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    crashGate();
    deferred();
    corruptDatabase();
    immediateAndPause();
    constructorCleanup();
    sequentialCorruptStops();
    shutdownReceipt();
    receiptCapacity();
    receiptImport();
    receiptValidation();
    persistenceFailure();
    std::puts("11 connected startup contract groups passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
