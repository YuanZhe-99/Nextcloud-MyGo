// SPDX-License-Identifier: GPL-3.0-only
#include "network_acceptance.h"

#include "control.h"
#include "login.h"
#include "network_tunnel.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <cstdio>
#include <stdexcept>
#include <tuple>

namespace {
/// require keeps real acceptance checks enabled in optimized builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
/// davRequest sends one bounded request as the named fixture user to an
/// approved origin and returns its status; the body comes back in body.
/// Notes: never replays a request; credentials stay in memory.
int davRequest(const QUrl &base, const QByteArray &method, const QString &path,
               const QString &user, const QString &password,
               const QByteArray &content = {}, QByteArray *body = nullptr,
               const QByteArray &type = {}) {
  const auto url = base.resolved(QUrl(path));
  require(withinServer(base, url),
          "owned multi-account request escaped origin");
  QNetworkAccessManager manager;
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setTransferTimeout(30000);
  request.setRawHeader(
      "Authorization",
      "Basic " + (user.toUtf8() + ':' + password.toUtf8()).toBase64());
  request.setRawHeader("OCS-APIRequest", "true");
  if (!type.isEmpty())
    request.setHeader(QNetworkRequest::ContentTypeHeader, type);
  auto *reply = manager.sendCustomRequest(request, method, content);
  QEventLoop loop;
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  if (!reply->isFinished())
    loop.exec();
  if (body)
    *body = reply->read(1048576);
  const auto status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  reply->deleteLater();
  return status;
}
/// home is a fixture user's DAV files root, relative to the origin.
QString home(const QString &uid) {
  return "remote.php/dav/files/" +
         QString::fromLatin1(QUrl::toPercentEncoding(uid)) + '/';
}
/// folderState reads one mapping's projected state; empty once it is gone.
QJsonObject folderState(Control &c, const QString &folder) {
  for (const auto &entry :
       c.projection().value("snapshot").toObject().value("folders").toArray())
    if (entry.toObject().value("id") == folder)
      return entry.toObject();
  return {};
}
/// Mapped is one folder of one account with its owner's credentials.
struct Mapped {
  QString account, folder, local, remote, login, secret, uid;
};
/// ownedUser creates a second user of the disposable server with a random
/// password and returns its name and password.
/// Side effects: reserves owned-user.json first, so this run and, after any
/// failure, the parent's cleanup remove the user again.
std::pair<QString, QString> ownedUser(const QString &root, const QUrl &base) {
  const auto user = "mygo-multi-" + QFileInfo(root).fileName().right(12);
  QByteArray random(24, Qt::Uninitialized);
  QRandomGenerator::system()->fillRange(
      reinterpret_cast<quint32 *>(random.data()), random.size() / 4);
  const auto password =
      QString::fromLatin1(random.toBase64(QByteArray::Base64UrlEncoding |
                                          QByteArray::OmitTrailingEquals)) +
      "!aA1";
  {
    QFile owned(QDir(root).filePath("owned-user.json"));
    require(owned.open(QIODevice::WriteOnly) &&
                owned.write(QJsonDocument(QJsonObject{{"user", user}})
                                .toJson(QJsonDocument::Compact)) > 0,
            "owned user reservation failed");
  }
  require(davRequest(base, "POST", "ocs/v2.php/cloud/users?format=json",
                     qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                     qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD"),
                     "userid=" + QUrl::toPercentEncoding(user) +
                         "&password=" + QUrl::toPercentEncoding(password),
                     nullptr, "application/x-www-form-urlencoded") == 200,
          "owned second fixture user could not be created");
  return {user, password};
}
/// setQuota gives an owned user a quota through the admin provisioning API.
void setQuota(const QUrl &base, const QString &user, const QByteArray &quota) {
  require(davRequest(base, "PUT",
                     "ocs/v2.php/cloud/users/" +
                         QString::fromLatin1(QUrl::toPercentEncoding(user)),
                     qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                     qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD"),
                     "key=quota&value=" + QUrl::toPercentEncoding(quota),
                     nullptr, "application/x-www-form-urlencoded") == 200,
          "owned fixture user quota could not be set");
}
/// removeOwnedUser deletes the owned user and its reservation.
void removeOwnedUser(const QString &root, const QUrl &base,
                     const QString &user) {
  require(davRequest(base, "DELETE",
                     "ocs/v2.php/cloud/users/" +
                         QString::fromLatin1(QUrl::toPercentEncoding(user)),
                     qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                     qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD")) == 200 &&
              QFile::remove(QDir(root).filePath("owned-user.json")),
          "owned second fixture user could not be removed");
}
} // namespace

void multiAccountAcceptance(Control &c, const QString &root,
                            const QString &first, const QString &target,
                            const QUrl &base, const AcceptanceCalls &calls) {
  const auto admin = qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
             adminPassword = qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD");
  // The same user on the same server again is refused, and its slot goes.
  const auto duplicate = calls.login(admin, adminPassword);
  require(duplicate.value("state") == "failed" &&
              duplicate.value("reasonCode") == "ACCOUNT_EXISTS",
          "the same user on the same server became a second account");
  std::puts("PASS multi-account: the same user on the same server is refused "
            "as ACCOUNT_EXISTS");
  // A second user of the disposable server, removed again by this run and,
  // after any failure, by the parent's cleanup (owned-user.json).
  const auto owned = ownedUser(root, base);
  const auto &user = owned.first, &password = owned.second;
  const auto added = calls.login(user, password);
  require(added.value("state") == "succeeded",
          "a second account on the same server was refused");
  const auto second = added.value("accountId").toString();
  require(!second.isEmpty() && second != first,
          "second account identity absent");
  const auto uid = [&](const QString &login, const QString &secret) {
    QByteArray body;
    require(davRequest(base, "GET", "ocs/v2.php/cloud/user?format=json", login,
                       secret, {}, &body) == 200,
            "owned fixture user metadata unavailable");
    return QJsonDocument::fromJson(body)
        .object()
        .value("ocs")
        .toObject()
        .value("data")
        .toObject()
        .value("id")
        .toString();
  };
  const auto firstUid = uid(admin, adminPassword),
             secondUid = uid(user, password);
  // The first account's folders live in its reserved collection, which the
  // parent's cleanup removes; the second's go with the owned user.
  const auto collection =
      "mygo-network-" + QFileInfo(root).fileName().right(32);
  {
    QSaveFile manifest(root + "/owned-content.json");
    const auto reservation =
        QJsonDocument(
            QJsonObject{{"uid", firstUid}, {"collection", collection}})
            .toJson(QJsonDocument::Compact);
    require(manifest.open(QIODevice::WriteOnly) &&
                manifest.write(reservation) == reservation.size() &&
                manifest.commit(),
            "owned content cleanup reservation failed");
  }
  require(davRequest(base, "MKCOL", home(firstUid) + collection, admin,
                     adminPassword) == 201,
          "owned reserved collection could not be created");
  QList<Mapped> mapped;
  for (const auto &[account, login, secret, id, prefix] :
       {std::tuple{first, admin, adminPassword, firstUid, collection + '/'},
        std::tuple{second, user, password, secondUid, QString{}}})
    for (const auto *name : {"one", "two"}) {
      Mapped m{account,
               {},
               root + "/multi/" + account.left(8) + '/' + name,
               '/' + prefix + name,
               login,
               secret,
               id};
      // The second account's second folder does not exist locally yet:
      // inspecting it is allowed and adding it creates it empty (1.9).
      const bool absent = account == second && QLatin1String(name) == "two";
      require(davRequest(base, "MKCOL", home(id) + m.remote.mid(1), login,
                         secret) == 201 &&
                  (absent ? QDir().mkpath(QFileInfo(m.local).absolutePath())
                          : QDir().mkpath(m.local)),
              "owned multi-account folder could not be created");
      const auto token = calls
                             .call("folders.inspect", {{"accountId", account},
                                                       {"remotePath", m.remote},
                                                       {"localPath", m.local}})
                             .value("result")
                             .toObject()
                             .value("inspectToken")
                             .toString();
      require(!token.isEmpty(), "owned multi-account inspection failed");
      require(
          calls.settled(calls.call("folders.add", {{"inspectToken", token}}))
                  .value("state") == "succeeded",
          "owned multi-account folder could not be added");
      for (const auto &entry : c.projection()
                                   .value("snapshot")
                                   .toObject()
                                   .value("folders")
                                   .toArray())
        if (entry.toObject().value("localPath") == QDir::cleanPath(m.local))
          m.folder = entry.toObject().value("id").toString();
      require(!m.folder.isEmpty() && QFileInfo(m.local).isDir(),
              "owned multi-account mapping absent or its new folder missing");
      mapped.append(m);
    }
  calls.wait(
      [&] {
        for (const auto &m : mapped)
          if (folderState(c, m.folder).value("state") != "upToDate")
            return false;
        return true;
      },
      "the four folders of two accounts did not reconcile");
  // Content both ways in every folder.
  const auto put = [&](const Mapped &m, const QString &name) {
    QFile file(m.local + '/' + name);
    require(file.open(QIODevice::WriteOnly) && file.write(name.toUtf8()) > 0,
            "owned multi-account local write failed");
  };
  const auto onServer = [&](const Mapped &m, const QString &name) {
    QByteArray body;
    return davRequest(base, "GET", home(m.uid) + m.remote.mid(1) + '/' + name,
                      m.login, m.secret, {}, &body) == 200 &&
           body == name.toUtf8();
  };
  for (const auto &m : mapped) {
    put(m, "up.txt");
    require(davRequest(base, "PUT", home(m.uid) + m.remote.mid(1) + "/down.txt",
                       m.login, m.secret, "down.txt") /
                    100 ==
                2,
            "owned multi-account remote write failed");
    calls.call("folders.syncNow", {{"folderId", m.folder}});
  }
  calls.wait(
      [&] {
        for (const auto &m : mapped)
          if (!onServer(m, "up.txt") || !QFile::exists(m.local + "/down.txt"))
            return false;
        return true;
      },
      "content did not travel both ways in all four folders");
  std::puts("PASS multi-account: two accounts with two folders each upload and "
            "download on one daemon");
  // A switch of the first account never pauses the second's folders.
  const auto before = calls.binding(second);
  const auto switching =
      calls.call("endpoints.requestSwitch",
                 {{"accountId", first}, {"endpointId", target}});
  require(!switching.value("result")
               .toObject()
               .value("operationId")
               .toString()
               .isEmpty(),
          "the first account's switch was refused");
  put(mapped[2], "during.txt");
  calls.call("folders.syncNow", {{"folderId", mapped[2].folder}});
  bool blockedOther = false;
  calls.wait(
      [&] {
        for (const auto &entry : calls.call("diagnostics.snapshot", {})
                                     .value("result")
                                     .toObject()
                                     .value("folders")
                                     .toArray()) {
          const auto folder = entry.toObject();
          if (folder.value("accountId") == second &&
              (folder.value("blockers")
                   .toArray()
                   .contains(QJsonValue("switchInProgress")) ||
               folder.value("state") == "paused"))
            blockedOther = true;
        }
        return calls.binding(first).first == target && calls.stable(first) &&
               onServer(mapped[2], "during.txt");
      },
      "the first account's switch or the second's upload did not finish");
  require(!blockedOther, "a switch of one account paused another's folders");
  const auto after = calls.binding(second);
  require(after == before,
          "a switch of one account changed another's binding or epoch");
  int firstAddresses = 0, secondAddresses = 0;
  for (const auto &entry : c.projection()
                               .value("snapshot")
                               .toObject()
                               .value("endpoints")
                               .toArray()) {
    firstAddresses += entry.toObject().value("accountId") == first;
    secondAddresses += entry.toObject().value("accountId") == second;
  }
  require(firstAddresses == 2 && secondAddresses == 1,
          "accounts did not keep their own addresses");
  std::puts("PASS multi-account: one account switched address while the "
            "other's folders kept syncing on their own binding and epoch");
  // A full server quota is the server's constraint, never a local storage
  // failure: the folder names it, a fixed policy still commits, the daemon
  // keeps serving and the other account keeps syncing (shared server,
  // 2026-11-12: the daemon stopped itself here).
  setQuota(base, user, "1 KB");
  {
    QFile file(mapped[2].local + "/full.bin");
    require(file.open(QIODevice::WriteOnly) &&
                file.write(QByteArray(256 * 1024, 'q')) == 256 * 1024,
            "owned multi-account local write failed");
  }
  calls.call("folders.syncNow", {{"folderId", mapped[2].folder}});
  calls.wait(
      [&] {
        return folderState(c, mapped[2].folder).value("reasonCode") ==
               "REMOTE_QUOTA_EXCEEDED";
      },
      "a full server quota was not named on the folder");
  std::puts("PASS multi-account: a full server quota is named on the folder");
  auto receipt = calls.settled(
      calls.call("endpoints.stage",
                 {{"accountId", second},
                  {"name", "Owned alternate"},
                  {"baseUrl", qEnvironmentVariable("NEXTCLOUD_TEST_ALTERNATE_URL")}}));
  require(receipt.value("state") == "succeeded",
          "the second account's alternate could not be staged");
  receipt = calls.settled(calls.call(
      "endpoints.approveOrigin",
      {{"stagingId",
        receipt.value("result").toObject().value("id").toString()}}));
  require(receipt.value("state") == "succeeded",
          "the second account's alternate could not be approved");
  const auto alternate =
      receipt.value("result").toObject().value("endpointId").toString();
  // The folder refused by its quota still reconciles at the new epoch, so the
  // policy commits; a quota blocker held this switch open for good before.
  receipt = calls.settled(calls.call("endpoints.setPolicy",
                                     {{"accountId", second},
                                      {"policy", "fixed"},
                                      {"fixedEndpointId", alternate}}));
  require(receipt.value("state") == "succeeded" &&
              calls.binding(second).first == alternate,
          "a fixed policy did not commit while the server quota was full");
  put(mapped[0], "quota.txt");
  calls.call("folders.syncNow", {{"folderId", mapped[0].folder}});
  calls.wait(
      [&] {
        return onServer(mapped[0], "quota.txt") &&
               folderState(c, mapped[2].folder).value("reasonCode") ==
                   "REMOTE_QUOTA_EXCEEDED";
      },
      "the other account stopped syncing or the full folder did not retry");
  require(!c.shutdownRequested(), "a full server quota stopped the daemon");
  std::puts("PASS multi-account: a full server quota keeps the daemon serving, "
            "commits a fixed policy and leaves the other account syncing");
  // Removing the second account leaves the first syncing and every file.
  require(calls.settled(calls.call("accounts.remove", {{"accountId", second}}))
                  .value("state") == "succeeded",
          "the second account could not be removed");
  for (const auto &m : mapped)
    require(m.account == first || (QFile::exists(m.local + "/up.txt") &&
                                   folderState(c, m.folder).isEmpty()),
            "removal left a mapping or touched a local file");
  put(mapped[0], "after.txt");
  calls.call("folders.syncNow", {{"folderId", mapped[0].folder}});
  calls.wait([&] { return onServer(mapped[0], "after.txt"); },
             "the remaining account stopped syncing after the other's removal");
  removeOwnedUser(root, base, user);
  std::puts("PASS multi-account: removing one account kept the other syncing, "
            "its files on both sides and its address");
}

void switchQuotaAcceptance(Control &c, const QString &root, const QUrl &base,
                           const AcceptanceCalls &calls) {
  const auto owned = ownedUser(root, base);
  const auto &user = owned.first, &password = owned.second;
  const auto added = calls.login(user, password);
  require(added.value("state") == "succeeded",
          "the owned user could not sign in");
  const auto account = added.value("accountId").toString();
  auto receipt = calls.settled(calls.call(
      "endpoints.stage",
      {{"accountId", account},
       {"name", "Owned alternate"},
       {"baseUrl", qEnvironmentVariable("NEXTCLOUD_TEST_ALTERNATE_URL")}}));
  require(receipt.value("state") == "succeeded",
          "the owned user's alternate could not be staged");
  receipt = calls.settled(calls.call(
      "endpoints.approveOrigin",
      {{"stagingId",
        receipt.value("result").toObject().value("id").toString()}}));
  require(receipt.value("state") == "succeeded",
          "the owned user's alternate could not be approved");
  const auto alternate =
      receipt.value("result").toObject().value("endpointId").toString();
  const auto primary = calls.binding(account).first;
  QByteArray body;
  require(davRequest(base, "GET", "ocs/v2.php/cloud/user?format=json", user,
                     password, {}, &body) == 200,
          "owned fixture user metadata unavailable");
  const auto uid = QJsonDocument::fromJson(body)
                       .object()
                       .value("ocs")
                       .toObject()
                       .value("data")
                       .toObject()
                       .value("id")
                       .toString();
  const auto local = root + "/quota/local";
  require(davRequest(base, "MKCOL", home(uid) + "quota", user, password) ==
                  201 &&
              QDir().mkpath(local),
          "owned quota folder could not be created");
  const auto token = calls
                         .call("folders.inspect", {{"accountId", account},
                                                   {"remotePath", "/quota"},
                                                   {"localPath", local}})
                         .value("result")
                         .toObject()
                         .value("inspectToken")
                         .toString();
  require(!token.isEmpty() &&
              calls.settled(calls.call("folders.add", {{"inspectToken", token}}))
                      .value("state") == "succeeded",
          "owned quota folder could not be added");
  QString folder;
  for (const auto &entry :
       c.projection().value("snapshot").toObject().value("folders").toArray())
    if (entry.toObject().value("accountId") == account)
      folder = entry.toObject().value("id").toString();
  calls.wait([&] { return folderState(c, folder).value("state") == "upToDate"; },
             "the owned quota folder did not reconcile");
  const auto network = [&] {
    for (const auto &entry : c.projection()
                                 .value("snapshot")
                                 .toObject()
                                 .value("networks")
                                 .toArray())
      if (entry.toObject().value("accountId") == account)
        return entry.toObject();
    return QJsonObject{};
  };
  const auto latest = [&](const char *kind) {
    for (const auto &entry : c.projection()
                                 .value("snapshot")
                                 .toObject()
                                 .value("activity")
                                 .toArray())
      if (entry.toObject().value("accountId") == account &&
          entry.toObject().value("kind") == QLatin1String(kind))
        return entry.toObject();
    return QJsonObject{};
  };
  const auto counter = [&](const char *name) {
    for (const auto &entry : calls.call("diagnostics.snapshot", {})
                                 .value("result")
                                 .toObject()
                                 .value("folders")
                                 .toArray())
      if (entry.toObject().value("folderId") == folder)
        return entry.toObject().value(name).toString().toULongLong();
    return 0ULL;
  };
  // What "Use this one" sends: a fixed policy for the alternate address.
  const auto useAlternate = [&] {
    return calls.call("endpoints.setPolicy", {{"accountId", account},
                                              {"policy", "fixed"},
                                              {"fixedEndpointId", alternate}});
  };
  // A 64 MiB upload held unfinished on the primary past the drain window.
  const auto policy = network();
  const auto sent = counter("uploadBytes");
  NetworkTunnel::setStallAfter(2 * 1048576);
  {
    QByteArray content(64 * 1048576, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32 *>(content.data()), content.size() / 4);
    QFile file(local + "/large.bin");
    require(file.open(QIODevice::WriteOnly) &&
                file.write(content) == content.size(),
            "owned large local write failed");
  }
  calls.call("folders.syncNow", {{"folderId", folder}});
  calls.wait(
      [&] {
        return counter("activeUploads") > 0 &&
               counter("uploadBytes") >= sent + 1048576;
      },
      "the owned upload was not held unfinished");
  const auto first = useAlternate()
                         .value("result")
                         .toObject()
                         .value("operationId")
                         .toString();
  require(!first.isEmpty(), "the switch request was refused at once");
  calls.wait(
      [&] {
        const auto n = network();
        return n.value("switching").toBool() &&
               n.value("requestedEndpointId") == alternate &&
               n.value("why").toArray().contains(QJsonValue("postponed"));
      },
      "the switch did not show as postponed during the transfer");
  try {
    calls.wait([&] { return !latest("switchFailed").isEmpty(); },
               "the switch request ended without a visible outcome");
  } catch (...) {
    // Sanitized state for the report: identifiers, states and reason codes.
    std::printf(
        "  network %s\n  receipt %s\n",
        QJsonDocument(network()).toJson(QJsonDocument::Compact).constData(),
        QJsonDocument(calls.call("operations.get", {{"operationId", first}})
                          .value("result")
                          .toObject())
            .toJson(QJsonDocument::Compact)
            .constData());
    throw;
  }
  const auto failed = latest("switchFailed");
  const auto n = network();
  require(failed.value("reason") == "QUIESCE_STALLED" &&
              failed.value("fromId") == primary &&
              failed.value("toId") == alternate &&
              !failed.value("name").toString().isEmpty(),
          "the refused switch did not name its reason and addresses");
  require(calls.binding(account).first == primary &&
              !n.value("switching").toBool() &&
              n.value("requestedEndpointId").toString().isEmpty() &&
              n.value("policy") == policy.value("policy") &&
              n.value("fixedEndpointId") == policy.value("fixedEndpointId"),
          "a refused switch changed the address or the policy");
  require(calls
              .call("operations.get", {{"operationId", first}})
              .value("result")
              .toObject()
              .value("state") == "failed",
          "the refused switch's receipt did not fail");
  std::puts("PASS switch-quota: a switch asked for during an upload held past "
            "the drain window ends as switchFailed with QUIESCE_STALLED, on "
            "the same address and policy");
  // The held upload finishes and fills the user's quota; the next one is
  // refused. The server sizes a PUT's room when it opens it, so a quota
  // lowered during the held upload does not refuse that one.
  NetworkTunnel::setStallAfter(0);
  calls.wait([&] { return folderState(c, folder).value("state") == "upToDate"; },
             "the held upload did not finish once released");
  setQuota(base, user, "1 MB");
  {
    QFile file(local + "/after.bin");
    require(file.open(QIODevice::WriteOnly) &&
                file.write(QByteArray(256 * 1024, 'q')) == 256 * 1024,
            "owned local write failed");
  }
  calls.call("folders.syncNow", {{"folderId", folder}});
  calls.wait(
      [&] {
        return folderState(c, folder).value("reasonCode") ==
               "REMOTE_QUOTA_EXCEEDED";
      },
      "the next upload was not refused by the user's quota");
  std::puts("PASS switch-quota: the held upload finished once released and "
            "filled the quota; the next upload names the full server");
  // Asked again at the safe point, with the quota still full, it switches.
  receipt = calls.settled(useAlternate());
  require(receipt.value("state") == "succeeded" &&
              calls.binding(account).first == alternate &&
              latest("switched").value("toId") == alternate,
          "the switch asked for again after the quota refusal did not happen");
  require(!c.shutdownRequested(), "the full quota stopped the daemon");
  std::puts("PASS switch-quota: asked again at the safe point with the quota "
            "full, the switch happens and is recorded");
  require(calls.settled(calls.call("accounts.remove", {{"accountId", account}}))
                  .value("state") == "succeeded",
          "the owned account could not be removed");
  removeOwnedUser(root, base, user);
}

void cleanupOwnedUser(const QString &root) {
  QFile file(QDir(root).filePath("owned-user.json"));
  if (!file.exists())
    return;
  require(file.open(QIODevice::ReadOnly) && file.size() < 4096,
          "owned user manifest unreadable");
  const auto user =
      QJsonDocument::fromJson(file.readAll()).object().value("user").toString();
  file.close();
  require(user == "mygo-multi-" + QFileInfo(root).fileName().right(12),
          "owned user cleanup escaped its reservation");
  const auto status =
      davRequest(canonicalServer(qEnvironmentVariable(
                     "NEXTCLOUD_TEST_CLEANUP_URL",
                     qEnvironmentVariable("NEXTCLOUD_TEST_URL"))),
                 "DELETE",
                 "ocs/v2.php/cloud/users/" +
                     QString::fromLatin1(QUrl::toPercentEncoding(user)),
                 qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                 qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD"));
  // 200 removed it; 404 means it was never created or is already gone.
  require(status == 200 || status == 404,
          "owned second fixture user could not be removed");
  require(file.remove(), "owned user manifest removal failed");
}
