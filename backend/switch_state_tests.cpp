// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "switch_state.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>
#include <stdexcept>

namespace {
/// require retains assertions in optimized contract builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// sql executes fixture setup without suppressing SQL failures.
void sql(QSqlDatabase db, const QString &text) {
  QSqlQuery q(db);
  require(q.exec(text), "fixture SQL failed");
}
/// refuses checks an observable refusal, not an implementation detail.
template <class F> void refuses(F action, const char *reason) {
  try {
    action();
  } catch (const std::exception &e) {
    require(QString::fromUtf8(e.what()) == reason, "wrong switch refusal");
    return;
  }
  throw std::runtime_error("unsafe switch accepted");
}
/// seed supplies two approved origins and two mapped roots, one independently
/// paused.
void seed(QSqlDatabase db) {
  sql(db, "INSERT INTO "
          "accounts(id,session_id,operation_id,endpoint_id,base_url,credential_"
          "ref,state) "
          "VALUES('account','session','login','primary','https://"
          "primary.example.invalid/','owned-reference','ready')");
  sql(db, "INSERT INTO "
          "endpoints(id,account_id,name,base_url,approved,state,expires,"
          "operation_id) "
          "VALUES('alternate','account','Alternate','https://"
          "alternate.example.invalid/',1,'verified','','verify')");
  sql(db, "INSERT INTO "
          "folders(id,account_id,local_path,remote_path,local_identity,remote_"
          "identity,journal_path,paused) "
          "VALUES('one','account','owned-one','/"
          "one','local-one','remote-one','owned-one.sqlite',0),('two','account'"
          ",'owned-two','/two','local-two','remote-two','owned-two.sqlite',1)");
}
/// liveReservation verifies IPC deduplication, concurrent exclusion and failed
/// activation gating (INV-03/06/12).
void liveReservation() {
  QTemporaryDir root;
  Control control(root.filePath("control.sqlite"));
  auto db = QSqlDatabase::addDatabase("QSQLITE", "live-switch-test");
  db.setDatabaseName(root.filePath("control.sqlite"));
  require(db.open(), "live fixture open failed");
  sql(db, "INSERT INTO "
          "accounts(id,session_id,operation_id,endpoint_id,base_url,credential_"
          "ref,state) "
          "VALUES('account','session','login','primary','https://"
          "primary.example.invalid/','unused-reference','ready')");
  sql(db, "INSERT INTO "
          "endpoints(id,account_id,name,base_url,approved,state,expires,"
          "operation_id) "
          "VALUES('alternate','account','Alternate','https://"
          "alternate.example.invalid/',1,'verified','','verify')");
  bool negotiated = true, subscribed = false;
  QJsonObject params{
      {"operationKey", "d668ee25-78e7-45a6-bf17-f0b3948cb6d1"},
      {"expectedRevision", control.projection().value("revision")},
      {"accountId", "account"},
      {"endpointId", "alternate"}};
  const auto call = [&](const QJsonObject &p) {
    return control
        .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                              {"id", 1},
                              {"method", "endpoints.requestSwitch"},
                              {"params", p}},
                  negotiated, subscribed)
        .toObject();
  };
  auto invalid = params;
  invalid.insert("endpointId", "unknown");
#ifdef SYNCD_WITH_ENGINE
  require(
      call(invalid).value("error").toObject().value("data").toObject().value(
          "reasonCode") == "ENDPOINT_UNTRUSTED",
      "unknown target reached activation");
  auto extra = params;
  extra.insert("force", true);
  require(call(extra).value("error").toObject().value("data").toObject().value(
              "reasonCode") == "INVALID_PARAMS",
          "force bypass accepted");
  const auto accepted = call(params);
  require(accepted.value("result").toObject().value("state") == "running",
          "live switch not reserved");
  require(call(params) == accepted, "switch retry created a second operation");
  require(SwitchState(db).binding("account").epoch == 0 &&
              SwitchState(db).gated("account"),
          "accepted receipt prematurely published a target");
  auto concurrent = params;
  concurrent.insert("operationKey",
                    QUuid::createUuid().toString(QUuid::WithoutBraces));
  concurrent.insert("expectedRevision", control.projection().value("revision"));
  require(call(concurrent)
                  .value("error")
                  .toObject()
                  .value("data")
                  .toObject()
                  .value("reasonCode") == "QUIESCE_STALLED",
          "overlapping switch accepted");
  QCoreApplication::processEvents();
  require(SwitchState(db).binding("account").epoch == 0 &&
              SwitchState(db).gated("account"),
          "failed verification published target or cleared gate");
  QSqlQuery q(db);
  require(q.exec("SELECT phase FROM switch_intents") && q.next() &&
              q.value(0) == "blocked",
          "failed activation not restart-safe");
  q.finish();
  require(call(params).value("result").toObject().value("state") == "failed",
          "retry replayed failed activation");
#else
  require(call(params).value("error").toObject().value("data").toObject().value(
              "reasonCode") == "UNSUPPORTED_CAPABILITY",
          "foundation build advertised switching");
#endif
  db.close();
}

/// durableSwitch tests crash gates, monotonic epochs, atomic publication and
/// stale results (INV-06/08/12/14).
void durableSwitch() {
  QTemporaryDir root;
  require(root.isValid(), "temporary profile unavailable");
  const auto path = root.filePath("control.sqlite");
  {
    Control initialize(path);
  }
  auto db = QSqlDatabase::addDatabase("QSQLITE", "durable-switch-test");
  db.setDatabaseName(path);
  require(db.open(), "profile open failed");
  seed(db);
  SwitchState store(db);
  require(db.transaction(), "prepare transaction failed");
  require(store.begin("account", "alternate", "switch-one", 0) == 1,
          "first epoch incorrect");
  sql(db,
      "INSERT INTO operations(key,id,digest,receipt,method) "
      "VALUES('switch-key','switch-one',X'01','{\"operationId\":\"switch-one\","
      "\"state\":\"running\",\"result\":{}}','endpoints.requestSwitch')");
  require(db.commit(), "prepare commit failed");
  require(store.binding("account").endpoint == "primary" &&
              store.gated("account"),
          "preparation changed active binding or lost gate");
  db.close();
  {
    Control restart(path);
    const auto folders = restart.projection()
                             .value("snapshot")
                             .toObject()
                             .value("folders")
                             .toArray();
    require(folders.size() == 2, "restart lost mapped folders");
    for (const auto &value : folders) {
      const auto folder = value.toObject();
      require(folder.value("state").toString() ==
                  (folder.value("id") == "two" ? "paused" : "error"),
              "interrupted switch projected success or erased a user pause");
    }
    bool negotiated = false, subscribed = false;
    restart.dispatch(
        QJsonObject{
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "session.hello"},
            {"params", QJsonObject{{"protocol", QJsonObject{{"major", 1}}}}}},
        negotiated, subscribed);
    const auto refusal =
        restart
            .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                                  {"id", 2},
                                  {"method", "endpoints.remove"},
                                  {"params",
                                   QJsonObject{
                                       {"operationKey",
                                        "b54d7a8a-5ae1-40af-b821-6fba65f467a4"},
                                       {"expectedRevision",
                                        restart.projection().value("revision")},
                                       {"endpointId", "alternate"}}}},
                      negotiated, subscribed)
            .toObject();
    require(refusal.value("error").toObject().value("data").toObject().value(
                "reasonCode") == "QUIESCE_STALLED",
            "pending switch target was removed through IPC");
#ifdef SYNCD_WITH_ENGINE
    const auto sequence = restart.sequence();
    restart.cycleFinished("one", {true, false, {}, {}, 0, 0, 0, 0});
    require(restart.sequence() == sequence,
            "blocked restart accepted a late completion");
#endif
  }
  require(db.open(), "restart profile open failed");
  require(store.binding("account").epoch == 0 && store.gated("account"),
          "restart cleared preparation gate");
  QSqlQuery receipt(db);
  require(
      receipt.exec("SELECT receipt FROM operations WHERE id='switch-one'") &&
          receipt.next(),
      "interrupted receipt missing");
  require(QJsonDocument::fromJson(receipt.value(0).toByteArray())
                  .object()
                  .value("state") == "failed",
          "restart replayed unfinished operation");
  receipt.finish();
  require(db.transaction(), "replacement transaction failed");
  require(store.begin("account", "alternate", "switch-two", 0) == 2,
          "interrupted epoch reused");
  store.commit("account", 2);
  require(db.commit(), "binding commit failed");
  require(store.binding("account").endpoint == "alternate" &&
              store.binding("account").epoch == 2 && store.gated("account"),
          "commit skipped reconciliation gate");
  require(db.transaction(), "reconciliation transaction failed");
  require(!store.reconciled("account", 1, "one") &&
              !store.reconciled("account", 2, "unknown"),
          "stale or foreign result consumed");
  require(store.reconciled("account", 2, "one") &&
              !store.reconciled("account", 2, "one") && store.gated("account"),
          "duplicate or partial reconciliation cleared account gate");
  require(store.reconciled("account", 2, "two") && !store.gated("account"),
          "complete reconciliation retained gate");
  require(db.commit(), "reconciliation commit failed");
  require(receipt.exec("SELECT paused FROM folders WHERE id='two'") &&
              receipt.next() && receipt.value(0).toBool(),
          "switch erased user pause");
  receipt.finish();
  db.close();
  {
    Control stable(path);
#ifdef SYNCD_WITH_ENGINE
    const auto sequence = stable.sequence();
    stable.cycleFinished("one",
                         {true, false, {}, "stale-proposal", 1, 0, 2, 1});
    require(stable.sequence() == sequence,
            "old epoch changed the committed projection");
#endif
  }
  require(db.open(), "stable binding reopen failed");
  require(db.transaction(), "rollback transaction failed");
  require(store.begin("account", "primary", "switch-three", 2) == 3,
          "fallback epoch did not increase");
  store.commit("account", 3);
  require(db.rollback(), "binding rollback failed");
  require(store.binding("account").epoch == 2 && !store.gated("account"),
          "failed transaction published binding");
  require(db.transaction(), "membership transaction failed");
  store.begin("account", "primary", "switch-four", 2);
  sql(db, "UPDATE folders SET remote_identity='changed' WHERE id='two'");
  refuses([&] { store.commit("account", 3); }, "REVISION_CONFLICT");
  require(db.rollback(), "membership rollback failed");
  require(db.transaction(), "path change transaction failed");
  store.begin("account", "primary", "path-change", 2);
  sql(db,
      "UPDATE folders SET local_path='relocated-owned-root' WHERE id='one'");
  refuses([&] { store.commit("account", 3); }, "REVISION_CONFLICT");
  require(db.rollback(), "path change rollback failed");
  require(db.transaction(), "defer transaction failed");
  const auto deferred =
      store.begin("account", "primary", "healthy-deferred", 2);
  store.defer("account", deferred);
  require(!store.gated("account") && store.binding("account").epoch == 2,
          "deferred switch changed binding or retained admission gate");
  require(store.begin("account", "primary", "after-defer", 2) > deferred,
          "deferred epoch was reused");
  require(db.rollback(), "defer rollback failed");
  require(db.transaction(), "consent transaction failed");
  store.begin("account", "alternate", "switch-five", 2);
  sql(db, "UPDATE endpoints SET approved=0 WHERE id='alternate'");
  refuses([&] { store.commit("account", 3); }, "ENDPOINT_UNTRUSTED");
  require(db.rollback(), "consent rollback failed");
  require(db.transaction(), "committed crash transaction failed");
  store.begin("account", "primary", "committed-crash", 2);
  store.commit("account", 3);
  sql(db, "INSERT INTO operations(key,id,digest,receipt,method) "
          "VALUES('committed-key','committed-crash',X'02','{\"operationId\":"
          "\"committed-crash\",\"state\":\"running\",\"result\":{}}','"
          "endpoints.requestSwitch')");
  require(db.commit(), "committed crash fixture failed");
  db.close();
  {
    Control restart(path);
  }
  require(db.open(), "committed crash reopen failed");
  require(store.binding("account").epoch == 3 &&
              store.binding("account").endpoint == "primary" &&
              store.gated("account"),
          "restart rolled back a committed binding or opened admission");
  require(!store.reconciled("account", 3, "one"),
          "late completion cleared restart block");
  refuses([&] { store.begin("account", "alternate", "stale-intent", 2); },
          "REVISION_CONFLICT");
  require(db.transaction(), "empty account transaction failed");
  sql(db, "INSERT INTO "
          "accounts(id,session_id,operation_id,endpoint_id,base_url,credential_"
          "ref,state) "
          "VALUES('empty','empty-session','empty-login','empty-primary','https:"
          "//empty.example.invalid/','empty-reference','ready')");
  require(store.begin("empty", "empty-primary", "empty-switch", 0) == 1,
          "empty account allocation failed");
  store.commit("empty", 1);
  require(!store.gated("empty"),
          "empty account retained an impossible reconciliation gate");
  require(db.commit(), "empty account commit failed");
  sql(db, "UPDATE endpoint_bindings SET allocated_epoch=9223372036854775807");
  refuses([&] { store.begin("account", "primary", "overflow", 3); },
          "INTERNAL_ERROR");
  db.close();
}
} // namespace
/// main runs durable switch contracts without credentials or network traffic.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    durableSwitch();
    liveReservation();
    QSqlDatabase::removeDatabase("durable-switch-test");
    QSqlDatabase::removeDatabase("live-switch-test");
    std::puts(
        "PASS durable switch crash gates, epochs and atomic recovery state");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
