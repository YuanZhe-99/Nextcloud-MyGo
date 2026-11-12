// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "namespace.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <limits>
#include <stdexcept>

/// ConstraintTestAccess drives durable account admission without server access,
/// secret injection or a production clock/configuration bypass (PLAN N18).
struct ConstraintTestAccess {
  /// throttle uses the exact typed metadata consumer, including its commit
  /// gate.
  static void throttle(Control &c, const QString &account, qint64 delay) {
    c.metadataFailed(account, MetadataError("SERVER_THROTTLED", delay));
  }
  /// remaining observes the production gate under deterministic monotonic time.
  static qint64 remaining(const Control &c, const QString &account,
                          qint64 now = -1) {
    return c.throttleRemaining(account, now);
  }
  /// restore replays the real restoration with a controllable UTC observation.
  static void restore(Control &c, qint64 utc) {
    c.restoreAccountConstraints(utc, 1000);
  }
  /// inactive proves restoration preceded all runtime/credential owners.
  static bool inactive(const Control &c) {
    bool inactive = !c.network_ && c.logins_.empty() && !c.stateAvailable_;
#ifdef SYNCD_WITH_ENGINE
    inactive = inactive && !c.runtime_;
#endif
    return inactive;
  }
  /// failed observes the same fail-closed state that prevents later admission.
  static bool failed(const Control &c) { return c.storageFailed_; }
  /// wake delivers the real network-generation notification after activation.
  static void wake(Control &c) {
    if (!c.network_) {
      c.network_ = std::make_unique<Control::NetworkState>();
      c.network_->clock.start();
    }
    c.networkChanged(true);
  }
  /// cleanup invokes credential cleanup while the persisted throttle is active.
  static void cleanup(Control &c, const QString &account,
                      const QString &session) {
    c.cleanupReauthentication(account, session);
  }
  /// retire runs the restart retry of a held revocation without its 60-second
  /// floor timer.
  static void retire(Control &c, const QString &account) {
    c.retireHeldPassword(account);
  }
};

namespace {
/// require keeps product-contract assertions enabled in optimized builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// uuid gives each disposable account, endpoint and SQL connection its own ID.
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
/// sql refuses silent fixture setup failures.
void sql(QSqlDatabase db, const QString &statement) {
  QSqlQuery q(db);
  require(q.exec(statement), "constraint fixture SQL failed");
}
/// command submits the production revision-bearing RPC envelope.
QJsonObject command(Control &c, const QString &method,
                    QJsonObject params = {}) {
  params.insert("operationKey", uuid());
  params.insert("expectedRevision", c.projection().value("revision"));
  bool negotiated = true, subscribed = false;
  return c
      .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                            {"id", 1},
                            {"method", method},
                            {"params", params}},
                negotiated, subscribed)
      .toObject();
}
/// reason reads only the stable refusal code, never server/credential content.
QString reason(const QJsonObject &reply) {
  return reply.value("error")
      .toObject()
      .value("data")
      .toObject()
      .value("reasonCode")
      .toString();
}
/// Fixture owns only a disposable Control profile and its inspection
/// connection.
struct Fixture {
  QTemporaryDir root;
  QString connection = uuid(), account = uuid(), primary = uuid(),
          alternate = uuid();
  QSqlDatabase db;
  /// Fixture reserves no credentials and contacts no real server.
  Fixture() {
    require(root.isValid(), "constraint profile unavailable");
    {
      Control create(path(), Control::StartupMode::Deferred);
    }
    db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path());
    require(db.open(), "constraint inspection connection unavailable");
    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO accounts(id,session_id,operation_id,endpoint_id,base_url,"
        "credential_ref,state,uid,root_id,login_name) "
        "VALUES(?,?,?,?,?,?,'ready',"
        "'owned-user','owned-root','owned-user')");
    for (const auto &value :
         {account, uuid(), uuid(), primary,
          QString("https://primary.example.invalid/"), uuid()})
      q.addBindValue(value);
    require(q.exec(), "constraint account reservation failed");
    q.finish();
    q.prepare("INSERT INTO endpoints(id,account_id,name,base_url,approved,state,expires,operation_id) VALUES(?,?,'Alternate',"
              "'https://alternate.example.invalid/',1,'verified','',?)");
    q.addBindValue(alternate);
    q.addBindValue(account);
    q.addBindValue(uuid());
    require(q.exec(), "constraint alternate reservation failed");
  }
  /// ~Fixture closes its SQL handle before deleting only its temporary profile.
  ~Fixture() {
    db.close();
    db = {};
    QSqlDatabase::removeDatabase(connection);
  }
  /// path identifies only this fixture's private Control database.
  QString path() const { return root.filePath("control.sqlite"); }
  /// count reads retained constraint rows without mutating product state.
  int count() const {
    QSqlQuery q(db);
    require(q.exec("SELECT COUNT(*) FROM account_constraints") && q.next(),
            "constraint count unavailable");
    return q.value(0).toInt();
  }
};

/// restart preserves finite/indefinite server deadlines, user pause and trusted
/// origin boundaries before any activation/recovery (N18, INV-03/13/14).
void restart(qint64 delay) {
  Fixture f;
  sql(f.db, "UPDATE settings SET paused=1");
  const auto pausedFolder = uuid();
  {
    QSqlQuery q(f.db);
    q.prepare(
        "INSERT INTO "
        "folders(id,account_id,local_path,remote_path,local_identity,"
        "remote_identity,journal_path,paused) VALUES(?,?,'paused-unused-root',"
        "'/paused','paused-local','paused-remote','paused-unused-journal',1)");
    q.addBindValue(pausedFolder);
    q.addBindValue(f.account);
    require(q.exec(), "paused mapping fixture failed");
  }
  {
    Control c(f.path(), Control::StartupMode::Deferred);
    ConstraintTestAccess::throttle(c, f.account, delay);
    require(f.count() == 1, "throttle was applied without durable evidence");
  }
  {
    Control c(f.path(), Control::StartupMode::Deferred);
    require(ConstraintTestAccess::inactive(c),
            "restoring throttle created a credential/network/engine owner");
    require(delay < 0
                ? ConstraintTestAccess::remaining(c, f.account) ==
                      std::numeric_limits<qint64>::max()
                : ConstraintTestAccess::remaining(c, f.account) > 0 &&
                      ConstraintTestAccess::remaining(c, f.account) <= delay,
            "restart cleared or shortened the account throttle");
    require(c.activate(), "constraint fixture activation failed");
    ConstraintTestAccess::wake(c);
    for (const auto &target : {f.primary, f.alternate}) {
      require(reason(command(c, "endpoints.verify",
                             {{"endpointId", target}})) == "SERVER_THROTTLED",
              "alternate verification bypassed restored throttle");
#ifdef SYNCD_WITH_ENGINE
      require(reason(command(c, "endpoints.requestSwitch",
                             {{"accountId", f.account},
                              {"endpointId", target}})) == "SERVER_THROTTLED",
              "manual switch bypassed restored throttle");
#endif
    }
    require(reason(command(c, "accounts.reauthenticate",
                           {{"accountId", f.account}})) == "SERVER_THROTTLED",
            "reauthentication bypassed restored throttle");
    require(c.projection()
                .value("snapshot")
                .toObject()
                .value("daemon")
                .toObject()
                .value("allPaused")
                .toBool(),
            "restoring throttle erased the user's global pause");
    require(command(c, "settings.update", {{"allPaused", false}})
                .contains("result"),
            "global resume fixture failed");
    require(ConstraintTestAccess::remaining(c, f.account) > 0,
            "global resume cleared the restored throttle");
    const auto folders =
        c.projection().value("snapshot").toObject().value("folders").toArray();
    require(folders.size() == 1 &&
                folders[0].toObject().value("userPaused").toBool(),
            "restoring throttle or global resume erased a folder's own pause");
    // A future mapping and a user's immediate sync cannot evade an account
    // deadline restored before any folder runtime existed.
    const auto futureFolder = uuid();
    {
      QSqlQuery q(f.db);
      q.prepare(
          "INSERT INTO "
          "folders(id,account_id,local_path,remote_path,local_identity,"
          "remote_identity,journal_path) VALUES(?,?,'future-unused-root',"
          "'/future','future-local','future-remote','future-unused-journal')");
      q.addBindValue(futureFolder);
      q.addBindValue(f.account);
      require(q.exec(), "future mapping fixture failed");
    }
#ifdef SYNCD_WITH_ENGINE
    require(reason(command(c, "folders.syncNow",
                           {{"folderId", futureFolder}})) == "SERVER_THROTTLED",
            "future folder/manual wake bypassed restored throttle");
    require(reason(command(c, "folders.syncNow",
                           {{"folderId", pausedFolder}})) == "PAUSED",
            "throttle changed the independent user-pause contract");
#endif
    // Installed credential cleanup remains owned and pending; no slot or
    // network need exist for a gate that is checked before either can be
    // accessed.
    QSqlQuery q(f.db);
    const auto session = uuid();
    q.prepare("INSERT INTO reauthentication(account_id,session_id,operation_id,"
              "phase,previous_login) VALUES(?,?,?,'installed','owned-user')");
    q.addBindValue(f.account);
    q.addBindValue(session);
    q.addBindValue(uuid());
    require(q.exec(), "constraint cleanup reservation failed");
    q.finish();
    ConstraintTestAccess::cleanup(c, f.account, session);
    require(q.exec("SELECT phase FROM reauthentication") && q.next() &&
                q.value(0) == "installed",
            "throttled cleanup discarded its durable credential ownership");
    q.finish();
  }
}

/// expiredRestart removes only a proven expired finite gate before activation;
/// a rejected expiry transaction keeps its row and prevents activation (N18).
void expiredRestart(bool rejectDelete) {
  Fixture f;
  {
    Control c(f.path(), Control::StartupMode::Deferred);
    ConstraintTestAccess::throttle(c, f.account, 60000);
  }
  const auto now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
  QSqlQuery q(f.db);
  q.prepare("UPDATE account_constraints SET received_utc=?,deadline_utc=?");
  q.addBindValue(now - 61000);
  q.addBindValue(now - 1000);
  require(q.exec(), "expired deadline fixture failed");
  q.finish();
  if (rejectDelete)
    sql(f.db,
        "CREATE TRIGGER reject_expiry BEFORE DELETE ON account_constraints "
        "BEGIN SELECT RAISE(ABORT,'fixture expiry refusal'); END");
  bool failed = false;
  try {
    Control c(f.path(), Control::StartupMode::Deferred);
    require(ConstraintTestAccess::inactive(c) &&
                ConstraintTestAccess::remaining(c, f.account) == 0,
            "expired restart started owners or retained a finite expired gate");
  } catch (const std::exception &) {
    failed = true;
  }
  require(failed == rejectDelete && f.count() == (rejectDelete ? 1 : 0),
          "failed expiry transaction erased evidence or admitted runtime");
}

/// clockRecovery uses deterministic UTC observations; rollback retains the full
/// original delay, while a proven expiry removes only the constraint row (N18).
void clockRecovery() {
  Fixture f;
  Control c(f.path(), Control::StartupMode::Deferred);
  ConstraintTestAccess::throttle(c, f.account, 120000);
  QSqlQuery q(f.db);
  require(q.exec("SELECT received_utc FROM account_constraints") && q.next(),
          "recorded UTC missing");
  const auto received = q.value(0).toLongLong();
  q.finish();
  ConstraintTestAccess::restore(c, received - 5000);
  require(ConstraintTestAccess::remaining(c, f.account, 1000) == 120000,
          "UTC rollback shortened the restored server deadline");
  ConstraintTestAccess::restore(c, received + 119000);
  require(ConstraintTestAccess::remaining(c, f.account, 1000) == 1000,
          "finite restoration did not retain the unelapsed interval");
  ConstraintTestAccess::restore(c, received + 120000);
  require(ConstraintTestAccess::remaining(c, f.account, 1000) == 0 &&
              f.count() == 0,
          "proven expiry did not release the durable constraint");
  ConstraintTestAccess::throttle(c, f.account, -1);
  ConstraintTestAccess::restore(c, received + 1000000000);
  ConstraintTestAccess::throttle(c, f.account, 60000);
  require(ConstraintTestAccess::remaining(c, f.account) ==
                  std::numeric_limits<qint64>::max() &&
              f.count() == 1,
          "indefinite deadline was erased or shortened after restart/response");
}

/// writeFailure fails admission rather than reporting fake busy when the real
/// SQLite write is rejected; earlier durable evidence survives (INV-10, N18).
void writeFailure(bool existing) {
  Fixture f;
  Control c(f.path(), Control::StartupMode::Deferred);
  if (existing)
    ConstraintTestAccess::throttle(c, f.account, 120000);
  sql(f.db,
      "CREATE TRIGGER reject_constraint BEFORE INSERT ON account_constraints "
      "BEGIN SELECT RAISE(ABORT,'fixture write refusal'); END");
  bool refused = false;
  try {
    ConstraintTestAccess::throttle(c, f.account, 240000);
  } catch (const std::exception &error) {
    refused = QString::fromUtf8(error.what()) == "INTERNAL_ERROR";
  }
  require(refused && ConstraintTestAccess::failed(c) && c.shutdownRequested() &&
              f.count() == (existing ? 1 : 0),
          "failed durable throttle write acknowledged busy or lost evidence");
}

/// HeldLogin turns the fixture account into a login the previous process left
/// interrupted with an acquired password in its vault slot.
struct HeldLogin {
  Fixture f;
  QString reference, operation = uuid();
  /// HeldLogin writes only this fixture's own vault slot and rows.
  explicit HeldLogin(bool recorded, bool credential = true) {
    reference =
        "Nextcloud-MyGo/" +
        QString::fromLatin1(QCryptographicHash::hash(
                                QFileInfo(f.path()).absolutePath().toUtf8(),
                                QCryptographicHash::Sha256)
                                .toHex()
                                .left(24)) +
        '/' + f.account + "/app-password";
    // A login that never became ready has no approved alternate address.
    sql(f.db, "DELETE FROM endpoints");
    QSqlQuery q(f.db);
    q.prepare("UPDATE accounts SET state='creating',credential_ref=?,"
              "operation_id=? WHERE id=?");
    q.addBindValue(reference);
    q.addBindValue(operation);
    q.addBindValue(f.account);
    require(q.exec(), "held login reservation failed");
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,x'00',?,'accounts.login.start')");
    q.addBindValue(uuid());
    q.addBindValue(operation);
    q.addBindValue(QJsonDocument(QJsonObject{{"operationId", operation},
                                             {"state", "succeeded"},
                                             {"result", QJsonObject{}}})
                       .toJson(QJsonDocument::Compact));
    require(q.exec(), "held login receipt failed");
    if (recorded)
      sql(f.db, "INSERT INTO held_revocations VALUES('" + f.account + "')");
    if (credential)
      writeCredential(reference, "owned-user", "owned-held-secret");
  }
  /// ~HeldLogin never leaves the fixture secret in the native vault.
  ~HeldLogin() {
    try {
      removeCredential(reference);
    } catch (...) {
      std::fputs("Owned held credential cleanup failed\n", stderr);
      std::abort();
    }
  }
  /// rows counts the interrupted account and its held record.
  int rows(const char *table) const {
    QSqlQuery q(f.db);
    require(q.exec(QString("SELECT COUNT(*) FROM %1").arg(table)) && q.next(),
            "held login count unavailable");
    return q.value(0).toInt();
  }
  /// outcome reads the login receipt's stable reason.
  QString outcome(Control &c) const {
    return reason(command(c, "operations.get", {{"operationId", operation}})
                      .value("result")
                      .toObject());
  }
};

/// heldRevocation: a login interrupted while the server delayed the DELETE of
/// its acquired password keeps its slot and reservation across a restart,
/// sends nothing while the restored delay runs, sends at most one DELETE, and
/// calls only an unconfirmed outcome unconfirmed (N18, INV-13).
void heldRevocation() {
  {
    // Recorded as held, bounded throttle still running after the restart.
    HeldLogin h(true);
    {
      Control seed(h.f.path(), Control::StartupMode::Deferred);
      ConstraintTestAccess::throttle(seed, h.f.account, 120000);
    }
    Control c(h.f.path());
    require(findCredential(h.reference) && h.rows("accounts") == 1 &&
                h.rows("held_revocations") == 1 &&
                h.outcome(c) == "LOGIN_SESSION_EXPIRED",
            "restart released a held revocation or misreported its login");
    ConstraintTestAccess::retire(c, h.f.account);
    require(findCredential(h.reference) && h.rows("held_revocations") == 1,
            "held revocation was sent inside the restored server delay");
  }
  {
    // A deadline with no automatic retry ends as unconfirmed, without sending.
    HeldLogin h(true);
    {
      Control seed(h.f.path(), Control::StartupMode::Deferred);
      ConstraintTestAccess::throttle(seed, h.f.account, -1);
    }
    Control c(h.f.path());
    ConstraintTestAccess::retire(c, h.f.account);
    require(!findCredential(h.reference) && h.rows("accounts") == 0 &&
                h.outcome(c) == "APP_PASSWORD_REVOCATION_UNCONFIRMED",
            "indefinite server delay was retried or reported as revoked");
  }
  {
    // Delay over: the record is cleared before the one DELETE leaves, and an
    // unanswered DELETE (unresolvable host) is unconfirmed, never repeated.
    HeldLogin h(true);
    Control c(h.f.path());
    ConstraintTestAccess::retire(c, h.f.account);
    require(h.rows("held_revocations") == 0 && h.rows("accounts") == 1,
            "held record outlived the sent DELETE or its outcome was assumed");
    ConstraintTestAccess::retire(c, h.f.account); // no record: sends nothing
    QEventLoop loop;
    QTimer poll, deadline;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
      if (h.rows("accounts") == 0)
        loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(20);
    deadline.start(45000);
    loop.exec();
    require(!findCredential(h.reference) && h.rows("accounts") == 0 &&
                h.outcome(c) == "APP_PASSWORD_REVOCATION_UNCONFIRMED",
            "unanswered restart revocation was hidden");
  }
  {
    // Not recorded as held (a DELETE may have been in flight): never sent
    // again, exactly as before.
    HeldLogin h(false);
    Control c(h.f.path());
    require(!findCredential(h.reference) && h.rows("accounts") == 0 &&
                h.outcome(c) == "APP_PASSWORD_REVOCATION_UNCONFIRMED",
            "unrecorded interrupted login was treated as held");
  }
  // A record without a password has nothing to revoke.
  HeldLogin h(true, false);
  Control c(h.f.path());
  require(h.rows("accounts") == 0 && h.outcome(c) == "LOGIN_SESSION_EXPIRED",
          "empty held record kept an interrupted login alive");
}

/// migrateSeven preserves schema-7 committed state and a read-only prior-schema
/// backup while adding only the tables later schemas introduced (INV-08/10).
void migrateSeven() {
  Fixture f;
  sql(f.db, "DROP TABLE account_constraints");
  sql(f.db, "DROP TABLE held_revocations");
  sql(f.db, "PRAGMA user_version=7");
  sql(f.db, "UPDATE settings SET paused=1");
  {
    Control c(f.path(), Control::StartupMode::Deferred);
    require(ConstraintTestAccess::inactive(c),
            "migration started runtime owners");
  }
  QSqlQuery q(f.db);
  require(q.exec("PRAGMA user_version") && q.next() && q.value(0).toInt() == 11,
          "schema-7 migration did not reach schema 11");
  q.finish();
  // Schema 10: existing mappings keep full-file mode (§2.3).
  require(q.exec("SELECT COUNT(*) FROM pragma_table_info('folders') WHERE "
                 "name='mode' AND dflt_value='''full'''") &&
              q.next() && q.value(0).toInt() == 1,
          "schema migration did not give folders a full-file mode");
  q.finish();
  require(q.exec("SELECT COUNT(*) FROM held_revocations") && q.next() &&
              q.value(0).toInt() == 0,
          "schema migration omitted or filled the held-revocation table");
  q.finish();
  require(q.exec("SELECT paused FROM settings") && q.next() &&
              q.value(0).toBool(),
          "schema migration erased user pause");
  q.finish();
  require(f.count() == 0 &&
              QDir(f.root.path())
                      .entryList({"*.pre-schema-11.*"}, QDir::Files)
                      .size() == 1,
          "schema-7 migration omitted its validated backup or invented "
          "constraints");
}
} // namespace

/// main runs isolated durable admission tests without real-server requests.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    restart(120000);
    restart(-1);
    clockRecovery();
    expiredRestart(false);
    expiredRestart(true);
    writeFailure(false);
    writeFailure(true);
    heldRevocation();
    migrateSeven();
    std::puts("PASS durable account throttle, restart/rollback, origin gates, "
              "cleanup ownership, held revocation across restart, write "
              "failures and schema-7 migration");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
