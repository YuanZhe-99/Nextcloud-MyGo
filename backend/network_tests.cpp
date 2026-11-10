// SPDX-License-Identifier: GPL-3.0-only
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "control.h"
#include "namespace.h"
#include "switch_state.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <stdexcept>

/// NetworkTestAccess checks frozen publication and durable policy without
/// introducing runtime bypass flags.
struct NetworkTestAccess {
  /// metadata delivers the same typed failure consumed by full verification
  /// before any generic exception/string normalization (N18).
  static void metadata(Control &c, const QString &account, qint64 delay) {
    c.metadataFailed(account, MetadataError("SERVER_THROTTLED", delay));
  }
  /// remaining observes the production monotonic account gate under a
  /// caller-controlled contract clock.
  static qint64 remaining(Control &c, const QString &account, qint64 now = -1) {
    return c.throttleRemaining(account, now);
  }
  /// verify invokes actual all-root admission without a test network bypass.
  static void verify(Control &c, const QString &account,
                     const QString &endpoint) {
    c.verifySwitchTarget(account, endpoint);
  }
  /// constraint retains health evidence for request constraints rather than
  /// manufacturing transport/authentication failures (PLAN §12.1, N18).
  static bool constraint(Control &c, const QString &account,
                         const QString &endpoint, const QString &reason) {
    c.network_ = std::make_unique<Control::NetworkState>();
    c.network_->clock.start();
    c.refreshNetwork(account);
    auto &state = c.network_->accounts.at(account);
    auto &node = state.nodes.at(endpoint);
    node.verified = true;
    node.config.namespaceVerified = node.config.tlsValid = true;
    state.policy.configure(endpoint, node.config);
    state.policy.businessSuccess(endpoint, 0);
    const auto ticket = state.policy.beginProbe(endpoint, 0);
    if (!ticket)
      throw std::runtime_error("constraint fixture ticket unavailable");
    c.networkOutcome(account, *ticket, reason, 90000);
    const auto health =
        state.policy.health(endpoint, c.network_->clock.elapsed());
    return health.fresh && !health.failed && state.authFailed.isEmpty() &&
           !state.networkBlocked &&
           state.nextSwitch > c.network_->clock.elapsed() &&
           (reason == "SERVER_THROTTLED" ? c.throttleRemaining(account) > 0
                                         : c.throttleRemaining(account) == 0);
  }
  /// hardBusiness exercises a real business failure while health admission is
  /// occupied or its circuit is open (INV-04/14).
  static bool hardBusiness(Control &c, const QString &account,
                           const QString &endpoint, const QString &reason,
                           bool open) {
    c.network_ = std::make_unique<Control::NetworkState>();
    c.network_->clock.start();
    c.refreshNetwork(account);
    auto &state = c.network_->accounts.at(account);
    auto &node = state.nodes.at(endpoint);
    node.verified = true;
    node.config.namespaceVerified = node.config.tlsValid = true;
    state.policy.configure(endpoint, node.config);
    if (open)
      for (qint64 now : {0, 5000, 10000}) {
        const auto probe = state.policy.beginProbe(endpoint, now);
        if (!probe)
          throw std::runtime_error("hard failure fixture probe unavailable");
        state.policy.observe(*probe, EndpointPolicy::Outcome::NetworkFailure,
                             now, 0, 1);
      }
    const auto pending = open ? std::optional<EndpointPolicy::Ticket>{}
                              : state.policy.beginProbe(endpoint, 0);
    if (!open && !pending)
      throw std::runtime_error("hard failure fixture ticket unavailable");
    c.endpointResult(account, false, reason, quint64(state.epoch),
                     c.network_->generation);
    if (pending && state.policy.observe(
                       *pending, EndpointPolicy::Outcome::Success, 1, 1, 1))
      return false;
    return state.policy.health(endpoint, c.network_->clock.elapsed()).failed &&
           state.networkBlocked;
  }
  /// halfOpen delivers one probe outcome to the active endpoint while its
  /// circuit is recovering. Returns: whether the account was blocked.
  static bool halfOpen(Control &c, const QString &account,
                       const QString &endpoint, const QString &reason) {
    c.network_ = std::make_unique<Control::NetworkState>();
    c.network_->clock.start();
    c.refreshNetwork(account);
    auto &state = c.network_->accounts.at(account);
    auto &node = state.nodes.at(endpoint);
    node.verified = true;
    node.config.namespaceVerified = node.config.tlsValid = true;
    state.policy.configure(endpoint, node.config);
    for (qint64 now : {0, 5000, 10000}) {
      const auto probe = state.policy.beginProbe(endpoint, now);
      if (!probe)
        throw std::runtime_error("recovery fixture probe unavailable");
      state.policy.observe(*probe, EndpointPolicy::Outcome::NetworkFailure, now,
                           0, 0);
    }
    const auto probe = state.policy.beginProbe(endpoint, 60000);
    if (!probe || !state.policy.health(endpoint, 60000).halfOpen)
      throw std::runtime_error("recovery fixture circuit did not half-open");
    c.networkOutcome(account, *probe, reason, 5);
    return state.networkBlocked;
  }
  /// history drives aggregate persistence under the production outbox
  /// transaction.
  static void history(Control &c, const QString &account) {
    c.accountChange([&] {
      for (int i = 0; i < 1005; ++i)
        c.recordActivity(account, "owned-folder", "uploaded", 1);
    });
  }
  /// evidence invokes the real switch publication guard.
  static void evidence(Control &c, const QString &a, qint64 epoch) {
    c.switchEvidence(a, epoch);
  }
  /// change injects a network generation through the same control-thread path
  /// as native callbacks.
  static void change(Control &c) {
    if (!c.network_) {
      c.network_ = std::make_unique<Control::NetworkState>();
      c.network_->clock.start();
    }
    c.networkChanged(true);
  }
  /// advance executes a stale prepared switch; the guard must reject it before
  /// any network access.
  static void advance(Control &c, const QString &a, qint64 epoch) {
    c.advanceSwitch(a, epoch, QDeadlineTimer(30000), false);
  }
  /// commit models proven target verification only for a storage-boundary
  /// contract, not network acceptance.
  static void commit(Control &c, const QString &a, qint64 epoch,
                     const QString &operation) {
    c.accountChange([&] {
      SwitchState(c.db_).commit(a, epoch);
      c.completePolicy(operation);
    });
  }
};

namespace {
/// require retains assertions in optimized contract executables.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// uuid allocates an independent fixture identity.
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
/// call submits an actual idempotent revision-bearing JSON-RPC command.
QJsonObject call(Control &c, const QString &method, QJsonObject params = {}) {
  if (!params.contains("operationKey"))
    params.insert("operationKey", uuid());
  if (!params.contains("expectedRevision"))
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
/// reason returns only the stable RPC refusal reason.
QString reason(const QJsonObject &reply) {
  return reply.value("error")
      .toObject()
      .value("data")
      .toObject()
      .value("reasonCode")
      .toString();
}
/// sql requires fixture persistence success rather than hiding an invalid
/// setup.
void sql(QSqlDatabase db, const QString &text) {
  QSqlQuery q(db);
  require(q.exec(text), "policy fixture SQL failed");
}
/// policies proves pending/fixed separation, stale evidence rejection and
/// restart durability (INV-06/12/13/14).
void policies(bool stale) {
#ifdef SYNCD_WITH_ENGINE
  QTemporaryDir root;
  require(root.isValid(), "policy temporary profile unavailable");
  const auto path = root.filePath("control.sqlite"), connection = uuid(),
             alternate = uuid();
  auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
  db.setDatabaseName(path);
  QString account, primary, operation;
  {
    Control c(path);
    call(c, "accounts.login.start",
         {{"canonicalBaseUrl", "https://primary.example.invalid/"}});
    require(db.open(), "policy fixture DB unavailable");
    sql(db, "UPDATE accounts SET "
            "state='ready',uid='owned-user',root_id='owned-root',login_name='"
            "owned-user'");
    QSqlQuery q(db);
    require(q.exec("SELECT id,endpoint_id FROM accounts") && q.next(),
            "policy account missing");
    account = q.value(0).toString();
    primary = q.value(1).toString();
    q.finish();
    q.prepare("INSERT INTO endpoints VALUES(?,?, "
              "'owned-alternate','https://alternate.example.invalid/"
              "',1,'verified','',?)");
    q.addBindValue(alternate);
    q.addBindValue(account);
    q.addBindValue(uuid());
    require(q.exec(), "alternate reservation failed");
    q.finish();
    QJsonObject params{{"operationKey", uuid()},
                       {"accountId", account},
                       {"policy", "auto"},
                       {"fixedEndpointId", ""},
                       {"expectedRevision", c.projection().value("revision")}};
    const auto automatic = call(c, "endpoints.setPolicy", params);
    require(automatic.contains("result") &&
                call(c, "endpoints.setPolicy", params) == automatic,
            "automatic policy retry changed operation");
    require(reason(call(c, "endpoints.setPolicy",
                        {{"accountId", account},
                         {"policy", "auto"},
                         {"fixedEndpointId", alternate}})) == "INVALID_PARAMS",
            "automatic policy accepted a fixed target");
    for (const auto &invalid :
         {QJsonValue(1), QJsonValue(false), QJsonValue(QJsonValue::Null),
          QJsonValue(QJsonArray{})})
      require(reason(call(c, "endpoints.setPolicy",
                          {{"accountId", account},
                           {"policy", "auto"},
                           {"fixedEndpointId", invalid}})) == "INVALID_PARAMS",
              "automatic policy accepted a nonstring target");
    const auto accepted = call(c, "endpoints.setPolicy",
                               {{"accountId", account},
                                {"policy", "fixed"},
                                {"fixedEndpointId", alternate}});
    operation =
        accepted.value("result").toObject().value("operationId").toString();
    require(!operation.isEmpty(), "fixed activation not durably accepted");
    auto network = c.projection()
                       .value("snapshot")
                       .toObject()
                       .value("networks")
                       .toArray()[0]
                       .toObject();
    require(network.value("policy") == "auto" &&
                network.value("activeId") == primary &&
                network.value("requestedEndpointId") == alternate,
            "pending fixed request replaced actual policy or binding");
    require(reason(call(c, "endpoints.setPolicy",
                        {{"accountId", account}, {"policy", "auto"}})) ==
                "QUIESCE_STALLED",
            "policy changed while fixed activation was pending");
    require(q.exec("SELECT target_epoch FROM switch_intents") && q.next(),
            "fixed switch epoch absent");
    const auto epoch = q.value(0).toLongLong();
    q.finish();
    NetworkTestAccess::evidence(c, account, epoch);
    if (stale) {
      NetworkTestAccess::change(c);
      bool refused = false;
      try {
        NetworkTestAccess::evidence(c, account, epoch);
      } catch (const std::exception &e) {
        refused = QString::fromUtf8(e.what()) == "REVISION_CONFLICT";
      }
      require(refused, "stale network generation passed publication guard");
      NetworkTestAccess::advance(c, account, epoch);
      require(SwitchState(db).binding(account).endpoint == primary,
              "stale verification changed committed endpoint");
      require(call(c, "operations.get", {{"operationId", operation}})
                      .value("result")
                      .toObject()
                      .value("state") == "failed",
              "failed fixed policy reported success");
    } else {
      // Storage-only model: no claim that the .invalid endpoints passed
      // verification.
      NetworkTestAccess::commit(c, account, epoch, operation);
    }
    network = c.projection()
                  .value("snapshot")
                  .toObject()
                  .value("networks")
                  .toArray()[0]
                  .toObject();
    require(network.value("policy") == (stale ? "auto" : "fixed"),
            "fixed policy commit/failure semantics drifted");
    require(!network.contains("requestedEndpointId"),
            "terminal fixed target remained pending");
    db.close();
  }
  {
    Control restart(path);
    const auto network = restart.projection()
                             .value("snapshot")
                             .toObject()
                             .value("networks")
                             .toArray()[0]
                             .toObject();
    require(network.value("policy") == (stale ? "auto" : "fixed") &&
                network.value("fixedEndpointId") ==
                    (stale ? QString{} : alternate),
            "policy configuration did not survive restart");
  }
  db = {};
  QSqlDatabase::removeDatabase(connection);
#else
  Q_UNUSED(stale);
#endif
}
/// independentFailures prevents rapid business/probe errors from manufacturing
/// three independent samples.
void independentFailures() {
  EndpointPolicy p;
  EndpointPolicy::Config config;
  config.approved = config.namespaceVerified = config.tlsValid = true;
  p.configure("one", config);
  for (qint64 now : {0, 1, 2}) {
    auto t = p.beginProbe("one", now);
    require(t.has_value(), "independent failure ticket missing");
    p.observe(*t, EndpointPolicy::Outcome::NetworkFailure, now, 0, 1);
  }
  require(!p.health("one", 2).failed,
          "correlated sub-five-second failures opened circuit");
  for (qint64 now : {5000, 10000}) {
    auto t = p.beginProbe("one", now);
    require(t.has_value(), "independent failure ticket missing");
    p.observe(*t, EndpointPolicy::Outcome::NetworkFailure, now, 0, 1);
  }
  require(p.health("one", 10000).failed,
          "independent transport failures did not open circuit");
  EndpointPolicy healthy;
  healthy.configure("one", config);
  healthy.businessSuccess("one", 0);
  require(healthy.health("one", 0).fresh,
          "business liveness was not refreshed");
  healthy.networkChanged(1);
  require(!healthy.health("one", 2).fresh,
          "old-generation business liveness survived notification");
}
/// privacyAndHistory checks bounded pagination, durable quarantine and absence
/// of private metadata in diagnostics (INV-03/04/12).
void privacyAndHistory() {
  QTemporaryDir root;
  require(root.isValid(), "privacy profile unavailable");
  const auto path = root.filePath("control.sqlite"), connection = uuid();
  auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
  db.setDatabaseName(path);
  QString account, endpoint;
  {
    Control c(path);
    call(c, "accounts.login.start",
         {{"canonicalBaseUrl", "https://primary.example.invalid/"}});
    require(db.open(), "privacy DB unavailable");
    sql(db, "UPDATE accounts SET "
            "state='ready',uid='private-owned-uid',login_name='private-owned-"
            "login',root_id='owned-home'");
    QSqlQuery q(db);
    require(q.exec("SELECT id,endpoint_id FROM accounts") && q.next(),
            "privacy reservation missing");
    account = q.value(0).toString();
    endpoint = q.value(1).toString();
    q.finish();
    q.prepare("INSERT INTO endpoint_quarantine VALUES(?,?,'TLS_ERROR')");
    q.addBindValue(account);
    q.addBindValue(endpoint);
    require(q.exec(), "security quarantine fixture failed");
    q.finish();
    NetworkTestAccess::history(c, account);
    bool n = true, s = false;
    const auto read = [&](const QString &method,
                          const QJsonObject &params = QJsonObject{}) {
      return c
          .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                                {"id", 1},
                                {"method", method},
                                {"params", params}},
                    n, s)
          .toObject();
    };
    auto first = read("activity.list").value("result").toArray();
    require(first.size() == 100, "default activity limit drifted");
    const auto cursor = first.last().toObject().value("cursor").toString();
    const auto second =
        read("activity.list",
             {{"cursor", cursor}, {"limit", 500}, {"folderId", "owned-folder"}})
            .value("result")
            .toArray();
    require(
        second.size() == 500 &&
            second.first().toObject().value("cursor").toString().toLongLong() <
                cursor.toLongLong(),
        "activity cursor duplicated or skipped its exclusive boundary");
    require(read("activity.list", {{"limit", 501}}).contains("error") &&
                read("activity.list", {{"cursor", "01"}}).contains("error"),
            "unbounded or ambiguous activity request accepted");
    require(q.exec("SELECT COUNT(*) FROM activity") && q.next() &&
                q.value(0).toInt() == 1000,
            "activity retention was not bounded");
    q.finish();
    const auto diagnostic =
        QJsonDocument(read("diagnostics.snapshot").value("result").toObject())
            .toJson(QJsonDocument::Compact);
    require(!diagnostic.contains("private-owned") &&
                !diagnostic.contains("example.invalid") &&
                !diagnostic.contains(root.path().toUtf8()),
            "diagnostics exposed private account or path metadata");
    QJsonObject exportParams{
        {"operationKey", uuid()},
        {"expectedRevision", c.projection().value("revision")},
        {"options", QJsonObject{}}};
    const auto exported = call(c, "diagnostics.export", exportParams);
    require(exported.contains("result") &&
                call(c, "diagnostics.export", exportParams) == exported,
            "diagnostic retry produced another bundle or receipt");
    const auto bundlePath = exported.value("result")
                                .toObject()
                                .value("result")
                                .toObject()
                                .value("path")
                                .toString();
    QFile bundle(bundlePath);
    require(bundle.open(QIODevice::ReadOnly),
            "sanitized diagnostic bundle unavailable");
    const auto content = bundle.readAll();
    require(content.size() < 1048576 &&
                QJsonDocument::fromJson(content).isObject() &&
                !content.contains("private-owned") &&
                !content.contains("example.invalid") &&
                !content.contains(root.path().toUtf8()),
            "support bundle copied private metadata or invalid content");
    bundle.close();
    for (int i = 0; i < 6; ++i) {
      const auto next = call(c, "diagnostics.export");
      require(next.value("result").toObject().value("state") == "succeeded",
              "bounded support export failed");
    }
    require(QDir(root.filePath("diagnostics"))
                    .entryList({"*.json"}, QDir::Files)
                    .size() == 5,
            "support bundle retention exceeded five files");
    require(call(c, "diagnostics.export",
                 {{"options", QJsonObject{{"includeCredentials", true}}}})
                .contains("error"),
            "unsupported diagnostic export option accepted");
    db.close();
  }
  {
    Control restart(path);
#ifdef SYNCD_WITH_ENGINE
    require(reason(call(restart, "endpoints.requestSwitch",
                        {{"accountId", account}, {"endpointId", endpoint}})) ==
                "TLS_ERROR",
            "restart silently cleared security quarantine");
#endif
    require(restart.projection()
                    .value("snapshot")
                    .toObject()
                    .value("activity")
                    .toArray()
                    .size() == 100,
            "activity history did not survive restart");
  }
  db = {};
  QSqlDatabase::removeDatabase(connection);
}
/// hardBusinessFailures cannot lose durable security quarantine or auth
/// blocking to a pending probe/open circuit (INV-04/14, N04).
void hardBusinessFailures() {
#ifdef SYNCD_WITH_ENGINE
  for (const auto &failure : {QStringLiteral("TLS_ERROR"),
                              QStringLiteral("ENDPOINT_IDENTITY_MISMATCH"),
                              QStringLiteral("AUTH_REQUIRED")})
    for (bool open : {false, true}) {
      QTemporaryDir root;
      require(root.isValid(), "hard business profile unavailable");
      const auto path = root.filePath("control.sqlite"), connection = uuid();
      auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
      db.setDatabaseName(path);
      {
        Control c(path);
        call(c, "accounts.login.start",
             {{"canonicalBaseUrl", "https://primary.example.invalid/"}});
        require(db.open(), "hard business fixture DB unavailable");
        sql(db, "UPDATE accounts SET "
                "state='ready',uid='owned-user',root_id='owned-root',login_"
                "name='owned-user'");
        QSqlQuery q(db);
        require(q.exec("SELECT id,endpoint_id FROM accounts") && q.next(),
                "hard business fixture account unavailable");
        const auto account = q.value(0).toString(),
                   endpoint = q.value(1).toString();
        q.finish();
        require(NetworkTestAccess::hardBusiness(c, account, endpoint, failure,
                                                open),
                "hard business failure was lost to probe admission");
        require(q.exec("SELECT COUNT(*) FROM endpoint_quarantine") &&
                    q.next() &&
                    q.value(0).toInt() == (failure == "AUTH_REQUIRED" ? 0 : 1),
                "hard business security failure was not durably quarantined");
        q.finish();
        db.close();
      }
      db = {};
      QSqlDatabase::removeDatabase(connection);
    }
#endif
}
/// recoveringProbes keeps a successful half-open probe from blocking the
/// active endpoint: a freshly verified manual switch onto a recovering address
/// must not be failed by its own recovery evidence, while a failed probe there
/// still blocks (INV-06/14).
void recoveringProbes() {
#ifdef SYNCD_WITH_ENGINE
  for (const auto &outcome :
       {QString{}, QStringLiteral("NETWORK_UNAVAILABLE")}) {
    QTemporaryDir root;
    require(root.isValid(), "recovery profile unavailable");
    const auto path = root.filePath("control.sqlite"), connection = uuid();
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path);
    {
      Control c(path);
      call(c, "accounts.login.start",
           {{"canonicalBaseUrl", "https://primary.example.invalid/"}});
      require(db.open(), "recovery fixture DB unavailable");
      sql(db, "UPDATE accounts SET "
              "state='ready',uid='owned-user',root_id='owned-root',login_"
              "name='owned-user'");
      QSqlQuery q(db);
      require(q.exec("SELECT id,endpoint_id FROM accounts") && q.next(),
              "recovery fixture account unavailable");
      const auto account = q.value(0).toString(),
                 endpoint = q.value(1).toString();
      q.finish();
      require(NetworkTestAccess::halfOpen(c, account, endpoint, outcome) ==
                  !outcome.isEmpty(),
              outcome.isEmpty()
                  ? "successful recovery probe blocked the active endpoint"
                  : "failed recovery probe left the active endpoint admitted");
      db.close();
    }
    db = {};
    QSqlDatabase::removeDatabase(connection);
  }
#endif
}
/// requestConstraints keeps permissions, locks and throttling outside endpoint
/// circuit and credential failures (PLAN §12.1, INV-04/13/14, N18).
void requestConstraints() {
#ifdef SYNCD_WITH_ENGINE
  for (const auto &constraint :
       {QStringLiteral("PERMISSION_DENIED"), QStringLiteral("REMOTE_LOCKED"),
        QStringLiteral("SERVER_THROTTLED")}) {
    QTemporaryDir root;
    require(root.isValid(), "constraint profile unavailable");
    const auto path = root.filePath("control.sqlite"), connection = uuid();
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path);
    {
      Control c(path);
      call(c, "accounts.login.start",
           {{"canonicalBaseUrl", "https://primary.example.invalid/"}});
      require(db.open(), "constraint fixture DB unavailable");
      sql(db, "UPDATE accounts SET state='ready',uid='owned-user',"
              "root_id='owned-root',login_name='owned-user'");
      QSqlQuery q(db);
      require(q.exec("SELECT id,endpoint_id FROM accounts") && q.next(),
              "constraint fixture account missing");
      const auto account = q.value(0).toString(),
                 endpoint = q.value(1).toString();
      q.finish();
      require(NetworkTestAccess::constraint(c, account, endpoint, constraint),
              "request constraint damaged endpoint health or credentials");
      require(q.exec("SELECT COUNT(*) FROM endpoint_quarantine") && q.next() &&
                  q.value(0).toInt() == 0,
              "request constraint quarantined the endpoint");
      q.finish();
      if (constraint == "PERMISSION_DENIED") {
        const auto accepted =
            call(c, "endpoints.requestSwitch",
                 {{"accountId", account}, {"endpointId", endpoint}});
        const auto operation =
            accepted.value("result").toObject().value("operationId").toString();
        require(!operation.isEmpty(), "constraint switch reservation failed");
        require(q.exec("SELECT target_epoch FROM switch_intents") && q.next(),
                "constraint switch epoch unavailable");
        const auto epoch = q.value(0).toLongLong();
        q.finish();
        NetworkTestAccess::metadata(c, account, 120000);
        NetworkTestAccess::advance(c, account, epoch);
        require(q.exec("SELECT phase FROM switch_intents") && q.next() &&
                    q.value(0) == "prepared",
                "throttled preparation became failed/fallback work");
        q.finish();
        require(call(c, "operations.get", {{"operationId", operation}})
                            .value("result")
                            .toObject()
                            .value("state") == "running" &&
                    SwitchState(db).binding(account).endpoint == endpoint,
                "throttled switch changed binding or falsely completed");
      }
      if (constraint == "SERVER_THROTTLED") {
        const auto alternate = uuid();
        q.prepare("INSERT INTO endpoints VALUES(?,?,'Alternate',"
                  "'https://alternate.example.invalid/',1,'verified','',?)");
        q.addBindValue(alternate);
        q.addBindValue(account);
        q.addBindValue(uuid());
        require(q.exec(), "constraint alternate fixture failed");
        q.finish();
        NetworkTestAccess::metadata(c, account, 120000);
        const auto remaining = NetworkTestAccess::remaining(c, account);
        require(remaining >= 119000, "typed full-verification delay was lost");
        NetworkTestAccess::change(c);
        require(NetworkTestAccess::remaining(c, account) >= 119000,
                "network wake cleared account throttle");
        for (const auto &target : {endpoint, alternate}) {
          require(reason(call(c, "endpoints.verify",
                              {{"endpointId", target}})) == "SERVER_THROTTLED",
                  "manual verification evaded throttle via another origin");
          require(reason(call(c, "endpoints.requestSwitch",
                              {{"accountId", account},
                               {"endpointId", target}})) == "SERVER_THROTTLED",
                  "manual switching evaded account throttle");
          bool refused = false;
          try {
            NetworkTestAccess::verify(c, account, target);
          } catch (const std::exception &error) {
            refused = QString::fromUtf8(error.what()) == "SERVER_THROTTLED";
          }
          require(refused, "full-root verification bypassed account throttle");
        }
        require(reason(call(c, "folders.list",
                            {{"accountId", account}, {"remote", true}})) ==
                    "SERVER_THROTTLED",
                "remote metadata listing bypassed account throttle");
        q.prepare(
            "INSERT INTO folders(id,account_id,local_path,remote_path,"
            "local_identity,remote_identity,journal_path) VALUES(?,?,'not-"
            "accessed-local-root','/','owned-local','owned-remote','not-"
            "accessed-journal')");
        const auto folder = uuid();
        q.addBindValue(folder);
        q.addBindValue(account);
        require(q.exec(), "manual sync throttle fixture failed");
        q.finish();
        require(reason(call(c, "folders.syncNow", {{"folderId", folder}})) ==
                    "SERVER_THROTTLED",
                "manual sync bypassed account throttle");
        require(NetworkTestAccess::remaining(c, account, 1000000) == 0,
                "monotonic deadline did not expire");
      }
      db.close();
    }
    db = {};
    QSqlDatabase::removeDatabase(connection);
  }
#endif
}
/// nativeResume checks queued power invalidation and synchronous recipient
/// destruction without suspending the machine.
void nativeResume() {
  HWND window = nullptr;
  {
    NetworkMonitor monitor;
    bool notified = false;
    monitor.changed = [&](bool) { notified = true; };
    const auto pump = [&] {
      QEventLoop loop;
      QTimer poll, deadline;
      poll.setInterval(5);
      deadline.setSingleShot(true);
      QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (notified)
          loop.quit();
      });
      QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
      poll.start();
      deadline.start(2000);
      loop.exec();
      require(notified,
              "native notification did not reach control-thread evaluation");
    };
    pump();
    notified = false;
    window = FindWindowExW(HWND_MESSAGE, nullptr,
                           L"NextcloudMyGo.NetworkMonitor", nullptr);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    require(window && process == GetCurrentProcessId(),
            "owned power-notification recipient missing");
    require(PostMessageW(window, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0),
            "owned resume notification injection failed");
    pump();
  }
  require(!IsWindow(window), "power callback recipient outlived its monitor");
}
} // namespace

/// main checks connected policy storage/evidence and native monitor teardown
/// without server mutations.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if (app.arguments().contains("--constraints")) {
      requestConstraints();
      return 0;
    }
    if (app.arguments().contains("--hard-business")) {
      hardBusinessFailures();
      return 0;
    }
    policies(false);
    policies(true);
    independentFailures();
    privacyAndHistory();
    hardBusinessFailures();
    recoveringProbes();
    nativeResume();
    requestConstraints();
    std::puts(
        "PASS policy persistence, pending fixed activation, stale publication, "
        "independent failure samples and native monitor drainage");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
