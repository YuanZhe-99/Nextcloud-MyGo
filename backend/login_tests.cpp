#include "control.h"
#include "login.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QLocale>
#include <QNetworkReply>
#include <QPointer>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

/// LoginTestAccess replaces only the transport and monotonic clock of one
/// flow, and drives its timer step, so no request leaves the process.
struct LoginTestAccess {
  /// attach installs the scripted transport and restarts session validity on
  /// the supplied clock.
  static void attach(LoginFlow &flow, std::function<qint64()> clock,
                     std::function<QNetworkReply *(const QNetworkRequest &,
                                                   const QByteArray &,
                                                   const QByteArray &)>
                         send) {
    flow.deadline_ = clock() + 20 * 60 * 1000;
    flow.clock_ = std::move(clock);
    flow.send_ = std::move(send);
  }
  /// tick runs the same step the 1.5-second timer runs.
  static void tick(LoginFlow &flow) { flow.tick(); }
};
/// ReauthTestAccess reaches a Control-owned login session so the scripted
/// transport can drive it; production has no such switch.
struct ReauthTestAccess {
  static LoginFlow &flow(Control &control, const QString &session) {
    return *control.logins_.at(session);
  }
};

namespace {
/// require keeps boundary assertions enabled in optimized acceptance builds.
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
/// uuid allocates a unique owned fixture identity.
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
/// VaultFixture removes only the unique credential created by this test, including failures.
struct VaultFixture {
    QString reference = "Nextcloud-MyGo/tests/" + uuid() + "/app-password";
    /// ~VaultFixture makes cleanup failure a failed acceptance run rather than hiding a secret residue.
    ~VaultFixture() { try { removeCredential(reference); } catch (...) { std::fputs("Owned credential cleanup failed\n", stderr); std::abort(); } }
};
/// call constructs an actual RPC envelope for the durable account boundary.
QJsonObject call(Control &c, const QString &method, const QJsonObject &params = {}) {
    bool negotiated = true, subscribed = false;
    return c.dispatch(QJsonObject{{"jsonrpc", "2.0"}, {"id", "test"}, {"method", method}, {"params", params}}, negotiated, subscribed).toObject();
}
/// testURLBoundary rejects credential-bearing and ambiguous addresses before any request (INV-03/04).
void testURLBoundary() {
    auto base = canonicalServer("EXAMPLE.com:443/cloud");
    require(base.toString() == "https://example.com/cloud/", "HTTPS normalization changed installation path");
    for (const auto &bad : {"http://example.com/", "https://user:secret@example.com/", "https://example.com/?key=x", "https://example.com/#x", "https://example.com/a/../b", "https://example.com/a%2fb", "https://example.com/a%252fb"}) {
        bool rejected = false; try { canonicalServer(bad); } catch (...) { rejected = true; }
        require(rejected, "unsafe canonical address accepted");
    }
    require(withinServer(base, QUrl("https://example.com/cloud/index.php/login/v2?flow=x")), "valid in-installation request refused");
    for (const auto &bad : {"http://example.com/cloud/", "https://other.example/cloud/", "https://example.com:444/cloud/", "https://example.com/cloud2/", "https://example.com/cloud/../outside", "https://example.com/cloud/a%2fb"})
        require(!withinServer(base, QUrl(bad)), "request escaped approved boundary");
}
/// testVaultAndRecovery proves no plaintext fallback and recovery of a crash between vault write and account readiness.
void testVaultAndRecovery() {
    VaultFixture fixture;
    writeCredential(fixture.reference, "fixture-user", "fixture-secret");
    require(readCredential(fixture.reference) == "fixture-secret", "vault roundtrip failed");
    removeCredential(fixture.reference); removeCredential(fixture.reference);
    QTemporaryDir dir; require(dir.isValid(), "test directory missing");
    QString operation;
    {
        Control c(dir.filePath("control.sqlite"));
        auto response = call(c, "accounts.login.start", {{"operationKey", uuid()}, {"canonicalBaseUrl", "https://example.invalid/"}});
        operation = response.value("result").toObject().value("operationId").toString(); require(!operation.isEmpty(), "login reservation failed");
        // Read the on-disk reservation as a crash fixture, without running the scheduled network request.
        const auto connection = uuid();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(dir.filePath("control.sqlite")); require(db.open(), "fixture DB read failed");
            QSqlQuery q(db); require(q.exec("SELECT credential_ref FROM accounts WHERE state='creating'") && q.next(), "creating intent absent");
            fixture.reference = q.value(0).toString();
            writeCredential(fixture.reference, "fixture-user", "fixture-secret");
            q.finish(); db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    {
        Control c(dir.filePath("control.sqlite"));
        require(call(c, "accounts.list").value("result").toArray().isEmpty(), "interrupted account became ready");
        const auto receipt = call(c, "operations.get", {{"operationId", operation}}).value("result").toObject();
        require(receipt.value("state") == "failed", "interrupted login not expired");
        bool missing = false; try { readCredential(fixture.reference); } catch (...) { missing = true; }
        require(missing, "orphan credential survived recovery");
    }
    QFile db(dir.filePath("control.sqlite")); require(db.open(QIODevice::ReadOnly), "DB inspection failed");
    require(!db.readAll().contains("fixture-secret"), "password entered Control DB");
}
/// testCancelReplay checks durable cancellation completion and operation-key replay (INV-12).
void testCancelReplay() {
    QTemporaryDir dir; require(dir.isValid(), "test directory missing");
    Control c(dir.filePath("control.sqlite"));
    call(c, "accounts.login.start", {{"operationKey", uuid()}, {"canonicalBaseUrl", "https://example.invalid/"}});
    QString session;
    const auto connection = uuid();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection); db.setDatabaseName(dir.filePath("control.sqlite"));
        require(db.open(), "fixture database unavailable");
        QSqlQuery q(db); require(q.exec("SELECT session_id FROM accounts WHERE state='creating'") && q.next(), "missing login reservation");
        session = q.value(0).toString(); q.finish(); db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    const QJsonObject params{{"operationKey", uuid()}, {"loginSessionId", session}};
    const auto accepted = call(c, "accounts.login.cancel", params).value("result").toObject();
    const auto id = accepted.value("operationId").toString(); require(!id.isEmpty(), "cancel operation missing");
    const auto completed = call(c, "operations.get", {{"operationId", id}}).value("result").toObject();
    require(completed.value("state") == "succeeded", "cancel did not finish durably");
    require(call(c, "accounts.login.cancel", params).value("result").toObject() == completed, "cancel replay changed receipt");
    require(call(c, "accounts.login.status", {{"loginSessionId", session}}).value("result").toObject().value("state") == "cancelled", "session did not cancel");
    require(call(c, "accounts.list").value("result").toArray().isEmpty(), "cancel created account");
}

/// Reply delivers one scripted HTTP outcome through real Qt reply signals.
class Reply final : public QNetworkReply {
public:
  /// Reply records the request line the flow actually issued.
  Reply(const QNetworkRequest &request, QByteArray verb, QObject *parent)
      : QNetworkReply(parent), verb(std::move(verb)) {
    setRequest(request);
    setUrl(request.url());
    open(QIODevice::ReadOnly);
  }
  /// finish settles the reply once; status zero models a transport failure.
  void finish(int status, const QByteArray &body = {},
              const QByteArray &retryAfter = {}, NetworkError error = NoError) {
    if (isFinished())
      return;
    if (status)
      setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    if (!retryAfter.isNull())
      setRawHeader("Retry-After", retryAfter);
    body_ = body;
    setError(error, {});
    setFinished(true);
    Q_EMIT finished();
  }
  /// abort models cancellation whose server-side outcome is unknown.
  void abort() override {
    aborted = true;
    finish(0, {}, {}, OperationCanceledError);
  }
  qint64 bytesAvailable() const override {
    return body_.size() - offset_ + QNetworkReply::bytesAvailable();
  }
  QByteArray verb;
  bool aborted = false;

protected:
  /// readData hands out the scripted body only.
  qint64 readData(char *data, qint64 size) override {
    const auto count = std::min<qint64>(size, body_.size() - offset_);
    std::memcpy(data, body_.constData() + offset_, size_t(count));
    offset_ += count;
    return count;
  }

private:
  QByteArray body_;
  qint64 offset_ = 0;
};

const QByteArray started =
    R"({"poll":{"token":"t","endpoint":"https://example.invalid/index.php/login/v2/poll"},"login":"https://example.invalid/index.php/login/v2/flow/x"})";
const QByteArray granted =
    R"({"server":"https://example.invalid","loginName":"u","appPassword":"p"})";
const QByteArray user = R"({"ocs":{"meta":{"status":"ok"},"data":{"id":"u"}}})";
const QByteArray revoked = R"({"ocs":{"meta":{"status":"ok"},"data":[]}})";
const QByteArray root =
    "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" "
    "xmlns:oc=\"http://owncloud.org/ns\"><d:response><d:href>/remote.php/dav/"
    "files/u/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></"
    "d:resourcetype><oc:id>00000001oc</oc:id></d:prop><d:status>HTTP/1.1 200 "
    "OK</d:status></d:propstat></d:response></d:multistatus>";

/// Harness owns one flow on a manual clock with a scripted transport.
struct Harness {
  QObject transport;
  qint64 now = 1000;
  QList<QPointer<Reply>> sent;
  QList<qint64> throttles;
  QList<bool> holds;
  QStringList published;
  bool persist = true, persistHold = true;
  std::unique_ptr<LoginFlow> flow = std::make_unique<LoginFlow>(
      canonicalServer("https://example.invalid/"), uuid(), uuid());
  /// Harness records every request, throttle persistence and publication.
  Harness() {
    LoginTestAccess::attach(
        *flow, [this] { return now; },
        [this](const QNetworkRequest &request, const QByteArray &verb,
               const QByteArray &) {
          auto *reply = new Reply(request, verb, &transport);
          sent.append(reply);
          return reply;
        });
    flow->saveCredential = [](const QString &, const QString &) {
      return true;
    };
    flow->throttled = [this](qint64 delay) {
      throttles.append(delay);
      return persist;
    };
    flow->changed = [this](LoginFlow &value) {
      published.append(value.status());
      return true;
    };
    flow->held = [this](bool value) {
      holds.append(value);
      return persistHold;
    };
  }
  /// last returns the newest request, which must still be unanswered.
  Reply &last() {
    require(!sent.isEmpty() && sent.last() && !sent.last()->isFinished(),
            "no request is in flight");
    return *sent.last();
  }
  /// tick advances the manual clock, then runs the flow's timer step.
  void tick(qint64 advance = 0) {
    now += advance;
    LoginTestAccess::tick(*flow);
  }
  /// reach drives the acquired-password path up to the named in-flight
  /// request: start, poll, user or root.
  void reach(const QString &phase) {
    flow->start();
    if (phase == "start")
      return;
    last().finish(200, started);
    tick();
    if (phase == "poll")
      return;
    last().finish(200, granted);
    if (phase == "user")
      return;
    last().finish(200, user);
    require(last().verb == "PROPFIND", "root verification was not requested");
  }
  /// terminal checks the final public state and that later timer steps send
  /// nothing.
  void terminal(const QString &state, const QString &reason,
                const char *message) {
    const auto count = sent.size();
    tick(8 * 86400000LL);
    tick();
    require(flow->status() == state && flow->reason() == reason &&
                sent.size() == count && !published.isEmpty() &&
                published.last() == state,
            message);
  }
};

/// testFlowSuccess proves the scripted transport reaches acceptance, so the
/// refusals below are not harness artifacts.
void testFlowSuccess() {
  Harness h;
  h.reach("root");
  h.last().finish(207, root);
  require(h.flow->status() == "succeeded" && h.flow->uid == "u" &&
              h.flow->rootId == "00000001oc" && h.sent.size() == 4,
          "scripted login did not succeed");
  h.terminal("succeeded", "", "accepted login sent a later request");
}

/// testFlowConstraints keeps 403/423/429 in their own domains at every
/// request, never AUTH_REQUIRED or ENDPOINT_IDENTITY_MISMATCH, and retires an
/// acquired password only once the server delay allows (INV-03, N18).
void testFlowConstraints() {
  for (const auto phase : {"start", "poll", "user", "root"}) {
    for (const auto status : {403, 423, 429}) {
      const QString expected = status == 403   ? "PERMISSION_DENIED"
                               : status == 423 ? "REMOTE_LOCKED"
                                               : "SERVER_THROTTLED";
      const bool owned = QString(phase) == "user" || QString(phase) == "root";
      Harness h;
      h.reach(phase);
      const auto count = h.sent.size();
      h.last().finish(status, {}, "120", QNetworkReply::ContentAccessDenied);
      require(h.throttles == (status == 429 ? QList<qint64>{120000}
                                            : QList<qint64>{}),
              "only a confirmed 429 is persisted, with its own delay");
      if (status != 403 && (owned || QString(phase) == "poll")) {
        // Nothing is sent before the server's own deadline.
        h.tick(119999);
        require(h.sent.size() == count &&
                    h.flow->status() == (owned ? "discarding" : "waiting"),
                "request admitted before Retry-After elapsed");
        h.tick(1);
        require(h.sent.size() == count + 1, "delayed request never resumed");
      }
      if (QString(phase) == "poll" && status != 403) {
        require(h.last().verb == "POST" && h.flow->status() == "waiting",
                "bounded poll delay ended the session");
        continue;
      }
      if (owned) {
        require(h.flow->status() == "discarding" &&
                    h.last().verb == "DELETE" && h.published.last() != "failed",
                "acquired password was not retired after the refusal");
        h.last().finish(200, revoked);
      }
      h.terminal("failed", expected, "constraint was misclassified");
    }
  }
}

/// testFlowRetryAfter covers numeric, HTTP-date, absent, malformed and
/// oversized delays; a delay outlasting the session ends it instead of
/// extending the 20-minute validity.
void testFlowRetryAfter() {
  struct Case {
    QByteArray header;
    qint64 low, high;
    bool waits;
  };
  const auto date = QLocale::c()
                        .toString(QDateTime::currentDateTimeUtc().addSecs(300),
                                  "ddd, dd MMM yyyy HH:mm:ss 'GMT'")
                        .toLatin1();
  for (const auto &[header, low, high, waits] :
       {Case{"120", 120000, 120000, true}, Case{"5", 60000, 60000, true},
        Case{{}, 60000, 60000, true}, Case{"soon", 60000, 60000, true},
        Case{date, 290000, 300000, true},
        Case{"1500", 1500000, 1500000, false},
        Case{"999999999", -1, -1, false}}) {
    Harness h;
    h.reach("poll");
    h.last().finish(429, {}, header);
    require(h.throttles.size() == 1 && h.throttles[0] >= low &&
                h.throttles[0] <= high,
            "Retry-After was not preserved for persistence");
    if (waits) {
      const auto count = h.sent.size();
      h.tick(low - 1);
      require(h.sent.size() == count && h.flow->status() == "waiting",
              "poll resumed early");
      continue;
    }
    h.terminal("failed", "SERVER_THROTTLED",
               "delay beyond the session was waited out or shortened");
  }
}

/// testFlowExpiry never extends validity for a server delay and never
/// accepts a candidate once the session expired.
void testFlowExpiry() {
  {
    Harness h;
    h.reach("poll");
    h.tick(18 * 60 * 1000);
    h.last().finish(404);
    h.tick();
    h.last().finish(429, {}, "70");
    require(h.flow->status() == "waiting", "delay inside the session failed");
    h.tick(120000 - 1);
    require(h.flow->status() == "waiting", "session expired early");
    h.tick(1);
    h.terminal("failed", "LOGIN_SESSION_EXPIRED",
               "server delay extended session validity");
  }
  Harness h;
  h.reach("root");
  auto &pending = h.last();
  h.tick(20 * 60 * 1000);
  require(pending.aborted && h.flow->status() == "discarding" &&
              h.last().verb == "DELETE",
          "expiry kept verifying an acquired candidate");
  h.flow->accept();
  h.flow->accept(true);
  require(h.flow->status() == "discarding", "expired session was accepted");
  h.last().finish(200, revoked);
  h.terminal("failed", "LOGIN_SESSION_EXPIRED", "expiry reason was lost");
}

/// testFlowRevocation keeps a still-owned candidate while its revocation is
/// throttled, even past session expiry, replays only refused DELETEs and
/// reports every unconfirmed outcome explicitly.
void testFlowRevocation() {
  {
    Harness h;
    h.reach("root");
    h.flow->cancel();
    h.last().finish(429, {}, "3600");
    h.flow->cancel();
    const auto count = h.sent.size();
    h.tick(20 * 60 * 1000);
    h.tick(3600000 - 20 * 60 * 1000 - 1);
    require(h.flow->status() == "discarding" && h.sent.size() == count &&
                !h.published.contains("failed") &&
                !h.published.contains("cancelled"),
            "throttled revocation released or replayed the candidate");
    // Only delayed, nothing in flight: recorded for a restart, and cleared
    // again before the DELETE leaves.
    require(h.holds == QList<bool>{true},
            "server-delayed revocation was not recorded as held");
    h.tick(1);
    require(h.last().verb == "DELETE", "refused revocation was never retried");
    require(h.holds == (QList<bool>{true, false}),
            "held record survived into an unanswered DELETE");
    h.last().finish(200, revoked);
    h.terminal("cancelled", "LOGIN_CANCELLED",
               "confirmed revocation lost the cancellation");
  }
  struct Case {
    int status;
    QByteArray header;
    QNetworkReply::NetworkError error;
  };
  for (const auto &[status, header, error] :
       {Case{0, {}, QNetworkReply::RemoteHostClosedError},
        Case{0, {}, QNetworkReply::TimeoutError},
        Case{500, {}, QNetworkReply::InternalServerError},
        Case{403, {}, QNetworkReply::ContentAccessDenied},
        Case{429, "999999999", QNetworkReply::UnknownContentError}}) {
    Harness h;
    h.reach("user");
    h.flow->cancel();
    require(h.last().verb == "DELETE", "cancel did not retire the candidate");
    h.last().finish(status, {}, header, error);
    h.terminal("failed", "APP_PASSWORD_REVOCATION_UNCONFIRMED",
               "uncertain or refused revocation was hidden or replayed");
  }
  {
    // A held record that cannot be cleared must not be followed by a DELETE:
    // a later boot would otherwise send a second one.
    Harness h;
    h.reach("root");
    h.flow->cancel();
    h.last().finish(429, {}, "120");
    h.persistHold = false;
    const auto count = h.sent.size();
    h.tick(120000);
    require(h.sent.size() == count &&
                h.flow->reason() == "APP_PASSWORD_REVOCATION_UNCONFIRMED",
            "DELETE sent although its held record could not be cleared");
  }
  {
    // An immediate revocation was never only delayed: nothing is recorded.
    Harness h;
    h.reach("user");
    h.flow->cancel();
    require(h.last().verb == "DELETE" && h.holds.isEmpty(),
            "in-flight revocation was recorded as held");
    h.last().finish(200, revoked);
  }
  Harness h;
  h.persist = false;
  h.reach("user");
  h.last().finish(429, {}, "120");
  require(h.sent.size() == 3, "request sent after throttle storage failed");
  h.terminal("failed", "APP_PASSWORD_REVOCATION_UNCONFIRMED",
             "uncommitted throttle was treated as a bounded delay");
}

/// testFlowOwnership covers uncertain POSTs, borrowed credentials and an
/// owner destroyed with a request in flight.
void testFlowOwnership() {
  for (const auto phase : {"start", "poll"}) {
    Harness h;
    h.reach(phase);
    h.last().finish(0, {}, {}, QNetworkReply::RemoteHostClosedError);
    h.terminal("failed", "NETWORK_UNAVAILABLE", "uncertain POST was replayed");
  }
  {
    Harness h;
    h.flow->verifyCredentials("u", "p"); // borrowed: never revoked here
    h.last().finish(429, {}, "120");
    require(h.sent.size() == 1 && h.sent[0]->verb == "GET" &&
                h.holds.isEmpty(),
            "borrowed credential was revoked or recorded as held");
    h.terminal("failed", "SERVER_THROTTLED", "borrowed refusal misclassified");
  }
  Harness h;
  h.reach("root");
  auto &pending = h.last();
  const auto publications = h.published.size();
  h.flow.reset();
  require(pending.aborted && h.published.size() == publications &&
              h.sent.size() == 4,
          "destroyed owner left a live request or received a callback");
}

/// awaitFlow pumps Qt until a bounded login transition completes without logging URLs or responses.
void awaitFlow(LoginFlow &flow, bool waiting = false) {
    if (flow.status() == "failed" || flow.status() == "cancelled" || flow.status() == "succeeded" || (waiting && flow.status() == "waiting")) return;
    QEventLoop loop; QTimer deadline; deadline.setSingleShot(true);
    bool completed = false;
    flow.changed = [&](LoginFlow &value) {
        if (value.status() == "failed" || value.status() == "cancelled" || value.status() == "succeeded" || (waiting && value.status() == "waiting")) { completed = true; loop.quit(); }
        return true;
    };
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit); deadline.start(70000); loop.exec();
    flow.changed = {};
    require(completed, "real login request deadline exceeded");
}
/// testLiveNamespace validates approved credentials and compares both supplied origins read-only (INV-03/15).
void testLiveNamespace() {
    const auto primary = qEnvironmentVariable("NEXTCLOUD_TEST_URL");
    if (primary.isEmpty()) { std::puts("SKIP real namespace checks: local test environment absent"); return; }
    const auto user = qEnvironmentVariable("NEXTCLOUD_TEST_USER"), password = qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD");
    require(!user.isEmpty() && !password.isEmpty(), "real test credentials missing");
    QString uid, root;
    QStringList addresses{primary}; const auto secondary = qEnvironmentVariable("NEXTCLOUD_TEST_ALTERNATE_URL"); if (!secondary.isEmpty()) addresses.append(secondary);
    for (const auto &address : addresses) {
        VaultFixture fixture;
        LoginFlow flow(canonicalServer(address), uuid(), uuid());
        flow.saveCredential = [&](const QString &name, const QString &secret) { try { writeCredential(fixture.reference, name, secret); return readCredential(fixture.reference) == secret; } catch (...) { return false; } };
        flow.verifyCredentials(user, password); awaitFlow(flow);
        if (flow.status() != "succeeded") { std::fprintf(stderr, "Namespace check failed: %s\n", qPrintable(flow.reason())); throw std::runtime_error("Real namespace verification failed"); }
        if (uid.isEmpty()) { uid = flow.uid; root = flow.rootId; }
        else require(uid == flow.uid && root == flow.rootId, "approved endpoints have different account/root identities");
    }
    LoginFlow browser(canonicalServer(primary), uuid(), uuid()); browser.start(); awaitFlow(browser, true);
    require(browser.status() == "waiting" && !browser.loginUrl.isEmpty(), "real Login Flow v2 did not start");
    browser.cancel(); require(browser.status() == "cancelled", "login cancellation failed");
    std::puts("PASS real credential vault; approved endpoint UID/root match; Login Flow v2 start/cancel (browser consent not exercised)");
}
/// testSecondAccount admits a second account and refuses the same user on the
/// same server a second time, retiring the password it acquired (PLAN §2.1).
void testSecondAccount() {
  QTemporaryDir dir;
  require(dir.isValid(), "test directory missing");
  Control c(dir.filePath("control.sqlite"));
  const auto existing = uuid();
  const auto connection = uuid();
  auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
  db.setDatabaseName(dir.filePath("control.sqlite"));
  require(db.open(), "fixture database unavailable");
  {
    QSqlQuery q(db);
    q.prepare("INSERT INTO accounts(id,session_id,operation_id,endpoint_id,"
              "base_url,login_name,uid,root_id,credential_ref,state) VALUES(?,?,"
              "?,?,'https://other.example.invalid/','u','u','00000001oc','"
              "unused','ready')");
    for (const auto &value : {existing, uuid(), uuid(), uuid()})
      q.addBindValue(value);
    require(q.exec(), "existing account fixture failed");
  }
  // Both scripted logins share one transport and clock that outlive them.
  QObject transport;
  QList<QPointer<Reply>> sent;
  qint64 now = 1000;
  // user names the server-side user the scripted login proves.
  const auto login = [&](const QByteArray &user, const QByteArray &rootBody) {
    const auto accepted = call(c, "accounts.login.start",
                              {{"operationKey", uuid()},
                               {"canonicalBaseUrl", "https://example.invalid/"}});
    const auto operation =
        accepted.value("result").toObject().value("operationId").toString();
    require(!operation.isEmpty(), "a second account's login was refused");
    QSqlQuery q(db);
    q.prepare("SELECT session_id,credential_ref FROM accounts WHERE "
              "operation_id=?");
    q.addBindValue(operation);
    require(q.exec() && q.next(), "second login reservation missing");
    const auto session = q.value(0).toString(), reference = q.value(1).toString();
    q.finish();
    auto &flow = ReauthTestAccess::flow(c, session);
    LoginTestAccess::attach(
        flow, [&] { return now; },
        [&](const QNetworkRequest &request, const QByteArray &verb,
            const QByteArray &) {
          auto *reply = new Reply(request, verb, &transport);
          sent.append(reply);
          return reply;
        });
    flow.start();
    sent.last()->finish(200, started);
    LoginTestAccess::tick(flow);
    sent.last()->finish(200, granted);
    sent.last()->finish(200, user);
    sent.last()->finish(207, rootBody);
    if (flow.status() == "discarding") {
      require(sent.last()->verb == "DELETE",
              "a refused duplicate kept its acquired password");
      sent.last()->finish(200, revoked);
    }
    const auto receipt = call(c, "operations.get", {{"operationId", operation}})
                             .value("result")
                             .toObject();
    return std::make_pair(receipt, reference);
  };
  const auto [duplicate, reference] = login(user, root);
  require(duplicate.value("state") == "failed" &&
              duplicate.value("error").toObject().value("data").toObject().value(
                  "reasonCode") == "ACCOUNT_EXISTS",
          "the same user on the same server was added twice");
  require(!findCredential(reference), "a refused duplicate kept its vault slot");
  const QByteArray other = R"({"ocs":{"meta":{"status":"ok"},"data":{"id":"v"}}})";
  QByteArray otherRoot = root;
  otherRoot.replace("files/u/", "files/v/").replace("00000001oc", "00000002oc");
  const auto [second, secondReference] = login(other, otherRoot);
  require(second.value("state") == "succeeded",
          "a second account on the same server was refused");
  require(call(c, "accounts.list").value("result").toArray().size() == 2,
          "two ready accounts were not both listed");
  removeCredential(secondReference);
  db.close();
  db = {};
  QSqlDatabase::removeDatabase(connection);
}
}

/// main runs login/credential contracts and opt-in read-only real-server checks, with owned-resource cleanup.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try { testURLBoundary(); testFlowSuccess(); testFlowConstraints(); testFlowRetryAfter(); testFlowExpiry(); testFlowRevocation(); testFlowOwnership(); testVaultAndRecovery(); testCancelReplay(); testSecondAccount(); testLiveNamespace(); std::puts("PASS login boundary and crash recovery"); return 0; }
    catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
