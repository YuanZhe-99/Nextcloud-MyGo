// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
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

/// networkAcceptance exercises real endpoint probes and process-local transport
/// faults without borrowed-password revocation.
void networkAcceptance(bool lifetimeOnly, bool activeTransfers,
                       bool wrongInstance = false);
/// chunkedTransferAcceptance switches the active-transfer acceptance to one
/// interrupted chunk session on an isolated server.
void chunkedTransferAcceptance();
/// cleanupNetworkContent removes only the manifest-owned remote collection
/// before native credential deletion.
void cleanupNetworkContent(const QString &root, const QString &credential);

/// ReauthTestAccess supplies borrowed credentials without adding a production
/// IPC bypass.
struct ReauthTestAccess {
  /// flow locates the owned test session while retaining normal Control
  /// callbacks.
  static LoginFlow &flow(Control &control, const QString &session) {
    return *control.logins_.at(session);
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
    require(call(control, "accounts.remove", {{"accountId", account}})
                    .value("error")
                    .toObject()
                    .value("data")
                    .toObject()
                    .value("reasonCode") == "QUIESCE_STALLED",
            "account removal discarded pending credential slots");
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
      cleanupManifest();
      return 0;
    }
    if (app.arguments().contains("--network")) {
      networkAcceptance(false, false);
      return 0;
    }
    if (app.arguments().contains("--network-lifetime")) {
      networkAcceptance(true, false);
      return 0;
    }
    if (app.arguments().contains("--lifetime")) {
      replacedDuringVerification();
      return 0;
    }
    if (app.arguments().contains("--network-active")) {
      networkAcceptance(false, true);
      return 0;
    }
    if (app.arguments().contains("--network-chunked")) {
      trustIsolatedCA();
      chunkedTransferAcceptance();
      networkAcceptance(false, true);
      return 0;
    }
    if (app.arguments().contains("--wrong-instance")) {
      trustIsolatedCA();
      networkAcceptance(false, false, true);
      return 0;
    }
    interrupted("waiting");
    interrupted("prepared");
    interrupted("prepared", false);
    interrupted("discarding");
    interrupted("installed");
    interrupted("installed-same");
    interrupted("cancelled");
    std::puts("PASS account-preserving renewal cancellation, vault rollback "
              "and committed restart");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
