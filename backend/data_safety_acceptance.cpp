// SPDX-License-Identifier: GPL-3.0-only
// Data-safety and recovery cases on the disposable server (PLAN §20.2, §20.4).
#include <windows.h>
#include <aclapi.h>

#include "network_acceptance.h"

#include "control.h"
#include "login.h"
#include "network_tunnel.h"
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTimer>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
/// require keeps real acceptance checks enabled in optimized builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// waitUntil drives real Qt callbacks until the condition holds, within its
/// own deadline; the step name is printed first so a killed child names it.
void waitUntil(const std::function<bool()> &condition, const char *message,
               int budget = 60000, int poll = 250) {
  static QElapsedTimer clock;
  if (!clock.isValid())
    clock.start();
  std::printf("STEP %lld %s\n", static_cast<long long>(clock.elapsed()),
              message);
  std::fflush(stdout);
  QDeadlineTimer deadline(budget, Qt::PreciseTimer);
  while (true) {
    if (condition())
      return;
    require(!deadline.hasExpired(), message);
    QEventLoop loop;
    QTimer::singleShot(poll, &loop, &QEventLoop::quit);
    loop.exec();
  }
}
/// spin lets the daemon run for a while with nothing to wait for.
void spin(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}
/// word passes only a stable state or reason code into a PASS line.
QString word(const QString &text) {
  bool safe = !text.isEmpty() && text.size() <= 80;
  for (const auto character : text)
    safe = safe && (character.isLetter() || character == '_');
  return safe ? text : QStringLiteral("none");
}
/// Reply is one bounded direct request's status and body.
struct Reply {
  int status = 0;
  QByteArray body;
};
/// request sends one bounded request to the disposable server's direct
/// loopback origin, which no relay fault reaches, as the fixture user unless
/// another user is named. Notes: never replays a request; credentials stay in
/// memory; the body is read up to 16 MiB.
Reply request(const QByteArray &method, const QString &path,
              const QByteArray &content = {},
              const QList<std::pair<QByteArray, QByteArray>> &headers = {},
              const QString &user = {}, const QString &password = {}) {
  const auto base =
      canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_CLEANUP_URL"));
  const auto url = base.resolved(QUrl(path, QUrl::StrictMode));
  require(base.isValid() && withinServer(base, url),
          "owned data-safety request escaped the fixture origin");
  QNetworkAccessManager manager;
  QNetworkRequest r(url);
  r.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                 QNetworkRequest::ManualRedirectPolicy);
  r.setTransferTimeout(60000);
  const auto login = user.isEmpty() ? qEnvironmentVariable("NEXTCLOUD_TEST_USER")
                                    : user;
  const auto secret = user.isEmpty()
                          ? qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD")
                          : password;
  r.setRawHeader("Authorization",
                 "Basic " + (login.toUtf8() + ':' + secret.toUtf8()).toBase64());
  r.setRawHeader("OCS-APIRequest", "true");
  for (const auto &[name, value] : headers)
    r.setRawHeader(name, value);
  auto *reply = manager.sendCustomRequest(r, method, content);
  QEventLoop loop;
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  if (!reply->isFinished())
    loop.exec();
  Reply result{
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
      reply->read(16 * 1048576)};
  reply->deleteLater();
  return result;
}
/// sha256 names content in assertions without printing it.
QByteArray sha256(const QByteArray &content) {
  return QCryptographicHash::hash(content, QCryptographicHash::Sha256);
}
/// randomContent returns fresh bytes no earlier file of this run contains.
QByteArray randomContent(qsizetype size) {
  QByteArray content(size, Qt::Uninitialized);
  QRandomGenerator::system()->fillRange(
      reinterpret_cast<quint32 *>(content.data()),
      content.size() / qsizetype(sizeof(quint32)));
  return content;
}
/// localHash reads one local file; a missing or unreadable one has none.
QByteArray localHash(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? sha256(file.readAll()) : QByteArray();
}
/// writeLocal creates or replaces one local file.
void writeLocal(const QString &path, const QByteArray &content) {
  require(QDir().mkpath(QFileInfo(path).absolutePath()),
          "owned local directory unavailable");
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) &&
              file.write(content) == content.size() && file.flush(),
          "owned local write failed");
}
/// Mapping is the run's reserved remote collection mapped to one folder.
struct Mapping {
  QString uid, collection, local, folder, remote;
  /// path is a file's DAV path below the mapped remote folder.
  QString path(const QString &name = {}) const {
    return "remote.php/dav/files/" +
           QString::fromLatin1(QUrl::toPercentEncoding(uid)) + '/' +
           QString::fromLatin1(QUrl::toPercentEncoding(remote.mid(1), "/")) +
           (name.isEmpty() ? QString{}
                           : '/' + QString::fromLatin1(
                                       QUrl::toPercentEncoding(name, "/")));
  }
};
/// remoteHash reads one remote file; a missing one has no hash. Notes: a file
/// the running cycle holds locked (423) or a server busy with parallel lanes
/// (5xx, no answer) is read again for up to 10 s; it never counts as missing.
QByteArray remoteHash(const Mapping &m, const QString &name) {
  auto reply = request("GET", m.path(name));
  for (int attempt = 0; attempt < 20 && (reply.status == 0 ||
                                         reply.status == 423 ||
                                         reply.status >= 500);
       ++attempt) {
    spin(500);
    reply = request("GET", m.path(name));
  }
  if (reply.status == 404)
    return {};
  if (reply.status != 200)
    std::fprintf(stderr, "remote read: HTTP %d\n", reply.status);
  require(reply.status == 200, "owned remote read failed");
  return sha256(reply.body);
}
/// putRemote writes one remote file as another client would.
void putRemote(const Mapping &m, const QString &name,
               const QByteArray &content) {
  const auto status = request("PUT", m.path(name), content).status;
  require(status == 201 || status == 204, "owned remote write failed");
}
/// mkcol creates one remote collection below the mapping as another client.
void mkcol(const Mapping &m, const QString &name) {
  require(request("MKCOL", m.path(name)).status == 201,
          "owned remote collection could not be created");
}
/// folder reads the single mapping's projected state.
QJsonObject folder(Control &c, const QString &id) {
  for (const auto &entry :
       c.projection().value("snapshot").toObject().value("folders").toArray())
    if (entry.toObject().value("id") == id)
      return entry.toObject();
  return {};
}
/// state reads one mapping's projected state.
QString state(Control &c, const QString &id) {
  return folder(c, id).value("state").toString();
}
/// decisions lists the pending user decisions.
QJsonArray decisions(Control &c) {
  return c.projection().value("snapshot").toObject().value("decisions").toArray();
}
/// transfers counts the mapping's unfinished upload and download replies.
std::pair<int, int> transfers(const AcceptanceCalls &calls, const QString &id) {
  for (const auto &entry : calls.call("diagnostics.snapshot", {})
                               .value("result")
                               .toObject()
                               .value("folders")
                               .toArray())
    if (entry.toObject().value("folderId") == id)
      return {entry.toObject().value("activeUploads").toString().toInt(),
              entry.toObject().value("activeDownloads").toString().toInt()};
  return {};
}
/// failure is the stable reason of a refused or failed command.
QString failure(const QJsonObject &reply) {
  return reply.value("error").toObject().value("data").toObject().value(
      "reasonCode").toString();
}
/// syncNow asks for a cycle; a refusal (paused, blocked) is not an error here.
void syncNow(Control &c, const AcceptanceCalls &calls, const QString &id) {
  const auto reply = calls.call("folders.syncNow", {{"folderId", id}});
  if (reply.contains("result"))
    calls.settled(reply);
}
/// mapFolder reserves the cleanup manifest, creates the collection (and a
/// subfolder), maps it to root/content and waits for its first cycle.
Mapping mapFolder(Control &c, const AcceptanceCalls &calls,
                  const QString &root, const QString &account,
                  const QString &sub = {}) {
  Mapping m;
  // A server busy with parallel lanes is asked again for up to 10 s.
  auto user = request("GET", "ocs/v2.php/cloud/user?format=json");
  for (int attempt = 0; attempt < 20 && user.status != 200; ++attempt) {
    std::fprintf(stderr, "fixture user: HTTP %d\n", user.status);
    spin(500);
    user = request("GET", "ocs/v2.php/cloud/user?format=json");
  }
  m.uid = QJsonDocument::fromJson(user.body)
              .object()
              .value("ocs")
              .toObject()
              .value("data")
              .toObject()
              .value("id")
              .toString();
  require(!m.uid.isEmpty(), "owned fixture user metadata unavailable");
  // The parent's cleanup removes exactly this reserved collection.
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
  m.remote = '/' + m.collection;
  mkcol(m, {});
  if (!sub.isEmpty()) {
    m.remote += '/' + sub;
    mkcol(m, {});
  }
  m.local = root + "/content";
  require(QDir().mkpath(m.local), "owned local root unavailable");
  const auto token = calls
                         .call("folders.inspect", {{"accountId", account},
                                                   {"remotePath", m.remote},
                                                   {"localPath", m.local}})
                         .value("result")
                         .toObject()
                         .value("inspectToken")
                         .toString();
  require(!token.isEmpty(), "owned mapping inspection failed");
  require(calls.settled(calls.call("folders.add", {{"inspectToken", token}}))
                  .value("state") == "succeeded",
          "owned mapping could not be added");
  for (const auto &entry :
       c.projection().value("snapshot").toObject().value("folders").toArray())
    if (entry.toObject().value("localPath") == QDir::cleanPath(m.local))
      m.folder = entry.toObject().value("id").toString();
  require(!m.folder.isEmpty(), "owned mapping absent");
  waitUntil([&] { return state(c, m.folder) == "upToDate"; },
            "owned mapping did not reconcile");
  return m;
}
/// converge asks for cycles until the condition holds on an idle mapping.
void converge(Control &c, const AcceptanceCalls &calls, const Mapping &m,
              const std::function<bool()> &condition, const char *message,
              int budget = 60000) {
  QElapsedTimer asked;
  asked.start();
  syncNow(c, calls, m.folder);
  // Only a stable state and reason code name what held the folder back.
  const auto named = qScopeGuard([&] {
    if (std::uncaught_exceptions())
      std::fprintf(stderr, "folder %s %s\n",
                   qPrintable(word(state(c, m.folder))),
                   qPrintable(word(folder(c, m.folder).value("reasonCode")
                                       .toString())));
  });
  waitUntil(
      [&] {
        if (state(c, m.folder) == "upToDate" && condition())
          return true;
        if (asked.elapsed() > 5000 && state(c, m.folder) != "syncing") {
          syncNow(c, calls, m.folder);
          asked.restart();
        }
        return false;
      },
      message, budget);
}
/// blocked waits until a cycle ended without reconciling the mapping and the
/// daemon has had time for several more; returns the folder's reason.
QString blocked(Control &c, const AcceptanceCalls &calls, const Mapping &m,
                const char *message) {
  syncNow(c, calls, m.folder);
  waitUntil(
      [&] {
        const auto now = state(c, m.folder);
        return now != "syncing" && now != "upToDate";
      },
      message, 90000);
  spin(5000);
  require(state(c, m.folder) != "upToDate",
          "owned mapping called itself up to date while blocked");
  return folder(c, m.folder).value("reasonCode").toString();
}
/// sameFiles compares the local files below the mapping (hidden temporary
/// files excluded) with the expected names.
bool sameFiles(const QString &local, QStringList expected) {
  QStringList found;
  QDirIterator it(local, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const auto path = it.next();
    if (!QFileInfo(path).fileName().startsWith('.'))
      found.append(QDir(local).relativeFilePath(path));
  }
  found.sort();
  expected.sort();
  return found == expected;
}
/// container runs one command in the disposable server's containers through
/// the runtime recorded beside the fixture root. Returns: standard output.
/// Notes: only the disposable server has one; output never leaves the process.
QByteArray container(const QStringList &arguments, const char *message) {
  QFile file(QFileInfo(qEnvironmentVariable("SYNCD_TEST_CA_CERT")).absolutePath() +
             "/state.json");
  require(file.open(QIODevice::ReadOnly) && file.size() < 65536,
          "this case needs the disposable server's container");
  const auto s = QJsonDocument::fromJson(file.readAll()).object();
  const auto runtime = s.value("Runtime").toString();
  require((runtime == "wslc" || runtime == "docker") &&
              QRegularExpression("^test-server-[0-9a-f]{32}$")
                  .match(s.value("Owner").toString())
                  .hasMatch(),
          "disposable server state unreadable");
  QProcess process;
  auto args = arguments;
  args.replaceInStrings("{owner}", s.value("Owner").toString());
  process.start(runtime + ".exe", args);
  QEventLoop loop;
  QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
  QObject::connect(&process, &QProcess::errorOccurred, &loop,
                   &QEventLoop::quit);
  QTimer::singleShot(60000, &loop, &QEventLoop::quit);
  if (process.state() != QProcess::NotRunning)
    loop.exec();
  if (process.state() != QProcess::NotRunning) {
    process.kill();
    process.waitForFinished();
  }
  require(process.exitStatus() == QProcess::NormalExit &&
              process.exitCode() == 0,
          message);
  return process.readAllStandardOutput();
}
/// occ runs one Nextcloud command as the web server user.
QByteArray occ(const QString &command, const char *message) {
  return container({"exec", "{owner}", "su", "-s", "/bin/sh", "www-data", "-c",
                    "php /var/www/html/occ " + command},
                   message);
}
/// junction creates a directory junction (a reparse point) at link.
void junction(const QString &link, const QString &target) {
  require(QDir().mkpath(target), "owned junction target unavailable");
  QProcess process;
  process.start("cmd.exe", {"/d", "/c", "mklink", "/J",
                            QDir::toNativeSeparators(link),
                            QDir::toNativeSeparators(target)});
  require(process.waitForFinished(30000) && process.exitCode() == 0 &&
              (GetFileAttributesW(reinterpret_cast<LPCWSTR>(
                   QDir::toNativeSeparators(link).utf16())) &
               FILE_ATTRIBUTE_REPARSE_POINT),
          "owned junction could not be created");
}
/// RestartRecord is what the first process of a restart case leaves for the
/// second: the mapping and the hashes of its files.
void saveRecord(const QString &root, const QJsonObject &record) {
  QSaveFile file(root + "/data-restart.json");
  file.setDirectWriteFallback(false);
  const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
              file.commit(),
          "owned restart record could not be written");
}
QJsonObject loadRecord(const QString &root) {
  QFile file(root + "/data-restart.json");
  require(file.open(QIODevice::ReadOnly) && file.size() < 65536,
          "owned restart record unreadable");
  return QJsonDocument::fromJson(file.readAll()).object();
}
/// die ends this process the way a kill does: no destructor runs.
[[noreturn]] void die() {
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(), 75);
  std::abort();
}
/// journalFile is the one native journal of the profile.
QString journalFile(const QString &root) {
  const auto files =
      QDir(root + "/journals").entryList({"*.sqlite"}, QDir::Files | QDir::Hidden);
  if (files.size() != 1)
    std::fprintf(stderr, "journal files: %s\n",
                 qPrintable(QDir(root + "/journals")
                                .entryList(QDir::Files | QDir::Hidden)
                                .join(',')
                                .replace(QRegularExpression("[0-9a-f-]{20,}"),
                                         "<id>")));
  require(files.size() == 1, "owned journal missing");
  return root + "/journals/" + files.first();
}

/// interruptedChange (S05): a local modification being uploaded and a remote
/// modification being downloaded when every connection is lost, with no
/// other address to fail over to. Neither side may end with a truncated or
/// replaced file, and both converge once the network is back.
void interruptedChange(Control &c, const AcceptanceCalls &calls,
                       const Mapping &m, const QString &account,
                       const std::function<void(bool)> &cut) {
  constexpr qsizetype size = 3 * 1048576, changed = 5 * 1048576;
  const auto v1 = randomContent(size), r1 = randomContent(size),
             v2 = randomContent(changed), r2 = randomContent(changed);
  writeLocal(m.local + "/edit.bin", v1);
  putRemote(m, "remote.bin", r1);
  converge(c, calls, m,
           [&] {
             return remoteHash(m, "edit.bin") == sha256(v1) &&
                    localHash(m.local + "/remote.bin") == sha256(r1);
           },
           "owned S05 baseline did not converge");
  const auto binding = calls.binding(account);
  // Both changes are made while the folder is paused, so that one cycle finds
  // them: upstream skips a file changed less than two seconds ago
  // (minimumFileAgeForUpload), and the held download keeps the next cycle
  // from starting.
  calls.settled(calls.call("folders.pause", {{"folderId", m.folder}}));
  waitUntil([&] { return state(c, m.folder) != "syncing"; },
            "owned S05 folder did not pause", 30000);
  writeLocal(m.local + "/edit.bin", v2);
  putRemote(m, "remote.bin", r2);
  spin(3000);
  // Both transfers are held past 2 MiB on the primary, then the network goes.
  NetworkTunnel::setStallAfter(2 * 1048576);
  calls.settled(calls.call("folders.resume", {{"folderId", m.folder}}));
  syncNow(c, calls, m.folder);
  const auto held = qScopeGuard([&] {
    if (std::uncaught_exceptions())
      std::fprintf(stderr, "folder %s %s %s\n",
                   qPrintable(word(state(c, m.folder))),
                   qPrintable(word(folder(c, m.folder).value("phase")
                                       .toString())),
                   qPrintable(word(folder(c, m.folder).value("reasonCode")
                                       .toString())));
  });
  waitUntil(
      [&] {
        const auto [up, down] = transfers(calls, m.folder);
        return up > 0 && down > 0;
      },
      "owned S05 transfers did not start", 60000, 50);
  spin(3000); // they run into the relay's hold
  const auto [uploading, downloading] = transfers(calls, m.folder);
  require(uploading + downloading > 0,
          "owned S05 transfers ended before the network was lost");
  cut(true);
  waitUntil(
      [&] {
        const auto now = state(c, m.folder);
        return now != "syncing" && now != "upToDate";
      },
      "owned S05 cycle did not end when the network was lost", 90000);
  // Held bytes must die with their connections, not reach the server late.
  NetworkTunnel::setStallAfter(0);
  spin(3000);
  // A transfer that was cut leaves the old version; one that had finished
  // the new one; never anything in between.
  const auto localCopy = localHash(m.local + "/remote.bin"),
             serverCopy = remoteHash(m, "edit.bin");
  require(localHash(m.local + "/edit.bin") == sha256(v2) &&
              remoteHash(m, "remote.bin") == sha256(r2),
          "owned S05 a modification itself was changed");
  require(localCopy == sha256(r1) ||
              (!downloading && localCopy == sha256(r2)),
          "owned S05 the interrupted download replaced the local file");
  if (serverCopy != sha256(v1) && serverCopy != sha256(v2))
    std::fprintf(stderr, "server copy %lld bytes, old %lld, new %lld\n",
                 static_cast<long long>(
                     request("GET", m.path("edit.bin")).body.size()),
                 static_cast<long long>(size), static_cast<long long>(changed));
  require(serverCopy == sha256(v1) || (!uploading && serverCopy == sha256(v2)),
          "owned S05 the interrupted upload replaced the server file");
  require(sameFiles(m.local, {"edit.bin", "remote.bin"}) &&
              decisions(c).isEmpty() && calls.binding(account) == binding,
          "owned S05 loss left a copy, a decision or another address");
  const auto reason = folder(c, m.folder).value("reasonCode").toString();
  // An interrupted download leaves .<name>.~<id> behind; one is planted too,
  // so the check does not depend on which transfer the cut caught.
  writeLocal(m.local + "/.planted.bin.~0123abcd", randomContent(4096));
  cut(false);
  converge(c, calls, m,
           [&] {
             return remoteHash(m, "edit.bin") == sha256(v2) &&
                    localHash(m.local + "/remote.bin") == sha256(r2);
           },
           "owned S05 transfers did not converge after the network came back",
           100000);
  require(sameFiles(m.local, {"edit.bin", "remote.bin"}) &&
              calls.binding(account).first == binding.first,
          "owned S05 recovery left a conflict copy or switched address");
  // Upstream's exclude list (`]*.~*`) keeps transfer temporaries local.
  const auto listing = request("PROPFIND", m.path(), {}, {{"Depth", "1"}});
  require(listing.status == 207 && listing.body.count("<d:response>") == 3 &&
              !listing.body.contains(".~"),
          "owned S05 a transfer's temporary file reached the server");
  std::printf("PASS data-safety S05: a 5 MiB local and a 5 MiB remote "
              "modification (%d upload, %d download unfinished) cut with both "
              "addresses down end as %s; each side kept a whole version, the "
              "old one where cut, no conflict copy, no temporary file sent, "
              "same address, then converge\n",
              uploading, downloading, qPrintable(word(reason)));
}

/// watcherBoundary (S09, S10): a local change written after discovery while
/// the cycle is still propagating must start the next cycle by itself, and a
/// real notification-buffer overflow must start a full audit that finds every
/// change. Notes: the daemon also polls every 48-72 s; each condition must
/// hold well inside that, three times in a row for S09.
void watcherBoundary(Control &c, const AcceptanceCalls &calls,
                     const Mapping &m) {
  for (int trial = 0; trial < 3; ++trial) {
    const auto held = randomContent(3 * 1048576), late = randomContent(4096);
    const auto heldName = QString("held-%1.bin").arg(trial),
               lateName = QString("late-%1.bin").arg(trial);
    NetworkTunnel::setStallAfter(1048576);
    writeLocal(m.local + '/' + heldName, held);
    syncNow(c, calls, m.folder);
    waitUntil([&] { return transfers(calls, m.folder).first > 0; },
              "owned S09 upload was not held after discovery", 60000, 50);
    writeLocal(m.local + '/' + lateName, late);
    spin(1000); // the notification arrives while the cycle still runs
    require(state(c, m.folder) == "syncing",
            "owned S09 cycle ended before the late change");
    NetworkTunnel::setStallAfter(0);
    QElapsedTimer released;
    released.start();
    waitUntil(
        [&] {
          return remoteHash(m, lateName) == sha256(late) &&
                 state(c, m.folder) == "upToDate";
        },
        "owned S09 change after discovery waited for the poll", 20000);
    require(remoteHash(m, heldName) == sha256(held),
            "owned S09 held upload did not complete");
    std::printf("STEP S09 trial %d after %lld ms\n", trial,
                static_cast<long long>(released.elapsed()));
  }
  std::puts("PASS data-safety S09: a change written after discovery while "
            "the cycle was still uploading started the next cycle itself, "
            "three times, each within 20 s");

  // S10: a probe watcher with the daemon's buffer size, served on the same
  // blocked thread, proves the burst really overflowed.
  for (int trial = 0; trial < 2; ++trial) {
    const auto native = QDir::toNativeSeparators(m.local);
    const auto probe = CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    require(probe != INVALID_HANDLE_VALUE, "owned S10 probe unavailable");
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    alignas(DWORD) static std::array<char, 64 * 1024> buffer;
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME |
                         FILE_NOTIFY_CHANGE_DIR_NAME |
                         FILE_NOTIFY_CHANGE_SIZE |
                         FILE_NOTIFY_CHANGE_LAST_WRITE;
    const auto arm = [&] {
      ResetEvent(operation.hEvent);
      return ReadDirectoryChangesW(probe, buffer.data(), DWORD(buffer.size()),
                                   TRUE, filter, nullptr, &operation, nullptr);
    };
    require(arm(), "owned S10 probe could not watch");
    // The burst runs without returning to the event loop, as a slow control
    // thread would: 300 files with long names, a rename and a modification.
    const auto modified = randomContent(4096);
    const auto prefix = QString("burst-%1-").arg(trial) + QString(120, 'n');
    for (int i = 0; i < 300; ++i)
      writeLocal(m.local + "/burst/" + prefix + QString::number(i), "x");
    require(QFile::rename(m.local + "/late-0.bin",
                          m.local + QString("/renamed-%1.bin").arg(trial)) &&
                QFile::rename(m.local + QString("/renamed-%1.bin").arg(trial),
                              m.local + "/late-0.bin"),
            "owned S10 rename failed");
    writeLocal(m.local + "/late-1.bin", modified);
    // The first read completed with the first notifications; everything
    // after them waited in the kernel's buffer of the same size.
    DWORD bytes = 0;
    require(WaitForSingleObject(operation.hEvent, 5000) == WAIT_OBJECT_0 &&
                GetOverlappedResult(probe, &operation, &bytes, FALSE),
            "owned S10 probe failed");
    require(arm(), "owned S10 probe could not watch again");
    const bool ok =
        WaitForSingleObject(operation.hEvent, 5000) == WAIT_OBJECT_0 &&
        GetOverlappedResult(probe, &operation, &bytes, FALSE);
    const bool overflowed =
        (ok && bytes == 0) || (!ok && GetLastError() == ERROR_NOTIFY_ENUM_DIR);
    CancelIoEx(probe, &operation);
    GetOverlappedResult(probe, &operation, &bytes, TRUE);
    CloseHandle(operation.hEvent);
    CloseHandle(probe);
    require(overflowed, "owned S10 burst did not overflow the watch buffer");
    QElapsedTimer since;
    since.start();
    waitUntil(
        [&] {
          return state(c, m.folder) == "upToDate" &&
                 remoteHash(m, "late-1.bin") == sha256(modified) &&
                 remoteHash(m, "burst/" + prefix + "299") == sha256("x") &&
                 remoteHash(m, "burst/" + prefix + "0") == sha256("x");
        },
        "owned S10 overflow did not start an audit before the poll", 30000,
        500);
    std::printf("STEP S10 trial %d after %lld ms\n", trial,
                static_cast<long long>(since.elapsed()));
  }
  std::puts("PASS data-safety S10: a real watch-buffer overflow (300 files, a "
            "rename and a modification while the thread was busy) started a "
            "full audit that found every change, twice, within 30 s");
}

/// rootDenied (S13): a mapped root the user may no longer list must not read
/// as empty: nothing is deleted on the server and the folder is blocked
/// until access returns.
void rootDenied(Control &c, const AcceptanceCalls &calls, const Mapping &m) {
  QStringList names{"a.bin", "b.bin", "sub/c.bin"};
  QHash<QString, QByteArray> hashes;
  for (const auto &name : names) {
    const auto content = randomContent(8192);
    hashes.insert(name, sha256(content));
    writeLocal(m.local + '/' + name, content);
  }
  converge(c, calls, m,
           [&] {
             for (const auto &name : names)
               if (remoteHash(m, name) != hashes.value(name))
                 return false;
             return true;
           },
           "owned S13 baseline did not converge");
  const auto native = QDir::toNativeSeparators(m.local);
  const auto path = reinterpret_cast<LPWSTR>(const_cast<ushort *>(native.utf16()));
  PACL original = nullptr, denied = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  require(GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                &original, nullptr,
                                &descriptor) == ERROR_SUCCESS,
          "owned S13 root ACL unreadable");
  HANDLE token = nullptr;
  alignas(TOKEN_USER) char user[256];
  DWORD length = 0;
  require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
              GetTokenInformation(token, TokenUser, user, sizeof(user),
                                  &length),
          "owned S13 user unknown");
  CloseHandle(token);
  EXPLICIT_ACCESS_W deny{};
  deny.grfAccessPermissions = FILE_LIST_DIRECTORY | FILE_TRAVERSE;
  deny.grfAccessMode = DENY_ACCESS;
  deny.grfInheritance = NO_INHERITANCE;
  deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  deny.Trustee.TrusteeType = TRUSTEE_IS_USER;
  deny.Trustee.ptstrName =
      reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER *>(user)->User.Sid);
  require(SetEntriesInAclW(1, &deny, original, &denied) == ERROR_SUCCESS,
          "owned S13 deny entry could not be built");
  const auto restore = [&] {
    SetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                          nullptr, nullptr, original, nullptr);
  };
  QString reason;
  try {
    require(SetNamedSecurityInfoW(path, SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                  denied, nullptr) == ERROR_SUCCESS &&
                QDir(m.local).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                    .isEmpty(),
            "owned S13 root could not be denied");
    reason = blocked(c, calls, m, "owned S13 denied root was not reported");
    for (const auto &name : names)
      require(remoteHash(m, name) == hashes.value(name),
              "owned S13 denied root deleted server files");
    require(decisions(c).isEmpty(),
            "owned S13 denied root proposed deletions");
  } catch (...) {
    restore();
    LocalFree(denied);
    LocalFree(descriptor);
    throw;
  }
  restore();
  LocalFree(denied);
  LocalFree(descriptor);
  // The block is a safety blocker: it stays until the daemon restarts.
  syncNow(c, calls, m.folder);
  spin(5000);
  for (const auto &name : names)
    require(localHash(m.local + '/' + name) == hashes.value(name) &&
                remoteHash(m, name) == hashes.value(name),
            "owned S13 a file changed after access returned");
  std::printf("PASS data-safety S13: a mapped root the user may not list is "
              "reported as %s, nothing deleted on the server, no deletion "
              "proposed; after access returned the folder stays %s %s with "
              "every file intact on both sides\n",
              qPrintable(word(reason)), qPrintable(word(state(c, m.folder))),
              qPrintable(word(folder(c, m.folder).value("reasonCode")
                                  .toString())));
}

/// maintenance (S14): a server in maintenance mode answers every request with
/// 503 and its maintenance page; that must never read as an empty folder.
void maintenance(Control &c, const AcceptanceCalls &calls, const Mapping &m) {
  const auto keep = randomContent(8192);
  writeLocal(m.local + "/keep.bin", keep);
  converge(c, calls, m,
           [&] { return remoteHash(m, "keep.bin") == sha256(keep); },
           "owned S14 baseline did not converge");
  QString reason;
  occ("maintenance:mode --on", "owned S14 maintenance could not start");
  try {
    require(request("GET", "status.php").body.contains("\"maintenance\":true"),
            "owned S14 server not in maintenance");
    require(QFile::remove(m.local + "/keep.bin"),
            "owned S14 local removal failed");
    writeLocal(m.local + "/during.bin", "during");
    reason = blocked(c, calls, m, "owned S14 maintenance was not reported");
  } catch (...) {
    occ("maintenance:mode --off", "owned S14 maintenance could not end");
    throw;
  }
  occ("maintenance:mode --off", "owned S14 maintenance could not end");
  require(remoteHash(m, "keep.bin") == sha256(keep) && decisions(c).isEmpty(),
          "owned S14 maintenance propagated a deletion");
  writeLocal(m.local + "/keep.bin", keep);
  converge(c, calls, m,
           [&] { return remoteHash(m, "during.bin") == sha256("during"); },
           "owned S14 folder did not recover after maintenance", 90000);
  std::printf("PASS data-safety S14: maintenance mode reported as %s with a "
              "local deletion and a new file held back, nothing deleted on "
              "either side, no deletion proposed, converged afterwards\n",
              qPrintable(word(reason)));
}

/// partialListing (S14): one member of the folder's listing is an external
/// storage that goes away, so the listing still answers 207 and that member
/// does not; its local files must stay.
void partialListing(Control &c, const AcceptanceCalls &calls,
                    const Mapping &m) {
  const auto keep = randomContent(8192);
  writeLocal(m.local + "/keep.bin", keep);
  const auto storage =
      "/var/www/mygo-ext-" + QFileInfo(m.local).absolutePath().right(12);
  occ("app:enable files_external", "owned S14 external storage app failed");
  occ("config:system:set files_external_allow_create_new_local --value=true "
      "--type=boolean",
      "owned S14 local storage could not be allowed");
  container({"exec", "{owner}", "su", "-s", "/bin/sh", "www-data", "-c",
             "mkdir -p " + storage + " && printf inside > " + storage +
                 "/inside.txt"},
            "owned S14 storage directory failed");
  const auto created = QString::fromLatin1(
      occ("files_external:create " + m.remote + "/ext local null::null -c "
          "datadir=" + storage,
          "owned S14 external storage could not be created"));
  const auto id = QRegularExpression("([0-9]+)").match(created).captured(1);
  require(!id.isEmpty(), "owned S14 external storage ID missing");
  QString reason, listing;
  const auto remove = [&] {
    container({"exec", "{owner}", "sh", "-c",
               "mv " + storage + "-gone " + storage + " 2>/dev/null; true"},
              "owned S14 storage could not be restored");
    occ("files_external:delete -y " + id,
        "owned S14 external storage could not be removed");
    container({"exec", "{owner}", "rm", "-rf", storage},
              "owned S14 storage directory could not be removed");
  };
  try {
    occ("files_external:applicable --add-user=" + m.uid + ' ' + id,
        "owned S14 external storage could not be assigned");
    // The mount's entries get IDs and ETags once scanned; a new mount does
    // not change the folder's ETag, a new file does.
    occ("files:scan --path=/" + m.uid + "/files" + m.remote + "/ext",
        "owned S14 external storage could not be scanned");
    putRemote(m, "mounted.txt", "mounted");
    converge(c, calls, m,
             [&] {
               return localHash(m.local + "/ext/inside.txt") ==
                          sha256("inside") &&
                      remoteHash(m, "keep.bin") == sha256(keep);
             },
             "owned S14 external storage content did not arrive", 90000);
    container({"exec", "{owner}", "mv", storage, storage + "-gone"},
              "owned S14 storage could not be taken away");
    putRemote(m, "after.txt", "after");
    const auto list = request(
        "PROPFIND", m.path(),
        "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:allprop/>"
        "</d:propfind>",
        {{"Depth", "1"}, {"Content-Type", "application/xml"}});
    const auto member = request("PROPFIND", m.path("ext"), {},
                                {{"Depth", "1"}});
    // What the server answers for the vanished storage: the folder's status,
    // the member's, and how many entries the member's listing holds.
    listing = QString("folder %1, member %2 with %3 entries")
                  .arg(list.status)
                  .arg(member.status)
                  .arg(member.body.count("<d:response>"));
    std::fprintf(stderr, "S14 listing: %s\n", qPrintable(listing));
    require(list.status == 207, "owned S14 folder listing failed");
    reason = blocked(c, calls, m, "owned S14 partial listing was not reported");
    require(localHash(m.local + "/ext/inside.txt") == sha256("inside") &&
                localHash(m.local + "/keep.bin") == sha256(keep) &&
                decisions(c).isEmpty(),
            "owned S14 partial listing deleted local files");
  } catch (...) {
    remove();
    throw;
  }
  remove();
  occ("app:disable files_external", "owned S14 external storage app stayed");
  std::printf("PASS data-safety S14: a 207 listing whose external-storage "
              "member failed (%s) reported as %s; the member's local files "
              "kept, no deletion proposed\n",
              qPrintable(listing), qPrintable(word(reason)));
}

/// names (S17): case clashes, names Windows cannot hold, Unicode, long paths
/// and a locked local file: upstream's rules apply, no other file is held
/// back, no server file is changed by them, and each problem has a reason.
void names(Control &c, const AcceptanceCalls &calls, const Mapping &m) {
  const auto locked = randomContent(4096), newer = randomContent(4096);
  writeLocal(m.local + "/locked.bin", locked);
  converge(c, calls, m,
           [&] { return remoteHash(m, "locked.bin") == sha256(locked); },
           "owned S17 baseline did not converge");
  const QString nfd = QString("Cafe") + QChar(0x0301) + ".txt",
                unicode = QString::fromUtf8("Ünïcødé-中文-😀.txt"),
                local = QString::fromUtf8("本地-ü-ß.txt");
  QString deep;
  for (int i = 0; i < 4; ++i)
    deep += QString("long-%1-").arg(i) + QString(55, 'd') + '/';
  const auto longRemote = deep + "remote-" + QString(60, 'f') + ".txt",
             longLocal = deep + "local-" + QString(60, 'f') + ".txt";
  // Remote entries another client created: a case clash, three names
  // Windows cannot hold, Unicode in both normal forms, a long path.
  QHash<QString, QByteArray> remote;
  for (const auto &name :
       {QString("Clash.txt"), QString("clash.txt"), QString("colon:name.txt"),
        QString("question?.txt"), QString("trailing."), nfd, unicode,
        QString("normal.txt")})
    remote.insert(name, randomContent(1024));
  QString path;
  for (const auto &part : deep.split('/', Qt::SkipEmptyParts)) {
    path += part + '/';
    mkcol(m, path.chopped(1));
  }
  remote.insert(longRemote, randomContent(1024));
  for (auto it = remote.begin(); it != remote.end(); ++it)
    putRemote(m, it.key(), it.value());
  const auto localContent = randomContent(1024), longContent = randomContent(1024);
  writeLocal(m.local + '/' + local, localContent);
  writeLocal(m.local + '/' + longLocal, longContent);
  putRemote(m, "locked.bin", newer);
  // The local copy is held open without sharing while the server has a newer
  // version: the download must wait for it, nothing else.
  const auto lockedPath =
      QDir::toNativeSeparators(m.local + "/locked.bin");
  auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(lockedPath.utf16()),
                            GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "owned S17 file could not be locked");
  QString reason;
  try {
    syncNow(c, calls, m.folder);
    waitUntil(
        [&] {
          return localHash(m.local + "/normal.txt") ==
                     sha256(remote.value("normal.txt")) &&
                 remoteHash(m, local) == sha256(localContent) &&
                 state(c, m.folder) != "syncing";
        },
        "owned S17 other files were held back by the problem names", 90000);
    reason = folder(c, m.folder).value("reasonCode").toString();
  } catch (...) {
    CloseHandle(handle);
    std::fprintf(stderr, "folder %s %s\n", qPrintable(word(state(c, m.folder))),
                 qPrintable(word(folder(c, m.folder).value("reasonCode")
                                     .toString())));
    throw;
  }
  CloseHandle(handle);
  require(localHash(m.local + "/locked.bin") == sha256(locked),
          "owned S17 locked file was replaced while open");
  converge(c, calls, m,
           [&] {
             return localHash(m.local + "/locked.bin") == sha256(newer) &&
                    remoteHash(m, longLocal) == sha256(longContent) &&
                    localHash(m.local + '/' + longRemote) ==
                        sha256(remote.value(longRemote));
           },
           "owned S17 locked file or long paths did not converge", 90000);
  for (auto it = remote.begin(); it != remote.end(); ++it)
    require(remoteHash(m, it.key()) == sha256(it.value()),
            "owned S17 a server file was changed or deleted");
  // The server stores names in NFC, so the NFD name arrives composed.
  require(localHash(m.local + '/' + nfd.normalized(QString::NormalizationForm_C)) ==
                  sha256(remote.value(nfd)) &&
              localHash(m.local + '/' + unicode) ==
                  sha256(remote.value(unicode)),
          "owned S17 Unicode names did not arrive intact");
  const auto clash = QDir(m.local).entryList({"*lash*"}, QDir::Files);
  require(!clash.isEmpty() && (localHash(m.local + '/' + clash.first()) ==
                                   sha256(remote.value("Clash.txt")) ||
                               localHash(m.local + '/' + clash.first()) ==
                                   sha256(remote.value("clash.txt"))),
          "owned S17 case clash lost both versions locally");
  int conflicts = 0;
  for (const auto &entry : decisions(c)) {
    const auto d = entry.toObject();
    require(d.value("remoteDeletes").toInteger() == 0 &&
                d.value("localDeletes").toInteger() == 0,
            "owned S17 names proposed deletions");
    ++conflicts;
  }
  std::printf("PASS data-safety S17: a case clash, three names Windows cannot "
              "hold, NFD and non-BMP Unicode, paths above 300 characters both "
              "ways and a locked local file: every other file synced, no "
              "server file changed, the locked copy kept until released "
              "(folder %s while locked), %lld local case-clash entries, %d "
              "conflict notices, no deletion proposed\n",
              qPrintable(word(reason)), static_cast<long long>(clash.size()),
              conflicts);
}

/// excluded (S18, S21): a local tree holding a reparse point and an offline
/// file is refused before anything of it is sent, and a reparse point that
/// appears later inside a mapping that synced normally blocks it before
/// propagation. Notes: the end-to-end encrypted halves need the
/// end_to_end_encryption server app, which the disposable server lacks.
void excluded(Control &c, const AcceptanceCalls &calls, const Mapping &m,
              const QString &root, const QString &account) {
  // S18: a local tree holding a junction and an offline file is refused
  // before its first propagation; nothing of it reaches the server.
  const Mapping parent{m.uid, m.collection, {}, {}, '/' + m.collection};
  const auto local = root + "/second";
  require(QDir().mkpath(local), "owned S18 local folder unavailable");
  mkcol(parent, "plain");
  junction(local + "/link", root + "/outside");
  writeLocal(root + "/outside/secret.txt", "outside");
  writeLocal(local + "/offline.txt", "offline");
  const auto offline = QDir::toNativeSeparators(local + "/offline.txt");
  require(SetFileAttributesW(reinterpret_cast<LPCWSTR>(offline.utf16()),
                             FILE_ATTRIBUTE_OFFLINE),
          "owned S18 offline attribute could not be set");
  const auto token = calls
                         .call("folders.inspect",
                               {{"accountId", account},
                                {"remotePath", parent.remote + "/plain"},
                                {"localPath", local}})
                         .value("result")
                         .toObject()
                         .value("inspectToken")
                         .toString();
  QString added = "refused at inspection";
  if (!token.isEmpty()) {
    const auto receipt =
        calls.settled(calls.call("folders.add", {{"inspectToken", token}}));
    QString second;
    for (const auto &entry :
         c.projection().value("snapshot").toObject().value("folders").toArray())
      if (entry.toObject().value("localPath") == QDir::cleanPath(local))
        second = entry.toObject().value("id").toString();
    if (second.isEmpty())
      added = "refused when added: " + word(failure(receipt));
    else {
      const Mapping other{m.uid, m.collection, local, second,
                          parent.remote + "/plain"};
      added = "blocked before its first propagation as " +
              word(blocked(c, calls, other,
                           "owned S18 refused tree was not reported"));
      calls.settled(calls.call("folders.remove", {{"folderId", second}}));
    }
  }
  require(request("PROPFIND", parent.path("plain/link"), {}, {{"Depth", "0"}})
                      .status == 404 &&
              request("PROPFIND", parent.path("plain/offline.txt"), {},
                      {{"Depth", "0"}})
                      .status == 404,
          "owned S18 refused tree reached the server");
  SetFileAttributesW(reinterpret_cast<LPCWSTR>(offline.utf16()),
                     FILE_ATTRIBUTE_NORMAL);
  require(QDir().rmdir(local + "/link"), "owned S18 junction removal failed");
  std::printf("PASS data-safety S18: a local tree with a junction and an "
              "offline file %s, nothing of it sent\n",
              qPrintable(added));

  // S21: a junction appears inside a mapping that synced normally.
  const auto keep = randomContent(4096);
  writeLocal(m.local + "/keep.bin", keep);
  converge(c, calls, m,
           [&] { return remoteHash(m, "keep.bin") == sha256(keep); },
           "owned S21 baseline did not converge");
  junction(m.local + "/link", root + "/outside");
  writeLocal(m.local + "/beside.txt", "beside");
  QFile::remove(m.local + "/keep.bin");
  putRemote(m, "after.txt", "after");
  const auto reason =
      blocked(c, calls, m, "owned S21 local reparse point was not reported");
  require(request("PROPFIND", m.path("link"), {}, {{"Depth", "0"}}).status ==
                  404 &&
              remoteHash(m, "beside.txt").isEmpty() &&
              remoteHash(m, "keep.bin") == sha256(keep) &&
              !QFileInfo::exists(m.local + "/after.txt") &&
              decisions(c).isEmpty(),
          "owned S21 propagated past a reparse point");
  require(QDir().rmdir(m.local + "/link"), "owned S21 junction removal failed");
  std::printf("PASS data-safety S21: a junction appearing in a mapped tree "
              "blocked it as %s before propagation: nothing sent, downloaded "
              "or deleted in either direction\n",
              qPrintable(word(reason)));
}

/// Captured collects every Qt message of this process, the daemon's log.
QMutex capturedLock;
QStringList captured;
void capture(QtMsgType, const QMessageLogContext &, const QString &message) {
  QMutexLocker lock(&capturedLock);
  captured.append(message);
}

/// revocation (R07, R22): an app password revoked on the server puts its
/// account into the revoked state and stops its folder without any secret
/// written in plain text; the support bundle and the log hold no secret,
/// user, server, path, file name or content.
void revocation(Control &c, const AcceptanceCalls &calls, const QString &root,
                const QString &account) {
  const auto base = canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_URL"));
  const auto owned = ownedUser(root, base);
  const auto &user = owned.first, &password = owned.second;
  const auto issued = request("GET", "ocs/v2.php/core/getapppassword?format=json",
                              {}, {}, user, password);
  const auto app = QJsonDocument::fromJson(issued.body)
                       .object()
                       .value("ocs")
                       .toObject()
                       .value("data")
                       .toObject()
                       .value("apppassword")
                       .toString();
  require(issued.status == 200 && app.size() >= 32,
          "owned R07 app password not issued");
  require(calls.login(user, app).value("state") == "succeeded",
          "owned R07 sign-in with the app password failed");
  QString second;
  for (const auto &entry :
       c.projection().value("snapshot").toObject().value("accounts").toArray())
    if (entry.toObject().value("id") != account)
      second = entry.toObject().value("id").toString();
  require(!second.isEmpty(), "owned R07 account missing");
  const auto home = "remote.php/dav/files/" +
                    QString::fromLatin1(QUrl::toPercentEncoding(user)) + '/';
  require(request("MKCOL", home + "r07", {}, {}, user, password).status == 201,
          "owned R07 collection could not be created");
  const auto local = root + "/r07";
  require(QDir().mkpath(local), "owned R07 local folder unavailable");
  const auto token = calls
                         .call("folders.inspect", {{"accountId", second},
                                                   {"remotePath", "/r07"},
                                                   {"localPath", local}})
                         .value("result")
                         .toObject()
                         .value("inspectToken")
                         .toString();
  require(!token.isEmpty() &&
              calls.settled(calls.call("folders.add", {{"inspectToken", token}}))
                      .value("state") == "succeeded",
          "owned R07 folder could not be added");
  QString id;
  for (const auto &entry :
       c.projection().value("snapshot").toObject().value("folders").toArray())
    if (entry.toObject().value("accountId") == second)
      id = entry.toObject().value("id").toString();
  const auto marker = "r22-content-marker-" + QString::fromLatin1(
                                                  randomContent(12).toHex());
  writeLocal(local + "/r22-private-name.txt", marker.toUtf8());
  syncNow(c, calls, id);
  waitUntil(
      [&] {
        return state(c, id) == "upToDate" &&
               request("GET", home + "r07/r22-private-name.txt", {}, {}, user,
                       password)
                       .body == marker.toUtf8();
      },
      "owned R07 folder did not sync with the app password");
  // The server revokes the token, as a user removing the device does.
  require(request("DELETE", "ocs/v2.php/core/apppassword", {}, {}, user, app)
                  .status == 200 &&
              request("GET", "ocs/v2.php/cloud/user?format=json", {}, {}, user,
                      app)
                      .status == 401,
          "owned R07 app password was not revoked by the server");
  writeLocal(local + "/after-revocation.txt", "after");
  syncNow(c, calls, id);
  const auto revoked = [&] {
    for (const auto &entry :
         c.projection().value("snapshot").toObject().value("accounts").toArray())
      if (entry.toObject().value("id") == second)
        return entry.toObject().value("auth").toString() == "revoked";
    return false;
  };
  waitUntil([&] { return revoked() && state(c, id) != "syncing"; },
            "owned R07 revocation did not reach the account", 90000);
  spin(3000);
  require(revoked() && state(c, id) != "upToDate" &&
              request("GET", home + "r07/after-revocation.txt", {}, {}, user,
                      password)
                      .status == 404,
          "owned R07 revoked account kept syncing");
  const auto reason = folder(c, id).value("reasonCode").toString();
  // R22: the support bundle, the daemon's log and every profile file.
  const auto exported = calls.settled(calls.call("diagnostics.export", {}));
  const auto bundlePath =
      exported.value("result").toObject().value("path").toString();
  QFile bundleFile(bundlePath);
  require(exported.value("state") == "succeeded" &&
              bundleFile.open(QIODevice::ReadOnly),
          "owned R22 support bundle not created");
  const auto bundle = bundleFile.readAll();
  bundleFile.close();
  QStringList log;
  {
    QMutexLocker lock(&capturedLock);
    log = captured;
  }
  const auto joined = log.join('\n').toUtf8();
  const QList<QByteArray> secrets{
      app.toUtf8(), password.toUtf8(),
      qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD").toUtf8()};
  const auto contains = [](const QByteArray &haystack, const QByteArray &needle) {
    return haystack.contains(needle) ||
           haystack.contains(QByteArray(
               reinterpret_cast<const char *>(QString::fromUtf8(needle).utf16()),
               needle.size() * 2));
  };
  int scanned = 0;
  QDirIterator files(root, QDir::Files | QDir::Hidden,
                     QDirIterator::Subdirectories);
  while (files.hasNext()) {
    const auto path = files.next();
    if (path.startsWith(local + '/'))
      continue;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      continue; // a database another connection holds exclusively
    const auto bytes = file.readAll();
    ++scanned;
    for (const auto &secret : secrets)
      require(!contains(bytes, secret),
              "owned R07 a secret was written to a profile file");
  }
  const QList<QByteArray> sensitive{
      user.toUtf8(),
      qEnvironmentVariable("NEXTCLOUD_TEST_USER").toUtf8(),
      base.host().toUtf8(),
      QDir::toNativeSeparators(local).toUtf8(),
      QDir::toNativeSeparators(local).replace('\\', "\\\\").toUtf8(),
      local.toUtf8(),
      "r22-private-name",
      marker.toUtf8()};
  for (const auto &secret : secrets + sensitive) {
    require(!contains(bundle, secret),
            "owned R22 the support bundle holds a secret or private detail");
    require(!contains(joined, secret),
            "owned R22 the daemon log holds a secret or private detail");
  }
  removeOwnedUser(root, base, user);
  std::printf("PASS data-safety R07/R22: an app password revoked on the "
              "server put its account into revoked and its folder %s, nothing "
              "sent after it; no secret in %d profile files, the support "
              "bundle or %lld log lines; no user, server, path, file name or "
              "content in the bundle or the log\n",
              qPrintable(word(reason)), scanned,
              static_cast<long long>(log.size()));
}

/// remoteFileId reads one remote entry's server file ID; a missing entry has
/// none. Notes: a busy server (5xx, no answer) is asked again for up to 10 s.
QString remoteFileId(const Mapping &m, const QString &name) {
  const QByteArray body =
      "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\" "
      "xmlns:oc=\"http://owncloud.org/ns\"><d:prop><oc:fileid/></d:prop>"
      "</d:propfind>";
  const QList<std::pair<QByteArray, QByteArray>> headers{
      {"Depth", "0"}, {"Content-Type", "application/xml"}};
  auto reply = request("PROPFIND", m.path(name), body, headers);
  for (int attempt = 0; attempt < 20 && (reply.status == 0 || reply.status >= 500);
       ++attempt) {
    spin(500);
    reply = request("PROPFIND", m.path(name), body, headers);
  }
  if (reply.status == 404)
    return {};
  const auto match = QRegularExpression("<oc:fileid>(\\d+)</oc:fileid>")
                         .match(QString::fromUtf8(reply.body));
  require(reply.status == 207 && match.hasMatch(),
          "owned remote file ID unavailable");
  return match.captured(1);
}
/// localIdentity is one local file's NTFS file ID: a download that replaced
/// the file gives it a new one. A missing file has none.
QByteArray localIdentity(const QString &path) {
  const auto native = QDir::toNativeSeparators(path);
  const auto handle =
      CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return {};
  FILE_ID_INFO identity{};
  const auto ok = GetFileInformationByHandleEx(handle, FileIdInfo, &identity,
                                               sizeof(identity));
  CloseHandle(handle);
  return ok ? QByteArray(reinterpret_cast<const char *>(&identity),
                         sizeof(identity))
            : QByteArray();
}

/// renameCut (S04 with a failed MOVE): the relay refuses the MOVE of a local
/// rename at its CONNECT, as a network cut or an address switch fails it. The
/// user's name must stay, and a later cycle must make the same MOVE on the server:
/// same server file ID and content, nothing deleted, uploaded or downloaded.
/// Notes: the old name sorts first, so discovery pairs the rename with the old
/// name's local deletion and sends nothing to the old path before the MOVE.
/// Before patch 0019 upstream renamed the file back and kept the old name.
void renameCut(Control &c, const AcceptanceCalls &calls, const Mapping &m,
               const std::function<bool(const QString &)> &cutNext) {
  const auto content = randomContent(256 * 1024);
  const QString from = "rename-1-old.bin", to = "rename-2-new.bin";
  writeLocal(m.local + '/' + from, content);
  converge(c, calls, m, [&] { return remoteHash(m, from) == sha256(content); },
           "owned rename baseline did not converge");
  const auto id = remoteFileId(m, from);
  const auto identity = localIdentity(m.local + '/' + from);
  require(!id.isEmpty() && !identity.isEmpty(),
          "owned rename baseline identity unavailable");
  cutNext('/' + from);
  require(QFile::rename(m.local + '/' + from, m.local + '/' + to),
          "owned local rename failed");
  // From here on the user's name must hold, through the failed cycle and after.
  bool undone = false;
  QTimer watch;
  QObject::connect(&watch, &QTimer::timeout, &watch, [&] {
    undone = undone || QFile::exists(m.local + '/' + from) ||
             !QFile::exists(m.local + '/' + to);
  });
  watch.start(50);
  // A failed wait names which side holds which name, never content or paths.
  const auto named = qScopeGuard([&] {
    if (std::uncaught_exceptions())
      std::fprintf(stderr,
                   "rename: local old %d new %d, server old %d new %d, same "
                   "file ID %d, undone %d\n",
                   QFile::exists(m.local + '/' + from),
                   QFile::exists(m.local + '/' + to),
                   !remoteFileId(m, from).isEmpty(),
                   !remoteFileId(m, to).isEmpty(), remoteFileId(m, to) == id,
                   undone);
  });
  syncNow(c, calls, m.folder);
  waitUntil([&] { return cutNext({}); }, "owned rename MOVE was not sent",
            60000, 50);
  converge(c, calls, m, [&] { return remoteFileId(m, to) == id; },
           "owned rename was not made on the server", 90000);
  watch.stop();
  require(!undone, "owned local rename was undone");
  require(remoteFileId(m, from).isEmpty() &&
              remoteHash(m, to) == sha256(content) &&
              localHash(m.local + '/' + to) == sha256(content),
          "owned rename left the old name or changed the content");
  require(localIdentity(m.local + '/' + to) == identity,
          "owned renamed file was replaced by a download");
  require(sameFiles(m.local, {to}) && decisions(c).isEmpty(),
          "owned rename left a copy or a decision");
  // A delete and upload instead of the MOVE leaves the old name in the trash.
  const auto trash = request(
      "PROPFIND",
      "remote.php/dav/trashbin/" +
          QString::fromLatin1(QUrl::toPercentEncoding(m.uid)) + "/trash",
      "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\" "
      "xmlns:nc=\"http://nextcloud.org/ns\"><d:prop>"
      "<nc:trashbin-original-location/></d:prop></d:propfind>",
      {{"Depth", "1"}, {"Content-Type", "application/xml"}});
  require(trash.status == 207 && !trash.body.contains(m.collection.toUtf8()),
          "owned rename deleted the old name on the server");
  std::printf("PASS data-safety S04: a local rename whose MOVE was refused at "
              "its connection kept the user's name throughout; a later cycle "
              "made the MOVE (same server file ID, same content, same local "
              "file), nothing deleted, uploaded or downloaded\n");
}
} // namespace

void dataSafetyAcceptance(Control &c, const QString &root,
                          const QString &account, const QString &name,
                          const AcceptanceCalls &calls,
                          const std::function<void(bool)> &cut,
                          const std::function<bool(const QString &)> &cutNext) {
  if (name == "revocation") {
    qInstallMessageHandler(capture);
    revocation(c, calls, root, account);
    return;
  }
  const auto m = mapFolder(c, calls, root, account,
                           name == "excluded" ? "main" : QString{});
  if (name == "interrupted-change")
    interruptedChange(c, calls, m, account, cut);
  else if (name == "watcher")
    watcherBoundary(c, calls, m);
  else if (name == "root-denied")
    rootDenied(c, calls, m);
  else if (name == "maintenance")
    maintenance(c, calls, m);
  else if (name == "partial-listing")
    partialListing(c, calls, m);
  else if (name == "names")
    names(c, calls, m);
  else if (name == "excluded")
    excluded(c, calls, m, root, account);
  else if (name == "rename-cut")
    renameCut(c, calls, m, cutNext);
  else if (name == "journal" || name == "root-replaced" ||
           name == "discovery-kill") {
    QJsonObject files;
    for (const auto *file : {"a.bin", "b.bin", "sub/c.bin"}) {
      const auto content = randomContent(65536);
      writeLocal(m.local + '/' + file, content);
      files.insert(file, QString::fromLatin1(sha256(content).toHex()));
    }
    converge(c, calls, m,
             [&] {
               for (auto it = files.begin(); it != files.end(); ++it)
                 if (remoteHash(m, it.key()).toHex() !=
                     it.value().toString().toLatin1())
                   return false;
               return true;
             },
             "owned restart baseline did not converge");
    QJsonObject record{{"folder", m.folder}, {"uid", m.uid},
                       {"remote", m.remote}, {"files", files}};
    if (name == "discovery-kill") {
      // A local and a remote change wait for a cycle whose discovery the
      // relay holds: every server answer comes 4 s late.
      const auto local = randomContent(65536), remote = randomContent(65536);
      writeLocal(m.local + "/new-local.bin", local);
      putRemote(m, "new-remote.bin", remote);
      record.insert("newLocal", QString::fromLatin1(sha256(local).toHex()));
      record.insert("newRemote", QString::fromLatin1(sha256(remote).toHex()));
      saveRecord(root, record);
      NetworkTunnel::setReplyDelay(4000);
      syncNow(c, calls, m.folder);
      QString seen;
      try {
        waitUntil(
            [&] {
              const auto f = folder(c, m.folder);
              const auto now = f.value("state").toString() + '/' +
                               f.value("phase").toString();
              if (!seen.endsWith(now))
                seen += ' ' + now;
              return now == "syncing/checking";
            },
            "owned R02 cycle did not reach discovery", 60000, 50);
      } catch (...) {
        std::fprintf(stderr, "states:%s\n",
                     qPrintable(seen.replace(QRegularExpression("[^a-zA-Z/ ]"),
                                             "")));
        throw;
      }
      spin(2000);
      require(folder(c, m.folder).value("phase") == "checking" &&
                  remoteHash(m, "new-local.bin").isEmpty(),
              "owned R02 cycle left discovery before the kill");
      die();
    }
    saveRecord(root, record);
    calls.settled(calls.call("daemon.shutdown", {{"reason", "user"}}));
    die();
  } else
    throw std::runtime_error("unknown data-safety case");
  calls.call("folders.remove", {{"folderId", m.folder}});
}

void dataSafetyBeforeRestart(const QString &root, const QString &name) {
  auto record = loadRecord(root);
  if (name == "journal") {
    // The journal's header and first page are overwritten as a failing disk
    // or another program would leave them.
    const auto path = journalFile(root);
    QFile file(path);
    require(file.open(QIODevice::ReadWrite) && file.size() > 4096,
            "owned S16 journal unavailable");
    require(file.write(QByteArray(4096, char(0xa5))) == 4096 && file.flush(),
            "owned S16 journal could not be damaged");
    file.close();
    record.insert("journal",
                  QString::fromLatin1(localHash(path).toHex()));
    saveRecord(root, record);
  } else if (name == "root-replaced") {
    // The folder is moved away and an empty one takes its place, as a
    // restore or another program would leave it.
    require(QDir().rename(root + "/content", root + "/content-moved") &&
                QDir().mkpath(root + "/content"),
            "owned S15 root could not be replaced");
  } else
    require(name == "discovery-kill", "unknown data-safety restart case");
}

void dataSafetyRecover(Control &c, const QString &root, const QString &name,
                       const AcceptanceCalls &calls) {
  const auto record = loadRecord(root);
  const Mapping m{record.value("uid").toString(), {}, root + "/content",
                  record.value("folder").toString(),
                  record.value("remote").toString()};
  const auto files = record.value("files").toObject();
  const auto remoteIntact = [&] {
    for (auto it = files.begin(); it != files.end(); ++it)
      if (remoteHash(m, it.key()).toHex() != it.value().toString().toLatin1())
        return false;
    return true;
  };
  require(!folder(c, m.folder).isEmpty(), "owned restart lost the mapping");
  if (name == "discovery-kill") {
    converge(c, calls, m,
             [&] {
               return remoteHash(m, "new-local.bin").toHex() ==
                          record.value("newLocal").toString().toLatin1() &&
                      localHash(m.local + "/new-remote.bin").toHex() ==
                          record.value("newRemote").toString().toLatin1() &&
                      remoteIntact();
             },
             "owned R02 restart did not finish the interrupted cycle", 90000);
    require(sameFiles(m.local, {"a.bin", "b.bin", "sub/c.bin", "new-local.bin",
                                "new-remote.bin"}) &&
                decisions(c).isEmpty(),
            "owned R02 restart left a copy or a decision");
    std::puts("PASS data-safety R02: a process killed while its cycle was in "
              "remote discovery restarted on the same journal and finished "
              "both waiting changes once, no copy, no decision");
    return;
  }
  // The daemon gets local and remote changes it must not act on.
  writeLocal(m.local + "/new-local.bin", "new");
  putRemote(m, "new-remote.bin", "remote");
  const auto reason = blocked(c, calls, m,
                              name == "journal"
                                  ? "owned S16 corrupt journal was not reported"
                                  : "owned S15 replaced root was not reported");
  require(remoteIntact() && remoteHash(m, "new-local.bin").isEmpty() &&
              !QFileInfo::exists(m.local + "/new-remote.bin") &&
              decisions(c).isEmpty(),
          "owned restart case propagated past its safety block");
  if (name == "journal") {
    require(localHash(journalFile(root)).toHex() ==
                record.value("journal").toString().toLatin1(),
            "owned S16 corrupt journal was rewritten or replaced");
    std::printf("PASS data-safety S16: a journal with a damaged header is "
                "reported as %s, kept byte for byte, and no local or remote "
                "change is propagated\n",
                qPrintable(word(reason)));
    return;
  }
  require(QDir(root + "/content-moved").exists("a.bin") &&
              sameFiles(m.local, {"new-local.bin"}),
          "owned S15 changed the moved or the replacing folder");
  // A drive alias (SUBST) of an ordinary folder is refused as a root.
  QString letter;
  const auto drives = GetLogicalDrives();
  for (char d = 'Z'; d > 'M' && letter.isEmpty(); --d)
    if (!(drives & (1u << (d - 'A'))))
      letter = QString(QChar(d)) + ':';
  require(!letter.isEmpty(), "owned S15 no free drive letter");
  const auto target = QDir::toNativeSeparators(root + "/content-moved");
  require(DefineDosDeviceW(0, reinterpret_cast<LPCWSTR>(letter.utf16()),
                           reinterpret_cast<LPCWSTR>(target.utf16())),
          "owned S15 drive alias could not be defined");
  QJsonObject refused;
  try {
    const auto accounts =
        c.projection().value("snapshot").toObject().value("accounts").toArray();
    refused = calls.call("folders.inspect",
                         {{"accountId", accounts.first().toObject().value("id")},
                          {"remotePath", m.remote + "/sub"},
                          {"localPath", letter + "\\"}});
  } catch (...) {
    DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE,
                     reinterpret_cast<LPCWSTR>(letter.utf16()),
                     reinterpret_cast<LPCWSTR>(target.utf16()));
    throw;
  }
  DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE,
                   reinterpret_cast<LPCWSTR>(letter.utf16()),
                   reinterpret_cast<LPCWSTR>(target.utf16()));
  require(!refused.contains("result"),
          "owned S15 a drive alias was accepted as a root");
  std::printf("PASS data-safety S15: a root replaced by an empty folder while "
              "stopped is reported as %s, nothing propagated either way, both "
              "folders untouched; a drive alias root refused as %s\n",
              qPrintable(word(reason)), qPrintable(word(failure(refused))));
}
