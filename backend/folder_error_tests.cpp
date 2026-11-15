// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "root.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#ifdef SYNCD_WITH_ENGINE
#include "platform/windows/ondemand.h"
#include <QScopeGuard>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

#include <cfapi.h>
#endif

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
  /// db exposes the fixture's Control connection for setup and checks.
  static QSqlDatabase db(Control &control) { return control.db_; }
  /// quiet prevents health probes of a Control reopened on a seeded profile.
  static void quiet(Control &control) {
    control.network_ = std::make_unique<Control::NetworkState>();
    control.network_->clock.start();
  }
  /// credential names the seeded account's owned vault reference.
  static QString credential(const Control &control, const QString &account) {
    return control.credentialReference(account);
  }
  /// report returns the start's self-check verdict and its reasons.
  static QJsonObject report(const Control &control) {
    return control.startupReport();
  }
  /// addFolder maps one more existing local root of the same account.
  static QString addFolder(Control &control, const QString &account,
                           const QString &local) {
    const auto folder = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery q(control.db_);
    q.prepare("INSERT INTO folders(id,account_id,local_path,remote_path,"
              "local_identity,remote_identity,journal_path) "
              "VALUES(?,?,?,'/present',?,'fixture-present',?)");
    for (const auto &value :
         {folder, account, QDir::cleanPath(local),
          RootLease(QDir::cleanPath(local)).identity(),
          control.directory_ + "/journals/" + folder + ".sqlite"})
      q.addBindValue(value);
    if (!q.exec())
      throw std::runtime_error("second mapping fixture failed");
    return folder;
  }
  /// interruptSwitch records a switch a previous process left at 'blocked'
  /// with an interrupted receipt, as startup records it.
  static void interruptSwitch(Control &control, const QString &account) {
    const auto operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery q(control.db_);
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,'',?,'endpoints.requestSwitch')");
    q.addBindValue(operation);
    q.addBindValue(operation);
    q.addBindValue(QByteArray(
        "{\"operationId\":\"" + operation.toUtf8() +
        "\",\"state\":\"failed\",\"error\":{\"code\":-32000,\"data\":"
        "{\"reasonCode\":\"OPERATION_INTERRUPTED\"}}}"));
    if (!q.exec())
      throw std::runtime_error("interrupted receipt fixture failed");
    q.prepare("INSERT INTO switch_intents VALUES(?,?,?,1,'blocked','[]','[]')");
    q.addBindValue(account);
    q.addBindValue(operation);
    q.addBindValue(account);
    if (!q.exec())
      throw std::runtime_error("interrupted switch fixture failed");
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
  const auto report = NetworkTestAccess::report(control);
  require(report.value("selfCheck") == "failed" &&
              report.value("reasonCodes").toArray().contains(expected),
          "the self-check passed a mapping that did not attach");
}
/// previousCycle reopens a profile whose last cycle left its folder in error:
/// CANCELLED (stopped at shutdown) clears to offline, a server failure stays
/// on the folder, and neither fails the self-check of a start whose
/// database, credential and attachment are fine (PLAN §16.2; B14 step 8).
/// Side effects: one fixture secret in the current user's vault under the
/// seeded account's own reference, removed again even on failure.
/// Notes: regression for "self-check failed CANCELLED" after a reinstall.
void previousCycle(const QString &left, const QString &status) {
  QTemporaryDir profile, local;
  require(profile.isValid() && local.isValid(), "reopen fixture unavailable");
  const auto root = QDir::cleanPath(local.path());
  QString folder, reference;
  {
    Control first(profile.filePath("control.sqlite"));
    folder = NetworkTestAccess::seed(first, root, RootLease(root).identity(),
                                     false);
    QSqlQuery q(NetworkTestAccess::db(first));
    require(q.exec("SELECT account_id FROM folders") && q.next(),
            "reopen fixture account missing");
    reference = NetworkTestAccess::credential(first, q.value(0).toString());
    q.finish();
    q.prepare("UPDATE folders SET status='error',reason=?");
    q.addBindValue(left);
    require(q.exec(), "previous cycle fixture failed");
  }
  // The engine adapter needs the account's credential to attach.
  writeCredential(reference, "fixture-login", "fixture-only-secret");
  const std::unique_ptr<const QString, void (*)(const QString *)> vault(
      &reference, [](const QString *owned) { removeCredential(*owned); });
  Control second(profile.filePath("control.sqlite"));
  NetworkTestAccess::quiet(second);
  NetworkTestAccess::pump(second);
  require(!NetworkTestAccess::stopped(second), "reopening stopped SyncD");
  QSqlQuery q(NetworkTestAccess::db(second));
  q.prepare("SELECT status,reason FROM folders WHERE id=?");
  q.addBindValue(folder);
  require(q.exec() && q.next() && q.value(0) == status,
          "the previous cycle's folder state was not handled as expected");
  const auto report = NetworkTestAccess::report(second);
  require(report.value("selfCheck") == "passed",
          "the previous process's cycle result failed this start's self-check");
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
/// inspectMissing accepts a folder that does not exist yet when its parent
/// does, and refuses one whose parent is missing (protocol 1.9).
void inspectMissing() {
  QTemporaryDir profile, local;
  require(profile.isValid() && local.isValid(), "inspect fixture unavailable");
  Control control(profile.filePath("control.sqlite"));
  NetworkTestAccess::seed(control, local.filePath("mapped"), "unused", false);
  QString account;
  {
    QSqlQuery q(NetworkTestAccess::db(control));
    require(q.exec("SELECT id FROM accounts") && q.next(),
            "inspect fixture account missing");
    account = q.value(0).toString();
  }
  bool negotiated = true, subscribed = false;
  const auto reason = [&](const QString &path) {
    return control
        .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                              {"id", 1},
                              {"method", "folders.inspect"},
                              {"params", QJsonObject{{"accountId", account},
                                                     {"remotePath", "/New"},
                                                     {"localPath", path}}}},
                  negotiated, subscribed)
        .toObject()
        .value("error")
        .toObject()
        .value("data")
        .toObject()
        .value("reasonCode")
        .toString();
  };
  require(reason(local.filePath("absent/new")) == "LOCAL_ROOT_UNAVAILABLE",
          "a folder whose parent is missing passed inspection");
  require(reason(local.filePath("new")) != "LOCAL_ROOT_UNAVAILABLE" &&
              !QFileInfo::exists(local.filePath("new")),
          "a not-yet-existing folder was refused or created by inspection");
}
/// removalWhileStuck removes an account whose interrupted switch cannot finish
/// because one mapped root is missing: engines stop, mappings and the switch
/// go, local files and engine journals stay (INV-01/05, PLAN §12.1).
/// Notes: regression for removal refused with QUIESCE_STALLED for good.
void removalWhileStuck() {
  QTemporaryDir profile, local;
  require(profile.isValid() && local.isValid(), "removal fixture unavailable");
  Control control(profile.filePath("control.sqlite"));
  const auto missing =
      NetworkTestAccess::seed(control, local.filePath("missing"),
                              "not-the-native-root-identity", false);
  QFile kept(local.filePath("present/kept.txt"));
  require(QDir().mkpath(local.filePath("present")) &&
              kept.open(QIODevice::WriteOnly) && kept.write("owned") == 5,
          "removal fixture file unavailable");
  kept.close();
  QString account;
  {
    QSqlQuery q(NetworkTestAccess::db(control));
    require(q.exec("SELECT account_id FROM folders") && q.next(),
            "removal fixture account missing");
    account = q.value(0).toString();
  }
  const auto present =
      NetworkTestAccess::addFolder(control, account, local.filePath("present"));
  NetworkTestAccess::interruptSwitch(control, account);
  NetworkTestAccess::pump(control);
  require(NetworkTestAccess::reason(control, missing) ==
              "LOCAL_ROOT_UNAVAILABLE",
          "missing root fixture did not stay blocked");
  QFile journal(profile.filePath("journals/" + present + ".sqlite"));
  require(QDir().mkpath(profile.filePath("journals")) &&
              (journal.exists() || journal.open(QIODevice::WriteOnly)),
          "removal journal fixture unavailable");
  journal.close();
  bool negotiated = true, subscribed = false;
  const auto call = [&](const QString &method, const QJsonObject &params) {
    auto withKey = params;
    withKey.insert("operationKey",
                   QUuid::createUuid().toString(QUuid::WithoutBraces));
    withKey.insert("expectedRevision", control.projection().value("revision"));
    return control
        .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                              {"id", 1},
                              {"method", method},
                              {"params", withKey}},
                  negotiated, subscribed)
        .toObject();
  };
  const auto accepted = call("accounts.remove", {{"accountId", account}});
  const auto operation =
      accepted.value("result").toObject().value("operationId").toString();
  require(!operation.isEmpty(), "removal of a stuck account was refused");
  QJsonObject receipt;
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < 30000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    receipt = call("operations.get", {{"operationId", operation}})
                  .value("result")
                  .toObject();
    if (receipt.value("state") != "running")
      break;
  }
  require(receipt.value("state") == "succeeded",
          "removal of a stuck account did not finish");
  require(!NetworkTestAccess::stopped(control), "removal stopped the daemon");
  QSqlQuery q(NetworkTestAccess::db(control));
  for (const auto &table : {"accounts", "folders", "switch_intents"})
    require(q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table)) &&
                q.next() && q.value(0).toInt() == 0,
            "removal left account, mapping or switch state behind");
  q.finish();
  require(q.exec("SELECT receipt FROM operations WHERE method="
                 "'endpoints.requestSwitch'") &&
              q.next() && q.value(0).toByteArray().contains("\"failed\""),
          "the interrupted switch receipt changed");
  require(QFile::exists(local.filePath("present/kept.txt")) && journal.exists(),
          "removal touched local files or an engine journal");
}
/// availabilityChanges drives folders.setAvailability (protocol 1.12) on real
/// Cloud Files through a running Control: a full-file folder refuses any
/// value but onDemand, then converts in place on request (its own
/// registration, conversion 'converted'); its root's pin then follows
/// alwaysLocal, onlineOnly and onDemand as the snapshot shows, and an unknown
/// value is refused.
/// Side effects: one fixture secret in the current user's vault and a sync
/// root of its own, both removed again, also on failure.
void availabilityChanges() {
  if (!OnDemandRoot::available()) {
    std::printf("SKIP availability: Cloud Files unavailable here\n");
    return;
  }
  QTemporaryDir profile, local;
  require(profile.isValid() && local.isValid(),
          "availability fixture missing");
  const auto root = QDir::cleanPath(local.path());
  QString folder, account, reference;
  {
    Control first(profile.filePath("control.sqlite"));
    folder = NetworkTestAccess::seed(first, root, RootLease(root).identity(),
                                     false);
    QSqlQuery q(NetworkTestAccess::db(first));
    require(q.exec("SELECT account_id FROM folders") && q.next(),
            "availability account missing");
    account = q.value(0).toString();
    reference = NetworkTestAccess::credential(first, account);
  }
  const OnDemandRoot::Identity identity{account, folder, {}};
  writeCredential(reference, "fixture-login", "fixture-only-secret");
  const auto cleanup = qScopeGuard([&] {
    OnDemandRoot::unregister(root, identity);
    removeCredential(reference);
  });
  Control control(profile.filePath("control.sqlite"));
  NetworkTestAccess::quiet(control);
  NetworkTestAccess::pump(control);
  bool negotiated = true, subscribed = false;
  // call answers one mutation with the reason it was refused, or "".
  const auto call = [&](const QString &availability) {
    const auto reply =
        control
            .dispatch(
                QJsonObject{
                    {"jsonrpc", "2.0"},
                    {"id", 1},
                    {"method", "folders.setAvailability"},
                    {"params",
                     QJsonObject{{"operationKey", QUuid::createUuid().toString(
                                                      QUuid::WithoutBraces)},
                                 {"expectedRevision",
                                  control.projection().value("revision")},
                                 {"folderId", folder},
                                 {"availability", availability}}}},
                negotiated, subscribed)
            .toObject();
    NetworkTestAccess::pump(control);
    return reply.value("error")
        .toObject()
        .value("data")
        .toObject()
        .value("reasonCode")
        .toString();
  };
  const auto shown = [&] {
    return control.projection()
        .value("snapshot")
        .toObject()
        .value("folders")
        .toArray()
        .first()
        .toObject();
  };
  require(shown().value("mode") == "full" &&
              shown().value("availability") == "",
          "the fixture folder does not start with full files");
  for (const auto *refused : {"alwaysLocal", "onlineOnly"})
    require(call(refused) == "INVALID_PARAMS",
            "a full-file folder took an on-demand availability");
  require(call("onDemand").isEmpty(), "converting on request was refused");
  require(shown().value("mode") == "onDemand" &&
              shown().value("conversion") == "converted" &&
              OnDemandRoot::ownershipError(root, identity).isEmpty(),
          "the folder did not convert under its own registration");
  for (const auto *availability : {"alwaysLocal", "onlineOnly", "onDemand"}) {
    require(call(availability).isEmpty(), "an availability was refused");
    require(shown().value("availability") == availability,
            "the snapshot does not show the root's new pin");
  }
  require(call("sideways") == "INVALID_PARAMS",
          "an unknown availability was accepted");
  require(!NetworkTestAccess::stopped(control), "availability stopped SyncD");
}
/// conversionStates runs the v0.1.0 full -> on-demand conversion through real
/// Control starts on real Cloud Files (schema 12, protocol 1.11): a folder
/// added as full (conversion '') is never converted; a pending one inside
/// another provider's root keeps full files with FOLDER_OWNED, keeps it on a
/// retry and converts once the root is gone; an eligible or interrupted one
/// ends onDemand/'converted' under its own registration; one whose pass fails
/// after registering (an unreadable journal) is released and back to full
/// with the reason, its journal untouched; a converted one stays.
/// Side effects: one fixture secret per case in the current user's vault and
/// sync roots of its own, all removed again, also on failure.
void conversionStates(const QString &name) {
  if (!OnDemandRoot::available()) {
    std::printf("SKIP conversion %s: Cloud Files unavailable here\n",
                qPrintable(name));
    return;
  }
  const struct Case {
    QString name, mode, conversion;
    bool foreign, unreadable;
    QString wantMode, wantConversion, wantReason;
  } cases[] = {
      {"added as full", "full", "", false, false, "full", "", ""},
      {"pending inside another root", "full", "pending", true, false, "full",
       "pending", "FOLDER_OWNED"},
      {"pending", "full", "pending", false, false, "onDemand", "converted", ""},
      {"interrupted pass", "onDemand", "converting", false, false, "onDemand",
       "converted", ""},
      {"pass fails", "full", "pending", false, true, "full", "pending",
       "JOURNAL_ERROR"},
      {"converted", "onDemand", "converted", false, false, "onDemand",
       "converted", ""},
  };
  const auto c = *std::find_if(std::begin(cases), std::end(cases),
                               [&](const Case &c) { return c.name == name; });
  QTemporaryDir profile, local;
  require(profile.isValid() && local.isValid(), "conversion fixture missing");
  const auto outer = QDir::cleanPath(local.path());
  const auto root = c.foreign ? outer + "/inner" : outer;
  require(QDir().mkpath(root), "conversion root missing");
  QString folder, account, reference;
  {
    Control first(profile.filePath("control.sqlite"));
    folder = NetworkTestAccess::seed(first, root, RootLease(root).identity(),
                                     false);
    QSqlQuery q(NetworkTestAccess::db(first));
    q.prepare("UPDATE folders SET mode=?,conversion=? WHERE id=?");
    q.addBindValue(c.mode);
    q.addBindValue(c.conversion);
    q.addBindValue(folder);
    require(q.exec(), "conversion row fixture failed");
    require(q.exec("SELECT account_id FROM folders") && q.next(),
            "conversion account missing");
    account = q.value(0).toString();
    reference = NetworkTestAccess::credential(first, account);
  }
  const OnDemandRoot::Identity identity{account, folder, {}},
      other{account, QUuid::createUuid().toString(QUuid::WithoutBraces), {}};
  writeCredential(reference, "fixture-login", "fixture-only-secret");
  const auto journalPath = profile.filePath("journals/" + folder + ".sqlite");
  const QByteArray unreadable(4096, 'x');
  if (c.unreadable) {
    QFile journal(journalPath);
    require(QDir().mkpath(profile.filePath("journals")) &&
                journal.open(QIODevice::WriteOnly) &&
                journal.write(unreadable) == unreadable.size(),
            "unreadable journal fixture failed");
  }
  const auto native = QDir::toNativeSeparators(outer).toStdWString();
  bool foreign = false;
  const auto cleanup = qScopeGuard([&] {
    if (foreign)
      CfUnregisterSyncRoot(native.c_str());
    OnDemandRoot::unregister(root, identity);
    removeCredential(reference);
  });
  if (c.foreign) {
    const std::wstring provider = L"SyncD-Contract-Foreign", version = L"1";
    CF_SYNC_REGISTRATION registration{};
    registration.StructSize = sizeof(registration);
    registration.ProviderName = provider.c_str();
    registration.ProviderVersion = version.c_str();
    CF_SYNC_POLICIES policies{};
    policies.StructSize = sizeof(policies);
    policies.Hydration.Primary = CF_HYDRATION_POLICY_FULL;
    policies.Population.Primary = CF_POPULATION_POLICY_ALWAYS_FULL;
    require(SUCCEEDED(CfRegisterSyncRoot(native.c_str(), &registration,
                                         &policies, CF_REGISTER_FLAG_NONE)),
            "foreign fixture root not registered");
    foreign = true;
  }
  struct Row {
    QString mode, conversion, reason, shownConversion, shownReason;
    qint64 revision = 0;
  };
  // start runs one SyncD start up to its folder attachment.
  const auto start = [&] {
    Control control(profile.filePath("control.sqlite"));
    NetworkTestAccess::quiet(control);
    NetworkTestAccess::pump(control);
    require(!NetworkTestAccess::stopped(control), "conversion stopped SyncD");
    Row row;
    QSqlQuery q(NetworkTestAccess::db(control));
    q.prepare("SELECT mode,conversion,conversion_reason FROM folders WHERE "
              "id=?");
    q.addBindValue(folder);
    require(q.exec() && q.next(), "conversion row missing");
    row.mode = q.value(0).toString();
    row.conversion = q.value(1).toString();
    row.reason = q.value(2).toString();
    const auto projection = control.projection();
    row.revision = projection.value("revision").toString().toLongLong();
    const auto shown = projection.value("snapshot")
                           .toObject()
                           .value("folders")
                           .toArray()
                           .first()
                           .toObject();
    row.shownConversion = shown.value("conversion").toString();
    row.shownReason = shown.value("conversionReason").toString();
    require(shown.value("mode").toString() == row.mode,
            "the snapshot's mode differs from the database");
    return row;
  };
  const auto expect = [&](const Row &row, const QString &mode,
                          const QString &conversion, const QString &reason) {
    if (row.mode != mode || row.conversion != conversion ||
        row.reason != reason)
      std::fprintf(stderr, "  got %s/%s/%s\n", qPrintable(row.mode),
                   qPrintable(row.conversion), qPrintable(row.reason));
    require(row.mode == mode && row.conversion == conversion &&
                row.reason == reason,
            "the conversion did not end in the expected state");
    require(row.shownConversion == conversion &&
                row.shownReason == (conversion == "pending" ? reason : ""),
            "the snapshot does not show the conversion as protocol 1.11 says");
    // Registered as this folder's sync root exactly when it is on demand.
    const bool registered =
        OnDemandRoot::ownershipError(root, identity).isEmpty() &&
        OnDemandRoot::ownershipError(root, other) == "FOLDER_OWNED";
    require(c.foreign || registered == (mode == "onDemand"),
            "the registration does not match the folder's mode");
  };
  auto row = start();
  expect(row, c.wantMode, c.wantConversion, c.wantReason);
  if (c.unreadable) {
    QFile journal(journalPath);
    require(journal.open(QIODevice::ReadOnly) &&
                journal.readAll() == unreadable,
            "a failed conversion changed the journal");
  }
  // A retry with the same answer writes nothing; the next start after the
  // obstacle is gone converts.
  const auto again = start();
  expect(again, c.wantMode, c.wantConversion, c.wantReason);
  if (c.foreign || c.wantConversion != "pending")
    require(again.revision == row.revision,
            "a start that changed nothing wrote the folder again");
  if (c.foreign) {
    require(SUCCEEDED(CfUnregisterSyncRoot(native.c_str())),
            "foreign fixture root not removed");
    foreign = false;
    expect(start(), "onDemand", "converted", "");
  }
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
  run("cancelled at shutdown",
      [] { previousCycle("CANCELLED", "offline"); });
  run("server failure before restart",
      [] { previousCycle("SYNC_FAILED", "error"); });
  run("internal", [] { failClosed(false); });
  run("persistence", [] { failClosed(true); });
  run("removal while stuck", [] { removalWhileStuck(); });
  run("inspect a folder that does not exist yet", [] { inspectMissing(); });
  for (const auto *name :
       {"added as full", "pending inside another root", "pending",
        "interrupted pass", "pass fails", "converted"})
    run(qPrintable(QStringLiteral("conversion: %1").arg(name)),
        [name] { conversionStates(name); });
  run("availability", [] { availabilityChanges(); });
  return failures ? 1 : 0;
#endif
}
