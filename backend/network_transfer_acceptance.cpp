// SPDX-License-Identifier: GPL-3.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "network_acceptance.h"

#include "control.h"
#include "namespace.h"
#include "network_tunnel.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QUuid>
#include <QXmlStreamReader>
#include <cstdio>
#include <stdexcept>

namespace {
// The three cases each leave one file behind and must fit the approved test
// account's quota together: at 16 MiB the third PUT was refused with HTTP 507,
// which read as a committed write that never became visible.
int contentSize = 8 * 1024 * 1024;
// Upstream sends files above its default 100 MiB threshold as a chunking-v2
// session whose first chunk is that size.
constexpr qint64 firstChunk = 100LL * 1024 * 1024;
bool chunked = false, uncertain = false, fileSafety = false,
     fileSafetyBulk = false, large = false, sleepResume = false;
// A standby endpoint is probed about once a minute once its health is stable,
// and readiness needs a fresh success on both: allow one full probe period.
constexpr int readyMilliseconds = 90000;
// The one transfer case this process runs; empty runs every case.
QString transferCase;
// Whether this process has broken the primary yet; until then it still holds
// the fresh binding the mapping started on.
bool faulted = false;
/// selected says whether this process runs the named transfer case.
bool selected(const char *name) {
  return transferCase.isEmpty() || transferCase == QLatin1String(name);
}
// Past 4 GiB every offset, size and counter must be 64-bit; the cut comes
// after that boundary.
constexpr qint64 largeSize = (4LL * 1024 + 200) * 1024 * 1024,
                 largeCut = (4LL * 1024 + 64) * 1024 * 1024;
// Small entries for the lost-reply MOVE/DELETE cases and the files a killed
// switch must not send again.
constexpr int smallSize = 1024 * 1024;
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
          int pollMilliseconds = 10, int budgetMilliseconds = 60000) {
  // The parent names the last step a killed child was in; the text is fixed.
  static QElapsedTimer clock;
  if (!clock.isValid())
    clock.start();
  std::printf("STEP %lld %s\n", static_cast<long long>(clock.elapsed()), message);
  std::fflush(stdout);
  QEventLoop loop;
  QTimer poll, deadline;
  // A condition may run a nested real-network event loop. Never enter another
  // observation until that request has finished, nor busy-poll a locked PUT.
  poll.setInterval(pollMilliseconds);
  poll.setSingleShot(true);
  deadline.setSingleShot(true);
  deadline.setTimerType(Qt::PreciseTimer);
  QDeadlineTimer budget(budgetMilliseconds, Qt::PreciseTimer);
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
  deadline.start(budgetMilliseconds);
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
               PendingRead *pending = nullptr, const QUrl &destination = {}) {
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
  if (method == "MOVE") {
    require(withinServer(base, destination),
            "owned transfer destination escaped approved origin");
    request.setRawHeader("Destination", destination.toEncoded());
    request.setRawHeader("Overwrite", "F");
  }
  if (method == "PROPFIND") {
    request.setRawHeader("Depth", "0");
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/xml; charset=utf-8");
  }
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
/// RemoteEntry is what the server says one path is: a replayed or misdirected
/// write changes the file ID or the ETag.
struct RemoteEntry {
  QString fileId, etag;
  bool operator==(const RemoteEntry &) const = default;
};
/// remoteEntry reads one existing entry's identity with a Depth-0 PROPFIND.
RemoteEntry remoteEntry(const QUrl &base, const QString &uid,
                        const QString &collection, const QString &credential,
                        const QString &file) {
  QXmlStreamReader xml(
      dav(base, uid, collection, "PROPFIND", credential,
          "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\" "
          "xmlns:oc=\"http://owncloud.org/ns\"><d:prop><d:getetag/><oc:fileid/>"
          "</d:prop></d:propfind>",
          file));
  RemoteEntry entry;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement() && xml.name() == QLatin1String("fileid"))
      entry.fileId = xml.readElementText();
    else if (xml.isStartElement() && xml.name() == QLatin1String("getetag"))
      entry.etag = xml.readElementText();
  }
  require(!xml.hasError() && !entry.fileId.isEmpty() && !entry.etag.isEmpty(),
          "owned remote entry identity unavailable");
  return entry;
}
/// randomContent returns fresh bytes no earlier file of this run contains.
QByteArray randomContent(int size) {
  QByteArray content(size, Qt::Uninitialized);
  QRandomGenerator::system()->fillRange(
      reinterpret_cast<quint32 *>(content.data()),
      content.size() / sizeof(quint32));
  return content;
}
/// sha256 names content in assertions without ever printing it.
QByteArray sha256(const QByteArray &content) {
  return QCryptographicHash::hash(content, QCryptographicHash::Sha256);
}
/// localHash reads one local file completely; a missing file has no hash.
QByteArray localHash(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? sha256(file.readAll()) : QByteArray();
}
/// writeLocal creates one local file the watcher and the next cycle will see.
void writeLocal(const QString &path, const QByteArray &content) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) &&
              file.write(content) == content.size() && file.flush(),
          "owned transfer local content write failed");
}
/// firstFolder returns the single mapping's entry of a projection array.
QJsonObject firstFolder(const QJsonValue &folders) {
  return folders.toArray().first().toObject();
}
/// diagnostics returns the mapping's sanitized runtime counters.
QJsonObject diagnostics(Control &c) {
  return firstFolder(call(c, "diagnostics.snapshot")
                         .value("result")
                         .toObject()
                         .value("folders"));
}
/// counter reads one decimal diagnostics counter.
quint64 counter(const QJsonObject &folder, const char *key) {
  return folder.value(QLatin1String(key)).toString().toULongLong();
}
/// upToDate reports the single mapping reconciled on the current epoch.
bool upToDate(Control &c) {
  const auto folders =
      c.projection().value("snapshot").toObject().value("folders").toArray();
  return folders.size() == 1 &&
         folders.first().toObject().value("state") == "upToDate";
}
/// inUse reports the endpoint the committed binding names.
bool inUse(Control &c, const QString &endpoint) {
  for (const auto &entry :
       c.projection().value("snapshot").toObject().value("endpoints").toArray())
    if (entry.toObject().value("id") == endpoint &&
        entry.toObject().value("inUse").toBool())
      return true;
  return false;
}
/// Mapping is the one reserved remote collection mapped to one local folder.
struct Mapping {
  QUrl base;
  QString uid, collection, local, folder;
};
/// removeMapping ends a case by removing its mapping, retrying while the
/// daemon refuses because the mapping's next poll cycle is still running.
/// Notes: teardown only; nothing a case asserts depends on it.
void removeMapping(Control &c, const QString &folder) {
  wait(
      [&] {
        const auto reply = call(c, "folders.remove", {{"folderId", folder}});
        if (reply.value("result").toObject().value("operationId").toString()
                .isEmpty())
          return false;
        settle(c, reply);
        return true;
      },
      "owned mapping removal kept being refused", 250, 30000);
}/// mapCollection reserves the cleanup manifest, creates the collection and
/// the mapping, and waits for its first reconciliation.
Mapping mapCollection(Control &c, const QString &root, const QString &account,
                      const QString &credential) {
  Mapping m;
  m.base = canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_URL"));
  m.uid =
      readServerJson(m.base, "ocs/v2.php/cloud/user?format=json",
                     qEnvironmentVariable("NEXTCLOUD_TEST_USER"), credential)
          .value("ocs")
          .toObject()
          .value("data")
          .toObject()
          .value("id")
          .toString();
  m.collection = "mygo-network-" + QFileInfo(root).fileName().right(32);
  QSaveFile manifest(root + "/owned-content.json");
  manifest.setDirectWriteFallback(false);
  const auto reservation =
      QJsonDocument(QJsonObject{{"uid", m.uid}, {"collection", m.collection}})
          .toJson(QJsonDocument::Compact);
  require(manifest.open(QIODevice::WriteOnly) &&
              manifest.write(reservation) == reservation.size() &&
              manifest.commit(),
          "owned content cleanup reservation failed");
  dav(m.base, m.uid, m.collection, "MKCOL", credential);
  m.local = root + "/content";
  require(QDir().mkpath(m.local), "owned content local directory unavailable");
  const auto inspection = call(c, "folders.inspect",
                               {{"accountId", account},
                                {"remotePath", '/' + m.collection},
                                {"localPath", m.local}})
                              .value("result")
                              .toObject()
                              .value("inspectToken")
                              .toString();
  require(!inspection.isEmpty(), "owned transfer mapping inspection failed");
  settle(c, call(c, "folders.add", {{"inspectToken", inspection}}));
  wait([&] { return upToDate(c); },
       "owned transfer initial mapping did not reconcile");
  m.folder =
      firstFolder(c.projection().value("snapshot").toObject().value("folders"))
          .value("id")
          .toString();
  return m;
}
/// upload writes one local file and waits until the real server holds exactly
/// that content and the mapping is reconciled.
void upload(Control &c, const Mapping &m, const QString &credential,
            const QString &name, const QByteArray &content) {
  writeLocal(m.local + '/' + name, content);
  settle(c, call(c, "folders.syncNow", {{"folderId", m.folder}}));
  PendingRead pending;
  wait(
      [&] {
        return sha256(dav(m.base, m.uid, m.collection, "GET", credential, {},
                          name, &pending)) == sha256(content);
      },
      "owned content did not reach the real server", 500);
  wait([&] { return upToDate(c); }, "owned upload did not reconcile");
}
/// failOver breaks the primary and waits for automatic cross-origin recovery
/// of the mapping. Notes: the fault proves nothing unless the primary is in
/// use when it starts.
void failOver(Control &c, const QString &primary, const QString &alternate,
              const TransferFaults &faults,
              int reconcileMilliseconds = 180000) {
  require(inUse(c, primary),
          "owned active transfer was not on the primary address");
  faulted = true;
  faults.failPrimary(true);
  faults.holdResponse(false);
  faults.withhold({});
  try {
    wait([&] { return inUse(c, alternate); },
         "owned active transfer did not automatically fail over");
  } catch (...) {
    // Name what held the account back; only a stable reason code leaves.
    const auto folder = firstFolder(
        c.projection().value("snapshot").toObject().value("folders"));
    const auto word = [](QString text) {
      bool safe = !text.isEmpty() && text.size() <= 80;
      for (const auto character : text)
        safe = safe && (character.isLetter() || character == '_');
      return safe ? text : QStringLiteral("unknown");
    };
    std::fprintf(stderr, "owned failover blocked, folder %s reason %s\n",
                 qPrintable(word(folder.value("state").toString())),
                 qPrintable(word(folder.value("reasonCode").toString())));
    throw;
  }
  wait([&] { return upToDate(c); },
       "owned active transfer did not reconcile after automatic failover", 10,
       reconcileMilliseconds);
}
/// onPrimary returns the account to a fully recovered primary before a fault.
void onPrimary(Control &c, const QString &account, const QString &primary,
               const TransferFaults &faults) {
  faults.holdResponse(false);
  faults.withhold({});
  faults.failPrimary(false);
  // A process that has not faulted yet is still on its fresh primary binding.
  if (!faulted && inUse(c, primary)) {
    wait(faults.ready, "owned transfer endpoints not ready", 10, readyMilliseconds);
    return;
  }
  // Switching back onto an address whose circuit is still recovering would
  // let the automatic policy leave it again before the next fault.
  wait(faults.ready,
       "owned transfer endpoints did not recover from the last fault", 10,
       readyMilliseconds);
  settle(c, call(c, "endpoints.requestSwitch",
                 {{"accountId", account}, {"endpointId", primary}}));
  wait(faults.ready, "owned transfer fresh endpoint evidence unavailable", 10, readyMilliseconds);
}
/// lostReplyAcceptance loses the reply of a write the real server committed:
/// a MOVE and a DELETE of existing entries whose path another client then
/// creates again, and the final MOVE of a chunk session. Recovery must be
/// rediscovery on the other origin, never a second request (PLAN 12.3, S07,
/// S08, INV-05/08/09).
void lostReplyAcceptance(Control &c, const QString &root,
                         const QString &account, const QString &primary,
                         const QString &alternate, const QString &credential,
                         const TransferFaults &faults, const Mapping &m,
                         const QByteArray &journal) {
  const auto other =
      canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_ALTERNATE_URL"));
  const auto entry = [&](const QUrl &origin, const QString &name) {
    return remoteEntry(origin, m.uid, m.collection, credential, name);
  };
  const auto remoteHash = [&](const QString &name) {
    return sha256(
        dav(m.base, m.uid, m.collection, "GET", credential, {}, name));
  };
  // committed waits until the other origin shows the write's effect while
  // the daemon still waits for its reply.
  const auto committed = [&](const std::function<bool()> &effect,
                             const char *message) {
    wait([&] { return faults.withheld() > 0; },
         "owned write was never sent to the real server");
    wait(effect, message, 500);
    require(counter(diagnostics(c), "activeRequests") > 0 && !upToDate(c),
            "owned write reply was not withheld");
  };
  const auto recovered = [&](const QJsonObject &baseline) {
    failOver(c, primary, alternate, faults);
    faults.failPrimary(false);
    require(counter(diagnostics(c), "uncertainWrites") >
                counter(baseline, "uncertainWrites"),
            "owned lost reply left no unconfirmed-write evidence");
    require(journalIdentity(root) == journal,
            "owned lost-reply recovery replaced the native journal");
  };

  if (selected("move")) {
    // MOVE: the server renamed the entry; another client then puts a new
    // object at the old path. A replayed MOVE would carry that object away.
    const auto content = randomContent(smallSize),
               replacement = randomContent(smallSize);
    onPrimary(c, account, primary, faults);
    upload(c, m, credential, "moved-from.bin", content);
    const auto original = entry(m.base, "moved-from.bin");
    const auto baseline = diagnostics(c);
    faults.withhold("/moved-from.bin");
    require(
        QFile::rename(m.local + "/moved-from.bin", m.local + "/moved-to.bin"),
        "owned local rename failed");
    settle(c, call(c, "folders.syncNow", {{"folderId", m.folder}}));
    PendingRead target, source;
    committed(
        [&] {
          return sha256(dav(other, m.uid, m.collection, "GET", credential, {},
                            "moved-to.bin", &target)) == sha256(content);
        },
        "owned MOVE did not commit on the real server");
    dav(other, m.uid, m.collection, "GET", credential, {}, "moved-from.bin",
        &source);
    require(source.status == 404, "owned MOVE left its source on the server");
    const auto moved = entry(other, "moved-to.bin");
    require(moved.fileId == original.fileId,
            "owned rename did not reach the server as a MOVE");
    dav(other, m.uid, m.collection, "PUT", credential, replacement,
        "moved-from.bin");
    const auto recreated = entry(other, "moved-from.bin");
    recovered(baseline);
    require(entry(m.base, "moved-to.bin") == moved &&
                remoteHash("moved-to.bin") == sha256(content),
            "owned committed MOVE target was written again");
    require(entry(m.base, "moved-from.bin") == recreated &&
                remoteHash("moved-from.bin") == sha256(replacement),
            "owned re-created MOVE source was moved, removed or overwritten");
    require(localHash(m.local + "/moved-to.bin") == sha256(content) &&
                localHash(m.local + "/moved-from.bin") == sha256(replacement),
            "owned lost MOVE reply did not converge locally");
    std::puts("PASS real MOVE of an existing entry committed with its reply "
              "lost, source path re-created by another client, recovered by "
              "rediscovery without a second MOVE");
  }
  if (selected("delete")) {
    // DELETE: the server removed the entry; another client then creates the
    // path again. A replayed DELETE would destroy the new object.
    const auto content = randomContent(smallSize),
               replacement = randomContent(smallSize);
    onPrimary(c, account, primary, faults);
    upload(c, m, credential, "deleted.bin", content);
    const auto baseline = diagnostics(c);
    faults.withhold("/deleted.bin");
    require(QFile::remove(m.local + "/deleted.bin"),
            "owned local delete failed");
    settle(c, call(c, "folders.syncNow", {{"folderId", m.folder}}));
    PendingRead gone;
    committed(
        [&] {
          dav(other, m.uid, m.collection, "GET", credential, {}, "deleted.bin",
              &gone);
          return gone.status == 404;
        },
        "owned DELETE did not commit on the real server");
    dav(other, m.uid, m.collection, "PUT", credential, replacement,
        "deleted.bin");
    const auto recreated = entry(other, "deleted.bin");
    recovered(baseline);
    require(entry(m.base, "deleted.bin") == recreated &&
                remoteHash("deleted.bin") == sha256(replacement),
            "owned re-created entry was removed by a replayed DELETE");
    require(localHash(m.local + "/deleted.bin") == sha256(replacement),
            "owned lost DELETE reply did not converge locally");
    std::puts("PASS real DELETE of an existing entry committed with its reply "
              "lost, path re-created by another client, recovered by "
              "rediscovery without a second DELETE");
  }
  if (selected("final-move")) {
    // Final MOVE of a chunk session: the server assembled the file; the
    // session row must be adopted by checksum, with no chunk and no MOVE sent
    // again.
    const auto content = randomContent(contentSize);
    onPrimary(c, account, primary, faults);
    const auto baseline = diagnostics(c);
    faults.withhold("/.file");
    writeLocal(m.local + "/final-move.bin", content);
    settle(c, call(c, "folders.syncNow", {{"folderId", m.folder}}));
    PendingRead assembled;
    committed(
        [&] {
          return sha256(dav(other, m.uid, m.collection, "GET", credential, {},
                            "final-move.bin", &assembled)) == sha256(content);
        },
        "owned final MOVE did not commit on the real server");
    const auto assembledEntry = entry(other, "final-move.bin");
    recovered(baseline);
    require(counter(diagnostics(c), "uploadBytes") -
                    counter(baseline, "uploadBytes") <
                quint64(contentSize) + smallSize,
            "owned committed chunk session was uploaded again");
    require(entry(m.base, "final-move.bin") == assembledEntry &&
                remoteHash("final-move.bin") == sha256(content),
            "owned committed final MOVE was written again");
    require(localHash(m.local + "/final-move.bin") == sha256(content),
            "owned lost final MOVE reply changed the local file");
    std::puts("PASS real chunk session whose final MOVE committed with its "
              "reply lost, adopted on the other origin without resending a "
              "chunk or the MOVE");
  }
}
/// streamHash hashes a device in bounded pieces and returns its SHA-256 and
/// length; nothing of it is kept.
std::pair<QByteArray, qint64> streamHash(QIODevice &device) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 length = 0;
  for (auto bytes = device.read(1048576); !bytes.isEmpty();
       bytes = device.read(1048576)) {
    hash.addData(bytes);
    length += bytes.size();
  }
  return {hash.result(), length};
}
/// remoteStreamHash downloads one entry from an approved origin without
/// keeping it and returns its SHA-256 and length.
std::pair<QByteArray, qint64> remoteStreamHash(const Mapping &m,
                                               const QString &credential,
                                               const QString &file) {
  const auto url =
      m.base.resolved(QUrl("remote.php/dav/files/" +
                           QString::fromLatin1(QUrl::toPercentEncoding(m.uid)) +
                           '/' + m.collection + '/' + file));
  require(withinServer(m.base, url),
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
  auto *reply = manager.get(request);
  reply->setReadBufferSize(4 * 1048576);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 length = 0;
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
    const auto bytes = reply->readAll();
    hash.addData(bytes);
    length += bytes.size();
  });
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
  deadline.start(1800000);
  if (!reply->isFinished())
    loop.exec();
  const auto rest = reply->readAll();
  hash.addData(rest);
  length += rest.size();
  require(
      reply->error() == QNetworkReply::NoError &&
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() ==
              200,
      "owned large remote read failed");
  return {hash.result(), length};
}
/// largeUploadAcceptance cuts a chunk session of a file above 4 GiB after its
/// 4 GiB boundary and requires it to resume on the other origin (S06,
/// INV-05/08/09). Notes: content is written, sent and verified as a stream.
void largeUploadAcceptance(Control &c, const QString &root,
                           const QString &account, const QString &primary,
                           const QString &alternate, const QString &credential,
                           const TransferFaults &faults, const Mapping &m,
                           const QByteArray &journal) {
  onPrimary(c, account, primary, faults);
  const auto baseline = diagnostics(c);
  QCryptographicHash written(QCryptographicHash::Sha256);
  {
    QFile file(m.local + "/large.bin");
    require(file.open(QIODevice::WriteOnly), "owned large file unavailable");
    for (qint64 done = 0; done < largeSize; done += 1048576) {
      const auto block = randomContent(1048576);
      written.addData(block);
      require(file.write(block) == block.size(), "owned large write failed");
    }
    require(file.flush(), "owned large file flush failed");
  }
  const auto expected = written.result();
  settle(c, call(c, "folders.syncNow", {{"folderId", m.folder}}));
  wait(
      [&] {
        const auto d = diagnostics(c);
        return counter(d, "activeUploads") > 0 &&
               counter(d, "uploadBytes") >=
                   counter(baseline, "uploadBytes") + quint64(largeCut);
      },
      "owned large upload did not pass its 4 GiB boundary", 250, 2400000);
  failOver(c, primary, alternate, faults, 1200000);
  faults.failPrimary(false);
  const auto recovered = diagnostics(c);
  require(counter(recovered, "uncertainWrites") >
              counter(baseline, "uncertainWrites"),
          "owned interrupted upload lost its unconfirmed-write evidence");
  // A restarted session would send more than 4 GiB a second time.
  require(counter(recovered, "uploadBytes") - counter(baseline, "uploadBytes") <
              quint64(largeSize) + 1536ULL * 1048576,
          "owned large chunk session was restarted instead of resumed");
  QFile local(m.local + "/large.bin");
  require(local.open(QIODevice::ReadOnly) &&
              streamHash(local) == std::pair(expected, largeSize),
          "owned large recovery local hash mismatch");
  require(remoteStreamHash(m, credential, "large.bin") ==
              std::pair(expected, largeSize),
          "owned large recovery remote hash or length mismatch");
  require(journalIdentity(root) == journal,
          "owned large recovery replaced the native journal");
  std::puts("PASS real chunk session of a file above 4 GiB cut after its "
            "4 GiB boundary, resumed on the other origin, matching streamed "
            "hashes and length, original native journal");
}
/// die ends this process the way a kill does: no destructor, no flush of
/// Qt or SQLite state, no reply to anyone.
[[noreturn]] void die() {
  TerminateProcess(GetCurrentProcess(), 75);
  std::abort();
}
/// spin drives the event loop for a bounded time without a condition.
void spin(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}
/// fileSafetyAcceptance runs fault-free cases against the real server: renames
/// and directory moves in both directions (S04), deletion protection for one
/// directory with many descendants with its direction and count (S11, S12),
/// a proposal invalidated by a later change (S20), a leased local root (S13)
/// and a remote root that answers 404 (S14); or, as file-safety-bulk on a
/// profile of its own, only 100 server deletions held by the absolute
/// threshold alone (S11).
void fileSafetyAcceptance(Control &c, const QString &credential,
                          const Mapping &m) {
  constexpr int size = 4096;
  const auto sync = [&] {
    settle(c, call(c, "folders.syncNow", {{"folderId", m.folder}}));
  };
  const auto folder = [&] {
    return firstFolder(
        c.projection().value("snapshot").toObject().value("folders"));
  };
  const auto decisions = [&] {
    return c.projection()
        .value("snapshot")
        .toObject()
        .value("decisions")
        .toArray();
  };
  const auto entry = [&](const QString &name) {
    return remoteEntry(m.base, m.uid, m.collection, credential, name);
  };
  const auto absent = [&](const QString &name) {
    PendingRead read;
    dav(m.base, m.uid, m.collection, "GET", credential, {}, name, &read);
    return read.status == 404;
  };
  const auto remoteHash = [&](const QString &name) {
    PendingRead read;
    return sha256(
        dav(m.base, m.uid, m.collection, "GET", credential, {}, name, &read));
  };
  const auto converge = [&](const std::function<bool()> &condition,
                            const char *message) {
    wait([&] { return condition() && upToDate(c); }, message, 250);
  };
  const auto move = [&](const QString &from, const QString &to) {
    const auto path = [&](const QString &name) {
      return m.base.resolved(
          QUrl("remote.php/dav/files/" +
               QString::fromLatin1(QUrl::toPercentEncoding(m.uid)) + '/' +
               m.collection + '/' + name));
    };
    dav(m.base, m.uid, m.collection, "MOVE", credential, {}, from, nullptr,
        path(to));
  };
  if (fileSafetyBulk) {
    // S11 at scale: another client deletes 100 single files, under a fifth
    // of a baseline above 500, so only the absolute threshold applies; they
    // are held as local deletions and no local file goes before approval.
    constexpr int bulk = 510, removed = 100;
    const auto bulkName = [](int i) {
      return QString("bulk/item-%1.bin").arg(i, 3, 10, QChar('0'));
    };
    require(QDir().mkpath(m.local + "/bulk"), "owned bulk directory failed");
    QHash<int, QByteArray> contents;
    for (int i = 0; i < bulk; ++i) {
      contents.insert(i, randomContent(64));
      writeLocal(m.local + '/' + bulkName(i), contents.value(i));
    }
    sync();
    converge(
        [&] {
          for (int i = 0; i < bulk; i += 51)
            if (remoteHash(bulkName(i)) != sha256(contents.value(i)))
              return false;
          return remoteHash(bulkName(bulk - 1)) ==
                 sha256(contents.value(bulk - 1));
        },
        "owned bulk files did not reach the real server");
    // Paused while the other client deletes: a poll in the middle of the
    // deletions would see fewer than 100 and apply them without review.
    settle(c, call(c, "folders.pause", {{"folderId", m.folder}}));
    for (int i = 0; i < removed; ++i)
      dav(m.base, m.uid, m.collection, "DELETE", credential, {},
          bulkName(i * 5));
    settle(c, call(c, "folders.resume", {{"folderId", m.folder}}));
    const auto localCount = [&] {
      return QDir(m.local + "/bulk").entryList(QDir::Files).size();
    };
    sync();
    wait([&] { return folder().value("state") == "needsReview"; },
         "owned 100 server deletions were not held for review", 250);
    const auto proposal = decisions().first().toObject();
    require(decisions().size() == 1 &&
                proposal.value("localDeletes").toInteger() == removed &&
                proposal.value("remoteDeletes").toInteger() == 0,
            "owned 100-deletion proposal has the wrong direction or count");
    require(localCount() == bulk &&
                localHash(m.local + '/' + bulkName(0)) ==
                    sha256(contents.value(0)),
            "owned local files were removed before the decision");
    require(call(c, "decisions.resolve",
                 {{"decisionId", proposal.value("id")},
                  {"proposalDigest", proposal.value("digest")},
                  {"action", "approveDeletion"}})
                .contains("result"),
            "owned 100-deletion approval refused");
    converge([&] { return localCount() == bulk - removed; },
             "owned approved server deletions did not reach the local root");
    require(QFile::exists(m.local + '/' + bulkName(1)) &&
                !QFile::exists(m.local + '/' + bulkName(5)) &&
                decisions().isEmpty(),
            "owned approved deletion removed other local files");
    std::printf("PASS real 100 server deletions under a fifth of a %d-file "
                "baseline held as 100 local deletions before propagation, "
                "applied after approval\n",
                bulk);
    return;
  }
  QHash<QString, QByteArray> hashes;
  QStringList names{"single.bin", "moved/a.bin", "moved/b.bin"};
  for (int i = 0; i < 8; ++i)
    names.append(QString("keep-%1.bin").arg(i, 2, 10, QChar('0')));
  for (int i = 0; i < 12; ++i)
    names.append(QString("tree/leaf-%1.bin").arg(i, 2, 10, QChar('0')));
  for (const auto &name : names) {
    const auto content = randomContent(size);
    hashes.insert(name, sha256(content));
    require(QDir().mkpath(QFileInfo(m.local + '/' + name).absolutePath()),
            "owned case directory unavailable");
    writeLocal(m.local + '/' + name, content);
  }
  sync();
  converge(
      [&] {
        for (const auto &name : names)
          if (remoteHash(name) != hashes.value(name))
            return false;
        return true;
      },
      "owned case content did not reach the real server");

  {
    // S04: a rename or move must arrive as one, on either side: the server
    // keeps the file ID, and no content travels again.
    const auto before = diagnostics(c);
    const auto single = entry("single.bin"), a = entry("moved/a.bin"),
               b = entry("moved/b.bin");
    require(
        QFile::rename(m.local + "/single.bin",
                      m.local + "/single-renamed.bin") &&
            QDir().mkpath(m.local + "/nested") &&
            QDir().rename(m.local + "/moved", m.local + "/nested/moved-away"),
        "owned local rename or directory move failed");
    sync();
    converge(
        [&] {
          return absent("single.bin") && absent("moved/a.bin") &&
                 remoteHash("nested/moved-away/b.bin") ==
                     hashes.value("moved/b.bin");
        },
        "owned local rename and directory move did not reach the server");
    require(entry("single-renamed.bin").fileId == single.fileId &&
                entry("nested/moved-away/a.bin").fileId == a.fileId &&
                entry("nested/moved-away/b.bin").fileId == b.fileId,
            "owned local rename was propagated as delete and upload");
    move("keep-00.bin", "keep-renamed.bin");
    move("nested/moved-away", "moved-back");
    sync();
    converge(
        [&] {
          return !QFile::exists(m.local + "/keep-00.bin") &&
                 !QFile::exists(m.local + "/nested/moved-away") &&
                 localHash(m.local + "/keep-renamed.bin") ==
                     hashes.value("keep-00.bin") &&
                 localHash(m.local + "/moved-back/a.bin") ==
                     hashes.value("moved/a.bin") &&
                 localHash(m.local + "/moved-back/b.bin") ==
                     hashes.value("moved/b.bin");
        },
        "owned remote rename and directory move did not reach the local root");
    const auto after = diagnostics(c);
    require(
        counter(after, "uploadBytes") - counter(before, "uploadBytes") < size &&
            counter(after, "downloadBytes") - counter(before, "downloadBytes") <
                size &&
            decisions().isEmpty(),
        "owned renames moved content again or asked for a deletion review");
    std::puts("PASS real file rename and directory move in both directions, "
              "server file IDs kept, no content transferred again, no "
              "deletion review");
  }
  {
    // S11/S12: one directory removed locally is thirteen deletions for the
    // server, past ten and a fifth of the baseline: nothing may be removed
    // before the user decides.
    require(QDir(m.local + "/tree").removeRecursively(),
            "owned local directory removal failed");
    sync();
    wait([&] { return folder().value("state") == "needsReview"; },
         "owned directory deletion was not held for review", 250);
    require(decisions().size() == 1, "owned deletion proposal missing");
    const auto first = decisions().first().toObject();
    require(first.value("remoteDeletes").toInteger() == 13 &&
                first.value("localDeletes").toInteger() == 0,
            "owned deletion proposal has the wrong direction or count");
    require(remoteHash("tree/leaf-00.bin") == hashes.value("tree/leaf-00.bin"),
            "owned directory was removed before the decision");
    // S20: the data changes after the proposal; approving the old proposal
    // must not authorize the new set.
    require(QFile::remove(m.local + "/keep-01.bin"),
            "owned later local deletion failed");
    require(call(c, "decisions.resolve",
                 {{"decisionId", first.value("id")},
                  {"proposalDigest", first.value("digest")},
                  {"action", "approveDeletion"}})
                .contains("result"),
            "owned deletion approval refused");
    wait(
        [&] {
          return decisions().size() == 1 &&
                 decisions().first().toObject().value("digest") !=
                     first.value("digest") &&
                 folder().value("state") == "needsReview";
        },
        "owned changed deletion set was not proposed again", 250);
    const auto second = decisions().first().toObject();
    require(second.value("remoteDeletes").toInteger() == 14 &&
                remoteHash("tree/leaf-11.bin") ==
                    hashes.value("tree/leaf-11.bin") &&
                remoteHash("keep-01.bin") == hashes.value("keep-01.bin"),
            "owned stale approval removed data from the server");
    require(call(c, "decisions.resolve",
                 {{"decisionId", second.value("id")},
                  {"proposalDigest", second.value("digest")},
                  {"action", "approveDeletion"}})
                .contains("result"),
            "owned second deletion approval refused");
    converge(
        [&] { return absent("tree/leaf-00.bin") && absent("keep-01.bin"); },
        "owned approved deletion did not reach the server");
    require(remoteHash("keep-02.bin") == hashes.value("keep-02.bin") &&
                decisions().isEmpty(),
            "owned approved deletion removed more than proposed");
    std::puts("PASS real directory with twelve descendants held before "
              "propagation with direction and count, stale approval after a "
              "later change refused, matching approval applied");
  }
  {
    // S13: the mapped root cannot be taken away under the daemon.
    const bool renamed = QDir().rename(m.local, m.local + "-gone");
    if (renamed)
      QDir().rename(m.local + "-gone", m.local);
    require(!renamed, "owned mapped local root could be renamed away");
    // S14: another client removes the remote root. A 404 there is not an
    // empty folder: no local file may be deleted for it.
    dav(m.base, m.uid, m.collection, "DELETE", credential);
    sync();
    wait(
        [&] {
          const auto state = folder().value("state").toString();
          return state == "error" || state == "needsReview";
        },
        "owned missing remote root was not reported", 250);
    spin(5000);
    require(folder().value("state") != "upToDate" &&
                localHash(m.local + "/keep-02.bin") ==
                    hashes.value("keep-02.bin") &&
                localHash(m.local + "/moved-back/a.bin") ==
                    hashes.value("moved/a.bin") &&
                localHash(m.local + "/single-renamed.bin") ==
                    hashes.value("single.bin"),
            "owned missing remote root emptied the local folder");
    // Only a stable state and reason code leave the process.
    const auto word = [](QString text) {
      bool safe = !text.isEmpty() && text.size() <= 80;
      for (const auto character : text)
        safe = safe && (character.isLetter() || character == '_');
      return safe ? text : QStringLiteral("none");
    };
    std::printf("PASS real mapped root refused removal while leased; remote "
                "root answering 404 reported as %s %s, no local file "
                "deleted\n",
                qPrintable(word(folder().value("state").toString())),
                qPrintable(word(folder().value("reasonCode").toString())));
  }
}
/// sleepAcceptance is the simulated sleep (R15, PLAN §15.2 item 7): an upload
/// held unfinished on the primary, then this whole process, the daemon with
/// it, suspended past the freshness, failure, success and drain windows while
/// the relay drops every connection and leaves only the alternate reachable.
/// After waking the daemon must notice the dead connections, read nothing as
/// deleted while the primary is unreachable, switch only once its folder went
/// idle and with a recorded reason, and finish the upload once on the
/// alternate with the same journal and folder identity (INV-05/06/08/09/12).
void sleepAcceptance(Control &c, const QString &root, const QString &account,
                     const QString &primary, const QString &alternate,
                     const QString &credential, const TransferFaults &faults,
                     const Mapping &m, const QByteArray &journal) {
  constexpr qint64 asleep = 35000;
  constexpr int size = 6 * 1048576;
  const auto kept = randomContent(4096), content = randomContent(size);
  upload(c, m, credential, "kept.bin", kept);
  const auto activity = [&] {
    return c.projection()
        .value("snapshot")
        .toObject()
        .value("activity")
        .toArray();
  };
  qint64 seen = 0;
  for (const auto &entry : activity())
    seen = std::max(seen,
                    entry.toObject().value("cursor").toString().toLongLong());
  const auto before = switchFacts(c, account);
  const auto baseline = diagnostics(c);
  writeLocal(m.local + "/asleep.bin", content);
  faults.stall(2 * 1048576);
  settle(c, call(c, "folders.syncNow", {{"folderId", m.folder}}));
  wait(
      [&] {
        const auto d = diagnostics(c);
        return counter(d, "activeUploads") > 0 &&
               counter(d, "uploadBytes") >=
                   counter(baseline, "uploadBytes") + 1048576;
      },
      "owned upload was not held unfinished before the sleep", 25);
  require(inUse(c, primary) && !upToDate(c),
          "owned held upload was not on the primary address");
  const auto [suspended, still] = faults.sleep(asleep);
  QElapsedTimer woken;
  woken.start();
  if (suspended < asleep - 50 || still < asleep - 1000)
    throw std::runtime_error("owned process did not stand still as a whole, "
                             "suspended " +
                             std::to_string(suspended) + " ms, still " +
                             std::to_string(still) + " ms");
  const auto other =
      canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_ALTERNATE_URL"));
  bool idle = false;
  qint64 moved = 0;
  wait(
      [&] {
        // Offline is not empty: nothing goes locally and nothing is proposed
        // for deletion while the server cannot be reached.
        require(localHash(m.local + "/kept.bin") == sha256(kept) &&
                    localHash(m.local + "/asleep.bin") == sha256(content),
                "owned local file removed while the server was unreachable");
        require(c.projection()
                    .value("snapshot")
                    .toObject()
                    .value("decisions")
                    .toArray()
                    .isEmpty(),
                "owned unreachable server was read as deleted files");
        const auto d = diagnostics(c);
        if (d.value("epoch").toString().toLongLong() == before.epoch) {
          require(!upToDate(c),
                  "owned unreachable primary was reported up to date");
          idle = idle || (counter(d, "activeRequests") == 0 &&
                          counter(d, "transportFailures") >
                              counter(baseline, "transportFailures"));
          return false;
        }
        moved = woken.elapsed();
        return true;
      },
      "owned daemon did not move to the reachable address after waking", 25);
  require(idle, "owned switch did not wait for the folder to go idle after "
                "its dead connections failed");
  PendingRead pending;
  // A cycle on the alternate the server refused (the fixture's SQLite answers
  // a busy database with 503 after the whole body) is retried and may send
  // the body once more; each failed cycle stays visible for its retry delay.
  qint64 refused = 0;
  bool failing = false;
  wait(
      [&] {
        const bool failed = diagnostics(c).value("state") == "error";
        refused += failed && !failing;
        failing = failed;
        return upToDate(c) &&
               sha256(dav(other, m.uid, m.collection, "GET", credential, {},
                          "asleep.bin", &pending)) == sha256(content);
      },
      "owned held upload did not complete after waking", 100);
  const auto completed = woken.elapsed();
  const auto after = switchFacts(c, account);
  require(after.endpoint == alternate && after.epoch > before.epoch,
          "owned binding did not move to the alternate");
  require(after.folders == before.folders &&
              after.credential == before.credential &&
              journalIdentity(root) == journal,
          "owned sleep changed the journal, folder identity or credential");
  bool recorded = false;
  qint64 uploads = 0;
  for (const auto &value : activity()) {
    const auto entry = value.toObject();
    if (entry.value("cursor").toString().toLongLong() <= seen)
      continue;
    recorded = recorded || (entry.value("kind") == "switched" &&
                            entry.value("fromId") == primary &&
                            entry.value("toId") == alternate &&
                            entry.value("reason") == "unreachable");
    if (entry.value("kind") == "uploaded")
      uploads += entry.value("count").toInt();
  }
  require(recorded, "owned switch after waking has no recorded reason");
  const auto recovered = diagnostics(c);
  require(counter(recovered, "uncertainWrites") >
              counter(baseline, "uncertainWrites"),
          "owned interrupted upload lost its unconfirmed-write evidence");
  // The interrupted attempt counts at most its whole body (the relay accepts
  // all of it), each refused one and the completed one a whole body each: a
  // second completed upload would exceed them all.
  require(uploads == 1 && counter(recovered, "uploadBytes") -
                                  counter(baseline, "uploadBytes") <=
                              quint64(2 + refused) * size,
          "owned held upload was sent more than once after waking");
  require(QDir(m.local).entryList(QDir::Files) ==
                  QStringList{"asleep.bin", "kept.bin"} &&
              sha256(dav(other, m.uid, m.collection, "GET", credential, {},
                         "kept.bin")) == sha256(kept),
          "owned sleep left a conflict copy or lost the kept file");
  std::printf("PASS simulated sleep: whole process suspended %lld ms, threads "
              "still %lld ms, during a held upload; woke where only the "
              "alternate answers; dead connections failed, nothing read as "
              "deleted, switch after the folder went idle %lld ms after "
              "waking with reason unreachable, upload completed once %lld ms "
              "after waking, %lld server refusals retried, hashes, journal "
              "and folder identity unchanged\n",
              static_cast<long long>(suspended), static_cast<long long>(still),
              static_cast<long long>(moved), static_cast<long long>(completed),
              static_cast<long long>(refused));
}
} // namespace

void largeTransferAcceptance() {
  chunkedTransferAcceptance();
  large = true;
}

void sleepResumeAcceptance() { sleepResume = true; }

void selectTransferCase(const QString &name) { transferCase = name; }

void fileSafetyTransferAcceptance(bool bulk) {
  fileSafety = true;
  fileSafetyBulk = bulk;
  NetworkTunnel::setThroughput(65536);
}

void uncertainTransferAcceptance() {
  chunkedTransferAcceptance();
  uncertain = true;
}

void switchKillAcceptance(Control &c, const QString &root,
                          const QString &account, const QString &primary,
                          const QString &alternate, const QString &credential,
                          const QString &stage, const TransferFaults &faults) {
#ifndef SYNCD_WITH_ENGINE
  Q_UNUSED(c);
  Q_UNUSED(root);
  Q_UNUSED(account);
  Q_UNUSED(primary);
  Q_UNUSED(alternate);
  Q_UNUSED(credential);
  Q_UNUSED(stage);
  Q_UNUSED(faults);
  throw std::runtime_error("switch kill acceptance requires an engine build");
#else
  require(QStringList{"prepared", "quiescing", "verifying", "committed",
                      "reconciling"}
              .contains(stage),
          "owned switch kill stage unknown");
  const auto m = mapCollection(c, root, account, credential);
  QJsonObject files;
  for (const auto &name : {"seed-a.bin", "seed-b.bin"}) {
    const auto content = randomContent(smallSize);
    upload(c, m, credential, name, content);
    files.insert(name, QString::fromLatin1(sha256(content).toHex()));
  }
  wait(faults.ready, "owned switch kill endpoints not ready", 10, readyMilliseconds);
  const bool transfers = stage == "quiescing" || stage == "reconciling";
  const auto content = randomContent(contentSize);
  if (transfers)
    files.insert("stage.bin", QString::fromLatin1(sha256(content).toHex()));
  const auto before = switchFacts(c, account);
  require(before.endpoint == primary && before.phase.isEmpty(),
          "owned switch kill did not start from a stable primary binding");
  // Everything the restarted process is held to is recorded before the kill.
  QSaveFile state(root + "/switch-kill.json");
  state.setDirectWriteFallback(false);
  const auto record =
      QJsonDocument(
          QJsonObject{
              {"account", account},
              {"primary", primary},
              {"alternate", alternate},
              {"epoch", QString::number(before.epoch)},
              {"credential", before.credential},
              {"folders", QJsonArray::fromStringList(before.folders)},
              {"journal", QString::fromLatin1(journalIdentity(root).toHex())},
              {"uid", m.uid},
              {"collection", m.collection},
              {"files", files}})
          .toJson(QJsonDocument::Compact);
  require(state.open(QIODevice::WriteOnly) &&
              state.write(record) == record.size() && state.commit(),
          "owned switch kill record failed");
  const auto request = [&] {
    require(!call(c, "endpoints.requestSwitch",
                  {{"accountId", account}, {"endpointId", alternate}})
                 .value("result")
                 .toObject()
                 .value("operationId")
                 .toString()
                 .isEmpty(),
            "owned switch request refused");
  };
  const auto uploading = [&](const QJsonObject &baseline) {
    wait(
        [&] {
          const auto d = diagnostics(c);
          return counter(d, "activeUploads") > 0 &&
                 counter(d, "uploadBytes") >=
                     counter(baseline, "uploadBytes") + 65536;
        },
        "owned unfinished upload was not observed");
  };
  if (stage == "prepared") {
    // The intent and its receipt are durable; nothing has drained yet.
    request();
    require(switchFacts(c, account).phase == "prepared",
            "owned switch intent was not persisted");
    die();
  }
  if (stage == "quiescing") {
    // A healthy transfer is still running on the old endpoint.
    const auto baseline = diagnostics(c);
    // The old address keeps the upload unfinished until the kill.
    NetworkTunnel::setStallAfter(2 * 1048576);
    writeLocal(m.local + "/stage.bin", content);
    settle(c, call(c, "folders.syncNow", {{"folderId", m.folder}}));
    uploading(baseline);
    request();
    spin(1000);
    const auto now = switchFacts(c, account);
    require(now.phase == "prepared" && now.endpoint == primary &&
                counter(diagnostics(c), "activeUploads") > 0,
            "owned switch was not waiting for an active transfer");
    die();
  }
  if (stage == "verifying") {
    // Old runtime retired; the target's credentialed verification is about
    // to be sent.
    faults.onTargetRequest([&](const QString &path) {
      if (path.contains("/cloud/capabilities") &&
          switchFacts(c, account).phase == "prepared")
        die();
    });
    request();
  }
  if (stage == "committed") {
    // Binding and epoch are published; no request of the new epoch has left.
    faults.onTargetRequest([&](const QString &) {
      const auto now = switchFacts(c, account);
      if (now.phase == "reconciling" && now.endpoint == alternate)
        die();
    });
    request();
  }
  if (stage == "reconciling") {
    // The new endpoint already received part of a file of the first cycle.
    settle(c, call(c, "folders.pause", {{"folderId", m.folder}}));
    writeLocal(m.local + "/stage.bin", content);
    request();
    wait(
        [&] {
          const auto now = switchFacts(c, account);
          return now.phase == "reconciling" && now.endpoint == alternate;
        },
        "owned switch did not commit with a paused mapping pending");
    const auto baseline = diagnostics(c);
    // The new address keeps the first cycle's upload unfinished until the kill.
    NetworkTunnel::setStallAfter(2 * 1048576, false);
    settle(c, call(c, "folders.resume", {{"folderId", m.folder}}));
    uploading(baseline);
    const auto now = switchFacts(c, account);
    require(now.phase == "reconciling" && now.endpoint == alternate,
            "owned reconciliation finished before the kill");
    die();
  }
  spin(120000);
  throw std::runtime_error("owned switch never reached its kill stage");
#endif
}

void switchRecoverAcceptance(Control &c, const QString &root,
                             const QString &stage) {
#ifndef SYNCD_WITH_ENGINE
  Q_UNUSED(c);
  Q_UNUSED(root);
  Q_UNUSED(stage);
  throw std::runtime_error("switch kill acceptance requires an engine build");
#else
  QFile file(root + "/switch-kill.json");
  require(file.open(QIODevice::ReadOnly) && file.size() < 65536,
          "owned switch kill record unreadable");
  const auto state = QJsonDocument::fromJson(file.readAll()).object();
  file.close();
  const auto account = state.value("account").toString();
  const auto accounts =
      c.projection().value("snapshot").toObject().value("accounts").toArray();
  require(accounts.size() == 1 &&
              accounts.first().toObject().value("id") == account,
          "restart lost or replaced the account");
  const auto identities = [&](const SwitchFacts &facts) {
    return facts.credential == state.value("credential").toString() &&
           QJsonArray::fromStringList(facts.folders) ==
               state.value("folders").toArray();
  };
  const auto started = switchFacts(c, account);
  require(identities(started) && !readCredential(started.credential).isEmpty(),
          "restart changed the credential reference or a folder binding");
  require(started.interrupted.contains("OPERATION_INTERRUPTED"),
          "restart did not fail the interrupted switch receipt");
  wait([&] { return switchFacts(c, account).phase.isEmpty() && upToDate(c); },
       "restart did not reconcile the interrupted switch");
  const auto settled = switchFacts(c, account);
  const bool committed = stage == "committed" || stage == "reconciling";
  // An uncommitted target is only a hint; a committed binding is never
  // rolled back. Either way the old process's epoch is not inherited.
  require(settled.endpoint ==
              state.value(committed ? "alternate" : "primary").toString(),
          committed ? "restart abandoned a committed binding"
                    : "restart adopted an uncommitted switch target");
  require(settled.epoch >= state.value("epoch").toString().toLongLong() + 2,
          "restart reused an endpoint epoch");
  require(identities(settled) &&
              settled.interrupted.contains("OPERATION_INTERRUPTED"),
          "recovery changed durable identities or revived the failed receipt");
  require(journalIdentity(root).toHex() ==
              state.value("journal").toString().toLatin1(),
          "restart replaced the native journal");
  const auto base = canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_URL"));
  const auto files = state.value("files").toObject();
  for (auto it = files.begin(); it != files.end(); ++it) {
    const auto expected = QByteArray::fromHex(it.value().toString().toLatin1());
    require(localHash(root + "/content/" + it.key()) == expected,
            "restart recovery local hash mismatch");
    require(sha256(dav(base, state.value("uid").toString(),
                       state.value("collection").toString(), "GET",
                       settled.credential, {}, it.key())) == expected,
            "restart recovery remote hash mismatch");
  }
  // Only the file the kill interrupted may travel again; the reconciled
  // ones must be neither uploaded nor downloaded.
  const auto d = diagnostics(c);
  require(counter(d, "uploadBytes") <
                  quint64(files.contains("stage.bin") ? contentSize : 0) +
                      smallSize / 2 &&
              counter(d, "downloadBytes") < quint64(smallSize / 2),
          "restart recovery transferred reconciled files again");
  // The mapping stays: a removal can be refused while the next poll cycle
  // runs, and the parent cleanup owns the collection and the profile.
  std::printf("PASS process kill at switch stage %s, restart on the %s "
              "binding with a fresh epoch, same credential reference, folder "
              "binding and native journal, matching hashes, no retransfer of "
              "reconciled files\n",
              qPrintable(stage), committed ? "committed" : "previous");
#endif
}

// chunkedTransferAcceptance selects one file past the upstream threshold.
// Notes: needs an isolated server without the shared account's quota; the
// relay runs faster so the first chunk fits the observation deadline.
void chunkedTransferAcceptance() {
  chunked = true;
  contentSize = int(firstChunk) + 12 * 1024 * 1024;
  NetworkTunnel::setThroughput(65536);
  NetworkTunnel::setTarget("127.0.0.1");
}

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
  // The cleanup child has no relay: an isolated fixture names its direct
  // loopback origin for this one request.
  dav(canonicalServer(
          qEnvironmentVariable("NEXTCLOUD_TEST_CLEANUP_URL",
                               qEnvironmentVariable("NEXTCLOUD_TEST_URL"))),
      manifest.value("uid").toString(), collection, "DELETE", credential);
  require(file.remove(), "owned content cleanup manifest removal failed");
}

// Notes: covers INV-02/05/06/08/09/13.
void networkTransferAcceptance(Control &c, const QString &root,
                               const QString &account, const QString &primary,
                               const QString &alternate,
                               const QString &credential,
                               const TransferFaults &faults) {
#ifndef SYNCD_WITH_ENGINE
  Q_UNUSED(c);
  Q_UNUSED(root);
  Q_UNUSED(account);
  Q_UNUSED(primary);
  Q_UNUSED(alternate);
  Q_UNUSED(credential);
  Q_UNUSED(faults);
  throw std::runtime_error(
      "owned transfer acceptance requires an engine build");
#else
  const auto &failPrimary = faults.failPrimary;
  const auto &holdResponse = faults.holdResponse;
  const auto m = mapCollection(c, root, account, credential);
  const auto journal = journalIdentity(root);
  if (uncertain) {
    lostReplyAcceptance(c, root, account, primary, alternate, credential,
                        faults, m, journal);
    removeMapping(c, m.folder);
    return;
  }
  if (large) {
    largeUploadAcceptance(c, root, account, primary, alternate, credential,
                          faults, m, journal);
    removeMapping(c, m.folder);
    return;
  }
  if (sleepResume) {
    sleepAcceptance(c, root, account, primary, alternate, credential, faults,
                    m, journal);
    removeMapping(c, m.folder);
    return;
  }
  if (fileSafety) {
    fileSafetyAcceptance(c, credential, m);
    require(journalIdentity(root) == journal,
            "owned file-safety cases replaced the native journal");
    removeMapping(c, m.folder);
    return;
  }
  const auto &base = m.base;
  const auto &uid = m.uid, &collection = m.collection, &local = m.local;
  const auto idle = [&] { return upToDate(c); };
  const auto content = randomContent(contentSize);
  const auto hash = sha256(content);
  for (const auto &direction :
       chunked ? QStringList{"chunked-upload"}
               : QStringList{"upload", "download", "unacknowledged-upload"}) {
    if (!selected(qPrintable(direction)))
      continue;
    const bool upload = direction != "download",
               unacknowledged = direction == "unacknowledged-upload";
    onPrimary(c, account, primary, faults);
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
    // Hold the transfer unfinished on the primary at a fixed offset instead of
    // throttling it: past the first chunk for a chunk session.
    NetworkTunnel::setStallAfter(chunked ? firstChunk + 5 * 1048576 : 2 * 1048576);
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
                     baseline.value(bytesKey).toString().toULongLong() +
                         // Past the first chunk means the server answered it
                         // and the second chunk is in flight.
                         (chunked ? quint64(firstChunk) + 1048576 : 65536);
        },
        "owned unfinished content transfer was not observed");
    // No event is processed before the fault below, so nothing moves on.
    NetworkTunnel::setStallAfter(0);
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
    faulted = true;
    failPrimary(true);
    holdResponse(false);
    try {
      wait([&] { return inUse(alternate); },
           "owned active transfer did not automatically fail over");
    } catch (...) {
      // Name what held the account back; only a stable reason code leaves.
      const auto folder = c.projection()
                              .value("snapshot")
                              .toObject()
                              .value("folders")
                              .toArray()
                              .first()
                              .toObject();
      const auto word = [](QString text) {
        bool safe = !text.isEmpty() && text.size() <= 80;
        for (const auto character : text)
          safe = safe && (character.isLetter() || character == '_');
        return safe ? text : QStringLiteral("unknown");
      };
      std::fprintf(stderr, "owned failover blocked, folder %s reason %s\n",
                   qPrintable(word(folder.value("state").toString())),
                   qPrintable(word(folder.value("reasonCode").toString())));
      throw;
    }
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
      // Resuming the same session on the other origin sends only what the
      // server lacks; a restarted upload would send the first chunk twice.
      require(
          !chunked ||
              recovered.value("uploadBytes").toString().toULongLong() -
                      baseline.value("uploadBytes").toString().toULongLong() <
                  quint64(contentSize) + 32 * 1048576,
          "owned chunk session was restarted instead of resumed");
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
  removeMapping(c, firstFolder(c.projection()
                                   .value("snapshot")
                                   .toObject()
                                   .value("folders"))
                       .value("id")
                       .toString());
  std::puts(
      chunked ? "PASS real chunk session interrupted in its second chunk, "
                "resumed without resending the first chunk, automatic "
                "mapped-root cross-origin recovery, matching hashes and "
                "original native journal"
              : "PASS real unfinished positive-byte upload/download faults, "
                "server-committed PUT with withheld acknowledgement, automatic "
                "mapped-root cross-origin recovery, matching hashes and "
                "original native journal");
#endif
}
