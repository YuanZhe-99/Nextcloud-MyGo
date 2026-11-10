// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>
#include <stdexcept>

#ifdef SYNCD_WITH_ENGINE
/// NetworkTestAccess reserves local-only startup records and prevents health
/// probes without changing production attachment or filesystem checks.
struct NetworkTestAccess {
  /// seed inserts one ready synthetic account and mapping without accessing a
  /// credential store, server or existing profile.
  static QString seed(Control &control, const QString &local,
                      const QString &identity, bool invalidBinding) {
    control.network_ = std::make_unique<Control::NetworkState>();
    control.network_->clock.start();
    const auto account = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto folder = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery q(control.db_);
    q.prepare("INSERT INTO accounts(id,session_id,operation_id,endpoint_id,"
              "base_url,login_name,uid,credential_ref,state) "
              "VALUES(?,?,?,?,'https://fixture.example.invalid/',?,"
              "'fixture-uid',?,'ready')");
    for (const auto &value :
         {account, account, account, account,
          invalidBinding ? QStringLiteral("") : QStringLiteral("fixture-login"),
          control.credentialReference(account)})
      q.addBindValue(value);
    if (!q.exec())
      throw std::runtime_error("folder account fixture failed");
    q.prepare("INSERT INTO folders(id,account_id,local_path,remote_path,"
              "local_identity,remote_identity,journal_path) "
              "VALUES(?,?,?,'/',?,'fixture-root',?)");
    for (const auto &value :
         {folder, account, local, identity,
          control.directory_ + "/journals/" + folder + ".sqlite"})
      q.addBindValue(value);
    if (!q.exec())
      throw std::runtime_error("folder mapping fixture failed");
    return folder;
  }
  /// pump executes the real queued restoration callback, with network probes
  /// disabled and no event-loop wait that could start a sync cycle.
  static void pump(Control &control) {
    QCoreApplication::sendPostedEvents(&control.networkTasks_,
                                       QEvent::MetaCall);
  }
  /// reason reads the durable attachment result without changing the IPC shape.
  static QString reason(const Control &control, const QString &folder) {
    QSqlQuery q(control.db_);
    q.prepare("SELECT status,reason FROM folders WHERE id=?");
    q.addBindValue(folder);
    if (!q.exec() || !q.next() || q.value(0) != "error")
      throw std::runtime_error("folder attachment error was not persisted");
    return q.value(1).toString();
  }
  /// stopped observes the fail-closed Control gate and completed idle shutdown.
  static bool stopped(const Control &control) {
    return control.storageFailed_ && control.shutdownRequested();
  }
  /// failPersistence rejects only durable error publication after the real
  /// unavailable-root exception, not attachment itself.
  static void failPersistence(Control &control) {
    QSqlQuery q(control.db_);
    if (!q.exec("CREATE TRIGGER refuse_folder_error BEFORE UPDATE OF status "
                "ON folders WHEN NEW.status='error' BEGIN SELECT "
                "RAISE(ABORT,'owned persistence fault'); END"))
      throw std::runtime_error("folder persistence fault unavailable");
  }
};
#endif

namespace {
/// require retains regression assertions in optimized contract executables.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
#ifdef SYNCD_WITH_ENGINE
/// startupReasons preserves unavailable, owned and unsupported local roots
/// rather than claiming their identity changed (INV-10/13, PLAN §12.1).
void startupReasons(const QString &kind, const QString &expected) {
  QTemporaryDir profile, local;
  require(profile.isValid() && local.isValid(), "folder fixture unavailable");
  Control control(profile.filePath("control.sqlite"));
  auto path = local.path();
  if (kind == "unavailable")
    path += "/missing";
  if (kind == "unsupported")
    path = QDir(path).rootPath();
  if (kind == "owned") {
    QFile other(local.filePath(".sync_owned.db"));
    require(other.open(QIODevice::WriteOnly),
            "foreign journal marker unavailable");
  }
  const auto folder = NetworkTestAccess::seed(
      control, path, "not-the-native-root-identity", false);
  NetworkTestAccess::pump(control);
  require(NetworkTestAccess::reason(control, folder) == expected,
          "startup attachment changed the actual stable failure reason");
  require(!NetworkTestAccess::stopped(control),
          "known local attachment failure stopped the whole daemon");
  bool negotiated = true, subscribed = false;
  const auto diagnostic =
      control
          .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                                {"id", 1},
                                {"method", "diagnostics.snapshot"}},
                    negotiated, subscribed)
          .toObject()
          .value("result")
          .toObject()
          .value("folders")
          .toArray();
  require(diagnostic.size() == 1 &&
              diagnostic.first().toObject().value("reasonCode") == expected,
          "diagnostics did not preserve the stable folder reason");
  const auto folders = control.projection()
                           .value("snapshot")
                           .toObject()
                           .value("folders")
                           .toArray();
  require(folders.size() == 1 &&
              folders.first().toObject().value("reasonCode") == expected,
          "normal projection lost the stable folder reason");
}
/// failClosed rejects unexpected engine-binding errors and failed Control
/// persistence without manufacturing a root-change explanation (INV-10).
void failClosed(bool persistence) {
  QTemporaryDir profile, local;
  require(profile.isValid() && local.isValid(),
          "folder fault fixture unavailable");
  Control control(profile.filePath("control.sqlite"));
  NetworkTestAccess::seed(
      control, persistence ? local.filePath("missing") : local.path(),
      "not-the-native-root-identity", !persistence);
  if (persistence)
    NetworkTestAccess::failPersistence(control);
  NetworkTestAccess::pump(control);
  require(NetworkTestAccess::stopped(control),
          "internal attachment/persistence failure did not close admission");
}
#endif
} // namespace

/// main executes actual local Control startup behavior without native
/// credentials, remote requests or source-text assertions.
int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
#ifndef SYNCD_WITH_ENGINE
  std::fprintf(stderr,
               "Folder error regressions require an engine-linked build\n");
  return 1;
#else
  int failures = 0;
  const auto run = [&](const char *name, const auto &test) {
    try {
      test();
      std::printf("PASS %s\n", name);
    } catch (const std::exception &error) {
      ++failures;
      std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
    }
  };
  run("unavailable",
      [] { startupReasons("unavailable", "LOCAL_ROOT_UNAVAILABLE"); });
  run("owned", [] { startupReasons("owned", "FOLDER_OWNED"); });
  run("unsupported",
      [] { startupReasons("unsupported", "UNSUPPORTED_FOLDER_MODE"); });
  run("changed", [] { startupReasons("changed", "FOLDER_ROOT_CHANGED"); });
  run("internal", [] { failClosed(false); });
  run("persistence", [] { failClosed(true); });
  return failures ? 1 : 0;
#endif
}
