#include "control.h"
#include "startup.h"
#include "switch_state.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopedValueRollback>
#include <QSqlQuery>
#include <QUuid>
#include <cmath>
#include <stdexcept>

namespace {
/// failure constructs a stable machine-readable error without database or
/// credential details.
QJsonObject failure(const QString &reason, int code = -32000) {
  return {{"code", code},
          {"message", "Request could not be completed"},
          {"data", QJsonObject{{"reasonCode", reason}}}};
}
/// execute requires SQL success without exposing database paths in RPC
/// failures.
void execute(QSqlQuery &q) {
  if (!q.exec())
    throw std::runtime_error("Control database operation failed");
}
/// addColumn adds one later-schema column unless the table already has it
/// (schema 10: folders.mode; schema 11: endpoints.kind, decisions.groups,
/// settings.daemon_version).
/// Returns: false on a SQL failure, which aborts the migration.
bool addColumn(QSqlQuery &q, const QString &table, const QString &column,
               const QString &definition) {
  if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('%1') "
                             "WHERE name='%2'")
                  .arg(table, column)) ||
      !q.next())
    return false;
  const bool present = q.value(0).toInt() > 0;
  q.finish();
  return present || q.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                               .arg(table, column, definition));
}
/// capabilities advertises only end-to-end implemented workflows.
/// Notes: `engine` separates a build linked to the upstream engine (folders,
/// switching) from a foundation build (pipe, login, settings only).
QJsonObject capabilities(bool available = true) {
#ifdef SYNCD_WITH_ENGINE
  constexpr bool engine = true;
#else
  constexpr bool engine = false;
#endif
  // multiAccount: two accounts with two folders each passed on a real server
  // (multi-account acceptance, 2026-11-12). endpointAutoSwitch: the
  // automatic policy's cases passed on real runs (switching acceptance,
  // 2026-11-12). fullSync: the basic-sync, file-safety and soak cases passed
  // on real runs and the installed interface showed a real cycle's activity
  // (capability evidence, 2026-11-13). Both need the engine.
  return {{"multiAccount", available},
          {"login", available},
          {"fullSync", engine && available},
          {"endpointAutoSwitch", engine && available},
          {"virtualFiles", false},
          {"endToEndEncryption", false},
          {"engine", engine && available}};
}
} // namespace

Control::Control(const QString &path, StartupMode mode, Startup *startup)
    : startup_(startup),
      boot_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
  directory_ = QFileInfo(path).absolutePath();
  profileKey_ = QString::fromLatin1(
      QCryptographicHash::hash(QFileInfo(path).absolutePath().toUtf8(),
                               QCryptographicHash::Sha256)
          .toHex()
          .left(24));
  activationAllowed_ = mode == StartupMode::Immediate ||
                       mode == StartupMode::Deferred ||
                       mode == StartupMode::Backoff;
  publishActivation_ = mode != StartupMode::Immediate;
  startupState_ = mode == StartupMode::Diagnostic ? "diagnostic"
                  : mode == StartupMode::Stopped  ? "stopped"
                  : mode == StartupMode::DatabaseUnavailable
                      ? "databaseUnavailable"
                      : "backoff";
  if (!activationAllowed_)
    return;
  try {
    backupControlBeforeMigration(path, schemaVersion);
    db_ = QSqlDatabase::addDatabase("QSQLITE", boot_);
    db_.setDatabaseName(path);
    if (!db_.open())
      throw std::runtime_error("Cannot open Control database");
    QSqlQuery q(db_);
    for (const auto &sql :
         {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL",
          "PRAGMA foreign_keys=ON", "PRAGMA busy_timeout=5000"}) {
      if (!q.exec(sql))
        throw std::runtime_error("Cannot configure Control database");
    }
    if (!q.exec("PRAGMA user_version") || !q.next() ||
        q.value(0).toInt() > schemaVersion)
      throw std::runtime_error("Unsupported Control database version");
    const auto schema = q.value(0).toInt();
    q.finish();
    if (!db_.transaction())
      throw std::runtime_error("Cannot begin migration");
    for (
        const auto &sql :
        {"CREATE TABLE IF NOT EXISTS settings(id INTEGER PRIMARY KEY "
         "CHECK(id=1), revision INTEGER NOT NULL, seq INTEGER NOT NULL, paused "
         "INTEGER NOT NULL)",
         "INSERT OR IGNORE INTO settings(id,revision,seq,paused) "
         "VALUES(1,0,0,0)",
         "CREATE TABLE IF NOT EXISTS operations(key TEXT PRIMARY KEY, id TEXT "
         "UNIQUE NOT NULL, digest BLOB NOT NULL, receipt BLOB NOT NULL)",
         "CREATE TABLE IF NOT EXISTS outbox(seq INTEGER PRIMARY KEY, payload "
         "BLOB NOT NULL)",
         "CREATE TABLE IF NOT EXISTS accounts(id TEXT PRIMARY KEY, session_id "
         "TEXT UNIQUE NOT NULL, operation_id TEXT NOT NULL, endpoint_id TEXT "
         "UNIQUE NOT NULL, base_url TEXT NOT NULL, login_name TEXT NOT NULL "
         "DEFAULT '', uid TEXT NOT NULL DEFAULT '', display_name TEXT NOT NULL "
         "DEFAULT '', root_id TEXT NOT NULL DEFAULT '', credential_ref TEXT "
         "UNIQUE NOT NULL, state TEXT NOT NULL CHECK(state IN "
         "('creating','ready','removing')))"}) {
      if (!q.exec(sql)) {
        db_.rollback();
        throw std::runtime_error("Control database migration failed");
      }
    }
    if (schema < 2 &&
        !q.exec("ALTER TABLE operations ADD COLUMN method TEXT NOT "
                "NULL DEFAULT ''")) {
      db_.rollback();
      throw std::runtime_error("Operation migration failed");
    }
    if (!q.exec(
            "CREATE TABLE IF NOT EXISTS folders(id TEXT PRIMARY KEY,account_id "
            "TEXT NOT NULL REFERENCES accounts(id),local_path TEXT NOT NULL "
            "UNIQUE,remote_path TEXT NOT NULL,local_identity TEXT NOT NULL "
            "UNIQUE,remote_identity TEXT NOT NULL,journal_path TEXT NOT NULL "
            "UNIQUE,paused INTEGER NOT NULL DEFAULT 0,status TEXT NOT NULL "
            "DEFAULT 'offline',reason TEXT NOT NULL DEFAULT '',last_sync TEXT "
            "NOT NULL DEFAULT '',UNIQUE(account_id,remote_path))") ||
        // Schema 10: a folder's file mode is fixed when it is added (§2.3).
        !addColumn(q, "folders", "mode",
                   "TEXT NOT NULL DEFAULT 'full' CHECK(mode IN "
                   "('full','onDemand'))") ||
        !q.exec(
            "CREATE TABLE IF NOT EXISTS decisions(id TEXT PRIMARY "
            "KEY,folder_id TEXT UNIQUE NOT NULL REFERENCES folders(id) ON "
            "DELETE CASCADE,digest TEXT NOT NULL,boot TEXT NOT NULL,expires "
            "TEXT NOT NULL,local_count INTEGER NOT NULL,remote_count INTEGER "
            "NOT NULL,baseline INTEGER NOT NULL)") ||
        !q.exec(
            "CREATE TABLE IF NOT EXISTS endpoints(id TEXT PRIMARY "
            "KEY,account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE "
            "CASCADE,name TEXT NOT NULL,base_url TEXT NOT NULL,approved "
            "INTEGER NOT NULL DEFAULT 0,state TEXT NOT NULL,expires TEXT NOT "
            "NULL,operation_id TEXT NOT NULL,UNIQUE(account_id,base_url))") ||
        !q.exec("CREATE TABLE IF NOT EXISTS reauthentication(account_id TEXT "
                "PRIMARY KEY REFERENCES accounts(id) ON DELETE "
                "CASCADE,session_id TEXT UNIQUE NOT NULL,operation_id TEXT NOT "
                "NULL,phase TEXT NOT NULL CHECK(phase IN "
                "('waiting','prepared','installed','discarding','succeeded','"
                "failed')),login_"
                "name TEXT NOT NULL DEFAULT '',previous_login TEXT NOT "
                "NULL,reason TEXT NOT NULL DEFAULT '',previous_present INTEGER "
                "NOT NULL DEFAULT 1,retired INTEGER NOT NULL DEFAULT 0)") ||
        !q.exec(
            "CREATE TABLE IF NOT EXISTS endpoint_policies(account_id TEXT "
            "PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,mode TEXT "
            "NOT NULL CHECK(mode IN ('auto','fixed')),fixed_id TEXT NOT "
            "NULL,revision INTEGER NOT NULL)") ||
        !q.exec(
            "CREATE TABLE IF NOT EXISTS pending_policies(account_id TEXT "
            "PRIMARY "
            "KEY REFERENCES accounts(id) ON DELETE CASCADE,operation_id TEXT "
            "UNIQUE NOT NULL,target_id TEXT NOT NULL)") ||
        !q.exec(
            "CREATE TABLE IF NOT EXISTS endpoint_quarantine(account_id TEXT "
            "NOT "
            "NULL REFERENCES accounts(id) ON DELETE CASCADE,endpoint_id TEXT "
            "NOT "
            "NULL,reason TEXT NOT NULL,PRIMARY KEY(account_id,endpoint_id))") ||
        !q.exec(
            "CREATE TABLE IF NOT EXISTS switch_context(account_id TEXT PRIMARY "
            "KEY REFERENCES accounts(id) ON DELETE CASCADE,operation_id TEXT "
            "NOT "
            "NULL,epoch INTEGER NOT NULL,from_id TEXT NOT NULL,generation TEXT "
            "NOT NULL,policy_revision INTEGER NOT NULL,binding_epoch INTEGER "
            "NOT "
            "NULL,cause TEXT NOT NULL)") ||
        !q.exec(
            "CREATE TABLE IF NOT EXISTS activity(cursor INTEGER PRIMARY KEY "
            "AUTOINCREMENT,account_id TEXT NOT NULL REFERENCES accounts(id) "
            "ON DELETE CASCADE,folder_id TEXT NOT NULL,at TEXT NOT NULL,kind "
            "TEXT NOT NULL,count INTEGER NOT NULL CHECK(count>0),from_id "
            "TEXT NOT NULL,to_id TEXT NOT NULL,reason TEXT NOT NULL)") ||
        !q.exec(
            "CREATE TABLE IF NOT EXISTS account_constraints(account_id TEXT "
            "PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,reason "
            "TEXT NOT NULL CHECK(reason='SERVER_THROTTLED'),record_version "
            "INTEGER NOT NULL CHECK(record_version=1),received_utc INTEGER "
            "NOT NULL CHECK(received_utc>0),delay_ms INTEGER NOT NULL "
            "CHECK(delay_ms=-1 OR delay_ms BETWEEN 60000 AND 604800000),"
            "deadline_utc INTEGER,CHECK((delay_ms=-1 AND deadline_utc IS NULL) "
            "OR (delay_ms>0 AND deadline_utc IS NOT NULL AND "
            "deadline_utc=received_utc+delay_ms)))") ||
        // A row means: this creating account's vault slot holds a password
        // the daemon acquired, its DELETE is only delayed by the server, and
        // none is in flight.
        !q.exec("CREATE TABLE IF NOT EXISTS held_revocations(account_id TEXT "
                "PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE)") ||
        // Schema 11: how an address is reached, what a deletion proposal
        // covers, conflict copies and the last version that ran (protocol 1.9).
        !addColumn(q, "endpoints", "kind",
                   "TEXT NOT NULL DEFAULT 'internet' CHECK(kind IN "
                   "('internet','home','vpn'))") ||
        !addColumn(q, "decisions", "groups", "TEXT NOT NULL DEFAULT '[]'") ||
        !addColumn(q, "settings", "daemon_version",
                   "TEXT NOT NULL DEFAULT ''") ||
        !q.exec("CREATE TABLE IF NOT EXISTS conflicts(id TEXT PRIMARY KEY,"
                "folder_id TEXT NOT NULL REFERENCES folders(id) ON DELETE "
                "CASCADE,file TEXT NOT NULL,copy TEXT NOT NULL,dismissed "
                "INTEGER NOT NULL DEFAULT 0,UNIQUE(folder_id,copy))") ||
        !SwitchState::createSchema(db_) || !q.exec("PRAGMA user_version=11")) {
      db_.rollback();
      throw std::runtime_error("Schema version update failed");
    }
    if (!db_.commit())
      throw std::runtime_error("Control database migration commit failed");
    if (!q.exec("SELECT revision,seq,paused FROM settings WHERE id=1") ||
        !q.next())
      throw std::runtime_error("Control database settings missing");
    revision_ = q.value(0).toLongLong();
    seq_ = q.value(1).toLongLong();
    paused_ = q.value(2).toBool();
    q.finish();
    // The installer asks after an update what this start migrated from.
    previousSchema_ = schema;
    if (!q.exec("SELECT daemon_version FROM settings WHERE id=1") || !q.next())
      throw std::runtime_error("Control database settings missing");
    previousVersion_ = q.value(0).toString();
    q.finish();
    q.prepare("UPDATE settings SET daemon_version=? WHERE id=1");
    q.addBindValue(QStringLiteral(SYNCD_VERSION));
    if (!q.exec())
      throw std::runtime_error("Control database settings update failed");
    q.finish();
    restoreAccountConstraints();
    if (startup_)
      startup_->importDiagnosticStop(db_);
    if (mode == StartupMode::Immediate)
      activate();
  } catch (...) {
    closeDatabase();
    throw;
  }
}

void Control::publishEvent(QSqlQuery &q) {
  q.prepare("INSERT INTO outbox VALUES(?,?)");
  q.addBindValue(seq_);
  q.addBindValue(QJsonDocument(projection()).toJson(QJsonDocument::Compact));
  execute(q);
  q.prepare("DELETE FROM outbox WHERE seq<=?");
  q.addBindValue(seq_ - 100);
  execute(q);
}

bool Control::activate(qint64 monotonicMs) {
  if (stateAvailable_)
    return true;
  if (startup_ && startup_->decision().diagnostic) {
    activationAllowed_ = false;
    startupState_ = "diagnostic";
  }
  if (!activationAllowed_ || shutdown_ || !shutdownOperation_.isEmpty() ||
      (startup_ && !startup_->engineAllowed(monotonicMs)))
    return false;
  // Close admission before any credential/network recovery can pump Qt events.
  activationAllowed_ = false;
  QSqlQuery q(db_);
  recoverAccounts();
  recoverReauthentication();
  recoverEndpoints();
  if (!q.exec("SELECT 1 FROM switch_intents WHERE phase!='blocked' LIMIT 1"))
    throw std::runtime_error("Cannot inspect interrupted switches");
  const bool interruptedSwitch = q.next();
  q.finish();
  if (interruptedSwitch)
    accountChange([&] {
      const auto interrupted = SwitchState(db_).interrupt();
      if (!q.exec("UPDATE folders SET status='error',reason='QUIESCE_STALLED' "
                  "WHERE account_id IN (SELECT account_id FROM switch_intents "
                  "WHERE phase='blocked')"))
        throw std::runtime_error("INTERNAL_ERROR");
      for (const auto &operation : interrupted) {
        q.prepare("SELECT receipt FROM operations WHERE id=?");
        q.addBindValue(operation);
        execute(q);
        if (!q.next())
          throw std::runtime_error("INTERNAL_ERROR");
        auto receipt =
            QJsonDocument::fromJson(q.value(0).toByteArray()).object();
        q.finish();
        receipt.insert("state", "failed");
        receipt.insert("error", failure("OPERATION_INTERRUPTED"));
        q.prepare("UPDATE operations SET receipt=? WHERE id=?");
        q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
        q.addBindValue(operation);
        execute(q);
      }
    });
  if (!q.exec("DELETE FROM pending_policies"))
    throw std::runtime_error("INTERNAL_ERROR");
  q.finish();
  restoreFolders();
  const auto previousSeq = seq_;
  if (publishActivation_ && !db_.transaction())
    throw std::runtime_error("INTERNAL_ERROR");
  try {
    startupState_ = "normal";
    stateAvailable_ = true;
    if (publishActivation_) {
      // Availability is not a configuration edit, but subscribed clients must
      // observe a strictly newer durable cursor even for a completely empty DB.
      ++seq_;
      q.prepare("UPDATE settings SET seq=? WHERE id=1");
      q.addBindValue(seq_);
      execute(q);
      publishEvent(q);
      q.finish();
      if (!db_.commit())
        throw std::runtime_error("INTERNAL_ERROR");
    }
  } catch (...) {
    if (publishActivation_)
      db_.rollback();
    seq_ = previousSeq;
    startupState_ = "backoff";
    stateAvailable_ = false;
    throw;
  }
  QTimer::singleShot(0, &networkTasks_, [this] { startNetwork(); });
#ifndef SYNCD_WITH_ENGINE
  QTimer::singleShot(0, &networkTasks_, [this] {
    resumeReauthentication();
    selfCheck();
  });
#endif
  return true;
}

Control::~Control() {
  network_.reset();
#ifdef SYNCD_WITH_ENGINE
  runtime_.reset();
#endif
  logins_.clear();
  closeDatabase();
}

void Control::closeDatabase() {
  if (!db_.isValid())
    return;
  auto name = db_.connectionName();
  db_.close();
  db_ = {};
  QSqlDatabase::removeDatabase(name);
}

QJsonObject Control::projection() const {
  const auto availableCapabilities = capabilities(stateAvailable_);
  QJsonObject snapshot{
      {"daemon", QJsonObject{{"version", SYNCD_VERSION},
                             {"protocol", 1},
                             {"demo", false},
                             {"allPaused", paused_},
                             {"stateAvailable", stateAvailable_},
                             {"startupState", startupState_},
                             {"capabilities", availableCapabilities}}}};
  if (!stateAvailable_)
    return {{"bootId", boot_},
            {"seq", QString::number(seq_)},
            {"revision", QString::number(revision_)},
            {"snapshot", snapshot}};
  {
    auto daemon = snapshot.value("daemon").toObject();
    daemon.insert("startupReport", startupReport());
    snapshot.insert("daemon", daemon);
  }
  for (const auto &key : {"accounts", "folders", "endpoints", "networks",
                          "switches", "decisions", "activity"})
    snapshot.insert(key, QJsonArray{});
  QJsonArray accounts, endpoints, networks;
  QSqlQuery q(db_);
  q.prepare("SELECT id,base_url,login_name,display_name,endpoint_id FROM "
            "accounts WHERE state='ready' ORDER BY id");
  execute(q);
  while (q.next()) {
    const auto account = q.value(0).toString(),
               endpoint = q.value(4).toString();
    const auto active = SwitchState(db_).binding(account).endpoint;
    QSqlQuery intent(db_);
    intent.prepare("SELECT phase FROM switch_intents WHERE account_id=?");
    intent.addBindValue(account);
    execute(intent);
    const bool switching = intent.next();
    const bool blocked = switching && intent.value(0).toString() == "blocked";
    const QUrl url(q.value(1).toString());
    // Protocol 1.9: revoked when the server refused the stored app password,
    // needsSignIn when it cannot be used here or a renewal waits for the
    // browser, signedIn otherwise.
    QString auth = QStringLiteral("signedIn");
    QSqlQuery reasons(db_);
    reasons.prepare("SELECT reason FROM folders WHERE account_id=?");
    reasons.addBindValue(account);
    execute(reasons);
    while (reasons.next()) {
      if (reasons.value(0) == "AUTH_REQUIRED")
        auth = QStringLiteral("revoked");
      else if (reasons.value(0) == "CREDENTIALS_LOCKED" && auth == "signedIn")
        auth = QStringLiteral("needsSignIn");
    }
    reasons.finish();
    if (network_ && network_->accounts.contains(account) &&
        !network_->accounts.at(account).authFailed.isEmpty())
      auth = QStringLiteral("revoked");
    reasons.prepare("SELECT 1 FROM reauthentication WHERE account_id=? AND "
                    "phase IN ('waiting','prepared','discarding')");
    reasons.addBindValue(account);
    execute(reasons);
    if (reasons.next())
      auth = QStringLiteral("needsSignIn");
    reasons.finish();
    accounts.append(QJsonObject{{"id", account},
                                {"user", q.value(2).toString()},
                                {"displayName", q.value(3).toString()},
                                {"server", url.host()},
                                {"auth", auth},
                                {"signedIn", auth == "signedIn"}});
    endpoints.append(QJsonObject{{"id", endpoint},
                                 {"accountId", account},
                                 {"name", url.host()},
                                 {"kind", "internet"},
                                 {"url", url.toString()},
                                 {"main", true},
                                 {"approved", true},
                                 {"verified", "verified"},
                                 {"inUse", active == endpoint && !blocked}});
    bool networkBlocked = false;
    if (network_ && network_->accounts.contains(account)) {
      const auto &state = network_->accounts.at(account);
      networkBlocked = state.networkBlocked;
      auto projection = endpoints.last().toObject();
      const auto it = state.nodes.find(endpoint);
      const bool verified = it != state.nodes.end() && it->second.verified &&
                            it->second.config.enabled;
      const auto health =
          state.policy.health(endpoint, network_->clock.elapsed());
      projection.insert("verified", verified ? "verified" : "pending");
      projection.insert("reachable", verified && !health.failed);
      projection.insert("blocked", !verified || health.failed);
      projection.insert("inUse",
                        active == endpoint && !blocked && !networkBlocked);
      endpoints.replace(endpoints.size() - 1, projection);
    }
    networks.append(QJsonObject{
        {"accountId", account},
        {"policy", "fixed"},
        {"fixedEndpointId", active},
        {"activeId", blocked || networkBlocked ? QString{} : active},
        {"switching", switching}});
    auto network = networks.last().toObject();
    QSqlQuery policy(db_);
    policy.prepare(
        "SELECT mode,fixed_id FROM endpoint_policies WHERE account_id=?");
    policy.addBindValue(account);
    execute(policy);
    if (policy.next()) {
      network.insert("policy", policy.value(0).toString());
      network.insert("fixedEndpointId", policy.value(1).toString());
    }
    policy.finish();
    policy.prepare("SELECT target_id FROM pending_policies WHERE account_id=?");
    policy.addBindValue(account);
    execute(policy);
    if (policy.next())
      network.insert("requestedEndpointId", policy.value(0).toString());
    networks.replace(networks.size() - 1, network);
  }
  snapshot.insert("accounts", accounts);
  for (const auto &endpoint : endpointProjection())
    endpoints.append(endpoint);
  // Protocol 1.9: measured latency, how an address is reached and one
  // unambiguous reason it cannot serve, strongest first.
  for (qsizetype i = 0; i < endpoints.size(); ++i) {
    auto item = endpoints.at(i).toObject();
    const auto account = item.value("accountId").toString(),
               id = item.value("id").toString();
    QString reason;
    QSqlQuery quarantine(db_);
    quarantine.prepare("SELECT reason FROM endpoint_quarantine WHERE "
                       "account_id=? AND endpoint_id=?");
    quarantine.addBindValue(account);
    quarantine.addBindValue(id);
    execute(quarantine);
    if (quarantine.next())
      reason = quarantine.value(0).toString();
    qint64 latency = 0;
    if (network_ && network_->accounts.contains(account)) {
      const auto health = network_->accounts.at(account).policy.health(
          id, network_->clock.elapsed());
      latency = health.latencyMs;
      if (reason.isEmpty() && health.authBlocked)
        reason = QStringLiteral("AUTH_REQUIRED");
      if (reason.isEmpty() && item.value("verified") == "failed")
        reason = QStringLiteral("ENDPOINT_IDENTITY_MISMATCH");
      if (reason.isEmpty() && item.value("verified") != "verified")
        reason = QStringLiteral("VERIFICATION_PENDING");
      if (reason.isEmpty() && health.failed)
        reason = QStringLiteral("NETWORK_UNAVAILABLE");
    } else if (reason.isEmpty() && item.value("blocked").toBool())
      reason = item.value("verified") == "failed"
                   ? QStringLiteral("ENDPOINT_IDENTITY_MISMATCH")
                   : QStringLiteral("VERIFICATION_PENDING");
    item.insert("latencyMs", latency);
    item.insert("blockedReason", reason);
    item.insert("blocked", !reason.isEmpty());
    if (!reason.isEmpty())
      item.insert("reachable", false);
    endpoints.replace(i, item);
  }
  // Protocol 1.9: why the address in use was chosen and why the last switch
  // happened, from the committed switch history.
  for (qsizetype i = 0; i < networks.size(); ++i) {
    auto item = networks.at(i).toObject();
    QSqlQuery last(db_);
    last.prepare("SELECT at,reason FROM activity WHERE account_id=? AND "
                 "kind='switched' ORDER BY cursor DESC LIMIT 1");
    last.addBindValue(item.value("accountId").toString());
    execute(last);
    const bool switched = last.next();
    if (switched) {
      item.insert("switched", last.value(0).toString());
      item.insert("switchReason", last.value(1).toString());
    }
    QJsonArray why;
    if (item.value("policy") == "fixed")
      why.append("userChoice");
    else
      why.append(switched ? last.value(1).toString()
                          : QStringLiteral("verified"));
    if (item.value("switching").toBool())
      why.append("postponed");
    item.insert("why", why);
    networks.replace(i, item);
  }
  snapshot.insert("endpoints", endpoints);
  snapshot.insert("networks", networks);
  snapshot.insert("folders", folderProjection());
  snapshot.insert("decisions", decisionProjection());
  const auto activity = activityProjection();
  snapshot.insert("activity", activity);
  QJsonArray switches;
  for (const auto &entry : activity)
    if (entry.toObject().value("kind") == "switched")
      switches.append(entry);
  snapshot.insert("switches", switches);
  return {{"bootId", boot_},
          {"seq", QString::number(seq_)},
          {"revision", QString::number(revision_)},
          {"snapshot", snapshot}};
}

QJsonValue Control::dispatch(const QJsonValue &request, bool &negotiated,
                             bool &subscribed) {
  if (!request.isObject())
    return QJsonObject{{"jsonrpc", "2.0"},
                       {"id", QJsonValue::Null},
                       {"error", failure("INVALID_REQUEST", -32600)}};
  const auto obj = request.toObject();
  const auto id = obj.value("id");
  QJsonObject reply{
      {"jsonrpc", "2.0"},
      {"id", id.isUndefined() ? QJsonValue(QJsonValue::Null) : id}};
  if (obj.value("jsonrpc") != "2.0" || !obj.value("method").isString() ||
      (!id.isUndefined() && !id.isString() && !id.isDouble() && !id.isNull()) ||
      (id.isDouble() && std::floor(id.toDouble()) != id.toDouble()) ||
      obj.contains("result") || obj.contains("error") ||
      (obj.contains("params") && !obj.value("params").isObject())) {
    reply.insert("error", failure("INVALID_REQUEST", -32600));
    return reply;
  }
  // Client notifications never execute mutations or alter session negotiation.
  if (id.isUndefined())
    return QJsonValue::Null;
  const auto method = obj.value("method").toString();
  const auto params = obj.value("params").toObject();
  if (dispatching_) {
    reply.insert("error", failure("QUIESCE_STALLED"));
    return reply;
  }
  QScopedValueRollback<bool> dispatchGuard(dispatching_, true);
  try {
    QJsonValue result;
    if (method == "session.hello") {
      if (params.value("protocol").toObject().value("major").toInt() != 1) {
        negotiated = false;
        subscribed = false;
        reply.insert("error", failure("PROTOCOL_MISMATCH"));
        return reply;
      }
      negotiated = true;
      result = QJsonObject{
          {"protocol", QJsonObject{{"major", 1}, {"minor", protocolMinor}}},
          {"bootId", boot_},
          {"daemonVersion", SYNCD_VERSION},
          {"capabilities", capabilities(stateAvailable_)},
          {"limits",
           QJsonObject{{"maxFrameBytes", 1048576}, {"maxInFlight", 32}}}};
    } else if (!negotiated) {
      reply.insert("error", failure("PROTOCOL_MISMATCH"));
      return reply;
    } else if (method == "state.bootstrap") {
      subscribed =
          stateAvailable_ ||
          (publishActivation_ && startupState_ == "backoff" && !shutdown_);
      result = projection();
    } else if (!stateAvailable_) {
      result = startupDispatch(method, params);
    } else if (method == "events.resume") {
      reply.insert("error", failure("RESYNC_REQUIRED"));
      return reply;
    } else if (method == "operations.get") {
      QSqlQuery q(db_);
      q.prepare("SELECT receipt FROM operations WHERE id=?");
      q.addBindValue(params.value("operationId").toString());
      execute(q);
      if (!q.next()) {
        reply.insert("error", failure("NOT_FOUND"));
        return reply;
      }
      result = loginReceipt(
          QJsonDocument::fromJson(q.value(0).toByteArray()).object());
      if (shutdownReady_ &&
          params.value("operationId").toString() == shutdownOperation_)
        shutdown_ = true;
    } else if (method == "accounts.login.status") {
      result = loginStatus(params.value("loginSessionId").toString());
    } else if (method == "accounts.list" || method == "endpoints.list") {
      result = projection()
                   .value("snapshot")
                   .toObject()
                   .value(method == "accounts.list" ? "accounts" : "endpoints");
    } else if (method == "folders.list" || method == "folders.inspect") {
      result = folderRead(method, params);
    } else if (method == "decisions.list") {
      result = decisionProjection();
    } else if (method == "activity.list") {
      result = activityProjection(params);
    } else if (method == "diagnostics.snapshot") {
      result = diagnosticsProjection();
    } else if (method == "settings.update" || method == "daemon.shutdown" ||
               method == "accounts.login.start" ||
               method == "accounts.login.cancel" ||
               method == "accounts.reauthenticate" ||
               method == "accounts.remove" || method == "folders.add" ||
               method == "folders.pause" || method == "folders.resume" ||
               method == "folders.syncNow" || method == "folders.remove" ||
               method == "decisions.resolve" || method == "endpoints.stage" ||
               method == "endpoints.approveOrigin" ||
               method == "endpoints.verify" || method == "endpoints.remove" ||
               method == "endpoints.requestSwitch" ||
               method == "endpoints.setPolicy" ||
               method == "diagnostics.export" ||
               method == "daemon.prepareUpdate") {
      auto receipt = mutate(method, params);
      if (receipt.contains("code")) {
        reply.insert("error", receipt);
        return reply;
      }
      result = receipt;
    } else {
      reply.insert("error", failure("UNSUPPORTED_CAPABILITY"));
      return reply;
    }
    reply.insert("result", result);
  } catch (const std::exception &e) {
    const QString reason = QString::fromUtf8(e.what());
    const QStringList known{"INVALID_PARAMS",
                            "STATE_UNAVAILABLE",
                            "OPERATION_KEY_CONFLICT",
                            "CREDENTIALS_LOCKED",
                            "JOURNAL_ERROR",
                            "RESUME_CONTEXT_UNSUPPORTED",
                            "FOLDER_OWNED",
                            "FOLDER_ROOT_CHANGED",
                            "LOCAL_ROOT_UNAVAILABLE",
                            "REMOTE_ROOT_UNAVAILABLE",
                            "ENDPOINT_IDENTITY_MISMATCH",
                            "UNSUPPORTED_FOLDER_MODE",
                            "AUTH_REQUIRED",
                            "TLS_ERROR",
                            "ENDPOINT_UNTRUSTED",
                            "NETWORK_UNAVAILABLE",
                            "PERMISSION_DENIED",
                            "REMOTE_LOCKED",
                            "SERVER_THROTTLED",
                            "INVALID_SERVER_RESPONSE",
                            "RATE_LIMITED",
                            "NOT_FOUND",
                            "UNSUPPORTED_CAPABILITY",
                            "REVISION_CONFLICT",
                            "PAUSED",
                            "DECISION_PENDING",
                            "ACCOUNT_EXISTS",
                            "QUIESCE_STALLED"};
    reply.insert("error",
                 failure(known.contains(reason) ? reason : "INTERNAL_ERROR"));
  }
  return reply;
}

QJsonObject Control::mutate(const QString &method, const QJsonObject &params) {
  const auto key = params.value("operationKey").toString();
  if (QUuid(key).isNull() || QUuid(key).toString(QUuid::WithoutBraces) != key)
    return failure("INVALID_PARAMS", -32602);
  auto digest = QCryptographicHash::hash(
      method.toUtf8() + '\n' +
          QJsonDocument(params).toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256);
  QSqlQuery q(db_);
  q.prepare("SELECT digest,receipt FROM operations WHERE key=?");
  q.addBindValue(key);
  execute(q);
  if (q.next()) {
    if (q.value(0).toByteArray() != digest)
      return failure("OPERATION_KEY_CONFLICT");
    return loginReceipt(
        QJsonDocument::fromJson(q.value(1).toByteArray()).object());
  }
  q.finish();
  if (storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
    return failure("QUIESCE_STALLED");
  if (method.startsWith("folders."))
    return folderMutation(method, params, digest);
  if (method == "decisions.resolve")
    return decisionMutation(params, digest);
  if (method == "endpoints.requestSwitch")
    return switchMutation(params, digest);
  if (method == "endpoints.setPolicy")
    return policyMutation(params, digest);
  if (method == "diagnostics.export")
    return exportDiagnostics(params, digest);
  if (method == "daemon.prepareUpdate")
    return prepareUpdate(params, digest);
  if (method == "accounts.reauthenticate")
    return reauthenticate(params, digest);
  if (method.startsWith("endpoints."))
    return endpointMutation(method, params, digest);
  if (method.startsWith("accounts."))
    return loginMutation(method, params, digest);
  if (method == "settings.update") {
    if (params.value("expectedRevision").toString() !=
        QString::number(revision_))
      return failure("REVISION_CONFLICT");
    if (!params.value("allPaused").isBool())
      return failure("INVALID_PARAMS", -32602);
  }
  for (auto it = params.begin(); it != params.end(); ++it) {
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        it.key() != (method == "settings.update" ? "allPaused" : "reason"))
      return failure("INVALID_PARAMS", -32602);
  }
  if (method == "daemon.shutdown" && startup_ &&
      !startup_->recordStopIntent()) {
    stopForStorageFailure();
    return failure("INTERNAL_ERROR");
  }
  const auto oldPaused = paused_;
  const auto oldRevision = revision_;
  const auto oldSeq = seq_;
  bool committed = false;
  if (!db_.transaction())
    throw std::runtime_error("Cannot begin mutation");
  try {
    if (method == "settings.update") {
      paused_ = params.value("allPaused").toBool();
      ++revision_;
    }
    ++seq_;
    const QJsonObject receipt{
        {"operationId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"state", method == "daemon.shutdown" ? "running" : "succeeded"},
        {"result", QJsonObject{}}};
    q.prepare("UPDATE settings SET revision=?,seq=?,paused=? WHERE id=1");
    q.addBindValue(revision_);
    q.addBindValue(seq_);
    q.addBindValue(paused_);
    execute(q);
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,?,?,?)");
    q.addBindValue(key);
    q.addBindValue(receipt.value("operationId").toString());
    q.addBindValue(digest);
    q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    q.addBindValue(method);
    execute(q);
    publishEvent(q);
    if (!db_.commit())
      throw std::runtime_error("Cannot commit mutation");
    committed = true;
    if (method == "daemon.shutdown") {
      shutdownOperation_ = receipt.value("operationId").toString();
#ifdef SYNCD_WITH_ENGINE
      runtime_->shutdown();
#else
      operationFinished(shutdownOperation_);
      shutdownReady_ = true;
#endif
      if (shutdownReady_) {
        shutdown_ = true;
        auto done = receipt;
        done.insert("state", "succeeded");
        return done;
      }
    }
#ifdef SYNCD_WITH_ENGINE
    if (method == "settings.update")
      runtime_->pause(paused_);
#endif
    return receipt;
  } catch (...) {
    if (committed)
      throw;
    db_.rollback();
    paused_ = oldPaused;
    revision_ = oldRevision;
    seq_ = oldSeq;
    if (method == "daemon.shutdown" && startup_)
      stopForStorageFailure();
    throw;
  }
}
