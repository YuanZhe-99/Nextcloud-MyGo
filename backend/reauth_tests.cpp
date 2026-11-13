// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "namespace.h"
#include "network_acceptance.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <stdexcept>

/// ReauthTestAccess supplies borrowed credentials without adding a production
/// IPC bypass.
struct ReauthTestAccess {
  /// flow locates the owned test session while retaining normal Control
  /// callbacks.
  static LoginFlow &flow(Control &control, const QString &session) {
    return *control.logins_.at(session);
  }
  /// stopped observes the fail-closed storage stop and any shutdown request.
  static bool stopped(const Control &control) {
    return control.storageFailed_ || control.shutdownRequested();
  }
};

/// LoginTestAccess marks a renewal's namespace verified without a server, so
/// the real installation and reload run next.
struct LoginTestAccess {
  /// verify sets the identity a verified Login Flow v2 would have produced
  /// and hands the flow to its owner.
  static void verify(LoginFlow &flow, const QString &user, const QString &uid,
                     const QString &root) {
    flow.loginName = user;
    flow.uid = uid;
    flow.rootId = root;
    flow.rootVerified_ = true;
    flow.state_ = "verifying";
    flow.verified(flow);
  }
};

namespace {
/// require preserves contract assertions in optimized builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// trustIsolatedCA installs only the user-acknowledged public fixture root in
/// this test process; normal chain/hostname checks remain enabled.
void trustIsolatedCA() {
  const auto fingerprint = qEnvironmentVariable("SYNCD_TEST_CA_SHA1").toUpper();
  require(QRegularExpression("^[0-9A-F]{40}$").match(fingerprint).hasMatch(),
          "owned fixture root acknowledgement missing");
  QFile file(qEnvironmentVariable("SYNCD_TEST_CA_CERT"));
  require(file.open(QIODevice::ReadOnly) && file.size() <= 65536,
          "owned fixture public root unavailable");
  const auto certificates =
      QSslCertificate::fromData(file.readAll(), QSsl::Pem);
  require(certificates.size() == 1 && certificates.first().isSelfSigned() &&
              certificates.first()
                      .digest(QCryptographicHash::Sha1)
                      .toHex()
                      .toUpper() == fingerprint.toLatin1() &&
              certificates.first().effectiveDate() <=
                  QDateTime::currentDateTimeUtc() &&
              certificates.first().expiryDate() >
                  QDateTime::currentDateTimeUtc(),
          "owned fixture root does not match the acknowledged certificate");
  auto configuration = QSslConfiguration::defaultConfiguration();
  auto roots = configuration.caCertificates();
  roots.append(certificates.first());
  configuration.setCaCertificates(roots);
  QSslConfiguration::setDefaultConfiguration(configuration);
}
/// uuid returns a unique fixture entity or operation identity.
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
/// sql executes fixture setup without allowing silent SQL failures.
void sql(QSqlDatabase db, const QString &text) {
  QSqlQuery q(db);
  require(q.exec(text), "reauthentication fixture SQL failed");
}
/// call sends an actual operation envelope with optimistic revision and a
/// retained key.
QJsonObject call(Control &control, const QString &method,
                 QJsonObject params = {}) {
  if (!params.contains("operationKey"))
    params.insert("operationKey", uuid());
  if (!params.contains("expectedRevision"))
    params.insert("expectedRevision", control.projection().value("revision"));
  bool negotiated = true, subscribed = false;
  return control
      .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                            {"id", 1},
                            {"method", method},
                            {"params", params}},
                negotiated, subscribed)
      .toObject();
}
/// VaultSlots cleans only the uniquely reserved fixture credentials on every
/// exit path.
struct VaultSlots {
  QString stable, candidate, previous;
  /// ~VaultSlots makes native cleanup failure an explicit failed test, not a
  /// secret residue.
  ~VaultSlots() {
    try {
      for (const auto &key : {stable, candidate, previous})
        if (!key.isEmpty())
          removeCredential(key);
    } catch (...) {
      std::fputs("Owned renewal credential cleanup failed\n", stderr);
      std::abort();
    }
  }
};

/// lifetimeRoot accepts only a uniquely named, precreated disposable lifetime
/// profile.
QString lifetimeRoot() {
  const auto root = QDir::cleanPath(QDir::fromNativeSeparators(
      qEnvironmentVariable("SYNCD_REAUTH_LIFETIME_ROOT")));
  require(QDir::isAbsolutePath(root) && QDir(root).exists() &&
              QRegularExpression("^reauth-lifetime-[a-f0-9]{32}$")
                  .match(QFileInfo(root).fileName())
                  .hasMatch(),
          "owned lifetime profile missing");
  return root;
}

/// manifest reserves known native-key cleanup before borrowed credentials can
/// be written or a child can crash.
void manifest(const QString &root, const QStringList &keys) {
  QJsonArray values;
  for (const auto &key : keys)
    values.append(key);
  QFile file(QDir(root).filePath("owned-refs.json"));
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "native cleanup manifest unavailable");
  require(file.write(QJsonDocument(values).toJson(QJsonDocument::Compact)) >
                  0 &&
              file.flush(),
          "native cleanup reservation failed");
}

/// cleanupManifest removes only this profile's validated remote-content
/// reservation and native keys, even after a child crash.
void cleanupManifest() {
  const auto root = lifetimeRoot();
  QFile file(QDir(root).filePath("owned-refs.json"));
  if (!file.exists())
    return;
  require(file.open(QIODevice::ReadOnly), "native cleanup manifest unreadable");
  const auto document = QJsonDocument::fromJson(file.readAll());
  require(document.isArray() && document.array().size() <= 32,
          "invalid cleanup manifest");
  const auto profile =
      QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256)
          .toHex()
          .left(24);
  const QString id =
      "[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}";
  const QRegularExpression owned(
      "^Nextcloud-MyGo/" + QString::fromLatin1(profile) + "/" + id +
      "/app-password(?:/reauthentication/" + id + "/(?:candidate|previous))?$");
  for (const auto &value : document.array()) {
    require(value.isString() && owned.match(value.toString()).hasMatch(),
            "cleanup reference escaped owned profile");
  }
  if (!document.array().isEmpty())
    cleanupNetworkContent(root, document.array().first().toString());
  for (const auto &value : document.array())
    removeCredential(value.toString());
}

/// waitFor pumps real Qt events with a bounded timer, without replacing
/// production lifecycle callbacks.
void waitFor(const std::function<bool()> &condition) {
  QEventLoop loop;
  QTimer timer, deadline;
  deadline.setSingleShot(true);
  timer.setInterval(10);
  QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
    if (condition())
      loop.quit();
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  timer.start();
  deadline.start(70000);
  loop.exec();
  require(condition(), "renewal lifetime transition deadline");
}

/// heldRevocationRestart proves on a real server that a restarted daemon
/// retires, once, the acquired password a previous process left held, and
/// that the server then rejects it (N18, INV-13). Notes: needs an isolated
/// server whose fixture password can issue an app password; that fixture
/// password itself is never revoked.
void heldRevocationRestart() {
  const auto root = lifetimeRoot(),
             path = QDir(root).filePath("control.sqlite"), connection = uuid();
  // No relay is involved: an isolated fixture is reached at its direct origin.
  const auto base = canonicalServer(
      qEnvironmentVariable("NEXTCLOUD_TEST_CLEANUP_URL",
                           qEnvironmentVariable("NEXTCLOUD_TEST_URL")));
  const auto user = qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
             password = qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD");
  require(!user.isEmpty() && !password.isEmpty(),
          "owned held-revocation credentials missing");
  QString reference, operation, issued;
  {
    Control control(path);
    operation = call(control, "accounts.login.start",
                     {{"canonicalBaseUrl", base.toString()}})
                    .value("result")
                    .toObject()
                    .value("operationId")
                    .toString();
    require(!operation.isEmpty(), "owned held login reservation failed");
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(path);
    require(db.open(), "owned held fixture DB unavailable");
    QSqlQuery q(db);
    require(q.exec("SELECT id,credential_ref FROM accounts") && q.next(),
            "owned held reservation missing");
    const auto account = q.value(0).toString();
    reference = q.value(1).toString();
    q.finish();
    manifest(root, {reference});
    // The fixture password only signs the request that issues the password
    // the daemon then owns; it never stays in the slot.
    writeCredential(reference, user, password);
    issued = readServerJson(base, "ocs/v2.php/core/getapppassword?format=json",
                            user, reference)
                 .value("ocs")
                 .toObject()
                 .value("data")
                 .toObject()
                 .value("apppassword")
                 .toString();
    require(!issued.isEmpty() && issued != password,
            "owned app password was not issued");
    writeCredential(reference, user, issued);
    // The state a process leaves when the server delayed the DELETE of a
    // password it acquired and it ends before the delay does.
    q.prepare("UPDATE accounts SET login_name=? WHERE id=?");
    q.addBindValue(user);
    q.addBindValue(account);
    require(q.exec(), "owned held login name failed");
    q.prepare("INSERT INTO held_revocations VALUES(?)");
    q.addBindValue(account);
    require(q.exec(), "owned held record failed");
    q.finish();
    db.close();
  }
  QSqlDatabase::removeDatabase(connection);
  const auto accepted = [&] {
    try {
      readServerJson(base, "ocs/v2.php/cloud/user?format=json", user,
                     reference);
      return QStringLiteral("accepted");
    } catch (const std::exception &error) {
      return QString::fromUtf8(error.what());
    }
  };
  require(accepted() == "accepted", "owned issued password never worked");
  Control restarted(path);
  const auto outcome = [&] {
    return call(restarted, "operations.get", {{"operationId", operation}})
        .value("result")
        .toObject()
        .value("error")
        .toObject()
        .value("data")
        .toObject()
        .value("reasonCode")
        .toString();
  };
  require(findCredential(reference) && outcome() == "LOGIN_SESSION_EXPIRED",
          "restart released the held password or misreported its login");
  QEventLoop loop;
  QTimer poll, deadline;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (!findCredential(reference))
      loop.quit();
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start(250);
  deadline.start(140000);
  loop.exec();
  require(
      !findCredential(reference) && outcome() == "LOGIN_SESSION_EXPIRED" &&
          call(restarted, "accounts.list").value("result").toArray().isEmpty(),
      "restart did not confirm the held revocation");
  writeCredential(reference, user, issued);
  const auto after = accepted();
  removeCredential(reference);
  require(after == "AUTH_REQUIRED",
          "server still accepts the password the restart reported revoked");
  std::puts("PASS real held revocation sent once by the restarted daemon "
            "after the delay floor, confirmed by the server, password "
            "rejected afterwards");
}

/// replacedDuringVerification proves cancellation/replacement cannot free a
/// flow still used by a nested DAV check (INV-03/06/12).
void replacedDuringVerification() {
  const auto root = lifetimeRoot(),
             path = QDir(root).filePath("control.sqlite"), connection = uuid();
  const auto base = qEnvironmentVariable("NEXTCLOUD_TEST_URL"),
             user = qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
             password = qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD");
  require(!base.isEmpty() && !user.isEmpty() && !password.isEmpty(),
          "approved read-only lifetime credentials missing");
  Control control(path);
  call(control, "accounts.login.start", {{"canonicalBaseUrl", base}});
  auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
  db.setDatabaseName(path);
  require(db.open(), "owned lifetime DB unavailable");
  QSqlQuery q(db);
  require(q.exec("SELECT id,session_id,credential_ref FROM accounts") &&
              q.next(),
          "owned lifetime reservation missing");
  const auto account = q.value(0).toString(), initial = q.value(1).toString(),
             stable = q.value(2).toString();
  q.finish();
  QStringList keys{stable};
  manifest(root, keys);
  ReauthTestAccess::flow(control, initial).verifyCredentials(user, password);
  waitFor([&] {
    return call(control, "accounts.login.status", {{"loginSessionId", initial}})
               .value("result")
               .toObject()
               .value("state") == "succeeded";
  });
  call(control, "settings.update", {{"allPaused", true}});
  q.prepare("INSERT INTO "
            "folders(id,account_id,local_path,remote_path,local_identity,"
            "remote_identity,journal_path,paused) "
            "VALUES('owned-folder',?,'owned-root',?,'owned-local','missing-"
            "owned-root',?,1)");
  q.addBindValue(account);
  q.addBindValue("/nextcloud-mygo-lifetime-" + uuid());
  q.addBindValue(QDir(root).filePath("journals/owned-folder.sqlite"));
  require(q.exec(), "lifetime mapped-root fixture failed");
  q.finish();
  call(control, "accounts.reauthenticate", {{"accountId", account}});
  require(q.exec("SELECT session_id FROM reauthentication") && q.next(),
          "lifetime renewal session missing");
  const auto session = q.value(0).toString();
  q.finish();
  keys.append(stable + "/reauthentication/" + session + "/candidate");
  keys.append(stable + "/reauthentication/" + session + "/previous");
  manifest(root, keys);
  auto &flow = ReauthTestAccess::flow(control, session);
  QPointer<LoginFlow> previous(&flow);
  auto verified = flow.verified;
  bool replaced = false, retained = false, callbackFailed = false;
  QString replacement;
  flow.verified = [&, verified](LoginFlow &value) {
    verified(value); // enqueue installation first; the next timer then runs
                     // inside its nested DAV event loop
    QTimer::singleShot(0, QCoreApplication::instance(), [&] {
      try {
        require(!call(control, "accounts.login.cancel",
                      {{"loginSessionId", session}})
                     .contains("error"),
                "in-flight renewal cancellation failed");
        require(
            !call(control, "accounts.reauthenticate", {{"accountId", account}})
                 .contains("error"),
            "replacement renewal could not start");
        require(q.exec("SELECT session_id FROM reauthentication") && q.next(),
                "replacement session missing");
        replacement = q.value(0).toString();
        q.finish();
        retained = !previous.isNull();
        replaced = true;
      } catch (...) {
        callbackFailed = true;
        replaced = true;
      }
    });
  };
  flow.verifyCredentials(user, password); // borrowed: this fixture must never
                                          // revoke the approved test credential
  waitFor([&] { return replaced && previous.isNull(); });
  require(!callbackFailed && retained,
          "renewal flow was destroyed while nested verification still used it");
  const auto status =
      call(control, "accounts.login.status", {{"loginSessionId", replacement}})
          .value("result")
          .toObject();
  require(
      status.value("state") != "failed" &&
          status.value("state") != "cancelled" && !control.shutdownRequested(),
      "old verification failed a replacement session or terminated the daemon");
  require(readCredential(stable) == password,
          "cancelled verification replaced the original credential");
  require(
      !call(control, "accounts.login.cancel", {{"loginSessionId", replacement}})
           .contains("error"),
      "replacement session cleanup failed");
  q.finish();
  db.close();
  db = {};
  QSqlDatabase::removeDatabase(connection);
  std::puts("PASS retained renewal lifetime across cancel/replacement during "
            "real read-only DAV verification");
}
/// interrupted proves rollback/roll-forward preserve account, journal identity
/// and vault ownership (INV-02/03/08/12/13).
void interrupted(const QString &phase, bool previousPresent = true) {
  QTemporaryDir root;
  require(root.isValid(), "temporary profile unavailable");
  VaultSlots vault;
  const auto path = root.filePath("control.sqlite"), connection = uuid();
  const auto journal = root.filePath("journals/owned-folder.sqlite");
  QString account, originalSession, renewal, operation;
  auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
  db.setDatabaseName(path);
  {
    Control control(path);
    call(control, "accounts.login.start",
         {{"canonicalBaseUrl", "https://primary.example.invalid/"}});
    require(db.open(), "fixture profile open failed");
    QSqlQuery q(db);
    require(q.exec("SELECT id,session_id,credential_ref FROM accounts") &&
                q.next(),
            "initial owned reservation missing");
    account = q.value(0).toString();
    originalSession = q.value(1).toString();
    vault.stable = q.value(2).toString();
    q.finish();
    sql(db, "UPDATE accounts SET "
            "state='ready',uid='owned-uid',root_id='owned-home',login_name='"
            "owned-user'");
    if (previousPresent)
      writeCredential(vault.stable, "owned-user", "owned-old-secret");
    if (phase == "waiting") {
      sql(db, "UPDATE accounts SET credential_ref='unowned-reference'");
      require(call(control, "accounts.reauthenticate", {{"accountId", account}})
                      .value("error")
                      .toObject()
                      .value("data")
                      .toObject()
                      .value("reasonCode") == "AUTH_REQUIRED",
              "renewal trusted an arbitrary stored credential reference");
      q.prepare("UPDATE accounts SET credential_ref=?");
      q.addBindValue(vault.stable);
      require(q.exec(), "owned reference restore failed");
      q.finish();
    }
    QJsonObject params{
        {"operationKey", uuid()},
        {"expectedRevision", control.projection().value("revision")},
        {"accountId", account}};
    const auto accepted = call(control, "accounts.reauthenticate", params);
    operation =
        accepted.value("result").toObject().value("operationId").toString();
    require(!operation.isEmpty() &&
                call(control, "accounts.reauthenticate", params) == accepted,
            "renewal retry created another operation");
    require(q.exec("SELECT session_id FROM reauthentication") && q.next(),
            "durable renewal reservation missing");
    renewal = q.value(0).toString();
    q.finish();
    vault.candidate =
        vault.stable + "/reauthentication/" + renewal + "/candidate";
    vault.previous =
        vault.stable + "/reauthentication/" + renewal + "/previous";
    require(call(control, "accounts.reauthenticate", {{"accountId", account}})
                    .value("error")
                    .toObject()
                    .value("data")
                    .toObject()
                    .value("reasonCode") == "QUIESCE_STALLED",
            "concurrent renewal accepted");
    writeCredential(vault.candidate, "owned-user", "owned-new-secret");
    if (phase == "cancelled") {
      require(
          !call(control, "accounts.login.cancel", {{"loginSessionId", renewal}})
               .contains("error"),
          "renewal cancellation rejected");
      require(readCredential(vault.stable) == "owned-old-secret" &&
                  !findCredential(vault.candidate),
              "cancel replaced old credential or leaked candidate");
    } else {
      if (previousPresent && phase != "waiting" && phase != "discarding")
        writeCredential(vault.previous, "owned-user",
                        phase == "installed-same" ? "owned-new-secret"
                                                  : "owned-old-secret");
      if (phase == "prepared" || phase == "installed" ||
          phase == "installed-same")
        writeCredential(vault.stable, "owned-user", "owned-new-secret");
      q.prepare("UPDATE reauthentication SET phase=?,previous_present=?");
      q.addBindValue(phase == "installed-same" ? QStringLiteral("installed")
                                               : phase);
      q.addBindValue(previousPresent);
      require(q.exec(), "crash phase fixture failed");
      q.finish();
    }
    q.prepare("INSERT INTO "
              "folders(id,account_id,local_path,remote_path,local_identity,"
              "remote_identity,journal_path,paused) SELECT "
              "'owned-folder',id,'owned-root','/"
              "owned','owned-local-id','owned-remote-id',?,1 FROM "
              "accounts");
    q.addBindValue(journal);
    require(q.exec(), "mapped journal fixture failed");
    q.finish();
    if (phase == "waiting") {
      sql(db, "UPDATE folders SET paused=0,status='upToDate'");
      require(call(control, "folders.list")
                      .value("result")
                      .toArray()[0]
                      .toObject()
                      .value("state") == "waitingSignIn",
              "pending renewal projected stale green status");
      sql(db, "UPDATE folders SET paused=1");
    }
    db.close();
  }
  {
    Control restart(path);
    const auto accounts =
        call(restart, "accounts.list").value("result").toArray();
    require(accounts.size() == 1 &&
                accounts[0].toObject().value("id") == account,
            "renewal restart replaced or removed account");
    const auto folders =
        call(restart, "folders.list").value("result").toArray();
    require(folders.size() == 1 &&
                folders[0].toObject().value("userPaused").toBool() &&
                folders[0].toObject().value("state") == "paused",
            "renewal restart lost mapping or user pause");
    require(call(restart, "accounts.login.status",
                 {{"loginSessionId", originalSession}})
                    .value("result")
                    .toObject()
                    .value("accountId") == account,
            "renewal changed original account session identity");
    const auto status =
        call(restart, "accounts.login.status", {{"loginSessionId", renewal}})
            .value("result")
            .toObject();
    if (phase == "installed-same") {
      QCoreApplication::processEvents();
      const auto complete =
          call(restart, "accounts.login.status", {{"loginSessionId", renewal}})
              .value("result")
              .toObject();
      require(!restart.shutdownRequested() &&
                  complete.value("state") == "succeeded" &&
                  complete.value("accountId") == account,
              "unavailable root undid committed credentials or caused startup "
              "termination");
      require(readCredential(vault.stable) == "owned-new-secret" &&
                  !findCredential(vault.previous) &&
                  !findCredential(vault.candidate),
              "identical prior credential was revoked or reserved vault "
              "cleanup lost");
    } else if (phase == "installed") {
      require(
          readCredential(vault.stable) == "owned-new-secret" &&
              findCredential(vault.previous).has_value(),
          "committed replacement was rolled back or cleanup reservation lost");
      require(status.value("state") == "verifying",
              "installed cleanup prematurely reported complete");
    } else {
      if (previousPresent)
        require(
            readCredential(vault.stable) == "owned-old-secret",
            "interrupted replacement failed to restore previous credential");
      else
        require(!findCredential(vault.stable),
                "rollback invented a previously absent credential");
      require(!findCredential(vault.candidate) &&
                  !findCredential(vault.previous),
              "interrupted renewal leaked temporary credentials");
      require(status.value("state") == "failed" ||
                  status.value("state") == "cancelled",
              "interrupted renewal became successful");
      require(call(restart, "operations.get", {{"operationId", operation}})
                      .value("result")
                      .toObject()
                      .value("state") == "failed",
              "interrupted start receipt replayed");
    }
    require(db.open(), "restart fixture DB unavailable");
    QSqlQuery q(db);
    require(q.exec("SELECT remote_identity,journal_path FROM folders") &&
                q.next() && q.value(0) == "owned-remote-id" &&
                q.value(1) == journal,
            "renewal changed mapped root or journal");
    q.finish();
    db.close();
  }
  db = {};
  QSqlDatabase::removeDatabase(connection);
  for (const auto &name : {path, path + "-wal"}) {
    QFile file(name);
    if (!file.exists())
      continue;
    require(file.open(QIODevice::ReadOnly), "profile audit read failed");
    const auto bytes = file.readAll();
    require(!bytes.contains("owned-old-secret") &&
                !bytes.contains("owned-new-secret"),
            "renewal persisted an application password");
  }
}
/// removalDuringRenewal removes an account while its renewal waits for the
/// browser: the renewal ends, every owned vault slot goes and the removal
/// reports the unconfirmed server revocation (INV-03/12).
/// Notes: the fixture origin never resolves, so no request reaches a server.
void removalDuringRenewal() {
  QTemporaryDir root;
  require(root.isValid(), "temporary profile unavailable");
  VaultSlots vault;
  const auto path = root.filePath("control.sqlite"), connection = uuid();
  auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
  db.setDatabaseName(path);
  {
    Control control(path);
    call(control, "accounts.login.start",
         {{"canonicalBaseUrl", "https://primary.example.invalid/"}});
    require(db.open(), "fixture profile open failed");
    QSqlQuery q(db);
    require(q.exec("SELECT id,credential_ref FROM accounts") && q.next(),
            "owned reservation missing");
    const auto account = q.value(0).toString();
    vault.stable = q.value(1).toString();
    q.finish();
    sql(db, "UPDATE accounts SET state='ready',uid='owned-uid',root_id='owned-"
            "home',login_name='owned-user'");
    writeCredential(vault.stable, "owned-user", "owned-old-secret");
    require(!call(control, "accounts.reauthenticate", {{"accountId", account}})
                 .contains("error"),
            "renewal fixture refused");
    require(q.exec("SELECT session_id FROM reauthentication") && q.next(),
            "durable renewal reservation missing");
    const auto renewal = q.value(0).toString();
    q.finish();
    vault.candidate =
        vault.stable + "/reauthentication/" + renewal + "/candidate";
    vault.previous =
        vault.stable + "/reauthentication/" + renewal + "/previous";
    writeCredential(vault.candidate, "owned-user", "owned-new-secret");
    const auto operation =
        call(control, "accounts.remove", {{"accountId", account}})
            .value("result")
            .toObject()
            .value("operationId")
            .toString();
    require(!operation.isEmpty(), "removal during a renewal was refused");
    QJsonObject receipt;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 60000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      receipt = call(control, "operations.get", {{"operationId", operation}})
                    .value("result")
                    .toObject();
      if (receipt.value("state") != "running")
        break;
    }
    require(receipt.value("state") == "succeeded" &&
                !receipt.value("result")
                     .toObject()
                     .value("serverRevocationConfirmed")
                     .toBool(),
            "removal during a renewal did not finish truthfully");
    require(!findCredential(vault.stable) && !findCredential(vault.candidate) &&
                !findCredential(vault.previous),
            "removal left an owned vault slot");
    require(q.exec("SELECT COUNT(*) FROM accounts") && q.next() &&
                q.value(0).toInt() == 0,
            "removal kept the account");
    q.finish();
    require(
        call(control, "accounts.login.status", {{"loginSessionId", renewal}})
                .value("result")
                .toObject()
                .value("state") != "succeeded",
        "a removed account's renewal reported success");
  }
  db.close();
  db = {};
  QSqlDatabase::removeDatabase(connection);
}
/// refusedReload makes the reload after a durable installation refuse with
/// QUIESCE_STALLED (an endpoint check is running) and proves SyncD keeps
/// serving, the renewal stays `installed` with the new credential and the old
/// one reserved, and the reload completes once the check ends (INV-03/12/13).
/// Inputs: atStartup restarts in `installed` first: the refusal must not stop
/// every restart.
/// Notes: regression for a refused reload read as a storage failure.
void refusedReload(bool atStartup) {
  QTemporaryDir root;
  require(root.isValid(), "temporary profile unavailable");
  VaultSlots vault;
  const auto path = root.filePath("control.sqlite"), connection = uuid();
  auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
  db.setDatabaseName(path);
  QString account, renewal;
  const auto phase = [&] {
    QSqlQuery q(db);
    q.prepare("SELECT phase FROM reauthentication WHERE session_id=?");
    q.addBindValue(renewal);
    return q.exec() && q.next() ? q.value(0).toString() : QString{};
  };
  const auto refuse = [&](bool on) {
    QSqlQuery q(db);
    q.prepare(on ? "INSERT INTO endpoints(id,account_id,name,base_url,approved,"
                   "state,expires,operation_id) VALUES('busy-check',?,'Busy',"
                   "'https://busy.example.invalid/',0,'probing','2999-01-01',?)"
                 : "DELETE FROM endpoints WHERE id='busy-check' AND "
                   "account_id=?");
    q.addBindValue(account);
    if (on)
      q.addBindValue(uuid());
    require(q.exec(), "endpoint check fixture failed");
  };
  // Holds on Qt events for a fixed span: long enough for a refused reload to
  // have stopped SyncD, shorter than its first one-second retry.
  const auto settle = [] {
    QElapsedTimer span;
    span.start();
    while (span.elapsed() < 500)
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  };
  const auto installedKept = [&](Control &control, const char *message) {
    require(!ReauthTestAccess::stopped(control) && phase() == "installed" &&
                readCredential(vault.stable) == "owned-new-secret" &&
                findCredential(vault.previous).has_value() &&
                call(control, "accounts.list").value("result").toArray().size() ==
                    1,
            message);
  };
  const auto completed = [&](Control &control, const char *message) {
    refuse(false);
    waitFor([&] { return phase() == "succeeded"; });
    require(!ReauthTestAccess::stopped(control) && phase() == "succeeded" &&
                readCredential(vault.stable) == "owned-new-secret" &&
                !findCredential(vault.previous) &&
                !findCredential(vault.candidate),
            message);
  };
  {
    Control control(path);
    call(control, "accounts.login.start",
         {{"canonicalBaseUrl", "https://primary.example.invalid/"}});
    require(db.open(), "fixture profile open failed");
    QSqlQuery q(db);
    require(q.exec("SELECT id,credential_ref FROM accounts") && q.next(),
            "owned reservation missing");
    account = q.value(0).toString();
    vault.stable = q.value(1).toString();
    q.finish();
    sql(db, "UPDATE accounts SET state='ready',uid='owned-uid',root_id='owned-"
            "home',login_name='owned-user'");
    writeCredential(vault.stable, "owned-user", "owned-old-secret");
    require(!call(control, "accounts.reauthenticate", {{"accountId", account}})
                 .contains("error"),
            "renewal fixture refused");
    require(q.exec("SELECT session_id FROM reauthentication") && q.next(),
            "durable renewal reservation missing");
    renewal = q.value(0).toString();
    q.finish();
    vault.candidate =
        vault.stable + "/reauthentication/" + renewal + "/candidate";
    vault.previous =
        vault.stable + "/reauthentication/" + renewal + "/previous";
    writeCredential(vault.candidate, "owned-user", "owned-new-secret");
    if (!atStartup) {
      refuse(true);
      LoginTestAccess::verify(ReauthTestAccess::flow(control, renewal),
                              "owned-user", "owned-uid", "owned-home");
      waitFor([&] { return phase() == "installed"; });
      settle();
      installedKept(control, "a refused reload after installation stopped "
                             "SyncD or undid the installed credential");
      completed(control, "the refused reload did not complete once the "
                         "endpoint check ended");
    } else {
      // The state a refused reload leaves when SyncD exits before cleanup.
      writeCredential(vault.previous, "owned-user", "owned-old-secret");
      writeCredential(vault.stable, "owned-user", "owned-new-secret");
      sql(db, "UPDATE reauthentication SET phase='installed',"
              "previous_present=1");
      q.prepare("INSERT INTO folders(id,account_id,local_path,remote_path,"
                "local_identity,remote_identity,journal_path,paused) VALUES("
                "'owned-folder',?,'owned-root','/owned','owned-local-id',"
                "'owned-remote-id',?,0)");
      q.addBindValue(account);
      q.addBindValue(root.filePath("journals/owned-folder.sqlite"));
      require(q.exec(), "mapped folder fixture failed");
      q.finish();
    }
  }
  if (atStartup) {
    Control restart(path);
    refuse(true); // Startup recovery has already settled interrupted checks.
    waitFor([&] {
      QSqlQuery q(db);
      return q.exec("SELECT reason FROM folders") && q.next() &&
             q.value(0) == "QUIESCE_STALLED";
    });
    settle();
    installedKept(restart, "a refused reload at startup stopped SyncD or "
                           "undid the installed credential");
    require(call(restart, "folders.list")
                    .value("result")
                    .toArray()[0]
                    .toObject()
                    .value("state") == "error",
            "a refused reload hid the account's attention state");
    completed(restart, "the refused startup reload did not complete once the "
                       "endpoint check ended");
  }
  db.close();
  db = {};
  QSqlDatabase::removeDatabase(connection);
}
} // namespace

/// main exercises native-vault crash contracts by default; explicit acceptance
/// modes use the approved real test origins.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if (app.arguments().contains("--cleanup")) {
      // Remote content on an isolated fixture server needs its acknowledged
      // root here as well.
      if (qEnvironmentVariableIsSet("SYNCD_TEST_CA_CERT"))
        trustIsolatedCA();
      cleanupOwnedUser(lifetimeRoot());
      cleanupManifest();
      return 0;
    }
    if (app.arguments().contains("--lifetime")) {
      // A disposable fixture server is addressed directly, with its root.
      if (qEnvironmentVariableIsSet("SYNCD_TEST_CA_CERT"))
        trustIsolatedCA();
      replacedDuringVerification();
      return 0;
    }
    if (app.arguments().contains("--held-revocation")) {
      trustIsolatedCA();
      heldRevocationRestart();
      return 0;
    }
    const auto scenario = app.arguments().value(1).mid(2);
    if (scenario.startsWith("sleep-relay="))
      return sleepRelay(scenario.section('=', 1).toUInt());
    const bool isolated = scenario == "network-chunked" ||
                          scenario.section('=', 0, 0) == "network-uncertain" ||
                          scenario == "network-large" ||
                          scenario == "wrong-instance";
    if (isolated || scenario == "network" || scenario == "network-lifetime" ||
        scenario.section('=', 0, 0) == "network-active" ||
        scenario == "file-safety" || scenario == "multi-account" ||
        scenario == "switch-quota" || scenario == "slow-server" ||
        scenario == "sleep-resume" || scenario.startsWith("switch-kill=") ||
        scenario.startsWith("switch-recover=")) {
      // Chunk sessions and destructive lost-reply cases need a server of
      // their own; the others accept one when its root is acknowledged.
      if (isolated || qEnvironmentVariableIsSet("SYNCD_TEST_CA_CERT"))
        trustIsolatedCA();
      if (scenario == "network-chunked")
        chunkedTransferAcceptance();
      if (scenario.section('=', 0, 0) == "network-uncertain")
        uncertainTransferAcceptance();
      if (scenario == "network-large")
        largeTransferAcceptance();
      if (scenario == "file-safety")
        fileSafetyTransferAcceptance();
      if (scenario == "sleep-resume")
        sleepResumeAcceptance();
      networkAcceptance(scenario);
      return 0;
    }
    interrupted("waiting");
    interrupted("prepared");
    interrupted("prepared", false);
    interrupted("discarding");
    interrupted("installed");
    interrupted("installed-same");
    interrupted("cancelled");
    removalDuringRenewal();
    refusedReload(false);
    refusedReload(true);
    std::puts("PASS account-preserving renewal cancellation, vault rollback "
              "and committed restart");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
