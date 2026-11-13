// SPDX-License-Identifier: GPL-3.0-only
#include "network_acceptance.h"

#include "control.h"
#include "network_tunnel.h"
#include "pipe_windows.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimer>
#include <QUuid>
#include <QXmlStreamReader>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

// A crash prints the crashing thread's stack (symbols when built with a PDB).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h> // after windows.h
#pragma comment(lib, "dbghelp.lib")
static LONG WINAPI soakCrashed(EXCEPTION_POINTERS *e) {
  HANDLE p = GetCurrentProcess(), t = GetCurrentThread();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(p, nullptr, TRUE);
  CONTEXT c = *e->ContextRecord;
  STACKFRAME64 f{};
  const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
  f.AddrPC.Offset = c.Pc;
  f.AddrFrame.Offset = c.Fp;
  f.AddrStack.Offset = c.Sp;
  f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
  std::fprintf(stderr, "CRASH code %08lx at %p\n",
               e->ExceptionRecord->ExceptionCode,
               e->ExceptionRecord->ExceptionAddress);
  for (int i = 0; i < 60 && StackWalk64(machine, p, t, &f, &c, nullptr,
                                        SymFunctionTableAccess64,
                                        SymGetModuleBase64, nullptr);
       ++i) {
    char buffer[sizeof(SYMBOL_INFO) + 512];
    auto *s = reinterpret_cast<SYMBOL_INFO *>(buffer);
    s->SizeOfStruct = sizeof(SYMBOL_INFO);
    s->MaxNameLen = 511;
    DWORD64 d = 0;
    IMAGEHLP_LINE64 line{sizeof(line)};
    DWORD ld = 0;
    char module[MAX_PATH] = "?";
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(f.AddrPC.Offset), &m))
      GetModuleFileNameA(m, module, MAX_PATH);
    const bool named = SymFromAddr(p, f.AddrPC.Offset, &d, s);
    const bool lined = SymGetLineFromAddr64(p, f.AddrPC.Offset, &ld, &line);
    const char *base = std::strrchr(module, '\\');
    std::fprintf(stderr, "  %s!%s+0x%llx %s:%lu\n", base ? base + 1 : module,
                 named ? s->Name : "?", static_cast<unsigned long long>(d),
                 lined ? line.FileName : "",
                 lined ? static_cast<unsigned long>(line.LineNumber) : 0ul);
  }
  std::fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;
}

namespace {
/// require keeps real acceptance checks enabled in optimized builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

// The daemon's timers run 24 times faster: the hour is a day of them.
constexpr int factor = 24;
constexpr int names = 16;
constexpr qint64 largeBase = 24 << 20;

/// hold drives the event loop for ms, so the daemon and the soak's own timers
/// keep running.
void hold(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

/// read sends one read-only JSON-RPC request through the production
/// dispatcher, without the mutation fields reads refuse.
QJsonValue read(Control &c, const QString &method, const QJsonObject &params) {
  bool negotiated = true, subscribed = false;
  const auto reply = c.dispatch(QJsonObject{{"jsonrpc", "2.0"},
                                            {"id", 1},
                                            {"method", method},
                                            {"params", params}},
                                negotiated, subscribed)
                         .toObject();
  require(reply.contains("result"), "owned soak read was refused");
  return reply.value("result");
}

/// reasonCode is a refused reply's fixed reason.
QString reasonCode(const QJsonObject &reply) {
  return reply.value("error")
      .toObject()
      .value("data")
      .toObject()
      .value("reasonCode")
      .toString();
}

QByteArray sha256(const QByteArray &data) {
  return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

/// random returns size bytes of fresh random content (size a multiple of 4).
QByteArray random(qint64 size) {
  QByteArray data(size, Qt::Uninitialized);
  QRandomGenerator::global()->fillRange(
      reinterpret_cast<quint32 *>(data.data()), size / 4);
  return data;
}

/// Dav sends the fixture's own requests to the direct loopback origin, which
/// no relay fault reaches. Notes: one manager for the whole process;
/// credentials stay in memory. No cookies: a server session of one user would
/// otherwise answer the next user's requests.
class Dav {
public:
  /// send performs one request and returns its HTTP status.
  int send(const QByteArray &method, const QString &path, const QString &user,
           const QString &password, const QByteArray &body = {},
           QByteArray *out = nullptr,
           const std::map<QByteArray, QByteArray> &headers = {}) {
    QNetworkRequest request(url(path));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(120000);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                         QNetworkRequest::Manual);
    request.setRawHeader(
        "Authorization",
        "Basic " + (user.toUtf8() + ':' + password.toUtf8()).toBase64());
    request.setRawHeader("OCS-APIRequest", "true");
    for (const auto &[name, value] : headers)
      request.setRawHeader(name, value);
    auto *reply = manager_.sendCustomRequest(request, method, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
      loop.exec();
    if (out)
      *out = reply->readAll();
    const auto status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return status;
  }
  /// url resolves a path below the direct origin.
  QUrl url(const QString &path) const { return origin_.resolved(QUrl(path)); }

private:
  QUrl origin_{qEnvironmentVariable("NEXTCLOUD_TEST_CLEANUP_URL")};
  QNetworkAccessManager manager_;
};

/// Folder is one mapping with the content both sides must end up with.
struct Folder {
  QString account, id, local, dav; // dav: the folder's DAV path, ending '/'
  bool owned = false;              // the owned user's, else the fixture user's
  std::map<QString, QByteArray> files; // expected name -> SHA-256
  std::map<QString, QString> last;     // name -> the soak's last change to it
  std::map<QString, QString> renamed;  // this cycle's local renames, to -> from
  // Remote name -> (ETag, SHA-256) and local name -> (size and mtime, hash)
  // already checked, so a check reads only what changed.
  std::map<QString, std::pair<QByteArray, QByteArray>> remoteSeen, localSeen;
};

/// Account is one signed-in account, its two addresses and the switches the
/// soak saw it make.
struct Account {
  QString id, primary, target, endpoint;
  qint64 epoch = -1;
};

/// Run holds one process's half of the soak.
struct Run {
  Control &c;
  const QString root;
  const SoakControls &k;
  const bool resume;
  Dav dav;
  const QString admin = qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                adminPassword = qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD");
  QString user, password;
  std::vector<Account> accounts;
  std::vector<Folder> folders;
  QJsonObject tally, reasons, facts;
  qint64 cursor = 0, lastTick = 0, lastPoll = 0;
  std::map<QString, quint64> uploadSeen;
  std::map<QString, qint64> started;
  QString failure, why; // why: what the last convergence check still missed
  QElapsedTimer clock;
  int cycle = 0, here = 0; // over both halves, and in this process
  bool polling = false;

  void add(const char *key, qint64 n = 1) {
    tally[key] = tally.value(key).toInteger() + n;
  }
  qint64 get(const char *key) const { return tally.value(key).toInteger(); }
  QString login(const Folder &f) const { return f.owned ? user : admin; }
  QString secret(const Folder &f) const {
    return f.owned ? password : adminPassword;
  }
  Folder *find(const QString &id) {
    for (auto &f : folders)
      if (f.id == id)
        return &f;
    return nullptr;
  }

  /// setup signs in the owned user as a second account with both addresses,
  /// sets automatic policy on both and maps two folders each.
  void setup() {
    const auto owned = ownedUser(root, k.base);
    user = owned.first;
    password = owned.second;
    const auto added = k.calls.login(user, password);
    require(added.value("state") == "succeeded",
            "owned soak second account was refused");
    const auto second = added.value("accountId").toString();
    auto receipt = k.calls.settled(
        k.calls.call("endpoints.stage", {{"accountId", second},
                                         {"name", "Owned alternate"},
                                         {"baseUrl", k.alternate.toString()},
                                         {"kind", "internet"}}));
    require(receipt.value("state") == "succeeded",
            "owned soak second alternate was not staged");
    receipt = k.calls.settled(k.calls.call(
        "endpoints.approveOrigin",
        {{"stagingId",
          receipt.value("result").toObject().value("id").toString()}}));
    require(receipt.value("state") == "succeeded",
            "owned soak second alternate was not approved");
    accounts = {
        {k.account, k.primary, k.target},
        {second, k.calls.binding(second).first,
         receipt.value("result").toObject().value("endpointId").toString()}};
    for (const auto &a : accounts)
      require(k.calls.settled(k.calls.call(
                                  "endpoints.setPolicy",
                                  {{"accountId", a.id}, {"policy", "auto"}}))
                      .value("state") == "succeeded",
              "owned soak automatic policy failed");
    const auto uid = [&](const QString &login, const QString &secret) {
      QByteArray body;
      require(dav.send("GET", "ocs/v2.php/cloud/user?format=json", login,
                       secret, {}, &body) == 200,
              "owned soak user metadata unavailable");
      return QJsonDocument::fromJson(body)
          .object()
          .value("ocs")
          .toObject()
          .value("data")
          .toObject()
          .value("id")
          .toString();
    };
    // The first account's folders live in its reserved collection, which the
    // parent's cleanup removes; the second's go with the owned user.
    const auto firstUid = uid(admin, adminPassword);
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
              "owned soak content reservation failed");
    }
    require(dav.send("MKCOL", home(firstUid) + collection, admin,
                     adminPassword) == 201,
            "owned soak collection could not be created");
    const auto secondUid = uid(user, password);
    for (const bool isOwned : {false, true})
      for (const auto *name : {"one", "two"}) {
        Folder f;
        f.account = isOwned ? second : k.account;
        f.owned = isOwned;
        f.local = root + "/soak/" + f.account.left(8) + '/' + name;
        const auto remote =
            (isOwned ? QString{} : collection + '/') + QLatin1String(name);
        f.dav = home(isOwned ? secondUid : firstUid) + remote + '/';
        require(dav.send("MKCOL", f.dav.chopped(1), login(f), secret(f)) ==
                        201 &&
                    QDir().mkpath(f.local),
                "owned soak folder could not be created");
        const auto token =
            k.calls
                .call("folders.inspect", {{"accountId", f.account},
                                          {"remotePath", '/' + remote},
                                          {"localPath", f.local}})
                .value("result")
                .toObject()
                .value("inspectToken")
                .toString();
        require(!token.isEmpty(), "owned soak inspection failed");
        require(k.calls.settled(k.calls.call("folders.add",
                                             {{"inspectToken", token}}))
                        .value("state") == "succeeded",
                "owned soak folder could not be added");
        for (const auto &entry : c.projection()
                                     .value("snapshot")
                                     .toObject()
                                     .value("folders")
                                     .toArray())
          if (entry.toObject().value("localPath") == QDir::cleanPath(f.local))
            f.id = entry.toObject().value("id").toString();
        require(!f.id.isEmpty(), "owned soak mapping absent");
        folders.push_back(std::move(f));
      }
    for (const auto &a : accounts) {
      const auto f = k.facts(a.id);
      QJsonArray rows;
      for (const auto &row : f.folders)
        rows.append(row);
      facts[a.id] =
          QJsonObject{{"folders", rows}, {"credential", f.credential}};
    }
  }

  /// save writes what the restarted daemon's half needs: identities, the
  /// expected content, the tallies and the owned user's credentials.
  /// Notes: the profile is ignored and removed by the parent; the user is
  /// a disposable server's throwaway one.
  void save() {
    QJsonArray as, fs;
    for (const auto &a : accounts)
      as.append(QJsonObject{
          {"id", a.id}, {"primary", a.primary}, {"target", a.target}});
    for (const auto &f : folders) {
      QJsonObject files;
      for (const auto &[name, hash] : f.files)
        files[name] = QString::fromLatin1(hash);
      fs.append(QJsonObject{{"account", f.account},
                            {"id", f.id},
                            {"local", f.local},
                            {"dav", f.dav},
                            {"owned", f.owned},
                            {"files", files}});
    }
    QSaveFile file(root + "/soak.json");
    const auto data =
        QJsonDocument(QJsonObject{{"user", user},
                                  {"password", password},
                                  {"accounts", as},
                                  {"folders", fs},
                                  {"tally", tally},
                                  {"reasons", reasons},
                                  {"facts", facts},
                                  {"cursor", QString::number(cursor)},
                                  {"cycle", cycle}})
            .toJson(QJsonDocument::Compact);
    require(file.open(QIODevice::WriteOnly) &&
                file.write(data) == data.size() && file.commit(),
            "owned soak state could not be saved");
  }

  /// load reads the first half's state and requires the restarted daemon to
  /// have kept every identity (INV-05/08).
  void load() {
    QFile file(root + "/soak.json");
    require(file.open(QIODevice::ReadOnly), "owned soak state missing");
    const auto state = QJsonDocument::fromJson(file.readAll()).object();
    user = state.value("user").toString();
    password = state.value("password").toString();
    for (const auto &value : state.value("accounts").toArray()) {
      const auto a = value.toObject();
      accounts.push_back({a.value("id").toString(),
                          a.value("primary").toString(),
                          a.value("target").toString()});
    }
    for (const auto &value : state.value("folders").toArray()) {
      const auto o = value.toObject();
      Folder f;
      f.account = o.value("account").toString();
      f.id = o.value("id").toString();
      f.local = o.value("local").toString();
      f.dav = o.value("dav").toString();
      f.owned = o.value("owned").toBool();
      const auto files = o.value("files").toObject();
      for (auto it = files.begin(); it != files.end(); ++it)
        f.files[it.key()] = it.value().toString().toLatin1();
      folders.push_back(std::move(f));
    }
    tally = state.value("tally").toObject();
    reasons = state.value("reasons").toObject();
    facts = state.value("facts").toObject();
    cursor = state.value("cursor").toString().toLongLong();
    cycle = state.value("cycle").toInt();
    require(accounts.size() == 2 && folders.size() == 4 && !user.isEmpty(),
            "owned soak state incomplete");
    for (const auto &a : accounts) {
      const auto now = k.facts(a.id);
      QJsonArray rows;
      for (const auto &row : now.folders)
        rows.append(row);
      require(facts.value(a.id).toObject().value("folders").toArray() == rows &&
                  facts.value(a.id).toObject().value("credential") ==
                      now.credential,
              "the restarted daemon changed a journal, folder identity or "
              "credential reference");
    }
    add("restarts");
  }

  /// tick runs the daemon's timers factor times faster and records every
  /// address change it sees. Notes: also the daemon's poll timer, as each
  /// folder's own 48-72 s poll would.
  void tick() {
    const auto now = clock.elapsed();
    k.accelerate(now - lastTick, factor);
    lastTick = now;
    // Polls wait for both addresses' health: while business requests succeed
    // the daemon does not probe the active address, as on a busy day.
    if (polling && now - lastPoll >= 60000 / factor) {
      lastPoll = now;
      for (const auto &f : folders)
        k.wake(f.id, false);
      add("polls", qint64(folders.size()));
    }
    for (auto &a : accounts) {
      const auto bound = k.calls.binding(a.id);
      if (bound.first != a.endpoint) {
        if (!a.endpoint.isEmpty())
          add("observedSwitches");
        a.endpoint = bound.first;
      }
      a.epoch = bound.second;
    }
  }

  /// seen records a cycle's start and checks at its end that the account's
  /// binding did not change while it ran: no switch with a running cycle.
  void seen(const QString &folder, bool begins, quint64 uploaded) {
    try {
      const auto *f = find(folder);
      if (!f)
        return;
      const auto epoch = k.calls.binding(f->account).second;
      if (begins) {
        started[folder] = epoch;
        add("cyclesObserved");
      } else if (const auto it = started.find(folder); it != started.end()) {
        add("engineUploaded", qint64(uploaded));
        if (it->second != epoch)
          add("cyclesSpanningSwitch");
        started.erase(it);
      }
    } catch (const std::exception &error) {
      if (failure.isEmpty())
        failure = QString::fromUtf8(error.what());
    }
  }

  /// localHash hashes a local file, reusing the last hash while its size and
  /// modification time are unchanged.
  QByteArray localHash(Folder &f, const QString &name) {
    const QFileInfo info(f.local + '/' + name);
    const auto stamp =
        QByteArray::number(info.size()) + ':' +
        QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    auto &cached = f.localSeen[name];
    if (cached.first == stamp)
      return cached.second;
    QFile file(info.filePath());
    if (!file.open(QIODevice::ReadOnly))
      return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
      return {};
    cached = {stamp, hash.result().toHex()};
    return cached.second;
  }

  /// matches compares both sides of a folder with its expected content.
  /// Notes: any extra entry (a conflict copy, a directory) is a mismatch.
  bool matches(Folder &f, bool full = false) {
    if (full) {
      f.localSeen.clear();
      f.remoteSeen.clear();
    }
    // Upstream renames a file back when its MOVE fails, a transport failure
    // included (PropagateRemoteMove::slotMoveJobFinished); if the MOVE did
    // reach the server, the next discovery renames it forward again. The
    // soak follows whichever name the daemon leaves locally; renames that
    // end at the old name are counted once the cycle converged (a failed
    // check).
    for (const auto &[to, from] : f.renamed) {
      const bool atTo = QFileInfo::exists(f.local + '/' + to),
                 atFrom = QFileInfo::exists(f.local + '/' + from);
      if (atFrom && !atTo && f.files.contains(to) && !f.files.contains(from)) {
        f.files[from] = f.files.at(to);
        f.files.erase(to);
        f.last[from] = "rename reverted by the daemon";
      } else if (atTo && !atFrom && f.files.contains(from) &&
                 !f.files.contains(to)) {
        f.files[to] = f.files.at(from);
        f.files.erase(from);
        f.last[to] = "rename reverted, then redone by the daemon";
      }
    }
    const auto names = QDir(f.local).entryList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    why = f.id.left(8) + ' ';
    if (names.size() != qsizetype(f.files.size())) {
      why += QString("local has %1 entries, expected %2: %3")
                 .arg(names.size())
                 .arg(f.files.size())
                 .arg(names.join(','));
      return false;
    }
    for (const auto &name : names) {
      const auto it = f.files.find(name);
      if (it == f.files.end()) {
        why += "local has unexpected " + name + " after " + f.last[name];
        return false;
      }
      if (localHash(f, name) != it->second) {
        why += "local differs at " + name + " after " + f.last[name];
        return false;
      }
    }
    QByteArray body;
    if (dav.send("PROPFIND", f.dav, login(f), secret(f),
                 "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop>"
                 "<d:getetag/><d:resourcetype/></d:prop></d:propfind>",
                 &body,
                 {{"Depth", "1"}, {"Content-Type", "application/xml"}}) !=
        207) {
      why += "remote listing failed";
      return false;
    }
    std::map<QString, QByteArray> listed;
    int collections = 0;
    QXmlStreamReader xml(body);
    QString href;
    QByteArray etag;
    bool collection = false;
    while (!xml.atEnd()) {
      xml.readNext();
      if (xml.isStartElement() && xml.namespaceUri() == u"DAV:") {
        if (xml.name() == u"response") {
          href.clear();
          etag.clear();
          collection = false;
        } else if (xml.name() == u"href")
          href = QUrl::fromPercentEncoding(xml.readElementText().toUtf8());
        else if (xml.name() == u"getetag")
          etag = xml.readElementText().toUtf8();
        else if (xml.name() == u"collection")
          collection = true;
      } else if (xml.isEndElement() && xml.name() == u"response") {
        if (collection)
          ++collections;
        else
          listed[href.section('/', -1)] = etag;
      }
    }
    if (xml.hasError() || collections != 1 || listed.size() != f.files.size()) {
      QStringList seen;
      for (const auto &entry : listed)
        seen << entry.first;
      why += QString("remote has %1 entries, expected %2: %3")
                 .arg(listed.size())
                 .arg(f.files.size())
                 .arg(seen.join(','));
      return false;
    }
    for (const auto &[name, tag] : listed) {
      const auto it = f.files.find(name);
      if (it == f.files.end()) {
        why += "remote has unexpected " + name + " after " + f.last[name];
        return false;
      }
      auto &cached = f.remoteSeen[name];
      if (cached.first != tag) {
        QByteArray content;
        if (dav.send("GET", f.dav + name, login(f), secret(f), {}, &content) !=
            200)
          return false;
        cached = {tag, sha256(content)};
      }
      if (cached.second != it->second) {
        why += "remote differs at " + name;
        return false;
      }
    }
    why.clear();
    return true;
  }

  /// converge waits until both sides of every folder hold exactly the
  /// expected content, then, with polling paused, until every folder is idle
  /// and still matches, so the next cycle changes settled names only; a
  /// deletion proposal or review state at any point fails the soak (no
  /// unexpected deletion, INV-10). Notes: polled 24 times as often, some
  /// folder is nearly always in a cycle, so idle is waited for apart.
  void converge(const char *step, bool full = false) {
    match(step, false, full);
    const bool was = polling;
    polling = false;
    match(step, true, full);
    polling = was;
  }

  /// match waits for the content of both sides, and with idle also for every
  /// folder up to date; full rehashes past every cache.
  void match(const char *step, bool idle, bool full) {
    QElapsedTimer since;
    since.start();
    qint64 checked = -2000;
    try {
      k.calls.wait(
          [&] {
            if (!failure.isEmpty())
              throw std::runtime_error(failure.toStdString());
            if (!read(c, "decisions.list", {}).toArray().isEmpty()) {
              add("proposals");
              throw std::runtime_error("owned soak met a deletion proposal");
            }
            for (const auto &entry : c.projection()
                                         .value("snapshot")
                                         .toObject()
                                         .value("folders")
                                         .toArray()) {
              const auto state = entry.toObject().value("state").toString();
              if (state == "needsReview")
                throw std::runtime_error("owned soak folder needs review");
              if (idle && state != "upToDate") {

                why = entry.toObject().value("id").toString().left(8) + " is " +
                      state + ' ' +
                      entry.toObject().value("reasonCode").toString();
                return false;
              }
            }
            if (since.elapsed() - checked < 2000)
              return false;
            checked = since.elapsed();
            for (auto &f : folders)
              if (!matches(f, full))
                return false;
            return true;
          },
          step);
    } catch (...) {
      // What was still missing, and each account's probe state.
      std::fprintf(stderr, "soak waiting on: %s\n", qPrintable(why));
      for (const auto &a : accounts)
        std::fprintf(stderr, "account %s: %s\n", qPrintable(a.id.left(8)),
                     qPrintable(k.explain(a.id)));
      std::fprintf(stderr, "diagnostics: %s\n",
                   QJsonDocument(read(c, "diagnostics.snapshot", {})
                                     .toObject()
                                     .value("folders")
                                     .toArray())
                       .toJson(QJsonDocument::Compact)
                       .constData());
      std::fprintf(stderr, "relays: %s\n",
                   qPrintable(NetworkTunnel::describe()));
      // Both sides of a folder that does not match, and its recent activity.
      for (auto &f : folders) {
        if (matches(f))
          continue;
        QStringList expected, remote, recent;
        for (const auto &[name, hash] : f.files)
          expected << name + '@' + hash.left(6) + '(' + f.last[name] + ')';
        QByteArray body;
        dav.send("PROPFIND", f.dav, login(f), secret(f),
                 "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\" "
                 "xmlns:oc=\"http://owncloud.org/ns\"><d:prop><d:getetag/>"
                 "<oc:fileid/><oc:permissions/></d:prop></d:propfind>",
                 &body, {{"Depth", "1"}, {"Content-Type", "application/xml"}});
        for (const auto &match :
             QRegularExpression(
                 "<d:href>([^<]+)</d:href>.*?<d:getetag>([^<]*)</d:getetag>"
                 ".*?<oc:fileid>([^<]*)</oc:fileid>.*?<oc:permissions>([^<]*)"
                 "</oc:permissions>",
                 QRegularExpression::DotMatchesEverythingOption)
                 .globalMatch(QString::fromUtf8(body)))
          remote << QUrl::fromPercentEncoding(match.captured(1).toUtf8())
                            .section('/', -1) +
                        " etag " + match.captured(2) + " id " +
                        match.captured(3) + " perm " + match.captured(4);
        for (const auto &value :
             read(c, "activity.list", {{"folderId", f.id}, {"limit", 12}})
                 .toArray())
          recent << value.toObject().value("kind").toString() + ' ' +
                        QString::number(
                            value.toObject().value("count").toInt());
        // The journal's view of the folder.
        QStringList journal;
        {
          const auto connection = QUuid::createUuid().toString();
          {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(root + "/journals/" + f.id + ".sqlite");
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            if (db.open()) {
              QSqlQuery q(db);
              if (q.exec("SELECT path,md5,fileid,remotePerm,inode,filesize "
                         "FROM metadata ORDER BY path"))
                while (q.next())
                  journal << QString("%1 etag %2 id %3 perm '%4' inode %5 "
                                     "size %6")
                                 .arg(q.value(0).toString(),
                                      q.value(1).toString(),
                                      q.value(2).toString(),
                                      q.value(3).toString())
                                 .arg(q.value(4).toLongLong())
                                 .arg(q.value(5).toLongLong());
              else
                journal << "unreadable";
              q.finish();
              db.close();
            }
          }
          QSqlDatabase::removeDatabase(connection);
        }
        std::fprintf(stderr, "  journal [%s]\n",
                     qPrintable(journal.join("; ")));
        std::fprintf(
            stderr,
            "folder %s expected [%s]\n  local [%s]\n  remote [%s]\n"
            "  activity (newest first) [%s]\n",
            qPrintable(f.id.left(8)), qPrintable(expected.join(' ')),
            qPrintable(
                QDir(f.local).entryList(QDir::Files | QDir::Hidden).join(' ')),
            qPrintable(remote.join(' ')), qPrintable(recent.join(", ")));
      }
      throw;
    }
  }

  /// absorb reads every activity entry recorded since the last call.
  void absorb() {
    QJsonArray fresh;
    QJsonObject params{{"limit", 500}};
    for (bool more = true; more;) {
      const auto page = read(c, "activity.list", params).toArray();
      more = page.size() == 500;
      for (const auto &value : page) {
        const auto entry = value.toObject();
        if (entry.value("cursor").toString().toLongLong() <= cursor) {
          more = false;
          break;
        }
        fresh.append(entry);
        params["cursor"] = entry.value("cursor");
      }
    }
    static const std::map<QString, const char *> kinds{
        {"uploaded", "actUploaded"},
        {"downloaded", "actDownloaded"},
        {"deletedRemote", "actDeletedRemote"},
        {"deletedLocal", "actDeletedLocal"},
        {"renamedRemote", "actRenamedRemote"},
        {"renamedLocal", "actRenamedLocal"},
        {"switched", "actSwitched"},
        {"switchFailed", "actSwitchFailed"}};
    for (const auto &value : fresh) {
      const auto entry = value.toObject();
      cursor = std::max(cursor, entry.value("cursor").toString().toLongLong());
      const auto kind = entry.value("kind").toString();
      if (const auto it = kinds.find(kind); it != kinds.end())
        add(it->second, entry.value("count").toInt());
      else
        add("actOther", entry.value("count").toInt());
      if (kind == "switched") {
        const auto reason = entry.value("reason").toString();
        if (reason.isEmpty() || entry.value("fromId") == entry.value("toId"))
          add("switchesWithoutReason");
        reasons[reason] = reasons.value(reason).toInteger() + 1;
      }
    }
    // The engine's own body-byte counters, per folder and process.
    for (const auto &value : read(c, "diagnostics.snapshot", {})
                                 .toObject()
                                 .value("folders")
                                 .toArray()) {
      const auto entry = value.toObject();
      const auto now = entry.value("uploadBytes").toString().toULongLong();
      auto &last = uploadSeen[entry.value("folderId").toString()];
      add("engineUploadBytes", qint64(now >= last ? now - last : now));
      last = now;
    }
  }

  /// storage reports the Control database's files and its larger tables, so
  /// growth shows where it comes from.
  QString storage() {
    QString text =
        QString("control %1 KB wal %2 KB")
            .arg(QFileInfo(root + "/control.sqlite").size() / 1024)
            .arg(QFileInfo(root + "/control.sqlite-wal").size() / 1024);
    const auto connection = QUuid::createUuid().toString();
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
      db.setDatabaseName(root + "/control.sqlite");
      db.setConnectOptions("QSQLITE_OPEN_READONLY");
      if (db.open()) {
        QSqlQuery q(db);
        QStringList tables;
        if (q.exec("SELECT name FROM sqlite_master WHERE type='table'"))
          while (q.next())
            tables << q.value(0).toString();
        for (const auto &table : tables)
          if (q.exec("SELECT count(*) FROM \"" + table + "\"") && q.next() &&
              q.value(0).toLongLong() >= 20)
            text += ' ' + table + '=' + q.value(0).toString();
        q.finish();
        db.close();
      }
    }
    QSqlDatabase::removeDatabase(connection);
    return text;
  }

  /// writeLocal writes a file in place, as an application would.
  void writeLocal(Folder &f, const QString &name, const QByteArray &content) {
    QFile file(f.local + '/' + name);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                file.write(content) == content.size(),
            "owned soak local write failed");
    f.files[name] = sha256(content);
    add("intendedUploadBytes", content.size());
  }

  /// change applies one cycle's local and remote changes to a folder on
  /// distinct names, so each produces exactly one transfer or rename.
  void change(Folder &f, QRandomGenerator &rng, bool large) {
    std::set<QString> touched;
    f.renamed.clear();
    const auto note = [&](const QString &name, const char *op) {
      f.last[name] = QString("cycle %1 %2").arg(cycle).arg(op);
    };
    const auto pick = [&](bool present) {
      QStringList options;
      for (int i = 0; i < names; ++i) {
        const auto name = QString("s%1.txt").arg(i, 2, 10, QChar('0'));
        if (f.files.contains(name) == present && !touched.contains(name))
          options << name;
      }
      if (options.isEmpty())
        return QString{};
      const auto name = options[rng.bounded(qsizetype(options.size()))];
      touched.insert(name);
      return name;
    };
    const auto small = [&] { return qint64(4 * (1 + rng.bounded(65536))); };
    const auto count = [&] {
      return qint64(f.files.size()) - qint64(f.files.contains("big.bin"));
    };
    if (large) {
      if (!f.files.contains("big.bin")) {
        writeLocal(f, "big.bin", random(largeBase + 4 * rng.bounded(2 << 20)));
        add("largeUploads");
      } else {
        require(QFile::remove(f.local + "/big.bin"),
                "owned soak local delete failed");
        f.files.erase("big.bin");
        add("localDeletes");
      }
    }
    for (int i = count() < 8 ? 2 : 1; i > 0; --i)
      if (const auto name = pick(false); !name.isEmpty()) {
        writeLocal(f, name, random(small()));
        note(name, "local add");
        add("localAdds");
      }
    if (const auto name = pick(true); !name.isEmpty()) {
      writeLocal(f, name, random(small()));
      note(name, "local modify");
      add("localModifies");
    }
    if (const auto from = pick(true), to = pick(false);
        !from.isEmpty() && !to.isEmpty()) {
      require(QFile::rename(f.local + '/' + from, f.local + '/' + to),
              "owned soak local rename failed");
      f.files[to] = f.files.at(from);
      f.files.erase(from);
      note(to, "local rename to");
      note(from, "local rename from");
      f.renamed[to] = from;
      add("localRenames");
    }
    if (count() > 6)
      if (const auto name = pick(true); !name.isEmpty()) {
        require(QFile::remove(f.local + '/' + name),
                "owned soak local delete failed");
        f.files.erase(name);
        note(name, "local delete");
        add("localDeletes");
      }
    const auto put = [&](const QString &name) {
      const auto content = random(small());
      require(dav.send("PUT", f.dav + name, login(f), secret(f), content) /
                      100 ==
                  2,
              "owned soak remote write failed");
      f.files[name] = sha256(content);
    };
    if (const auto name = pick(false); !name.isEmpty()) {
      put(name);
      note(name, "remote add");
      add("remoteAdds");
    }
    if (const auto name = pick(true); !name.isEmpty()) {
      put(name);
      note(name, "remote modify");
      add("remoteModifies");
    }
    if (const auto from = pick(true), to = pick(false);
        !from.isEmpty() && !to.isEmpty()) {
      require(dav.send("MOVE", f.dav + from, login(f), secret(f), {}, nullptr,
                       {{"Destination", dav.url(f.dav + to).toEncoded()},
                        {"Overwrite", "F"}}) /
                      100 ==
                  2,
              "owned soak remote move failed");
      f.files[to] = f.files.at(from);
      f.files.erase(from);
      note(to, "remote move to");
      note(from, "remote move from");
      add("remoteMoves");
    }
    if (count() > 6)
      if (const auto name = pick(true); !name.isEmpty()) {
        require(dav.send("DELETE", f.dav + name, login(f), secret(f)) / 100 ==
                    2,
                "owned soak remote delete failed");
        f.files.erase(name);
        note(name, "remote delete");
        add("remoteDeletes");
      }
  }

  /// refuse sets which addresses answer; notify delivers the network-change
  /// notification an interface change brings (a server outage brings none).
  void refuse(bool primary, bool alternate, bool notify) {
    k.failPrimary(primary);
    k.failAlternate(alternate);
    if (notify)
      k.changed();
  }

  /// counter reads one engine counter of a folder from diagnostics.
  quint64 counter(const QString &folder, const char *name) {
    for (const auto &value : read(c, "diagnostics.snapshot", {})
                                 .toObject()
                                 .value("folders")
                                 .toArray())
      if (value.toObject().value("folderId") == folder)
        return value.toObject().value(name).toString().toULongLong();
    return 0;
  }

  /// cut breaks the address a large upload is running on, once it has sent
  /// 4 MiB of its body, and restores it 15 s later.
  void cut(const Folder &f) {
    const auto before = counter(f.id, "uploadBytes");
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 60000 &&
           !(counter(f.id, "activeUploads") > 0 &&
             counter(f.id, "uploadBytes") >= before + (4 << 20)))
      hold(25);
    if (waited.elapsed() >= 60000) {
      add("cutsMissed");
      return;
    }
    const auto &a =
        *std::find_if(accounts.begin(), accounts.end(),
                      [&](const Account &x) { return x.id == f.account; });
    const bool primary = k.calls.binding(a.id).first == a.primary;
    refuse(primary, !primary, false);
    add("cutsDuringUpload");
    hold(15000);
    refuse(false, false, true);
  }

  /// renew replaces an account's credential through the production renewal
  /// path with its user's own password (no server-side expiry exists to wait
  /// for); the installed password is the same, so nothing is revoked.
  void renew(const Account &a, const QString &login, const QString &secret) {
    QJsonObject reply;
    k.calls.wait(
        [&] {
          reply =
              k.calls.call("accounts.reauthenticate", {{"accountId", a.id}});
          return !reply.contains("error") ||
                 reasonCode(reply) != "QUIESCE_STALLED";
        },
        "owned soak renewal did not start");
    require(!reply.contains("error"), "owned soak renewal was refused");
    QString session, reference;
    const auto connection = QUuid::createUuid().toString();
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
      db.setDatabaseName(root + "/control.sqlite");
      require(db.open(), "owned soak renewal DB unavailable");
      QSqlQuery q(db);
      q.prepare("SELECT r.session_id,a.credential_ref FROM reauthentication r "
                "JOIN accounts a ON a.id=r.account_id WHERE r.account_id=?");
      q.addBindValue(a.id);
      require(q.exec() && q.next(), "owned soak renewal session absent");
      session = q.value(0).toString();
      reference = q.value(1).toString();
      q.finish();
      db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    // Both temporary vault slots enter the parent's cleanup before they exist.
    QFile manifest(root + "/owned-refs.json");
    require(manifest.open(QIODevice::ReadOnly), "owned manifest unreadable");
    auto refs = QJsonDocument::fromJson(manifest.readAll()).array();
    manifest.close();
    for (const auto *slot : {"/candidate", "/previous"})
      refs.append(reference + "/reauthentication/" + session + slot);
    require(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                manifest.write(
                    QJsonDocument(refs).toJson(QJsonDocument::Compact)) > 0 &&
                manifest.flush(),
            "owned manifest update failed");
    manifest.close();
    k.verify(session, login, secret);
    require(k.calls.settled(reply).value("state") == "succeeded",
            "owned soak renewal did not succeed");
    add("renewals");
  }

  /// once runs one cycle: changes in all four folders, one network event,
  /// convergence with matching hashes, and the activity it recorded.
  void once() {
    ++cycle;
    ++here;
    QRandomGenerator rng(quint32(cycle) * 7919u);
    const bool large = cycle % 4 == 1;
    const int bigFolder = (cycle / 4) % 4;
    // One renewal in each half, and the day's one audit.
    const bool renewal = here == 3;
    if (renewal)
      renew(accounts[resume ? 0 : 1], resume ? admin : user,
            resume ? adminPassword : password);
    if (resume && here == 6) {
      for (const auto &f : folders)
        k.wake(f.id, true);
      add("audits");
    }
    for (int i = 0; i < 4; ++i)
      change(folders[i], rng, large && i == bigFolder);
    QString fault = "none";
    if (large && folders[bigFolder].files.contains("big.bin")) {
      fault = "cut";
      cut(folders[bigFolder]);
    } else if (!renewal)
      switch (cycle % 5) {
      case 1:
        fault = "primary-outage";
        refuse(true, false, false);
        hold(20000);
        refuse(false, false, false);
        add("primaryOutages");
        break;
      case 2:
        fault = "alternate-outage";
        refuse(false, true, false);
        hold(20000);
        refuse(false, false, false);
        add("alternateOutages");
        break;
      case 3:
        fault = "network-loss";
        refuse(true, true, true);
        hold(12000);
        refuse(false, false, true);
        add("networkLosses");
        break;
      case 4: {
        const bool primary = (cycle / 5) % 2 == 0;
        fault = primary ? "primary-flaps" : "alternate-flaps";
        for (int i = 0; i < 3; ++i) {
          refuse(primary, !primary, true);
          hold(2000);
          refuse(false, false, true);
          hold(3000);
          add("flaps");
        }
        break;
      }
      default:
        break;
      }
    converge("owned soak cycle did not converge with matching hashes");
    add("convergedCycles");
    for (const auto &f : folders)
      for (const auto &[to, from] : f.renamed)
        if (f.files.contains(from))
          add("renamesReverted");
    absorb();
    std::printf("SOAK storage %s\n", qPrintable(storage()));
    std::printf(
        "SOAK cycle %d at %lld s fault %s uploads %lld/%lld/%lld switches "
        "%lld epochs %lld/%lld\n",
        cycle, static_cast<long long>(clock.elapsed() / 1000),
        qPrintable(fault), static_cast<long long>(get("actUploaded")),
        static_cast<long long>(get("engineUploaded")),
        static_cast<long long>(get("localAdds") + get("localModifies") +
                               get("largeUploads")),
        static_cast<long long>(get("actSwitched")),
        static_cast<long long>(accounts[0].epoch),
        static_cast<long long>(accounts[1].epoch));
    std::fflush(stdout);
  }

  /// finish closes this half: the relay's counters join the tallies; the
  /// second half then checks every soak assertion and writes the result.
  void finish() {
    // The half ends at rest: no more polls, every folder idle, both sides
    // rehashed past every cache, then the last activity.
    polling = false;
    converge("owned soak final content did not match", true);
    absorb();
    const auto relay = NetworkTunnel::totals();
    add("relayCuts", qint64(relay.cut));
    add("relayCutUpstreamBytes", qint64(relay.cutUpstreamBytes));
    add("relayUpstreamBytes", qint64(relay.upstreamBytes));
    add("relayDownstreamBytes", qint64(relay.downstreamBytes));
    if (!resume) {
      save();
      return;
    }
    QJsonObject checks;
    const auto check = [&](const char *name, bool passed) {
      checks[name] = passed;
    };
    const auto uploads =
        get("localAdds") + get("localModifies") + get("largeUploads");
    const auto downloads = get("remoteAdds") + get("remoteModifies");
    // (3) No upload beyond one per change; the bytes beyond the changes' own
    // stay within what the relay's cuts carried (a cut chunk is sent again),
    // plus 1 MiB per cut for what was still in socket buffers.
    check("noDuplicateUploads", get("actUploaded") <= uploads);
    check("uploadBytesWithinFaults",
          get("engineUploadBytes") - get("intendedUploadBytes") <=
              get("relayCutUpstreamBytes") + get("relayCuts") * (1 << 20));
    check("noDuplicateDownloads", get("actDownloaded") <= downloads);
    // (4) No deletion beyond the soak's own, no proposal; every cycle's
    // content check also found nothing missing. Activity counts confirmed
    // items only: a change whose reply a cut lost and the next discovery found
    // done is not counted again (unconfirmed below).
    check("noUnexpectedDeletions",
          get("actDeletedRemote") <= get("localDeletes") &&
              get("actDeletedLocal") <= get("remoteDeletes"));
    tally["unconfirmed"] = uploads + downloads + get("localDeletes") +
                           get("remoteDeletes") + get("localRenames") +
                           get("remoteMoves") -
                           (get("actUploaded") + get("actDownloaded") +
                            get("actDeletedRemote") + get("actDeletedLocal") +
                            get("actRenamedRemote") + get("actRenamedLocal"));
    check("noDeletionProposals", get("proposals") == 0);
    // (5) Every switch recorded with a reason, none across a running cycle.
    check("switchesRecordedWithReason",
          get("switchesWithoutReason") == 0 &&
              get("actSwitched") >= get("observedSwitches") &&
              get("observedSwitches") > 0);
    // A local rename undone by the daemon after a failed MOVE.
    check("noRenameReverted", get("renamesReverted") == 0);
    check("noSwitchDuringTransfer",
          get("cyclesSpanningSwitch") == 0 && get("cyclesObserved") > 0);
    // (2) Every cycle had to converge with matching hashes to continue; the
    // final one rehashed both sides past every cache.
    bool passed = true;
    for (auto it = checks.begin(); it != checks.end(); ++it)
      passed = passed && it.value().toBool();
    QSaveFile file(root + "/soak-result.json");
    const auto data = QJsonDocument(QJsonObject{{"tally", tally},
                                                {"reasons", reasons},
                                                {"checks", checks},
                                                {"cycles", cycle},
                                                {"passed", passed}})
                          .toJson(QJsonDocument::Indented);
    require(file.open(QIODevice::WriteOnly) &&
                file.write(data) == data.size() && file.commit(),
            "owned soak result could not be written");
    require(passed, "owned soak assertions failed (soak-result.json)");
  }
};
} // namespace

void soakAcceptance(Control &c, const QString &root, const SoakControls &k,
                    bool resume) {
  const auto profile = qEnvironmentVariable("SYNCD_SOAK_PROFILE");
  const auto seconds = qEnvironmentVariableIntValue("SYNCD_SOAK_SECONDS");
  require(!profile.isEmpty() && seconds > 0,
          "owned soak needs SYNCD_SOAK_PROFILE and SYNCD_SOAK_SECONDS");
  SetUnhandledExceptionFilter(
      soakCrashed); // A crash prints the crashing thread's stack (symbols when
                    // built with a PDB).
  Run run{c, root, k, resume};
  run.clock.start();
  // The UI connects over the real pipe of this daemon while it runs; the
  // pipe goes before the shutdown, which would end every event loop.
  auto pipe = std::make_unique<PipeServer>(c, profile);
  if (resume)
    run.load();
  else
    run.setup();
  // The wrapper samples the journals' sizes; they live where the mappings say.
  {
    QSaveFile list(root + "/soak-journals.txt");
    QByteArray paths;
    for (const auto &a : run.accounts) {
      const auto rows = k.facts(a.id).folders;
      for (qsizetype i = 1; i < rows.size(); i += 4)
        paths += QDir::toNativeSeparators(rows[i]).toUtf8() + '\n';
    }
    require(list.open(QIODevice::WriteOnly) &&
                list.write(paths) == paths.size() && list.commit(),
            "owned soak journal list could not be written");
  }
  // The engine keeps the observer after this half ends: it then does nothing.
  using Seen = std::function<void(const QString &, bool, quint64)>;
  const auto seen = std::make_shared<Seen>(
      [&](const QString &folder, bool begins, quint64 uploaded) {
        run.seen(folder, begins, uploaded);
      });
  struct Forget {
    std::shared_ptr<Seen> seen;
    ~Forget() { *seen = nullptr; }
  } forget{seen};
  k.observe([seen](const QString &folder, bool begins, quint64 uploaded) {
    if (*seen)
      (*seen)(folder, begins, uploaded);
  });
  QTimer ticker;
  ticker.setInterval(250);
  QObject::connect(&ticker, &QTimer::timeout, &ticker, [&] {
    try {
      run.tick();
    } catch (const std::exception &error) {
      if (run.failure.isEmpty())
        run.failure = QString::fromUtf8(error.what());
    }
  });
  ticker.start();
  try {
    for (const auto &a : run.accounts) {
      QElapsedTimer waited;
      waited.start();
      bool told = false;
      k.calls.wait(
          [&] {
            // A slow start shows why once.
            if (!told && waited.elapsed() > 20000) {
              told = true;
              std::fprintf(stderr, "slow start, account %s: %s\n",
                           qPrintable(a.id.left(8)),
                           qPrintable(k.explain(a.id)));
            }
            return k.ready(a.id);
          },
          "owned soak addresses did not become healthy");
    }
  } catch (...) {
    for (const auto &a : run.accounts)
      std::fprintf(stderr, "account %s: %s\n", qPrintable(a.id.left(8)),
                   qPrintable(k.explain(a.id)));
    throw;
  }
  run.polling = true;
  run.converge("owned soak start did not converge");
  run.absorb();
  std::printf("PASS soak %s half ready at %lld s\n",
              resume ? "second" : "first",
              static_cast<long long>(run.clock.elapsed() / 1000));
  std::fflush(stdout);
  while (run.clock.elapsed() < qint64(seconds) * 1000)
    run.once();
  run.finish();
  std::printf("PASS soak %s half: %d cycles\n", resume ? "second" : "first",
              run.cycle);
}
