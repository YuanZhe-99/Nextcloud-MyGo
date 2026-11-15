#include "control.h"
#include "switch.h"
#include "wire.h"
#ifdef SYNCD_WITH_ENGINE
#include "platform/windows/ondemand.h"
#endif
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>

namespace {
/// require fails a contract test even in optimized release builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// request creates a real JSON-RPC call for the dispatcher boundary.
QJsonObject request(const QString &method, const QJsonObject &params = {}) {
  return {{"jsonrpc", "2.0"},
          {"id", "test"},
          {"method", method},
          {"params", params}};
}
/// testWire runs shared Go/C++ malformed-message fixtures and depth boundaries
/// (§20 IPC).
void testWire() {
  QFile file(PROTOCOL_FIXTURES);
  require(file.open(QIODevice::ReadOnly), "fixtures missing");
  const auto fixtures = QJsonDocument::fromJson(file.readAll()).array();
  require(!fixtures.isEmpty(), "fixtures empty");
  for (const auto &value : fixtures) {
    const auto f = value.toObject();
    QJsonDocument parsed;
    require(decodeMessage(f.value("json").toString().toUtf8(), parsed) ==
                f.value("valid").toBool(),
            qPrintable(f.value("name").toString()));
  }
  QJsonDocument parsed;
  require(
      decodeMessage(QByteArray(64, '[') + "0" + QByteArray(64, ']'), parsed),
      "64 containers rejected");
  require(
      !decodeMessage(QByteArray(65, '[') + "0" + QByteArray(65, ']'), parsed),
      "65 containers accepted");
  require(!decodeMessage(QByteArray("[\"") + char(0xff) + "\"]", parsed),
          "invalid UTF-8 accepted");
}
/// testPersistence proves durable pause, same-key replay and conflict refusal
/// after restart (INV-12/13).
void testPersistence() {
  QTemporaryDir dir;
  require(dir.isValid(), "temporary database missing");
  const auto key = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QJsonObject params{
      {"operationKey", key}, {"expectedRevision", "0"}, {"allPaused", true}};
  QJsonValue original;
  {
    Control c(dir.filePath("control.sqlite"));
    bool hello = false, subscribed = false;
    auto denied =
        c.dispatch(request("settings.update", params), hello, subscribed)
            .toObject();
    require(denied.contains("error") && c.sequence() == 0,
            "mutation before negotiation");
    c.dispatch(
        request("session.hello", {{"protocol", QJsonObject{{"major", 1}}}}),
        hello, subscribed);
    auto notification = request("settings.update", params);
    notification.remove("id");
    require(c.dispatch(notification, hello, subscribed).isNull() &&
                c.sequence() == 0,
            "notification mutated state");
    original = c.dispatch(request("settings.update", params), hello, subscribed)
                   .toObject()
                   .value("result");
    require(original.toObject().value("state") == "succeeded" &&
                c.sequence() == 1,
            "pause did not commit");
    require(c.dispatch(request("settings.update", params), hello, subscribed)
                        .toObject()
                        .value("result") == original &&
                c.sequence() == 1,
            "duplicate command reexecuted");
  }
  {
    Control c(dir.filePath("control.sqlite"));
    bool hello = true, subscribed = false;
    require(c.projection()
                .value("snapshot")
                .toObject()
                .value("daemon")
                .toObject()
                .value("allPaused")
                .toBool(),
            "pause lost on restart");
    require(c.dispatch(request("settings.update", params), hello, subscribed)
                    .toObject()
                    .value("result") == original,
            "receipt lost on restart");
    params.insert("allPaused", false);
    auto conflict =
        c.dispatch(request("settings.update", params), hello, subscribed)
            .toObject();
    require(conflict.value("error").toObject().value("data").toObject().value(
                "reasonCode") == "OPERATION_KEY_CONFLICT",
            "key conflict accepted");
    params.insert("operationKey",
                  QUuid::createUuid().toString(QUuid::WithoutBraces));
    require(c.dispatch(request("settings.update", params), hello, subscribed)
                    .toObject()
                    .contains("error") &&
                c.sequence() == 1,
            "stale revision changed settings");
    params.insert("expectedRevision", "1");
    require(c.dispatch(request("settings.update", params), hello, subscribed)
                .toObject()
                .contains("result"),
            "resume failed");
    require(!c.projection()
                 .value("snapshot")
                 .toObject()
                 .value("daemon")
                 .toObject()
                 .value("allPaused")
                 .toBool(),
            "resume not visible");
  }
}
/// testOutboxBounded: the outbox keeps only the latest 100 events however many
/// state changes commit, and the newest is always there. Notes: regression for
/// the accelerated soak (R21), whose Control database grew about 2 MB a
/// minute with a full projection per state change.
void testOutboxBounded() {
  QTemporaryDir dir;
  require(dir.isValid(), "temporary database missing");
  const auto connection = QUuid::createUuid().toString();
  {
    Control c(dir.filePath("control.sqlite"));
    bool hello = false, subscribed = false;
    c.dispatch(
        request("session.hello", {{"protocol", QJsonObject{{"major", 1}}}}),
        hello, subscribed);
    for (int i = 0; i < 150; ++i)
      require(c.dispatch(request("settings.update",
                                 {{"operationKey", QUuid::createUuid().toString(
                                                       QUuid::WithoutBraces)},
                                  {"expectedRevision", QString::number(i)},
                                  {"allPaused", i % 2 == 0}}),
                         hello, subscribed)
                  .toObject()
                  .contains("result"),
              "outbox fixture mutation refused");
    auto observer = QSqlDatabase::addDatabase("QSQLITE", connection);
    observer.setDatabaseName(dir.filePath("control.sqlite"));
    require(observer.open(), "outbox observer open failed");
    {
      QSqlQuery q(observer);
      require(q.exec("SELECT COUNT(*),MAX(seq) FROM outbox") && q.next(),
              "outbox unreadable");
      require(q.value(0).toInt() == 100 &&
                  q.value(1).toLongLong() == qint64(c.sequence()),
              "the outbox grew past its bound or lost the latest event");
    }
    observer.close();
  }
  QSqlDatabase::removeDatabase(connection);
}
/// testSwitch proves trust, all-folder quiescence, revalidation and stale-epoch
/// rejection (INV-03/04/05/06).
void testSwitch() {
  SwitchCoordinator c;
  SwitchCoordinator::Target target{"lan", false, true, true, false};
  require(c.begin(target, 0, {"one", "two"}) == "ENDPOINT_UNTRUSTED",
          "unapproved endpoint accepted");
  target.approved = true;
  require(c.begin(target, 0, {"one", "two"}).isEmpty(),
          "trusted switch refused");
  SwitchCoordinator::Proof p{0, true, false, true, true, true};
  require(!c.observe("one", p), "finished signal treated as quiescence");
  p.propagatorDestroyed = true;
  require(c.observe("one", p), "complete proof refused");
  require(c.commit(target) == "QUIESCE_STALLED" && c.epoch() == 0,
          "partial account switched");
  p.epoch = 1;
  require(!c.observe("two", p), "wrong epoch accepted");
  p.epoch = 0;
  require(c.observe("two", p), "second proof refused");
  target.tlsValid = false;
  require(c.commit(target) == "TLS_ERROR", "changed trust ignored");
  target.tlsValid = true;
  require(c.commit(target).isEmpty() && c.epoch() == 1 && c.active() == "lan",
          "atomic switch failed");
  require(!c.observe("one", p), "old proof survived completion");
  require(c.recovering(), "commit erased pending reconciliation");
  c.cancel();
  require(c.recovering(), "cancel erased committed recovery");
  require(!c.reconciled("one", 0) && !c.reconciled("unknown", 1),
          "stale or foreign recovery accepted");
  require(c.reconciled("one", 1) && !c.reconciled("one", 1),
          "reconciliation was not one-use");
  require(c.begin(target, 1, {"one", "two"}).isEmpty(),
          "new-epoch fallback preparation refused during recovery");
  c.cancel();
  require(c.recovering(), "fallback cancellation erased previous recovery");
  require(c.reconciled("two", 1) && !c.recovering(),
          "all-folder reconciliation did not finish");
}
} // namespace

/// testDecisionEnvelope checks the shared proposalDigest field before domain
/// lookup (INV-11).
void testDecisionEnvelope() {
#ifdef SYNCD_WITH_ENGINE
  QTemporaryDir dir;
  Control control(dir.filePath("control.sqlite"));
  bool negotiated = false, subscribed = false;
  control.dispatch(
      request("session.hello", {{"protocol", QJsonObject{{"major", 1}}}}),
      negotiated, subscribed);
  QFile file(
      QFileInfo(PROTOCOL_FIXTURES).dir().filePath("folders.examples.json"));
  require(file.open(QIODevice::ReadOnly), "folder examples missing");
  auto example = QJsonDocument::fromJson(file.readAll())
                     .object()
                     .value("approveDeletion")
                     .toObject();
  auto result = control.dispatch(example, negotiated, subscribed).toObject();
  require(result.value("error").toObject().value("data").toObject().value(
              "reasonCode") == "NOT_FOUND",
          "valid approval envelope rejected before decision lookup");
  auto params = example.value("params").toObject();
  params.insert("digest", params.take("proposalDigest"));
  example.insert("params", params);
  result = control.dispatch(example, negotiated, subscribed).toObject();
  require(result.value("error").toObject().value("data").toObject().value(
              "reasonCode") == "INVALID_PARAMS",
          "incorrect approval field accepted");
#endif
}

/// testFolderMode checks the protocol 1.8 folders.add mode before any
/// inspection lookup: only "full" and "onDemand" pass (PLAN §2.3).
void testFolderMode() {
#ifdef SYNCD_WITH_ENGINE
  QTemporaryDir dir;
  Control control(dir.filePath("control.sqlite"));
  bool negotiated = false, subscribed = false;
  const auto hello =
      control
          .dispatch(request("session.hello",
                            {{"protocol", QJsonObject{{"major", 1}}}}),
                    negotiated, subscribed)
          .toObject();
  require(hello.value("result")
                  .toObject()
                  .value("protocol")
                  .toObject()
                  .value("minor") == Control::protocolMinor,
          "this build's protocol not negotiated");
  QFile file(
      QFileInfo(PROTOCOL_FIXTURES).dir().filePath("folders.examples.json"));
  require(file.open(QIODevice::ReadOnly), "folder examples missing");
  const auto example = QJsonDocument::fromJson(file.readAll())
                           .object()
                           .value("addOnDemand")
                           .toObject();
  const auto reason = [&](const QJsonValue &mode, const QString &key) {
    auto message = example;
    auto params = message.value("params").toObject();
    params.insert("operationKey", key);
    if (mode.isUndefined())
      params.remove("mode");
    else
      params.insert("mode", mode);
    message.insert("params", params);
    return control.dispatch(message, negotiated, subscribed)
        .toObject()
        .value("error")
        .toObject()
        .value("data")
        .toObject()
        .value("reasonCode")
        .toString();
  };
  // A valid mode reaches the (unknown) inspection token.
  require(reason(example.value("params").toObject().value("mode"),
                 QUuid::createUuid().toString(QUuid::WithoutBraces)) ==
              "REVISION_CONFLICT",
          "on-demand add envelope rejected before inspection lookup");
  require(reason(QJsonValue::Undefined,
                 QUuid::createUuid().toString(QUuid::WithoutBraces)) ==
              "REVISION_CONFLICT",
          "add without a mode rejected");
  require(reason("sideways", QUuid::createUuid().toString(
                                 QUuid::WithoutBraces)) == "INVALID_PARAMS",
          "unknown folder mode accepted");
  require(reason(1, QUuid::createUuid().toString(QUuid::WithoutBraces)) ==
              "INVALID_PARAMS",
          "non-string folder mode accepted");
#endif
}

/// testVirtualFilesCapability protects the virtualFiles capability: true
/// exactly when the engine is linked and on-demand folders work here
/// (OnDemandRoot::available), the same answer in session.hello and the
/// snapshot, and false while the state is unavailable; folders.add with
/// onDemand is admitted exactly when it is advertised (protocol 1.11, still
/// in 1.12).
void testVirtualFilesCapability() {
#ifdef SYNCD_WITH_ENGINE
  const bool expected = OnDemandRoot::available();
#else
  const bool expected = false;
#endif
  QTemporaryDir dir;
  Control control(dir.filePath("control.sqlite"));
  bool negotiated = false, subscribed = false;
  const auto hello =
      control
          .dispatch(request("session.hello",
                            {{"protocol", QJsonObject{{"major", 1}}}}),
                    negotiated, subscribed)
          .toObject()
          .value("result")
          .toObject();
  require(hello.value("protocol").toObject().value("minor") == 12 &&
              Control::protocolMinor == 12,
          "this build does not speak protocol 1.12");
  require(hello.value("capabilities").toObject().value("virtualFiles") ==
                  expected &&
              control.projection()
                      .value("snapshot")
                      .toObject()
                      .value("daemon")
                      .toObject()
                      .value("capabilities")
                      .toObject()
                      .value("virtualFiles") == expected,
          "virtualFiles does not follow on-demand availability");
  {
    Control unavailable(dir.filePath("other.sqlite"),
                        Control::StartupMode::DatabaseUnavailable);
    require(!unavailable.projection()
                 .value("snapshot")
                 .toObject()
                 .value("daemon")
                 .toObject()
                 .value("capabilities")
                 .toObject()
                 .value("virtualFiles")
                 .toBool(),
            "virtualFiles advertised without a usable state");
  }
#ifdef SYNCD_WITH_ENGINE
  auto add = request("folders.add",
                     {{"operationKey",
                       QUuid::createUuid().toString(QUuid::WithoutBraces)},
                      {"expectedRevision", "0"},
                      {"inspectToken",
                       QUuid::createUuid().toString(QUuid::WithoutBraces)},
                      {"mode", "onDemand"}});
  // An unknown token answers REVISION_CONFLICT once the mode is admitted.
  require(control.dispatch(add, negotiated, subscribed)
                      .toObject()
                      .value("error")
                      .toObject()
                      .value("data")
                      .toObject()
                      .value("reasonCode") ==
              (expected ? "REVISION_CONFLICT" : "UNSUPPORTED_CAPABILITY"),
          "folders.add disagrees with the virtualFiles capability");
#endif
}

/// testFolderModeMigration opens a schema-9 database with an existing mapping:
/// schema 10 keeps the row and reads it as a full-file folder (PLAN §2.3).
void testFolderModeMigration() {
  QTemporaryDir dir;
  const auto path = dir.filePath("control.sqlite");
  {
    Control control(path);
  }
  const auto connection = QUuid::createUuid().toString();
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path);
    require(db.open(), "schema-9 fixture open failed");
    QSqlQuery q(db);
    // Schema 9 had no mode column.
    require(
        q.exec("ALTER TABLE folders DROP COLUMN mode") &&
            q.exec("INSERT INTO "
                   "accounts(id,session_id,operation_id,endpoint_id,base_url,"
                   "credential_ref,state) VALUES('account','session','"
                   "operation','endpoint','https://cloud.example.invalid/','"
                   "unused-test-reference','ready')") &&
            q.exec("INSERT INTO "
                   "folders(id,account_id,local_path,remote_path,local_"
                   "identity,remote_identity,journal_path) VALUES('folder',"
                   "'account','unused-local','/Example','local','remote','"
                   "unused-journal')") &&
            q.exec("PRAGMA user_version=9"),
        "schema-9 fixture not written");
  }
  QSqlDatabase::removeDatabase(connection);
  Control control(path);
  bool negotiated = false, subscribed = false;
  control.dispatch(
      request("session.hello", {{"protocol", QJsonObject{{"major", 1}}}}),
      negotiated, subscribed);
  const auto folders =
      control.dispatch(request("folders.list"), negotiated, subscribed)
          .toObject()
          .value("result")
          .toArray();
  require(folders.size() == 1 &&
              folders.first().toObject().value("id") == "folder" &&
              folders.first().toObject().value("mode") == "full",
          "schema-9 mapping lost or not read as full-file");
}

/// testSchema11Migration opens a schema-10 database: addresses read as reached
/// over the internet, and the start reports what it migrated from and which
/// version ran before (protocol 1.9, PLAN §16.2).
void testSchema11Migration() {
  QTemporaryDir dir;
  const auto path = dir.filePath("control.sqlite");
  {
    Control control(path);
  }
  const auto account = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto connection = QUuid::createUuid().toString();
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path);
    require(db.open(), "schema-10 fixture open failed");
    QSqlQuery q(db);
    q.prepare("INSERT INTO accounts(id,session_id,operation_id,endpoint_id,"
              "base_url,credential_ref,state) VALUES(?,'session','operation',"
              "'main','https://cloud.example.invalid/','unused','ready')");
    q.addBindValue(account);
    require(q.exec(), "schema-10 account fixture failed");
    q.prepare("INSERT INTO endpoints(id,account_id,name,base_url,approved,"
              "state,expires,operation_id) VALUES('alternate',?,'Alternate',"
              "'https://alternate.example.invalid/',1,'verified','','probe')");
    q.addBindValue(account);
    // Schema 10 had none of schema 11's columns or the conflicts table.
    require(q.exec() && q.exec("ALTER TABLE endpoints DROP COLUMN kind") &&
                q.exec("ALTER TABLE decisions DROP COLUMN groups") &&
                q.exec("ALTER TABLE settings DROP COLUMN daemon_version") &&
                q.exec("DROP TABLE conflicts") &&
                q.exec("PRAGMA user_version=10"),
            "schema-10 fixture not written");
  }
  QSqlDatabase::removeDatabase(connection);
  const auto report = [&](Control &control) {
    return control.projection()
        .value("snapshot")
        .toObject()
        .value("daemon")
        .toObject()
        .value("startupReport")
        .toObject();
  };
  {
    Control control(path);
    const auto first = report(control);
    require(first.value("previousSchema") == 10 &&
                first.value("schema") == Control::schemaVersion &&
                first.value("previousVersion").toString().isEmpty(),
            "schema-11 start did not report what it migrated from");
    bool found = false;
    for (const auto &entry : control.projection()
                                 .value("snapshot")
                                 .toObject()
                                 .value("endpoints")
                                 .toArray())
      found = found || (entry.toObject().value("id") == "alternate" &&
                        entry.toObject().value("kind") == "internet");
    require(found, "a schema-10 address was not read as an internet one");
  }
  Control again(path);
  require(report(again).value("previousSchema") == Control::schemaVersion &&
              report(again).value("previousVersion") ==
                  QStringLiteral(SYNCD_VERSION),
          "a second start did not name the version that ran before");
}
/// testSchema12Conversion protects the one-time conversion mark: a schema-11
/// database is backed up and its full-file folders become 'pending' while
/// on-demand ones keep ''; a schema-12 database is never re-marked, so a
/// value set later (or a folder added as full under schema 12) survives every
/// start.
void testSchema12Conversion() {
  // sqlOn runs statements on a private connection to the Control file.
  const auto sqlOn = [](const QString &path,
                        const std::function<void(QSqlQuery &)> &body) {
    const auto connection = QUuid::createUuid().toString();
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
      db.setDatabaseName(path);
      require(db.open(), "schema-12 fixture open failed");
      QSqlQuery q(db);
      body(q);
    }
    QSqlDatabase::removeDatabase(connection);
  };
  // conversions reads id -> conversion for every folder.
  const auto conversions = [&](const QString &path) {
    std::map<QString, QString> out;
    sqlOn(path, [&](QSqlQuery &q) {
      require(q.exec("SELECT id,conversion,conversion_reason FROM folders"),
              "conversion columns missing");
      while (q.next()) {
        require(q.value(2).toString().isEmpty(),
                "migration invented a conversion reason");
        out[q.value(0).toString()] = q.value(1).toString();
      }
    });
    return out;
  };
  const auto schemaOf = [&](const QString &path) {
    int version = -1;
    sqlOn(path, [&](QSqlQuery &q) {
      require(q.exec("PRAGMA user_version") && q.next(), "no user_version");
      version = q.value(0).toInt();
    });
    return version;
  };
  // insertFolder writes a row with folders.add's column list, which leaves
  // conversion to its default.
  const auto insertFolder = [](QSqlQuery &q, const QString &id,
                               const QString &mode) {
    q.prepare("INSERT INTO folders(id,account_id,local_path,remote_path,"
              "local_identity,remote_identity,journal_path,mode) "
              "VALUES(?,'account',?,?,?,?,?,?)");
    for (const auto &value :
         {id, "local-" + id, "/" + id, "identity-" + id, "remote-" + id,
          "journal-" + id, mode})
      q.addBindValue(value);
    require(q.exec(), "folder fixture not written");
  };
  const auto insertAccount = [](QSqlQuery &q) {
    require(q.exec("INSERT INTO accounts(id,session_id,operation_id,"
                   "endpoint_id,base_url,credential_ref,state) VALUES("
                   "'account','session','operation','endpoint',"
                   "'https://cloud.example.invalid/','unused','ready')"),
            "account fixture not written");
  };

  // A fresh database is at the current schema with both columns and no
  // backup.
  QTemporaryDir fresh;
  const auto freshPath = fresh.filePath("control.sqlite");
  { Control control(freshPath, Control::StartupMode::Deferred); }
  require(schemaOf(freshPath) == Control::schemaVersion &&
              Control::schemaVersion == 13,
          "a fresh database is not at schema 13");
  sqlOn(freshPath, [](QSqlQuery &q) {
    require(q.exec("SELECT COUNT(*) FROM pragma_table_info('folders') WHERE "
                   "(name='conversion' OR name='conversion_reason') AND "
                   "\"notnull\"=1 AND dflt_value=''''''") &&
                q.next() && q.value(0).toInt() == 2,
            "a fresh database lacks the conversion columns");
  });
  require(QDir(fresh.path())
              .entryList({"*.pre-schema-*"}, QDir::Files)
              .isEmpty(),
          "a fresh database was backed up");
  // A folder added as full under schema 12 keeps '' across starts.
  sqlOn(freshPath, [&](QSqlQuery &q) {
    insertAccount(q);
    insertFolder(q, "added", "full");
  });
  { Control control(freshPath, Control::StartupMode::Deferred); }
  require(conversions(freshPath) == std::map<QString, QString>{{"added", ""}},
          "a folder added as full under schema 12 was marked for conversion");

  // A schema-11 database: full -> pending, onDemand -> '', backed up first.
  QTemporaryDir old;
  const auto oldPath = old.filePath("control.sqlite");
  { Control control(oldPath, Control::StartupMode::Deferred); }
  sqlOn(oldPath, [&](QSqlQuery &q) {
    // Schema 11 had neither conversion column.
    require(q.exec("ALTER TABLE folders DROP COLUMN conversion") &&
                q.exec("ALTER TABLE folders DROP COLUMN conversion_reason"),
            "schema-11 fixture columns not dropped");
    insertAccount(q);
    insertFolder(q, "full", "full");
    insertFolder(q, "onDemand", "onDemand");
    require(q.exec("PRAGMA user_version=11"), "schema-11 fixture not marked");
  });
  { Control control(oldPath, Control::StartupMode::Deferred); }
  require(schemaOf(oldPath) == Control::schemaVersion,
          "a schema-11 database did not reach the current schema");
  require(conversions(oldPath) ==
              std::map<QString, QString>{{"full", "pending"},
                                         {"onDemand", ""}},
          "schema-11 folders were not marked full -> pending only");
  require(QDir(old.path())
                  .entryList({"*.pre-schema-13.*"}, QDir::Files)
                  .size() == 1,
          "the schema-11 database was not backed up before migration");

  // Reopening never re-marks: hand-set values survive the next starts.
  const struct {
    const char *folder, *value;
  } kept[] = {{"full", ""}, {"full", "converted"}, {"onDemand", "converting"}};
  for (const auto &row : kept) {
    sqlOn(oldPath, [&](QSqlQuery &q) {
      q.prepare("UPDATE folders SET conversion=? WHERE id=?");
      q.addBindValue(QString::fromLatin1(row.value));
      q.addBindValue(QString::fromLatin1(row.folder));
      require(q.exec(), "conversion fixture not updated");
    });
    const auto before = conversions(oldPath);
    { Control control(oldPath, Control::StartupMode::Deferred); }
    require(conversions(oldPath) == before,
            "reopening a schema-12 database changed a conversion value");
  }
  sqlOn(oldPath, [](QSqlQuery &q) {
    require(!q.exec("UPDATE folders SET conversion='sideways'"),
            "an unknown conversion state was accepted");
  });
}
/// testCyclePersistence exercises null-success persistence and rollback on
/// outbox write failure (INV-10/11).
void testCyclePersistence() {
#ifdef SYNCD_WITH_ENGINE
  QTemporaryDir dir;
  const auto connection = QUuid::createUuid().toString();
  {
    Control control(dir.filePath("control.sqlite"));
    auto observer = QSqlDatabase::addDatabase("QSQLITE", connection);
    observer.setDatabaseName(dir.filePath("control.sqlite"));
    require(observer.open(), "cycle observer open failed");
    {
      QSqlQuery q(observer);
      require(
          q.exec("INSERT INTO "
                 "accounts(id,session_id,operation_id,endpoint_id,base_url,"
                 "credential_ref,state) "
                 "VALUES('account','session','operation','endpoint','https://"
                 "cloud.example.invalid/','unused-test-reference','ready')"),
          "cycle account fixture failed");
      require(
          q.exec("INSERT INTO "
                 "folders(id,account_id,local_path,remote_path,local_identity,"
                 "remote_identity,journal_path,status) "
                 "VALUES('folder','account','unused-local','/"
                 "fixture','local-id','remote-id','unused-journal','syncing')"),
          "cycle folder fixture failed");
      control.cycleFinished("folder", {true, false, {}});
      require(!control.shutdownRequested(),
              "null success reason stopped daemon");
      require(q.exec("SELECT status,reason,last_sync FROM folders WHERE "
                     "id='folder'") &&
                  q.next(),
              "persisted result missing");
      require(q.value(0).toString() == "upToDate" && !q.value(1).isNull() &&
                  q.value(1).toString().isEmpty() &&
                  !q.value(2).toString().isEmpty(),
              "successful result did not commit non-null empty reason");
      q.finish();
      const auto sequence = control.sequence();
      require(
          q.exec("CREATE TRIGGER reject_cycle_event BEFORE INSERT ON outbox "
                 "BEGIN SELECT RAISE(ABORT, 'injected storage failure'); END"),
          "storage fault injection failed");
      control.cycleFinished("folder",
                            {false, false, "DELETION_CONFIRMATION_REQUIRED",
                             "proposal", 0, 12, 14});
      require(control.shutdownRequested(),
              "storage failure left daemon alive and frozen");
      require(control.sequence() == sequence,
              "failed completion advanced event cursor");
      require(q.exec("SELECT status FROM folders WHERE id='folder'") &&
                  q.next() && q.value(0).toString() == "upToDate",
              "failed transaction changed folder state");
      q.finish();
      require(q.exec("SELECT COUNT(*) FROM decisions") && q.next() &&
                  q.value(0).toInt() == 0,
              "failed transaction left a deletion proposal");
    }
    observer.close();
  }
  QSqlDatabase::removeDatabase(connection);
#endif
}

/// testQuotaReconciles: a cycle refused only by the server's full quota
/// completes the switch it reconciles (the address and credential work) and
/// names the quota; any other failure keeps the switch open.
/// Notes: regression for a credential reload that stayed "switching" for good
/// on a full test account (renewal fixture, 2026-11-12).
void testQuotaReconciles() {
#ifdef SYNCD_WITH_ENGINE
  for (const auto *reason : {"REMOTE_QUOTA_EXCEEDED", "SYNC_FAILED"}) {
    QTemporaryDir dir;
    const auto connection = QUuid::createUuid().toString();
    {
      Control control(dir.filePath("control.sqlite"));
      auto observer = QSqlDatabase::addDatabase("QSQLITE", connection);
      observer.setDatabaseName(dir.filePath("control.sqlite"));
      require(observer.open(), "quota observer open failed");
      QSqlQuery q(observer);
      for (const auto *sql :
           {"INSERT INTO accounts(id,session_id,operation_id,endpoint_id,"
            "base_url,credential_ref,state) VALUES('account','session','op',"
            "'endpoint','https://cloud.example.invalid/','unused','ready')",
            "INSERT INTO folders(id,account_id,local_path,remote_path,"
            "local_identity,remote_identity,journal_path,status) VALUES("
            "'folder','account','unused-local','/fixture','local-id',"
            "'remote-id','unused-journal','syncing')",
            "INSERT INTO endpoint_bindings VALUES('account','endpoint',2,2)",
            "INSERT INTO operations(key,id,digest,receipt,method) VALUES('k',"
            "'switch','','{\"operationId\":\"switch\",\"state\":\"running\"}',"
            "'endpoints.requestSwitch')",
            "INSERT INTO switch_intents VALUES('account','switch','endpoint',"
            "2,'reconciling','[]','[\"folder\"]')"})
        require(q.exec(QString::fromLatin1(sql)), "quota fixture failed");
      EngineAdapter::Result result;
      result.reason = QString::fromLatin1(reason);
      result.epoch = 2;
      control.cycleFinished("folder", result);
      require(!control.shutdownRequested(), "quota completion stopped daemon");
      const bool quota = QLatin1String(reason) == "REMOTE_QUOTA_EXCEEDED";
      require(q.exec("SELECT COUNT(*) FROM switch_intents") && q.next() &&
                  (q.value(0).toInt() == 0) == quota,
              quota ? "a full quota held the switch open"
                    : "a failed cycle completed a switch");
      q.finish();
      require(q.exec("SELECT receipt FROM operations WHERE id='switch'") &&
                  q.next() &&
                  q.value(0).toByteArray().contains("succeeded") == quota,
              "switch receipt does not match the reconciliation");
      q.finish();
      require(q.exec("SELECT status,reason FROM folders") && q.next() &&
                  q.value(0) == "error" && q.value(1).toString() == reason,
              "the cycle's reason was not kept on the folder");
      q.finish();
      observer.close();
    }
    QSqlDatabase::removeDatabase(connection);
  }
#endif
}
/// testRestartedCycle: a cycle upstream restarted after a local removal in an
/// on-demand folder (patch 0020) leaves the folder syncing without a reason,
/// keeps a switch it reconciles open and holds no decision; paused, the idle
/// folder reads paused. Notes: regression for a folder that read error /
/// SYNC_FAILED after the user deleted one file (2026-11-14).
void testRestartedCycle() {
#ifdef SYNCD_WITH_ENGINE
  QTemporaryDir dir;
  const auto connection = QUuid::createUuid().toString();
  {
    Control control(dir.filePath("control.sqlite"));
    auto observer = QSqlDatabase::addDatabase("QSQLITE", connection);
    observer.setDatabaseName(dir.filePath("control.sqlite"));
    require(observer.open(), "restart observer open failed");
    QSqlQuery q(observer);
    for (const auto *sql :
         {"INSERT INTO accounts(id,session_id,operation_id,endpoint_id,"
          "base_url,credential_ref,state) VALUES('account','session','op',"
          "'endpoint','https://cloud.example.invalid/','unused','ready')",
          "INSERT INTO folders(id,account_id,local_path,remote_path,"
          "local_identity,remote_identity,journal_path,status) VALUES("
          "'folder','account','unused-local','/fixture','local-id',"
          "'remote-id','unused-journal','syncing')",
          "INSERT INTO endpoint_bindings VALUES('account','endpoint',2,2)",
          "INSERT INTO operations(key,id,digest,receipt,method) VALUES('k',"
          "'switch','','{\"operationId\":\"switch\",\"state\":\"running\"}',"
          "'endpoints.requestSwitch')",
          "INSERT INTO switch_intents VALUES('account','switch','endpoint',"
          "2,'reconciling','[]','[\"folder\"]')"})
      require(q.exec(QString::fromLatin1(sql)), "restart fixture failed");
    EngineAdapter::Result result;
    result.followUp = true;
    result.restarted = true;
    result.epoch = 2;
    control.cycleFinished("folder", result);
    require(!control.shutdownRequested(), "a restarted cycle stopped daemon");
    require(q.exec("SELECT status,reason FROM folders") && q.next() &&
                q.value(0) == "syncing" && q.value(1).toString().isEmpty(),
            "a restarted cycle left the folder in an error state");
    q.finish();
    require(q.exec("SELECT COUNT(*) FROM switch_intents") && q.next() &&
                q.value(0).toInt() == 1,
            "a restarted cycle completed a switch");
    q.finish();
    require(q.exec("SELECT COUNT(*) FROM decisions") && q.next() &&
                q.value(0).toInt() == 0,
            "a restarted cycle raised a decision");
    q.finish();
    bool negotiated = true, subscribed = false;
    const auto state = [&] {
      return control
          .dispatch(request("folders.list"), negotiated, subscribed)
          .toObject()
          .value("result")
          .toArray()
          .at(0)
          .toObject();
    };
    require(state().value("state") == "syncing" &&
                state().value("reasonCode").toString().isEmpty(),
            "the restarted folder does not read syncing");
    require(q.exec("UPDATE folders SET paused=1"), "pause fixture failed");
    require(state().value("state") == "paused",
            "a paused folder waiting for its follow-up reads syncing");
    observer.close();
  }
  QSqlDatabase::removeDatabase(connection);
#endif
}
/// testCycleActivity: a finished cycle records local deletions caused by the
/// server and renames both ways next to its transfers (protocol 1.10).
/// Notes: regression for an Activity without local deletions or renames (B14
/// step 2).
void testCycleActivity() {
#ifdef SYNCD_WITH_ENGINE
  QTemporaryDir dir;
  Control control(dir.filePath("control.sqlite"));
  const auto connection = QUuid::createUuid().toString();
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(dir.filePath("control.sqlite"));
    require(db.open(), "activity observer open failed");
    QSqlQuery q(db);
    for (const auto *sql :
         {"INSERT INTO accounts(id,session_id,operation_id,endpoint_id,"
          "base_url,credential_ref,state) VALUES('account','session','op',"
          "'endpoint','https://cloud.example.invalid/','unused','ready')",
          "INSERT INTO folders(id,account_id,local_path,remote_path,"
          "local_identity,remote_identity,journal_path,status) VALUES("
          "'folder','account','unused-local','/fixture','local-id',"
          "'remote-id','unused-journal','syncing')",
          "INSERT INTO endpoint_bindings VALUES('account','endpoint',1,1)"})
      require(q.exec(QString::fromLatin1(sql)), "activity fixture failed");
    db.close();
  }
  QSqlDatabase::removeDatabase(connection);
  EngineAdapter::Result result;
  result.success = true;
  result.epoch = 1;
  result.uploaded = 1;
  result.deletedLocal = 2;
  result.renamedRemote = 3;
  result.renamedLocal = 4;
  control.cycleFinished("folder", result);
  bool negotiated = true, subscribed = false;
  std::map<QString, int> counts;
  for (const auto &entry :
       control.dispatch(request("activity.list"), negotiated, subscribed)
           .toObject()
           .value("result")
           .toArray())
    counts[entry.toObject().value("kind").toString()] =
        entry.toObject().value("count").toInt();
  require(counts == std::map<QString, int>{{"uploaded", 1},
                                           {"deletedLocal", 2},
                                           {"renamedRemote", 3},
                                           {"renamedLocal", 4}},
          "a cycle's local deletions or renames are missing from Activity");
#endif
}
/// testProtocolVersion: the contract's version, session.hello, Diagnostics and
/// the startup diagnostics all report Control::protocolMinor (§20 IPC).
/// Notes: regression for Diagnostics reporting 1.8 while SyncD spoke 1.9 (B14).
void testProtocolVersion() {
  QFile file(QFileInfo(PROTOCOL_FIXTURES).dir().filePath("contract.json"));
  require(file.open(QIODevice::ReadOnly), "contract missing");
  const auto contract =
      QJsonDocument::fromJson(file.readAll()).object().value("protocol");
  require(contract.toObject().value("major") == 1 &&
              contract.toObject().value("minor") == Control::protocolMinor,
          "protocol/contract.json and Control::protocolMinor differ");
  const auto text = QStringLiteral("1.%1").arg(Control::protocolMinor);
  QTemporaryDir dir;
  require(dir.isValid(), "temporary database missing");
  for (const auto mode :
       {Control::StartupMode::Immediate, Control::StartupMode::Deferred}) {
    Control control(dir.filePath("control.sqlite"), mode);
    bool negotiated = false, subscribed = false;
    const auto hello =
        control
            .dispatch(request("session.hello",
                              {{"protocol", QJsonObject{{"major", 1}}}}),
                      negotiated, subscribed)
            .toObject()
            .value("result")
            .toObject()
            .value("protocol")
            .toObject();
    require(hello.value("minor") == Control::protocolMinor,
            "session.hello reports another protocol");
    const auto diagnostics =
        control.dispatch(request("diagnostics.snapshot"), negotiated, subscribed)
            .toObject()
            .value("result")
            .toObject();
    require(diagnostics.value("stateAvailable") ==
                (mode == Control::StartupMode::Immediate),
            "diagnostics fixture reached the wrong startup state");
    require(diagnostics.value("protocol") == text,
            "diagnostics report another protocol");
  }
}
/// main runs real core contracts against a disposable database, without server
/// or Shell side effects.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testWire();
    testProtocolVersion();
    testPersistence();
    testOutboxBounded();
    testSwitch();
    testDecisionEnvelope();
    testFolderMode();
    testVirtualFilesCapability();
    testFolderModeMigration();
    testSchema11Migration();
    testSchema12Conversion();
    testCyclePersistence();
    testQuotaReconciles();
    testRestartedCycle();
    testCycleActivity();
    std::puts("PASS shared framing; persistence and cycle storage faults; "
              "switch guards; decision envelope");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
