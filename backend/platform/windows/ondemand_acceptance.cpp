// SPDX-License-Identifier: GPL-3.0-only
// Real on-demand acceptance against a disposable Nextcloud server (PLAN §2.3,
// M6): placeholders, hydration on open, population, free up space, always keep
// on this device, the switch boundary, a crash during a hydration and
// unregistration. The parent owns the server data, the vault entry, the local
// root and cleanup; a child process is SyncD's engine runtime serving the
// sync root, so it can be killed mid-hydration and restarted on the same
// journal. Applications are played by cmd.exe readers.
// windows.h and winternl.h (NTSTATUS) must precede cfapi.h.
#include <windows.h>
#include <winternl.h>

#include <cfapi.h>

#include "engine_runtime.h"
#include "login.h"
#include "namespace.h"
#include "platform/windows/ondemand.h"
#include "vfs/cfapi/cfapiwrapper.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <functional>
#include <iostream>
#include <sddl.h>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
QElapsedTimer total;

/// require fails the current step with a sanitized message.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

/// step prints a timed step name so a failure says where it stopped.
void step(const char *name) {
  std::fprintf(stderr, "STEP %lld %s\n",
               static_cast<long long>(total.elapsed()), name);
  std::fflush(stderr);
}

/// trustFixtureRoot accepts the disposable server's acknowledged root in this
/// process only; chain and hostname checks stay on.
void trustFixtureRoot() {
  const auto fingerprint = qEnvironmentVariable("SYNCD_TEST_CA_SHA1").toUpper();
  QFile file(qEnvironmentVariable("SYNCD_TEST_CA_CERT"));
  require(QRegularExpression("^[0-9A-F]{40}$").match(fingerprint).hasMatch() &&
              file.open(QIODevice::ReadOnly) && file.size() <= 65536,
          "fixture root unavailable");
  const auto certificates =
      QSslCertificate::fromData(file.readAll(), QSsl::Pem);
  require(certificates.size() == 1 && certificates.first().isSelfSigned() &&
              certificates.first()
                      .digest(QCryptographicHash::Sha1)
                      .toHex()
                      .toUpper() == fingerprint.toLatin1(),
          "fixture root does not match its acknowledgement");
  auto configuration = QSslConfiguration::defaultConfiguration();
  auto roots = configuration.caCertificates();
  roots.append(certificates.first());
  configuration.setCaCertificates(roots);
  QSslConfiguration::setDefaultConfiguration(configuration);
}

/// Dav is the test's own WebDAV client for fixture data; SyncD never uses it.
struct Dav {
  QNetworkAccessManager network;
  QUrl files;
  QByteArray authorization;
  /// send runs one request with its own deadline. Returns: the HTTP status.
  int send(const QByteArray &verb, const QString &path,
           const QByteArray &body = {}) {
    QNetworkRequest request(files.resolved(QUrl(path)));
    request.setRawHeader("Authorization", authorization);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(60000);
    auto *reply = network.sendCustomRequest(request, verb, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const auto status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return status;
  }
};

/// native converts a path for Win32 calls.
std::wstring native(const QString &path) {
  return QDir::toNativeSeparators(path).toStdWString();
}

/// attributes reads attributes without opening content.
DWORD attributes(const QString &path) {
  return GetFileAttributesW(native(path).c_str());
}

/// online reports an online-only placeholder: content is fetched on access.
bool online(const QString &path) {
  const auto value = attributes(path);
  return value != INVALID_FILE_ATTRIBUTES &&
         (value & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS);
}

/// onDisk reads how much of a placeholder's content is local, without
/// requesting any.
qint64 onDisk(const QString &path) {
  auto handle = CreateFileW(
      native(path).c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return -1;
  CF_PLACEHOLDER_STANDARD_INFO info{};
  DWORD length = 0;
  const auto result = CfGetPlaceholderInfo(handle, CF_PLACEHOLDER_INFO_STANDARD,
                                           &info, sizeof(info), &length);
  CloseHandle(handle);
  return SUCCEEDED(result) || result == HRESULT_FROM_WIN32(ERROR_MORE_DATA)
             ? info.OnDiskDataSize.QuadPart
             : -1;
}

/// unpopulated reports an on-demand directory whose entries were never
/// fetched, without listing it.
bool unpopulated(const QString &path) {
  WIN32_FIND_DATAW entry{};
  auto search = FindFirstFileExW(native(path).c_str(), FindExInfoBasic, &entry,
                                 FindExSearchNameMatch, nullptr, 0);
  if (search == INVALID_HANDLE_VALUE)
    return false;
  FindClose(search);
  // Windows disguises placeholders from processes other than the provider
  // (no reparse bit), but keeps RECALL_ON_DATA_ACCESS on an unpopulated one.
  return (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
         (entry.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS);
}

/// sha reads a local file's hash; only called once its content is on disk.
QByteArray sha(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(&file);
  return hash.result();
}

/// Reader plays an application opening a file through Explorer's provider.
struct Reader {
  QProcess process;
  /// start begins reading (type) or listing (dir) a path.
  void start(const QString &path, bool list = false) {
    process.setProgram("cmd.exe");
    process.setNativeArguments(
        (list ? QString("/d /c dir /b \"%1\"")
              : QString("/d /c copy /b /y \"%1\" nul > nul"))
            .arg(QDir::toNativeSeparators(path)));
    process.start();
    require(process.waitForStarted(10000), "reader did not start");
  }
  /// finish waits for the reader. Returns: exit code, -1 after the deadline.
  int finish(int milliseconds) {
    if (!process.waitForFinished(milliseconds)) {
      process.kill();
      process.waitForFinished(10000);
      return -1;
    }
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode()
                                                        : -1;
  }
};

/// Child is the serving SyncD runtime, driven by one command per line.
struct Child {
  QProcess process;
  /// start launches the runtime on the shared state and waits for its first
  /// successful cycle.
  void start(const QString &state) {
    process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    process.start(QCoreApplication::applicationFilePath(), {"--serve", state});
    require(process.waitForStarted(10000), "runtime did not start");
    const auto ready = reply(90000);
    if (ready != "READY")
      std::fprintf(stderr, "STEP first cycle answered %s\n", ready.constData());
    require(ready == "READY", "first on-demand cycle failed");
  }
  /// command sends one line and waits for its one-line answer.
  QByteArray command(const QByteArray &line, int milliseconds) {
    process.write(line + "\n");
    return reply(milliseconds);
  }
  /// reply reads one answer with its own deadline.
  QByteArray reply(int milliseconds) {
    const QDeadlineTimer deadline(milliseconds);
    while (!process.canReadLine()) {
      if (deadline.hasExpired() || process.state() == QProcess::NotRunning)
        return {};
      process.waitForReadyRead(100);
    }
    return process.readLine().trimmed();
  }
  /// kill ends the runtime without any shutdown, as a crash would.
  void kill() {
    if (process.state() != QProcess::NotRunning) {
      process.kill();
      process.waitForFinished(10000);
    }
  }
  /// stop closes the command channel and waits for a clean shutdown.
  bool stop() {
    if (process.state() == QProcess::NotRunning)
      return true;
    process.closeWriteChannel();
    if (!process.waitForFinished(60000)) {
      kill();
      return false;
    }
    return process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
  }
};

/// serve runs SyncD's runtime for one on-demand folder until stdin closes.
int serve(const QString &statePath) {
  QFile stateFile(statePath);
  require(stateFile.open(QIODevice::ReadOnly), "state missing");
  const auto state = QJsonDocument::fromJson(stateFile.readAll()).object();
  trustFixtureRoot();
  EngineAdapter::Binding binding;
  binding.server = canonicalServer(state.value("base").toString());
  binding.login = state.value("login").toString();
  binding.uid = state.value("uid").toString();
  binding.credentialReference = state.value("reference").toString();
  binding.localPath = state.value("local").toString();
  binding.remotePath = state.value("remote").toString();
  binding.journalPath = state.value("journal").toString();
  binding.excludePath =
      QCoreApplication::applicationDirPath() + "/sync-exclude.lst";
  binding.remoteIdentity = state.value("remoteIdentity").toString();
  binding.onDemand = true;
  const auto folder = state.value("folder").toString(),
             account = state.value("account").toString();
  EngineRuntime runtime;
  runtime.add(folder, account, binding);
  // Diagnosis only: upstream's VFS log against the disposable server.
  if (qEnvironmentVariableIsSet("SYNCD_TEST_VFS_LOG"))
    QLoggingCategory::setFilterRules("nextcloud.sync.vfs.*=true\n"
                                     "nextcloud.sync.vfs.*.debug=false");
  quint64 epoch = 0;
  bool ready = false;
  std::function<void(const EngineAdapter::Result &)> waiting;
  runtime.cycle = [&](const QString &, const EngineAdapter::Result &done) {
    if (!ready) {
      ready = true;
      std::puts(done.success ? "READY"
                             : ("FAILED " + done.reason.toUtf8()).constData());
    } else if (waiting) {
      auto callback = std::move(waiting);
      waiting = {};
      callback(done);
    }
    std::fflush(stdout);
  };
  runtime.stopped = [] { QCoreApplication::quit(); };
  const auto answer = [](const QByteArray &text) {
    std::puts(text.constData());
    std::fflush(stdout);
  };
  const auto handle = [&](const QByteArray &line) {
    const auto words = line.split(' ');
    if (words.value(0) == "cycle") {
      waiting = [answer](const EngineAdapter::Result &done) {
        answer(done.success ? QByteArray("CYCLE ok")
                            : "CYCLE " + done.reason.toUtf8());
      };
      runtime.wake(folder, Scheduler::Reason::User);
    } else if (words.value(0) == "hold") {
      // Leave the control thread unserved until an application's request has
      // reached the provider, then start a switch as Control would.
      // A poll cycle would also make the folder busy; let it end first. No
      // new cycle can start while the control thread is not served.
      const QDeadlineTimer idle(30000);
      while (!runtime.idle() && !idle.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      answer("HOLDING");
      const QDeadlineTimer deadline(20000);
      while (runtime.quiescent(folder) && !deadline.hasExpired())
        QThread::msleep(2);
      answer(runtime.quiesceAccount(account, false) ? "QUIESCE 1"
                                                    : "QUIESCE 0");
    } else if (words.value(0) == "quiesce") {
      answer(runtime.quiesceAccount(account, false) ? "QUIESCE 1"
                                                    : "QUIESCE 0");
    } else if (words.value(0) == "rebind") {
      try {
        runtime.rebindAccount(account, binding.server, ++epoch);
        runtime.resumeAccount(account);
        answer("REBOUND ok");
      } catch (const std::exception &error) {
        answer(QByteArray("REBOUND ") + error.what());
      }
    } else if (words.value(0) == "stall" && words.size() == 3) {
      // Serve a hydration until part of it is on disk, then stop serving, so
      // the parent can kill this process in the middle of the transfer.
      const auto path = binding.localPath + '/' + QString::fromUtf8(words[1]);
      const auto size = words[2].toLongLong();
      const QDeadlineTimer deadline(30000);
      qint64 local = 0;
      while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        local = onDisk(path);
        if (local > 0 && local < size)
          break;
      }
      answer("STALLED " + QByteArray::number(local));
      for (;;)
        QThread::sleep(1);
    } else {
      answer("UNKNOWN");
    }
  };
  // stdin is read on its own thread; every command runs on the control thread.
  std::thread input([&] {
    std::string line;
    while (std::getline(std::cin, line)) {
      const auto bytes = QByteArray::fromStdString(line).trimmed();
      QMetaObject::invokeMethod(
          qApp, [&, bytes] { handle(bytes); }, Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(
        qApp, [&] { runtime.shutdown(); }, Qt::QueuedConnection);
  });
  input.detach();
  QCoreApplication::exec();
  return 0;
}

/// randomBytes makes incompressible fixture content.
QByteArray randomBytes(qsizetype size) {
  QByteArray data(size, Qt::Uninitialized);
  QRandomGenerator::global()->fillRange(
      reinterpret_cast<quint32 *>(data.data()), size / 4);
  return data;
}
} // namespace

/// main is the parent: it prepares the fixture, drives the child runtime and
/// readers, checks every step and always cleans up.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  total.start();
  if (app.arguments().size() == 3 && app.arguments().at(1) == "--serve") {
    try {
      return serve(app.arguments().at(2));
    } catch (const std::exception &error) {
      std::fprintf(stderr, "FAIL serve %s\n", error.what());
      return 1;
    }
  }
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto reference = "Nextcloud-MyGo/tests/" + id + "/app-password";
  const auto work = QDir::cleanPath(QCoreApplication::applicationDirPath() +
                                    "/../ondemand-" + id);
  const auto root = work + "/root";
  const auto remote = "/ondemand-" + id;
  OnDemandRoot::Identity identity{
      QUuid::createUuid().toString(QUuid::WithoutBraces),
      QUuid::createUuid().toString(QUuid::WithoutBraces),
      {}};
  Dav dav;
  Child child;
  bool remoteCreated = false;
  int result = 1;
  try {
    trustFixtureRoot();
    const auto base =
        canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_URL"));
    const auto user = qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
               secret = qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD");
    require(!user.isEmpty() && !secret.isEmpty(),
            "fixture credentials missing");
    require(OnDemandRoot::available(), "CFAPI plugin did not load");

    step("verify the fixture account and store its credential");
    LoginFlow verification(base, QUuid::createUuid().toString(),
                           QUuid::createUuid().toString());
    verification.saveCredential = [&](const QString &name,
                                      const QString &password) {
      try {
        writeCredential(reference, name, password);
        return true;
      } catch (...) {
        return false;
      }
    };
    QEventLoop loginLoop;
    verification.changed = [&](LoginFlow &) {
      loginLoop.quit();
      return true;
    };
    QTimer::singleShot(30000, &loginLoop, &QEventLoop::quit);
    verification.verifyCredentials(user, secret);
    if (verification.status() == "verifying")
      loginLoop.exec();
    require(verification.status() == "succeeded", "fixture login failed");

    step("create the remote fixture");
    dav.files =
        base.resolved(QUrl("remote.php/dav/files/" + verification.uid + '/'));
    dav.authorization = "Basic " + (user + ':' + secret).toUtf8().toBase64();
    require(dav.send("MKCOL", remote.mid(1) + '/') == 201, "MKCOL failed");
    remoteCreated = true;
    const QHash<QString, QByteArray> content{
        {"small.txt", randomBytes(65536)},
        {"pin.txt", randomBytes(65536)},
        {"switch.txt", randomBytes(65536)},
        {"big.bin", randomBytes(64 * 1048576)},
        {"sub/inner.txt", randomBytes(4096)}};
    require(dav.send("MKCOL", remote.mid(1) + "/sub/") == 201,
            "MKCOL sub failed");
    for (auto it = content.cbegin(); it != content.cend(); ++it)
      require(dav.send("PUT", remote.mid(1) + '/' + it.key(), it.value()) ==
                  201,
              "PUT failed");
    const auto collection =
        readCollections(base, verification.uid, user, reference, remote, false);

    step("first cycle creates placeholders, not files");
    require(QDir().mkpath(root), "local root not created");
    const auto statePath = work + "/state.json";
    {
      QFile file(statePath);
      require(file.open(QIODevice::WriteOnly), "state not written");
      file.write(
          QJsonDocument(
              QJsonObject{{"base", base.toString()},
                          {"login", user},
                          {"uid", verification.uid},
                          {"reference", reference},
                          {"local", root},
                          {"remote", remote},
                          {"journal", work + "/journal.sqlite"},
                          {"remoteIdentity", collection.front().identity},
                          {"folder", identity.folder},
                          {"account", identity.account}})
              .toJson());
    }
    child.start(statePath);
    for (const auto &name : {"small.txt", "pin.txt", "switch.txt", "big.bin"}) {
      const auto path = root + '/' + name;
      require(online(path), "a remote file arrived as content, not on demand");
      require(QFileInfo(path).size() == content.value(name).size(),
              "placeholder size differs from the server");
    }
    // Probing a child path would itself ask the provider to populate; read
    // the directory's own state from its parent's listing instead.
    require(unpopulated(root + "/sub"),
            "remote directory was populated before anyone opened it");
    require(OnDemandRoot::ownershipError(root, identity).isEmpty(),
            "sync root is not registered as this folder's");

    step("opening a placeholder hydrates it");
    {
      Reader reader;
      reader.start(root + "/small.txt");
      require(reader.finish(60000) == 0, "hydration on open failed");
    }
    require(!online(root + "/small.txt") &&
                sha(root + "/small.txt") ==
                    QCryptographicHash::hash(content.value("small.txt"),
                                             QCryptographicHash::Sha256),
            "hydrated content differs from the server");

    step("listing an unpopulated directory populates it");
    {
      Reader reader;
      reader.start(root + "/sub", true);
      require(reader.finish(60000) == 0 &&
                  reader.process.readAllStandardOutput().contains("inner.txt"),
              "directory population failed");
      require(online(root + "/sub/inner.txt"),
              "populated entry is not a placeholder");
    }

    step("free up space dehydrates on the next cycle");
    require(bool(OCC::CfApiWrapper::setPinState(
                QDir::toNativeSeparators(root + "/small.txt"),
                OCC::PinState::OnlineOnly, OCC::CfApiWrapper::NoRecurse)),
            "online-only pin not set");
    // A cycle that began before the pin changed may end first; allow one more.
    const auto cycleUntil = [&](const std::function<bool()> &done) {
      for (int i = 0; i < 2 && !done(); ++i)
        require(child.command("cycle", 60000) == "CYCLE ok", "cycle failed");
      return done();
    };
    require(cycleUntil([&] {
              return online(root + "/small.txt") &&
                     QFileInfo(root + "/small.txt").size() == 65536;
            }),
            "free up space kept the content");

    step("always keep on this device hydrates on the next cycle");
    require(bool(OCC::CfApiWrapper::setPinState(
                QDir::toNativeSeparators(root + "/pin.txt"),
                OCC::PinState::AlwaysLocal, OCC::CfApiWrapper::NoRecurse)),
            "always-local pin not set");
    require(cycleUntil([&] { return !online(root + "/pin.txt"); }),
            "pinned file is not local");
    require(sha(root + "/pin.txt") ==
                QCryptographicHash::hash(content.value("pin.txt"),
                                         QCryptographicHash::Sha256),
            "pinned content differs from the server");

    step("a switch waits for a running hydration (INV-05)");
    {
      require(child.command("hold", 10000) == "HOLDING", "hold not accepted");
      Reader reader;
      reader.start(root + "/switch.txt");
      require(child.reply(30000) == "QUIESCE 0",
              "switch passed a running hydration");
      const auto code = reader.finish(60000);
      if (code != 0)
        std::fprintf(stderr, "STEP switch reader exit %d\n", code);
      require(code == 0, "hydration was cut by the switch");
      require(child.command("quiesce", 10000) == "QUIESCE 1",
              "switch never reached quiescence");
    }
    {
      step("a closed epoch refuses new hydrations");
      Reader reader;
      QElapsedTimer refusal;
      refusal.start();
      reader.start(root + "/small.txt");
      require(reader.finish(30000) > 0 && refusal.elapsed() < 15000,
              "closed epoch served a hydration");
      require(online(root + "/small.txt"), "refused hydration left content");
    }
    require(child.command("rebind", 60000) == "REBOUND ok", "rebind failed");
    {
      step("the next epoch serves hydrations under the same registration");
      Reader reader;
      reader.start(root + "/small.txt");
      require(reader.finish(60000) == 0, "rebound epoch did not hydrate");
      require(OnDemandRoot::ownershipError(root, identity).isEmpty(),
              "rebinding changed the registration");
    }

    step("a crash during a hydration leaves a placeholder that hydrates later");
    {
      const auto size = content.value("big.bin").size();
      Reader reader;
      reader.start(root + "/big.bin");
      const auto stalled =
          child.command("stall big.bin " + QByteArray::number(size), 40000);
      require(stalled.startsWith("STALLED "), "no stall answer");
      const auto partial = stalled.mid(8).toLongLong();
      require(partial > 0 && partial < size,
              "never observed a partial hydration");
      child.kill();
      require(reader.finish(60000) != 0,
              "a killed provider completed a hydration");
      require(onDisk(root + "/big.bin") < size,
              "interrupted hydration claims complete content");
      child.start(statePath);
      Reader retry;
      retry.start(root + "/big.bin");
      require(retry.finish(60000) == 0, "hydration after restart failed");
      require(sha(root + "/big.bin") ==
                  QCryptographicHash::hash(content.value("big.bin"),
                                           QCryptographicHash::Sha256),
              "recovered content differs from the server");
    }

    step("removal unregisters the sync root");
    require(child.stop(), "runtime did not shut down cleanly");
    require(OnDemandRoot::unregister(root, identity).isEmpty(),
            "unregistration failed");
    // Not under any sync root any more, so no folder ID owns it.
    require(OnDemandRoot::ownershipError(root, identity).isEmpty() &&
                !(attributes(root) & FILE_ATTRIBUTE_REPARSE_POINT),
            "the folder is still a sync root");
    // small.txt stays pinned online-only, so a later cycle may have freed it.
    if (QFileInfo::exists(root + "/small.txt"))
      require(
          !online(root + "/small.txt") &&
              !(attributes(root + "/small.txt") & FILE_ATTRIBUTE_REPARSE_POINT),
          "placeholder left after unregistration");
    for (const auto &name : {"pin.txt", "switch.txt", "big.bin"})
      require(
          sha(root + '/' + name) ==
                  QCryptographicHash::hash(content.value(name),
                                           QCryptographicHash::Sha256) &&
              !(attributes(root + '/' + name) & FILE_ATTRIBUTE_REPARSE_POINT),
          "hydrated file is not an ordinary file after unregistration");
    require(!QFileInfo::exists(root + "/sub/inner.txt"),
            "online-only placeholder left without a provider");
    std::fprintf(stderr, "PASS ondemand acceptance %lld ms\n",
                 static_cast<long long>(total.elapsed()));
    result = 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
  }
  step("cleanup");
  child.kill();
  bool clean = OnDemandRoot::unregister(root, identity).isEmpty();
  if (remoteCreated && dav.send("DELETE", remote.mid(1) + '/') != 204)
    clean = false;
  try {
    removeCredential(reference);
  } catch (...) {
    clean = false;
  }
  if (!QDir(work).removeRecursively())
    clean = false;
  std::fprintf(stderr, "CLEANUP %s %lld ms\n", clean ? "ok" : "incomplete",
               static_cast<long long>(total.elapsed()));
  return clean ? result : 1;
}
