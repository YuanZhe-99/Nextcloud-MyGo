// SPDX-License-Identifier: GPL-3.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "control.h"
#include "namespace.h"
#include "network_tunnel.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QUuid>
#include <cstdio>
#include <stdexcept>

namespace {
// The three cases each leave one file behind and must fit the approved test
// account's quota together: at 16 MiB the third PUT was refused with HTTP 507,
// which read as a committed write that never became visible.
constexpr int contentSize = 8 * 1024 * 1024;
/// require preserves live acceptance conditions in optimized executables.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// call sends a normal revision-bearing command through the production
/// boundary.
QJsonObject call(Control &c, const QString &method, QJsonObject params = {}) {
  params.insert("operationKey",
                QUuid::createUuid().toString(QUuid::WithoutBraces));
  params.insert("expectedRevision", c.projection().value("revision"));
  bool negotiated = true, subscribed = false;
  auto reply = c.dispatch(QJsonObject{{"jsonrpc", "2.0"},
                                      {"id", 1},
                                      {"method", method},
                                      {"params", params}},
                          negotiated, subscribed)
                   .toObject();
  reply.insert("fixtureMethod", QString(method).replace('.', '-'));
  return reply;
}
/// wait drives real Qt callbacks with a bounded deadline; failures carry no
/// private request context.
void wait(const std::function<bool()> &condition, const char *message,
          int pollMilliseconds = 10) {
  QEventLoop loop;
  QTimer poll, deadline;
  // A condition may run a nested real-network event loop. Never enter another
  // observation until that request has finished, nor busy-poll a locked PUT.
  poll.setInterval(pollMilliseconds);
  poll.setSingleShot(true);
  deadline.setSingleShot(true);
  deadline.setTimerType(Qt::PreciseTimer);
  QDeadlineTimer budget(180000, Qt::PreciseTimer);
  bool done = false;
  std::exception_ptr error;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    try {
      if (condition()) {
        done = !budget.hasExpired();
        loop.quit();
      }
    } catch (...) {
      error = std::current_exception();
      loop.quit();
    }
    if (!done && !error && !budget.hasExpired())
      poll.start();
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  deadline.start(180000);
  loop.exec();
  if (error)
    std::rethrow_exception(error);
  require(done, message);
}
/// settle observes the accepted operation once without resubmitting its
/// mutation.
void settle(Control &c, const QJsonObject &reply) {
  const auto operation =
      reply.value("result").toObject().value("operationId").toString();
  require(!operation.isEmpty(), "owned transfer command refused");
  wait(
      [&] {
        const auto result =
            call(c, "operations.get", {{"operationId", operation}})
                .value("result")
                .toObject();
        const auto state = result.value("state").toString();
        if (state == "failed") {
          auto reason = result.value("error")
                            .toObject()
                            .value("data")
                            .toObject()
                            .value("reasonCode")
                            .toString();
          bool safe = !reason.isEmpty() && reason.size() <= 80;
          for (const auto character : reason)
            safe = safe &&
                   ((character >= 'A' && character <= 'Z') || character == '_');
          if (!safe)
            reason = "INTERNAL_ERROR";
          throw std::runtime_error(("owned transfer " +
                                    reply.value("fixtureMethod").toString() +
                                    " failed " + reason)
                                       .toStdString());
        }
        return state == "succeeded";
      },
      "owned transfer operation deadline");
}
/// PendingRead bounds all readback attempts together and retains only numeric
/// transport observations; no URL, header, account or response content escapes.
struct PendingRead {
  QDeadlineTimer budget{180000, Qt::PreciseTimer};
  quint64 attempts = 0, missing = 0, locked = 0;
  qint64 bytes = 0;
  int status = 0, error = 0;
  bool http2 = false, timedOut = false;
};
/// dav performs one bounded real request to an approved origin; mutating
/// requests are never replayed. Pending GETs share one absolute deadline.
QByteArray dav(const QUrl &base, const QString &uid, const QString &collection,
               const QByteArray &method, const QString &credential,
               const QByteArray &content = {}, const QString &file = {},
               PendingRead *pending = nullptr) {
  require(!pending || (method == "GET" && !pending->budget.hasExpired()),
          "owned transfer pending read deadline");
  require(!uid.isEmpty() && !uid.contains('/') && !uid.contains('\\'),
          "owned transfer user metadata invalid");
  const auto path = "remote.php/dav/files/" +
                    QString::fromLatin1(QUrl::toPercentEncoding(uid)) + '/' +
                    collection + (file.isEmpty() ? QString{} : '/' + file);
  const auto url = base.resolved(QUrl(path));
  require(withinServer(base, url),
          "owned transfer request escaped approved origin");
  QNetworkAccessManager manager;
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  auto secret = readCredential(credential).toUtf8();
  request.setRawHeader(
      "Authorization",
      "Basic " +
          (qEnvironmentVariable("NEXTCLOUD_TEST_USER").toUtf8() + ':' + secret)
              .toBase64());
  secret.fill(0);
  auto *reply = manager.sendCustomRequest(request, method, content);
  reply->setReadBufferSize(contentSize + 1);
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  deadline.setTimerType(Qt::PreciseTimer);
  QByteArray body;
  QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
    body += reply->readAll();
    if (body.size() > contentSize)
      reply->abort();
  });
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  bool timedOut = false;
  QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
    timedOut = true;
    reply->abort();
  });
  deadline.start(pending
                     ? int(qMin<qint64>(90000, pending->budget.remainingTime()))
                     : 90000);
  if (!reply->isFinished())
    loop.exec();
  body += reply->readAll();
  const auto status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const bool absent = method == "DELETE" && status == 404;
  if (pending) {
    ++pending->attempts;
    pending->status = status;
    pending->error = int(reply->error());
    pending->bytes = body.size();
    pending->http2 =
        reply->attribute(QNetworkRequest::Http2WasUsedAttribute).toBool();
    pending->timedOut = timedOut || pending->budget.hasExpired();
    pending->missing += status == 404;
    pending->locked += status == 423;
    if (!pending->timedOut && body.size() <= contentSize &&
        (status == 404 || status == 423))
      return {};
  }
  require(!timedOut && (!pending || !pending->timedOut),
          "owned transfer real DAV request deadline");
  require(absent || (reply->error() == QNetworkReply::NoError &&
                     status >= 200 && status < 300),
          "owned transfer real DAV request failed");
  require(body.size() <= contentSize,
          "owned transfer response exceeded its bound");
  return body;
}
/// journalIdentity observes the native journal file ID without reading or
/// changing its contents.
QByteArray journalIdentity(const QString &root) {
  const auto files = QDir(root + "/journals")
                         .entryList({"*.sqlite"}, QDir::Files | QDir::Hidden);
  require(files.size() == 1, "owned transfer journal missing");
  const auto path =
      QDir::toNativeSeparators(root + "/journals/" + files.first());
  const auto handle =
      CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, 0, nullptr);
  require(handle != INVALID_HANDLE_VALUE,
          "owned journal identity handle unavailable");
  FILE_ID_INFO identity{};
  const auto ok = GetFileInformationByHandleEx(handle, FileIdInfo, &identity,
                                               sizeof(identity));
  CloseHandle(handle);
  require(ok, "owned journal native identity unavailable");
  return QByteArray(reinterpret_cast<const char *>(&identity),
                    sizeof(identity));
}
} // namespace

/// cleanupNetworkContent removes only the manifest-reserved collection on the
/// approved real test origin. Notes: called by the parent-owned cleanup child
/// after any failure; the caller validates the native credential reference
/// first.
void cleanupNetworkContent(const QString &root, const QString &credential) {
  QFile file(root + "/owned-content.json");
  if (!file.exists())
    return;
  require(file.open(QIODevice::ReadOnly) && file.size() < 8192,
          "owned content cleanup manifest unreadable");
  const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
  file.close();
  const auto collection = manifest.value("collection").toString();
  require(collection == "mygo-network-" + QFileInfo(root).fileName().right(32),
          "owned content cleanup escaped reserved collection");
  dav(canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_URL")),
      manifest.value("uid").toString(), collection, "DELETE", credential);
  require(file.remove(), "owned content cleanup manifest removal failed");
}

/// networkTransferAcceptance breaks a real positive-byte transfer through the
/// owned relay and requires automatic mapped-root recovery
/// (INV-02/05/06/08/09/13).
void networkTransferAcceptance(Control &c, const QString &root,
                               const QString &account, const QString &primary,
                               const QString &alternate,
                               const QString &credential,
                               const std::function<void(bool)> &failPrimary,
                               const std::function<bool()> &ready,
                               const std::function<void(bool)> &holdResponse) {
#ifndef SYNCD_WITH_ENGINE
  Q_UNUSED(c);
  Q_UNUSED(root);
  Q_UNUSED(account);
  Q_UNUSED(primary);
  Q_UNUSED(alternate);
  Q_UNUSED(credential);
  Q_UNUSED(failPrimary);
  Q_UNUSED(ready);
  Q_UNUSED(holdResponse);
  throw std::runtime_error(
      "owned transfer acceptance requires an engine build");
#else
  const auto base = canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_URL"));
  const auto uid =
      readServerJson(base, "ocs/v2.php/cloud/user?format=json",
                     qEnvironmentVariable("NEXTCLOUD_TEST_USER"), credential)
          .value("ocs")
          .toObject()
          .value("data")
          .toObject()
          .value("id")
          .toString();
  const auto collection =
      "mygo-network-" + QFileInfo(root).fileName().right(32);
  QSaveFile manifest(root + "/owned-content.json");
  manifest.setDirectWriteFallback(false);
  const auto reservation =
      QJsonDocument(QJsonObject{{"uid", uid}, {"collection", collection}})
          .toJson(QJsonDocument::Compact);
  require(manifest.open(QIODevice::WriteOnly) &&
              manifest.write(reservation) == reservation.size() &&
              manifest.commit(),
          "owned content cleanup reservation failed");
  dav(base, uid, collection, "MKCOL", credential);
  const auto local = root + "/content";
  require(QDir().mkpath(local), "owned content local directory unavailable");
  const auto inspection = call(c, "folders.inspect",
                               {{"accountId", account},
                                {"remotePath", '/' + collection},
                                {"localPath", local}})
                              .value("result")
                              .toObject()
                              .value("inspectToken")
                              .toString();
  require(!inspection.isEmpty(), "owned transfer mapping inspection failed");
  settle(c, call(c, "folders.add", {{"inspectToken", inspection}}));
  const auto idle = [&] {
    const auto folders =
        c.projection().value("snapshot").toObject().value("folders").toArray();
    return folders.size() == 1 &&
           folders.first().toObject().value("state") == "upToDate";
  };
  wait(idle, "owned transfer initial mapping did not reconcile");
  const auto journal = journalIdentity(root);
  QByteArray content(contentSize, Qt::Uninitialized);
  QRandomGenerator::system()->fillRange(
      reinterpret_cast<quint32 *>(content.data()),
      content.size() / sizeof(quint32));
  const auto hash =
      QCryptographicHash::hash(content, QCryptographicHash::Sha256);
  for (const auto &direction :
       {QStringLiteral("upload"), QStringLiteral("download"),
        QStringLiteral("unacknowledged-upload")}) {
    const bool upload = direction != "download",
               unacknowledged = direction == "unacknowledged-upload";
    holdResponse(false);
    failPrimary(false);
    // Switching back onto an address whose circuit is still recovering would
    // let the automatic policy leave it again before the next fault.
    wait(ready, "owned transfer endpoints did not recover from the last fault");
    settle(c, call(c, "endpoints.requestSwitch",
                   {{"accountId", account}, {"endpointId", primary}}));
    wait(ready, "owned transfer fresh endpoint evidence unavailable");
    const auto name = "active-" + direction + ".bin";
    const auto baseline = call(c, "diagnostics.snapshot")
                              .value("result")
                              .toObject()
                              .value("folders")
                              .toArray()
                              .first()
                              .toObject();
    const auto bytesKey = upload ? "uploadBytes" : "downloadBytes";
    const auto activeKey = upload ? "activeUploads" : "activeDownloads";
    if (upload) {
      QFile file(local + '/' + name);
      require(file.open(QIODevice::WriteOnly) &&
                  file.write(content) == content.size() && file.flush(),
              "owned transfer local content write failed");
    } else
      dav(base, uid, collection, "PUT", credential, content, name);
    const auto folder = c.projection()
                            .value("snapshot")
                            .toObject()
                            .value("folders")
                            .toArray()
                            .first()
                            .toObject()
                            .value("id")
                            .toString();
    settle(c, call(c, "folders.syncNow", {{"folderId", folder}}));
    wait(
        [&] {
          const auto d = call(c, "diagnostics.snapshot")
                             .value("result")
                             .toObject()
                             .value("folders")
                             .toArray()
                             .first()
                             .toObject();
          return d.value(activeKey).toString().toULongLong() > 0 &&
                 d.value(bytesKey).toString().toULongLong() >=
                     baseline.value(bytesKey).toString().toULongLong() + 65536;
        },
        "owned unfinished content transfer was not observed");
    if (unacknowledged) {
      holdResponse(true);
      const auto other =
          canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_ALTERNATE_URL"));
      PendingRead pending;
      try {
        wait(
            [&] {
              return QCryptographicHash::hash(
                         dav(other, uid, collection, "GET", credential, {},
                             name, &pending),
                         QCryptographicHash::Sha256) == hash;
            },
            "owned unacknowledged PUT did not become visible on the real "
            "server",
            1000);
      } catch (...) {
        const auto d = call(c, "diagnostics.snapshot")
                           .value("result")
                           .toObject()
                           .value("folders")
                           .toArray()
                           .first()
                           .toObject();
        const auto relay = NetworkTunnel::heldProgress();
        // Keep this numeric-only line compatible with the parent's strict
        // failure-summary allowlist; raw Qt messages and bodies stay private.
        std::fprintf(stderr,
                     "owned PUT readback attempts %llu missing %llu "
                     "locked %llu status %d qtError %d responseBytes %lld "
                     "http2 %d timedOut %d activeUploads %llu uploadBytes %llu "
                     "heldRelays %llu relayMaxUpstreamBytes %llu "
                     "relayClientPending %lld relayRemotePending %lld "
                     "relayUpstreamQueued %lld\n",
                     pending.attempts, pending.missing, pending.locked,
                     pending.status, pending.error, pending.bytes,
                     int(pending.http2), int(pending.timedOut),
                     d.value("activeUploads").toString().toULongLong(),
                     d.value("uploadBytes").toString().toULongLong() -
                         baseline.value("uploadBytes").toString().toULongLong(),
                     relay.count, relay.maxUpstreamBytes, relay.clientPending,
                     relay.remotePending, relay.upstreamQueued);
        throw;
      }
      const auto unfinished = call(c, "diagnostics.snapshot")
                                  .value("result")
                                  .toObject()
                                  .value("folders")
                                  .toArray()
                                  .first()
                                  .toObject();
      require(unfinished.value("activeUploads").toString().toULongLong() > 0,
              "owned PUT acknowledgement was not withheld");
    }
    const auto inUse = [&](const QString &endpoint) {
      const auto endpoints = c.projection()
                                 .value("snapshot")
                                 .toObject()
                                 .value("endpoints")
                                 .toArray();
      for (const auto &entry : endpoints)
        if (entry.toObject().value("id") == endpoint &&
            entry.toObject().value("inUse").toBool())
          return true;
      return false;
    };
    // The fault below proves nothing unless the transfer runs on the primary.
    require(inUse(primary),
            "owned active transfer was not on the primary address");
    failPrimary(true);
    holdResponse(false);
    wait([&] { return inUse(alternate); },
         "owned active transfer did not automatically fail over");
    wait(idle,
         "owned active transfer did not reconcile after automatic failover");
    if (upload) {
      const auto recovered = call(c, "diagnostics.snapshot")
                                 .value("result")
                                 .toObject()
                                 .value("folders")
                                 .toArray()
                                 .first()
                                 .toObject();
      require(recovered.value("uncertainWrites").toString().toULongLong() >
                  baseline.value("uncertainWrites").toString().toULongLong(),
              "owned interrupted upload lost its unconfirmed-write evidence");
    }
    failPrimary(false);
    QFile file(local + '/' + name);
    require(file.open(QIODevice::ReadOnly) &&
                QCryptographicHash::hash(file.readAll(),
                                         QCryptographicHash::Sha256) == hash,
            "owned automatic recovery local hash mismatch");
    file.close();
    require(QCryptographicHash::hash(
                dav(base, uid, collection, "GET", credential, {}, name),
                QCryptographicHash::Sha256) == hash,
            "owned automatic recovery remote hash mismatch");
    require(journalIdentity(root) == journal,
            "owned automatic recovery replaced the native journal");
    std::printf("PASS real %s automatic recovery, matching local/remote hashes "
                "and original native journal\n",
                qPrintable(direction));
  }
  settle(c, call(c, "folders.remove",
                 {{"folderId", c.projection()
                                   .value("snapshot")
                                   .toObject()
                                   .value("folders")
                                   .toArray()
                                   .first()
                                   .toObject()
                                   .value("id")}}));
  std::puts("PASS real unfinished positive-byte upload/download faults, "
            "server-committed PUT with withheld acknowledgement, automatic "
            "mapped-root cross-origin recovery, matching hashes and "
            "original native journal");
#endif
}
