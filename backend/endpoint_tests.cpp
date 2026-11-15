// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "namespace.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>
#include <stdexcept>

namespace {
/// require keeps endpoint consent assertions active in Release builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
/// command supplies a fresh idempotency key and current revision to the actual
/// RPC boundary.
QJsonObject command(Control &c, const QString &method, QJsonObject params) {
  params.insert("operationKey",
                QUuid::createUuid().toString(QUuid::WithoutBraces));
  params.insert("expectedRevision", c.projection().value("revision"));
  bool negotiated = true, subscribed = false;
  return c
      .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                            {"id", "fixture"},
                            {"method", method},
                            {"params", params}},
                negotiated, subscribed)
      .toObject();
}
/// consentRecovery proves durable permission precedes credential work and
/// survives interrupted verification (INV-03/11/14).
void consentRecovery() {
  QTemporaryDir dir;
  const auto connection = QUuid::createUuid().toString();
  QString endpoint, operation;
  {
    Control control(dir.filePath("control.sqlite"));
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(dir.filePath("control.sqlite"));
    require(db.open(), "endpoint fixture database unavailable");
    {
      QSqlQuery q(db);
      require(
          q.exec("INSERT INTO "
                 "accounts(id,session_id,operation_id,endpoint_id,base_url,"
                 "credential_ref,state) "
                 "VALUES('account','session','operation','primary','https://"
                 "primary.example.invalid/','unused-test-reference','ready')"),
          "endpoint account fixture failed");
      require(
          q.exec("INSERT INTO "
                 "endpoints(id,account_id,name,base_url,state,expires,"
                 "operation_id) "
                 "VALUES('failed-public-probe','account','Alternate','https://"
                 "alternate.example.invalid/"
                 "','rejected','2000-01-01T00:00:00.000Z','old-operation')"),
          "failed public probe fixture unavailable");
      auto reply = command(control, "endpoints.stage",
                           {{"accountId", "account"},
                            {"name", "Alternate"},
                            {"baseUrl", "https://alternate.example.invalid/"}});
      require(reply.value("result").toObject().value("state") == "running",
              "public stage did not reserve its operation");
      require(q.exec("SELECT id,approved FROM endpoints") && q.next() &&
                  !q.value(1).toBool(),
              "staging authorized credentials");
      endpoint = q.value(0).toString();
      q.finish();
      // Complete only the public stage in the fixture; queued network work
      // never runs.
      require(q.exec("UPDATE endpoints SET state='staged'"),
              "public stage fixture failed");
      reply = command(control, "endpoints.verify", {{"endpointId", endpoint}});
      require(reply.value("error").toObject().value("data").toObject().value(
                  "reasonCode") == "ENDPOINT_UNTRUSTED",
              "unapproved verification reached credential work");
      reply = command(control, "endpoints.approveOrigin",
                      {{"stagingId", endpoint}});
      operation =
          reply.value("result").toObject().value("operationId").toString();
      require(!operation.isEmpty(), "origin approval failed");
      require(q.exec("SELECT approved,state FROM endpoints") && q.next() &&
                  q.value(0).toBool() && q.value(1).toString() == "verifying",
              "credential permission was not durable before verification");
      q.finish();
    }
    db.close();
  }
  QSqlDatabase::removeDatabase(connection);
  {
    Control restarted(dir.filePath("control.sqlite"));
    const auto endpoints = restarted.projection()
                               .value("snapshot")
                               .toObject()
                               .value("endpoints")
                               .toArray();
    bool found = false;
    for (const auto &value : endpoints) {
      const auto item = value.toObject();
      if (item.value("id") != endpoint)
        continue;
      found = true;
      require(item.value("approved").toBool() &&
                  item.value("verified") == "pending" &&
                  !item.value("inUse").toBool(),
              "restart promoted or forgot origin consent");
    }
    require(found, "approved endpoint disappeared after restart");
    const auto receipt =
        command(restarted, "operations.get", {{"operationId", operation}})
            .value("result")
            .toObject();
    require(
        receipt.value("state") == "failed" &&
            receipt.value("error").toObject().value("data").toObject().value(
                "reasonCode") == "OPERATION_INTERRUPTED",
        "interrupted probe remained running");
  }
}
/// capabilities validates advertised sync features without importing background
/// or unapproved URL services (INV-03/04).
void capabilities() {
  const auto response = [](const QString &version, const QString &chunking,
                           int code = 100) {
    const QJsonObject caps{
        {"files", QJsonObject{}},
        {"dav", QJsonObject{{"chunking", chunking}}},
        {"checksums", QJsonObject{}},
        {"user_status", QJsonObject{{"enabled", true}}},
        {"notify_push",
         QJsonObject{{"endpoint", "https://unapproved.example.invalid/"}}},
        {"enterprise", QJsonObject{{"enabled", true}}}};
    const QJsonObject data{{"version", QJsonObject{{"string", version}}},
                           {"capabilities", caps}};
    const QJsonObject ocs{
        {"meta", QJsonObject{{"status", "ok"}, {"statuscode", code}}},
        {"data", data}};
    return QJsonObject{{"ocs", ocs}};
  };
  const auto parsed = syncCapabilities(response("35.0.2", "1.0"));
  require(parsed.version == "35.0.2" && parsed.sync.size() == 3 &&
              parsed.sync.contains("dav") &&
              !parsed.sync.contains("notify_push") &&
              !parsed.sync.contains("user_status") &&
              !parsed.sync.contains("enterprise"),
          "capability installation imported background services or lost sync "
          "metadata");
  for (const auto &invalid : QList<QJsonObject>{
           QJsonObject{}, response("", "1.0"), response("35.0.2/suffix", "1.0"),
           response("35.0.2", "0.9"), response("35.0.2", "invalid"),
           response("35.0.2", "1.0", 401)}) {
    bool refused = false;
    try {
      syncCapabilities(invalid);
    } catch (const std::exception &) {
      refused = true;
    }
    require(refused,
            "malformed or unsupported capability response was accepted");
  }
}
/// removalWaitsForProbe accepts removal of an account whose credentialed
/// address check is still pending, and removes nothing before it ends
/// (INV-03/12). Notes: queued network work never runs in this executable.
void removalWaitsForProbe() {
  QTemporaryDir dir;
  Control control(dir.filePath("control.sqlite"));
  const auto connection = QUuid::createUuid().toString();
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(dir.filePath("control.sqlite"));
    require(db.open(), "removal fixture database unavailable");
    QSqlQuery q(db);
    require(
        q.exec("INSERT INTO "
               "accounts(id,session_id,operation_id,endpoint_id,base_url,"
               "credential_ref,state) "
               "VALUES('6f1c8d52-6a5e-4c8e-9f39-2d0f1f9a3b11','session',"
               "'operation','primary','https://primary.example.invalid/',"
               "'unused-test-reference','ready')") &&
            q.exec("INSERT INTO endpoints(id,account_id,name,base_url,"
                   "approved,state,expires,operation_id) VALUES('alternate',"
                   "'6f1c8d52-6a5e-4c8e-9f39-2d0f1f9a3b11','Alternate','https:/"
                   "/alternate.example."
                   "invalid/',1,'verifying','2000-01-01T00:00:00.000Z',"
                   "'probe')"),
        "removal fixture rows failed");
    const auto reply =
        command(control, "accounts.remove",
                {{"accountId", "6f1c8d52-6a5e-4c8e-9f39-2d0f1f9a3b11"}});
    require(reply.value("result").toObject().value("state") == "running",
            "removal of an account with a pending probe was refused");
    require(q.exec("SELECT a.state,e.state FROM accounts a JOIN endpoints e "
                   "ON e.account_id=a.id") &&
                q.next() && q.value(0) == "removing" &&
                q.value(1) == "verifying",
            "removal released the account before its probe ended");
    q.finish();
    db.close();
  }
  QSqlDatabase::removeDatabase(connection);
}
/// renameKeepsTrust renames the main address and an approved one (protocol
/// 1.12): the name and kind change and survive a restart, trust and
/// verification do not move, and an empty name, an unknown kind or an
/// address not approved is refused (INV-03).
void renameKeepsTrust() {
  QTemporaryDir dir;
  const auto path = dir.filePath("control.sqlite");
  const auto connection = QUuid::createUuid().toString();
  // reason is the refusal reasonCode of a reply, or "" for a success.
  const auto reason = [](const QJsonObject &reply) {
    return reply.value("error")
        .toObject()
        .value("data")
        .toObject()
        .value("reasonCode")
        .toString();
  };
  // endpoint is one address of a Control's snapshot by ID.
  const auto endpoint = [](const Control &c, const QString &id) {
    for (const auto &value :
         c.projection().value("snapshot").toObject().value("endpoints").toArray())
      if (value.toObject().value("id") == id)
        return value.toObject();
    throw std::runtime_error("a renamed address disappeared");
  };
  {
    Control control(path);
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path);
    require(db.open(), "rename fixture database unavailable");
    {
      QSqlQuery q(db);
      require(
          q.exec("INSERT INTO accounts(id,session_id,operation_id,endpoint_id,"
                 "base_url,credential_ref,state) VALUES('account','session',"
                 "'operation','primary','https://primary.example.invalid/',"
                 "'unused-test-reference','ready')") &&
              q.exec("INSERT INTO endpoints(id,account_id,name,base_url,"
                     "approved,state,expires,operation_id) VALUES('home',"
                     "'account','Home','https://home.example.invalid/',1,"
                     "'verified','2000-01-01T00:00:00.000Z','probe'),('staged',"
                     "'account','Staged','https://staged.example.invalid/',0,"
                     "'staged','2999-01-01T00:00:00.000Z','stage')"),
          "rename fixture rows failed");
    }
    db.close();
    require(reason(command(control, "endpoints.rename",
                           {{"endpointId", "primary"},
                            {"name", " Tailnet "},
                            {"kind", "vpn"}})).isEmpty(),
            "renaming the main address was refused");
    require(reason(command(control, "endpoints.rename",
                           {{"endpointId", "home"},
                            {"name", "Living room"},
                            {"kind", "home"}})).isEmpty(),
            "renaming an approved address was refused");
    for (const auto &[params, expected] :
         std::initializer_list<std::pair<QJsonObject, const char *>>{
             {{{"endpointId", "home"}, {"name", "  "}, {"kind", "home"}},
              "INVALID_PARAMS"},
             {{{"endpointId", "home"}, {"name", "x"}, {"kind", "moon"}},
              "INVALID_PARAMS"},
             {{{"endpointId", "staged"}, {"name", "x"}, {"kind", "home"}},
              "NOT_FOUND"},
             {{{"endpointId", "missing"}, {"name", "x"}, {"kind", "home"}},
              "NOT_FOUND"}})
      require(reason(command(control, "endpoints.rename", params)) == expected,
              "an invalid rename was not refused with its reason");
    const auto home = endpoint(control, "home");
    require(home.value("verified") == "verified" &&
                home.value("approved").toBool() &&
                home.value("url") == "https://home.example.invalid/",
            "renaming changed an address's trust");
  }
  QSqlDatabase::removeDatabase(connection);
  // A start sets every approved address to be verified again; the names stay.
  Control restarted(path);
  const auto main = endpoint(restarted, "primary"),
             home = endpoint(restarted, "home");
  require(main.value("name") == "Tailnet" && main.value("kind") == "vpn" &&
              main.value("main").toBool() &&
              main.value("url") == "https://primary.example.invalid/",
          "the main address's name or kind did not stay");
  require(home.value("name") == "Living room" && home.value("kind") == "home" &&
              home.value("approved").toBool(),
          "an approved address's name or kind did not stay");
}
} // namespace
/// main verifies origin consent and crash boundaries without contacting a
/// server or creating credentials.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    consentRecovery();
    capabilities();
    removalWaitsForProbe();
    renameKeepsTrust();
    std::puts("PASS endpoint consent, interrupted verification recovery and "
              "renaming");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
