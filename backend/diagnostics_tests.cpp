// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>
#include <stdexcept>

namespace {
/// require preserves contract checks in optimized regression executables.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// uuid reserves an independent fixture identity without external resources.
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
/// call exercises the negotiated production JSON-RPC boundary.
QJsonObject call(Control &control, const QString &method,
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
/// exportBundle requests a fresh local sanitized support bundle.
QJsonObject exportBundle(Control &control, QJsonObject params = {}) {
  if (!params.contains("operationKey"))
    params.insert("operationKey", uuid());
  return call(control, "diagnostics.export", params);
}
/// bundlePath requires a committed successful support-export receipt.
QString bundlePath(const QJsonObject &reply) {
  const auto receipt = reply.value("result").toObject();
  require(receipt.value("state") == "succeeded", "support export failed");
  const auto path = receipt.value("result").toObject().value("path").toString();
  require(!path.isEmpty(), "support export path missing");
  return path;
}
/// retentionOwnership prevents support retention from deleting unrelated UUID
/// JSON files, including another method's operation identity (INV-11/12).
void retentionOwnership() {
  QTemporaryDir root;
  require(root.isValid(), "retention profile unavailable");
  Control control(root.filePath("control.sqlite"));
  const auto directory = root.filePath("diagnostics");
  require(QDir().mkpath(directory), "retention directory unavailable");
  const auto other =
      call(control, "settings.update",
           {{"operationKey", uuid()},
            {"expectedRevision", control.projection().value("revision")},
            {"allPaused", false}});
  const auto otherId =
      other.value("result").toObject().value("operationId").toString();
  require(!otherId.isEmpty(), "unrelated operation reservation failed");
  QStringList foreign;
  for (int i = 0; i < 7; ++i) {
    const auto path = directory + '/' + (i ? uuid() : otherId) + ".json";
    QFile file(path);
    require(
        file.open(QIODevice::WriteOnly) &&
            file.write("unrelated-private-data") == 22 &&
            file.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-3600 - i),
                             QFileDevice::FileModificationTime),
        "unrelated retention fixture unavailable");
    foreign.append(path);
  }
  QStringList owned;
  for (int i = 0; i < 7; ++i)
    owned.append(bundlePath(exportBundle(control)));
  for (const auto &path : foreign) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) &&
                file.readAll() == "unrelated-private-data",
            "support retention deleted an unrelated UUID JSON file");
  }
  int retained = 0;
  for (const auto &path : owned)
    retained += QFileInfo::exists(path);
  require(retained == 5, "owned support retention did not keep five files");
}
/// atomicityAndRetries checks durable write failure, malformed options and
/// interrupted-receipt recovery without server or credential access
/// (INV-12/R22).
void atomicityAndRetries() {
  QTemporaryDir root;
  require(root.isValid(), "atomicity profile unavailable");
  const auto path = root.filePath("control.sqlite"), connection = uuid();
  auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
  db.setDatabaseName(path);
  QJsonObject interruptedParams{{"operationKey", uuid()}};
  QString operation;
  {
    Control control(path);
    const auto sequence = control.sequence();
    for (const auto &value : {QJsonValue(QJsonValue::Null), QJsonValue(true),
                              QJsonValue(QJsonArray{}),
                              QJsonValue(QJsonObject{{"includePaths", true}})})
      require(exportBundle(control, {{"options", value}}).contains("error") &&
                  control.sequence() == sequence,
              "invalid support options reserved a mutation");
    QFile obstruction(root.filePath("diagnostics"));
    require(obstruction.open(QIODevice::WriteOnly),
            "support write obstruction unavailable");
    obstruction.close();
    const QJsonObject params{{"operationKey", uuid()}};
    const auto failed = exportBundle(control, params);
    require(failed.value("result").toObject().value("state") == "failed" &&
                exportBundle(control, params) == failed,
            "support write failure was not durably idempotent");
    require(obstruction.remove(), "support write obstruction cleanup failed");
    require(db.open(), "atomicity fixture DB unavailable");
    QSqlQuery q(db);
    require(q.exec("CREATE TRIGGER refuse_export_receipt BEFORE UPDATE OF "
                   "receipt ON operations WHEN OLD.method='diagnostics.export' "
                   "BEGIN SELECT RAISE(ABORT,'owned receipt fault'); END"),
            "support receipt fault unavailable");
    require(exportBundle(control, interruptedParams).contains("error"),
            "uncommitted support receipt reported success");
    q.prepare("SELECT id,receipt FROM operations WHERE key=?");
    q.addBindValue(interruptedParams.value("operationKey").toString());
    require(q.exec() && q.next() &&
                QJsonDocument::fromJson(q.value(1).toByteArray())
                        .object()
                        .value("state") == "running",
            "interrupted support reservation missing");
    operation = q.value(0).toString();
    q.finish();
    require(
        QFileInfo::exists(root.filePath("diagnostics/" + operation + ".json")),
        "atomic support commit missing before receipt failure");
    require(q.exec("DROP TRIGGER refuse_export_receipt"),
            "support receipt fault cleanup failed");
    db.close();
  }
  {
    Control restart(path);
    const auto retried = exportBundle(restart, interruptedParams);
    const auto receipt = retried.value("result").toObject();
    require(
        receipt.value("operationId") == operation &&
            receipt.value("state") == "failed" &&
            receipt.value("error").toObject().value("data").toObject().value(
                "reasonCode") == "OPERATION_INTERRUPTED" &&
            QDir(root.filePath("diagnostics"))
                    .entryList({"*.json"}, QDir::Files)
                    .size() == 1,
        "restart replayed an interrupted support export");
  }
  db = {};
  QSqlDatabase::removeDatabase(connection);
}
/// activityBoundary checks malformed filters and strict decimal pagination at
/// the actual RPC boundary without introducing history or filesystem secrets.
void activityBoundary() {
  QTemporaryDir root;
  require(root.isValid(), "activity profile unavailable");
  Control control(root.filePath("control.sqlite"));
  const auto sequence = control.sequence();
  for (const auto &params :
       {QJsonObject{{"cursor", "0"}}, QJsonObject{{"cursor", "-1"}},
        QJsonObject{{"cursor", "01"}}, QJsonObject{{"cursor", "1.0"}},
        QJsonObject{{"cursor", "9223372036854775808"}},
        QJsonObject{{"cursor", 1}}, QJsonObject{{"limit", "1"}},
        QJsonObject{{"limit", 1.5}}, QJsonObject{{"limit", 0}},
        QJsonObject{{"limit", 501}},
        QJsonObject{{"folderId", QJsonValue::Null}},
        QJsonObject{{"accountId", ""}}, QJsonObject{{"unknown", true}}})
    require(call(control, "activity.list", params).contains("error"),
            "malformed activity request accepted");
  for (const auto &params :
       {QJsonObject{}, QJsonObject{{"limit", 1}},
        QJsonObject{{"limit", 500}, {"cursor", "9223372036854775807"}},
        QJsonObject{{"accountId", "' OR 1=1 --"}, {"folderId", "other"}}}) {
    const auto reply = call(control, "activity.list", params);
    require(reply.value("result").isArray() &&
                reply.value("result").toArray().isEmpty(),
            "valid empty activity filter rejected");
  }
  require(control.sequence() == sequence,
          "activity reads changed durable configuration");
}
} // namespace

/// main runs local support/history regressions without any server fixtures.
int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  int failed = 0;
  const auto run = [&](const char *name, void (*test)()) {
    try {
      test();
      std::printf("PASS %s\n", name);
    } catch (const std::exception &error) {
      ++failed;
      std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
    }
  };
  run("retention", retentionOwnership);
  run("atomicity", atomicityAndRetries);
  run("activity", activityBoundary);
  return failed ? 1 : 0;
}
