#include "control.h"
#include "switch.h"
#include "wire.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>
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

/// main runs real core contracts against a disposable database, without server
/// or Shell side effects.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testWire();
    testPersistence();
    testSwitch();
    testDecisionEnvelope();
    testCyclePersistence();
    std::puts("PASS shared framing; persistence and cycle storage faults; "
              "switch guards; decision envelope");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
