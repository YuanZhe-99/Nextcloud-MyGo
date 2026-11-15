// SPDX-License-Identifier: GPL-3.0-only
// Real on-demand acceptance against a disposable Nextcloud server (PLAN §2.3,
// M6): placeholders, hydration on open, population, free up space, always keep
// on this device, local deletions (upstream's restarted cycle, deletion
// review), a directory move, sparse files, the switch boundary, a crash during
// a hydration, unregistration and the v0.1.0 conversion. The parent owns the
// server data, the vault entry, the local
// root and cleanup; a child process is SyncD's engine runtime serving the
// sync root, so it can be killed mid-hydration and restarted on the same
// journal. Applications are played by cmd.exe readers.
// windows.h and winternl.h (NTSTATUS) must precede cfapi.h.
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>

#include <cfapi.h>

#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
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
#include <QHash>
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
#include <deque>
#include <functional>
#include <mutex>
#include <iostream>
#include <sddl.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
  /// received is the body of the last answer.
  QByteArray received;
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
    received = reply->readAll();
    reply->deleteLater();
    return status;
  }
  /// change replaces a server file as another client would, at least a second
  /// after the file's last write. Returns: the HTTP status.
  /// Notes: Nextcloud 35.0.3 kept a file's ETag when it was rewritten within
  /// the second of its last write (new size and content, same ETag and
  /// modification time): no client could see the change, and an upload
  /// conditioned on that ETag replaced it (2026-11-14).
  int change(const QString &path, const QByteArray &body) {
    QThread::msleep(1100);
    return send("PUT", path, body);
  }
  /// sha reads a server file's hash. Returns: empty when it is not there.
  QByteArray sha(const QString &path) {
    return send("GET", path) == 200
               ? QCryptographicHash::hash(received, QCryptographicHash::Sha256)
               : QByteArray{};
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

/// Placeholder is what Cloud Files records for one entry.
struct Placeholder {
  bool found = false, inSync = false;
  QByteArray identity;
};

/// placeholder reads an entry's placeholder record without requesting any
/// content. Returns: found false for an ordinary file.
Placeholder placeholder(const QString &path) {
  auto handle = CreateFileW(
      native(path).c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return {};
  std::vector<char> buffer(sizeof(CF_PLACEHOLDER_STANDARD_INFO) + 4096);
  DWORD length = 0;
  const auto result =
      CfGetPlaceholderInfo(handle, CF_PLACEHOLDER_INFO_STANDARD, buffer.data(),
                           DWORD(buffer.size()), &length);
  CloseHandle(handle);
  if (FAILED(result))
    return {};
  const auto *info =
      reinterpret_cast<const CF_PLACEHOLDER_STANDARD_INFO *>(buffer.data());
  return {true, info->InSyncState == CF_IN_SYNC_STATE_IN_SYNC,
          QByteArray(reinterpret_cast<const char *>(info->FileIdentity),
                     int(info->FileIdentityLength))};
}

/// wideIdentity is a file ID as upstream's conversion stores it: UTF-16 with
/// its terminator.
QByteArray wideIdentity(const QByteArray &fileId) {
  const auto text = QString::fromUtf8(fileId);
  return QByteArray(reinterpret_cast<const char *>(text.utf16()),
                    int((text.size() + 1) * sizeof(char16_t)));
}

/// journalDigest hashes every record field a conversion must leave alone,
/// read while no runtime holds the journal. Returns: count and digest.
std::pair<int, QByteArray> journalDigest(const QString &path) {
  OCC::SyncJournalDb journal(path);
  int count = 0;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  require(journal.getFilesBelowPath(
              {},
              [&](const OCC::SyncJournalFileRecord &record) {
                ++count;
                for (const auto &field :
                     {record._path, record._etag, record._fileId,
                      QByteArray::number(int(record._type)),
                      QByteArray::number(qint64(record._modtime)),
                      QByteArray::number(record._fileSize),
                      QByteArray::number(record._inode),
                      record._remotePerm.toDbValue(), record._checksumHeader})
                  hash.addData(field + '\0');
              }),
          "journal not readable");
  journal.close();
  return {count, hash.result()};
}

/// fileIds maps each journal path to its file ID.
QHash<QString, QByteArray> fileIds(const QString &path) {
  OCC::SyncJournalDb journal(path);
  QHash<QString, QByteArray> ids;
  require(
      journal.getFilesBelowPath({},
                                [&](const OCC::SyncJournalFileRecord &record) {
                                  ids.insert(record.path(), record._fileId);
                                }),
      "journal not readable");
  journal.close();
  return ids;
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

/// hashOf is the SHA-256 of fixture bytes.
QByteArray hashOf(const QByteArray &bytes) {
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

/// age sets a file's modification time seconds into the past, so upstream
/// neither skips it as too new to upload nor takes an edit for no change.
void age(const QString &path, int seconds) {
  auto handle = CreateFileW(native(path).c_str(), FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "fixture time not opened");
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER value{{now.dwLowDateTime, now.dwHighDateTime}};
  value.QuadPart -= quint64(seconds) * 10000000;
  const FILETIME then{value.LowPart, value.HighPart};
  const bool set = SetFileTime(handle, nullptr, nullptr, &then);
  CloseHandle(handle);
  require(set, "fixture time not set");
}

/// writeSparse writes content as an NTFS sparse file whose first half is a
/// hole, as a database or a virtual disk leaves one. Returns: the bytes the
/// file now holds (zeros, then the second half of content).
QByteArray writeSparse(const QString &path, const QByteArray &content) {
  auto handle = CreateFileW(native(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                            0, nullptr, CREATE_ALWAYS, 0, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "sparse fixture not created");
  DWORD returned = 0;
  FILE_ZERO_DATA_INFORMATION hole{};
  hole.BeyondFinalZero.QuadPart = content.size() / 2;
  const bool written =
      DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                      &returned, nullptr) &&
      WriteFile(handle, content.constData(), DWORD(content.size()), &returned,
                nullptr) &&
      DeviceIoControl(handle, FSCTL_SET_ZERO_DATA, &hole, sizeof(hole), nullptr,
                      0, &returned, nullptr);
  CloseHandle(handle);
  require(written && (attributes(path) & FILE_ATTRIBUTE_SPARSE_FILE),
          "sparse fixture not written");
  age(path, 3600);
  return QByteArray(content.size() / 2, '\0') + content.mid(content.size() / 2);
}

/// editSparse overwrites a sparse file's second half in place, keeping its
/// hole, as an application edits it. Returns: the bytes the file now holds.
QByteArray editSparse(const QString &path, const QByteArray &half,
                      int ageSeconds) {
  QFile file(path);
  require(file.open(QIODevice::ReadWrite) && file.size() == 2 * half.size() &&
              file.seek(half.size()) && file.write(half) == half.size(),
          "sparse fixture not edited");
  file.close();
  // Windows hides the sparse bit of a placeholder from this process, which is
  // not its provider, so it is not checked here.
  age(path, ageSeconds);
  return QByteArray(half.size(), '\0') + half;
}

/// recordType reads one journal record's item type while no runtime holds
/// the journal. Returns: -1 without a record.
int recordType(const QString &journalPath, const QByteArray &path) {
  OCC::SyncJournalDb journal(journalPath);
  OCC::SyncJournalFileRecord record;
  const bool read = journal.getFileRecord(path, &record);
  journal.close();
  require(read, "journal not readable");
  return record.isValid() ? int(record._type) : -1;
}

/// Reader plays an application opening a file through Explorer's provider.
struct Reader {
  QProcess process;
  /// start begins reading (type) or listing (dir) a path.
  void start(const QString &path, bool list = false) {
    run(list ? QString("/d /c dir /b \"%1\"")
             : QString("/d /c copy /b /y \"%1\" nul > nul"),
        path);
  }
  /// run starts one cmd.exe command line on a path ("%1").
  void run(const QString &command, const QString &path) {
    process.setProgram("cmd.exe");
    process.setNativeArguments(command.arg(QDir::toNativeSeparators(path)));
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
  /// downloaded is how many files the first cycle downloaded.
  quint64 downloaded = 0;
  /// start launches the runtime on the shared state and waits for its first
  /// successful cycle.
  void start(const QString &state) {
    process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    process.start(QCoreApplication::applicationFilePath(), {"--serve", state});
    require(process.waitForStarted(10000), "runtime did not start");
    const auto ready = reply(90000);
    if (!ready.startsWith("READY "))
      std::fprintf(stderr, "STEP first cycle answered %s\n", ready.constData());
    require(ready.startsWith("READY "), "first on-demand cycle failed");
    downloaded = ready.mid(6).toULongLong();
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

/// Diagnosis keeps upstream's log and the root's change notifications in
/// memory and prints them only when a step fails: printing them as they
/// happened changed the timing a failure depended on (2026-11-14).
struct Diagnosis {
  std::mutex lock;
  std::deque<QByteArray> lines;
  /// keep records one line with the child's elapsed time.
  void keep(const QByteArray &line) {
    const std::lock_guard guard(lock);
    lines.push_back(QByteArray::number(total.elapsed()) + ' ' + line);
    if (lines.size() > 60000)
      lines.pop_front();
  }
  /// print writes and forgets what was kept, each line marked "LOG".
  void print(const char *why) {
    const std::lock_guard guard(lock);
    std::fprintf(stderr, "LOG begin %s (%zu lines)\n", why, lines.size());
    for (const auto &line : lines)
      std::fprintf(stderr, "LOG %s\n", line.constData());
    std::fprintf(stderr, "LOG end\n");
    std::fflush(stderr);
    lines.clear();
  }
};
Diagnosis diagnosis;

/// imageName names a running process for a Cloud Files request's requester.
QByteArray imageName(DWORD id) {
  auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
  if (!process)
    return "?";
  wchar_t name[MAX_PATH];
  DWORD size = MAX_PATH;
  const bool named = QueryFullProcessImageNameW(process, 0, name, &size);
  CloseHandle(process);
  return named ? QFileInfo(QString::fromWCharArray(name, int(size)))
                     .fileName()
                     .toUtf8()
               : QByteArray("?");
}

/// keepMessage is the child's Qt message handler: upstream's log goes to
/// Diagnosis, with the image name of a process a Cloud Files request names.
void keepMessage(QtMsgType, const QMessageLogContext &context,
                 const QString &message) {
  static const QRegularExpression requester("process id: (\\d+)");
  auto line = QByteArray(context.category ? context.category : "-") + ": " +
              message.toUtf8();
  if (const auto match = requester.match(message); match.hasMatch())
    line += " [" + imageName(match.captured(1).toULong()) + ']';
  diagnosis.keep(line);
}

/// watchRoot records every change notification under the root, with its
/// name, on its own thread for the life of the process.
void watchRoot(const QString &root) {
  std::thread([path = native(root)] {
    auto directory = CreateFileW(
        path.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE)
      return;
    alignas(DWORD) static char buffer[64 * 1024];
    DWORD bytes = 0;
    while (ReadDirectoryChangesW(
        directory, buffer, sizeof(buffer), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SECURITY,
        &bytes, nullptr, nullptr)) {
      if (!bytes) {
        diagnosis.keep("watch: overflow");
        continue;
      }
      for (auto *entry = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(buffer);;
           entry = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(
               reinterpret_cast<char *>(entry) + entry->NextEntryOffset)) {
        diagnosis.keep("watch: " + QByteArray::number(entry->Action) + ' ' +
                       QString::fromWCharArray(entry->FileName,
                                               int(entry->FileNameLength / 2))
                           .toUtf8());
        if (!entry->NextEntryOffset)
          break;
      }
    }
  }).detach();
}

/// readBinding reads the folder binding the parent prepared; "full" serves
/// the folder with full files, as v0.1.0 did, "convert" sets convertExisting.
EngineAdapter::Binding readBinding(const QJsonObject &state) {
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
  binding.onDemand = !state.value("full").toBool();
  binding.convertExisting = state.value("convert").toBool();
  binding.folderId = state.value("folder").toString();
  binding.accountId = state.value("account").toString();
  return binding;
}

/// convertOnly builds the folder's adapter with convertExisting, as Control
/// does at a conversion's attach, and ends it before any cycle.
/// Returns: 0 after printing "CONVERTED <milliseconds>".
int convertOnly(const QString &statePath) {
  QFile stateFile(statePath);
  require(stateFile.open(QIODevice::ReadOnly), "state missing");
  auto binding =
      readBinding(QJsonDocument::fromJson(stateFile.readAll()).object());
  binding.convertExisting = true;
  QElapsedTimer clock;
  clock.start();
  {
    EngineAdapter adapter(binding);
    std::printf("CONVERTED %lld\n", static_cast<long long>(clock.elapsed()));
  }
  std::fflush(stdout);
  return 0;
}

/// serve runs SyncD's runtime for one folder until stdin closes.
int serve(const QString &statePath) {
  QFile stateFile(statePath);
  require(stateFile.open(QIODevice::ReadOnly), "state missing");
  const auto state = QJsonDocument::fromJson(stateFile.readAll()).object();
  trustFixtureRoot();
  const auto binding = readBinding(state);
  const auto folder = binding.folderId, account = binding.accountId;
  EngineRuntime runtime;
  runtime.add(folder, account, binding);
  // Upstream's log against the disposable server, kept until a step fails;
  // building an adapter turns it off again.
  const auto keepLog = [] {
    QLoggingCategory::setFilterRules("nextcloud.*.debug=false\n"
                                     "nextcloud.*.info=true\n"
                                     "nextcloud.*.warning=true\n"
                                     "nextcloud.*.critical=true");
  };
  qInstallMessageHandler(keepMessage);
  keepLog();
  watchRoot(binding.localPath);
  runtime.started = [](const QString &) { diagnosis.keep("cycle: start"); };
  quint64 epoch = 0;
  bool ready = false;
  std::function<void(const EngineAdapter::Result &)> waiting;
  // What cycles did since the last "stats"; a restarted cycle is counted
  // and never answers a command: its follow-up must run on its own.
  quint64 restarts = 0, failedRestarts = 0, uploaded = 0, downloaded = 0,
          deletedRemote = 0, renamedRemote = 0;
  QString proposal, lastReason, awaited;
  const auto answer = [](const QByteArray &text) {
    std::puts(text.constData());
    std::fflush(stdout);
  };
  runtime.cycle = [&](const QString &, const EngineAdapter::Result &done) {
    diagnosis.keep("cycle: end success=" + QByteArray::number(done.success) +
                   " restarted=" + QByteArray::number(done.restarted) +
                   " followUp=" + QByteArray::number(done.followUp) +
                   " reason=" + done.reason.toUtf8());
    if (!done.success && !done.restarted &&
        done.reason != "DELETION_CONFIRMATION_REQUIRED")
      diagnosis.print("failed cycle");
    uploaded += done.uploaded;
    downloaded += done.downloaded;
    deletedRemote += done.deletedRemote;
    renamedRemote += done.renamedRemote;
    if (done.restarted) {
      if (!ready)
        std::fprintf(stderr, "STEP first cycle restarted, its follow-up "
                             "answers\n");
      ++restarts;
      if (done.success || !done.reason.isEmpty() || !done.followUp)
        ++failedRestarts;
      return;
    }
    if (!done.proposalDigest.isEmpty())
      proposal = done.proposalDigest;
    lastReason = done.reason;
    if (!awaited.isEmpty() && done.reason == awaited) {
      awaited.clear();
      answer("AWAITED");
    }
    if (!ready) {
      ready = true;
      std::puts(
          done.success
              ? ("READY " + QByteArray::number(done.downloaded)).constData()
              : ("FAILED " + done.reason.toUtf8()).constData());
    } else if (waiting) {
      auto callback = std::move(waiting);
      waiting = {};
      callback(done);
    }
    std::fflush(stdout);
  };
  runtime.stopped = [] { QCoreApplication::quit(); };
  const auto handle = [&](const QByteArray &line) {
    const auto words = line.split(' ');
    if (words.value(0) == "cycle") {
      waiting = [answer](const EngineAdapter::Result &done) {
        answer(done.success ? QByteArray("CYCLE ok")
                            : "CYCLE " + done.reason.toUtf8());
      };
      runtime.wake(folder, Scheduler::Reason::User);
    } else if (words.value(0) == "await" && words.size() == 2) {
      // Answers once a cycle ended with this reason, at once if the last one
      // did; wakes the folder in case no change woke it yet.
      if (lastReason == QString::fromUtf8(words[1])) {
        answer("AWAITED");
      } else {
        awaited = QString::fromUtf8(words[1]);
        runtime.wake(folder, Scheduler::Reason::User);
      }
    } else if (words.value(0) == "approve") {
      // The user's answer to the held deletion proposal, as Control passes
      // it on (decisions.answer).
      waiting = [answer](const EngineAdapter::Result &done) {
        answer(done.success ? QByteArray("CYCLE ok")
                            : "CYCLE " + done.reason.toUtf8());
      };
      runtime.authorize(folder, proposal, 600000);
    } else if (words.value(0) == "stats") {
      answer("STATS " + QByteArray::number(restarts) + ' ' +
             QByteArray::number(failedRestarts) + ' ' +
             QByteArray::number(uploaded) + ' ' +
             QByteArray::number(downloaded) + ' ' +
             QByteArray::number(deletedRemote) + ' ' +
             QByteArray::number(renamedRemote));
      restarts = failedRestarts = uploaded = downloaded = deletedRemote =
          renamedRemote = 0;
    } else if (words.value(0) == "hold") {
      // A running cycle would also make the folder busy: admission is frozen,
      // so the watcher's cycles end and no new one starts, and the end is
      // polled from the event loop. Waiting inside this handler kept a cycle
      // from ever ending: a command can run inside the nested event loop of a
      // cycle's preflight request, which then cannot return (2026-11-14).
      runtime.hold(true);
      auto *poll = new QTimer(qApp);
      QElapsedTimer waited;
      waited.start();
      QObject::connect(poll, &QTimer::timeout, qApp, [&, poll, waited] {
        if (!runtime.idle() && waited.elapsed() < 30000)
          return;
        poll->stop();
        poll->deleteLater();
        std::fprintf(stderr, "STEP hold waited %lld ms for a running cycle\n",
                     static_cast<long long>(waited.elapsed()));
        if (!runtime.idle())
          diagnosis.print("cycle still running at hold");
        answer("HOLDING");
        // Leave the control thread unserved until an application's request
        // has reached the provider, so the hydration cannot end first, then
        // start a switch as Control would.
        const QDeadlineTimer deadline(20000);
        while (runtime.quiescent(folder) && !deadline.hasExpired())
          QThread::msleep(2);
        answer(runtime.quiesceAccount(account, false) ? "QUIESCE 1"
                                                      : "QUIESCE 0");
        // The switch's own block keeps the folder from cycling until rebind.
        runtime.hold(false);
      });
      poll->start(10);
    } else if (words.value(0) == "quiesce") {
      // Control polls for quiescence too: the hydration the reader waited
      // for reports its end to the control thread after the reader's open
      // returned, so one immediate check raced it (ARM64, 2026-11-14).
      const QDeadlineTimer deadline(10000);
      bool quiescent = runtime.quiesceAccount(account, false);
      while (!quiescent && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        quiescent = runtime.quiesceAccount(account, false);
      }
      answer(quiescent ? "QUIESCE 1" : "QUIESCE 0");
    } else if (words.value(0) == "rebind") {
      try {
        runtime.rebindAccount(account, binding.server, ++epoch);
        keepLog();
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
    } else if (words.value(0) == "dump") {
      // The parent's step failed: print what this process kept.
      diagnosis.print("parent step failed");
      answer("DUMPED");
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
  if (app.arguments().size() == 3 && app.arguments().at(1) == "--convert") {
    try {
      return convertOnly(app.arguments().at(2));
    } catch (const std::exception &error) {
      std::fprintf(stderr, "FAIL convert %s\n", error.what());
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
  const auto convertRoot = work + "/convert";
  const auto convertRemote = remote + "-convert";
  const OnDemandRoot::Identity convertIdentity{
      identity.account, QUuid::createUuid().toString(QUuid::WithoutBraces), {}};
  Child convertChild;
  bool convertCreated = false;
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
    QHash<QString, QByteArray> content{
        {"small.txt", randomBytes(65536)},
        {"pin.txt", randomBytes(65536)},
        {"switch.txt", randomBytes(65536)},
        {"big.bin", randomBytes(64 * 1048576)},
        {"sub/inner.txt", randomBytes(4096)},
        {"gone.txt", randomBytes(4096)},
        {"movedir/m1.txt", randomBytes(4096)},
        {"movedir/m2.txt", randomBytes(4096)}};
    QStringList bulk;
    for (int i = 0; i < 12; ++i) {
      bulk.append(QString("bulk/b%1.txt").arg(i, 2, 10, QChar('0')));
      content.insert(bulk.last(), randomBytes(4096));
    }
    for (const auto *directory : {"/sub/", "/bulk/", "/movedir/"})
      require(dav.send("MKCOL", remote.mid(1) + directory) == 201,
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

    // stats reads what cycles did since the last call: restarted cycles,
    // restarted cycles reported as failures, files uploaded, downloaded,
    // deleted and renamed on the server.
    const auto stats = [&] {
      const auto words = child.command("stats", 10000).split(' ');
      require(words.size() == 7 && words[0] == "STATS", "no stats answer");
      std::vector<quint64> values;
      for (int i = 1; i < words.size(); ++i)
        values.push_back(words[i].toULongLong());
      return values;
    };
    // A watcher cycle that began before a change may answer first; a later
    // one must see it.
    const auto cycleFor = [&](const std::function<bool()> &done) {
      for (int i = 0; i < 3 && !done(); ++i) {
        const auto answer = child.command("cycle", 60000);
        if (answer != "CYCLE ok")
          std::fprintf(stderr, "STEP cycle answered %s\n", answer.constData());
        require(answer == "CYCLE ok", "cycle failed");
      }
      return done();
    };
    stats();

    // A local removal in an on-demand folder makes upstream restart its cycle
    // once; SyncD carries that into the next cycle's engine (patch 0020), so
    // the follow-up propagates the removal by itself, and the restart is no
    // error (deletion review, INV-13, PLAN §3).
    step("a local deletion reaches the server in the follow-up");
    require(DeleteFileW(native(root + "/gone.txt").c_str()),
            "online-only file not deleted");
    require(cycleFor([&] {
              return dav.send("HEAD", remote.mid(1) + "/gone.txt") == 404;
            }),
            "a local deletion never reached the server");
    {
      const auto counts = stats();
      std::fprintf(stderr, "STEP deletion: %llu restarted cycle(s)\n",
                   counts[0]);
      require(counts[0] >= 1 && counts[1] == 0,
              "the restart did not happen, or was reported as a failure");
      require(counts[4] == 1 && counts[2] == 0, "the deletion was not alone");
    }

    step("a local directory move is a move, not deletion and upload");
    {
      Reader reader;
      reader.start(root + "/movedir/m1.txt");
      require(reader.finish(60000) == 0, "moved file not hydrated");
    }
    require(MoveFileW(native(root + "/movedir").c_str(),
                      native(root + "/moved").c_str()),
            "directory not moved");
    require(cycleFor([&] {
              return dav.send("HEAD", remote.mid(1) + "/moved/m2.txt") == 200;
            }),
            "the move never reached the server");
    {
      const auto counts = stats();
      require(counts[4] == 0 && counts[2] == 0 && counts[5] >= 1,
              "a directory move was sent as deletion and upload");
      require(dav.send("HEAD", remote.mid(1) + "/movedir/") == 404 &&
                  dav.sha(remote.mid(1) + "/moved/m1.txt") ==
                      hashOf(content.value("movedir/m1.txt")),
              "the moved directory differs on the server");
      require(online(root + "/moved/m2.txt") &&
                  !online(root + "/moved/m1.txt"),
              "moving changed what is on this device");
    }

    step("deleting many files waits for the user (INV-13)");
    {
      Reader reader;
      reader.start(root + '/' + bulk.first());
      require(reader.finish(60000) == 0, "bulk file not hydrated");
      Reader remover;
      remover.run("/d /c rmdir /s /q \"%1\"", root + "/bulk");
      require(remover.finish(60000) == 0 && !QFileInfo::exists(root + "/bulk"),
              "bulk directory not deleted");
    }
    {
      // The watcher's cycle may hold them before a command could; the held
      // folder then runs no cycle until the user answers.
      require(child.command("await DELETION_CONFIRMATION_REQUIRED", 60000) ==
                  "AWAITED",
              "many deletions were not held for the user");
      const auto counts = stats();
      require(counts[0] >= 1 && counts[1] == 0 && counts[4] == 0,
              "held deletions reached the server");
      for (const auto &path : bulk)
        require(dav.send("HEAD", remote.mid(1) + '/' + path) == 200,
                "a held deletion reached the server");
    }
    require(child.command("approve", 60000) == "CYCLE ok",
            "the approved deletion failed");
    for (const auto &path : bulk)
      require(dav.send("HEAD", remote.mid(1) + '/' + path) == 404,
              "an approved deletion did not reach the server");
    require(stats()[4] >= 1, "the approved deletion was not counted");

    // An NTFS sparse file with its content is a file like any other: upstream
    // read every sparse file as a dehydrated placeholder, deleted a new one
    // and never uploaded an edited one (patch 0021).
    step("a new sparse file uploads intact");
    const auto sparsePath = root + "/sparse-new.bin";
    auto sparse = writeSparse(sparsePath, randomBytes(2 * 1048576));
    require(cycleFor([&] {
              return dav.sha(remote.mid(1) + "/sparse-new.bin") ==
                     hashOf(sparse);
            }),
            "a new sparse file did not upload intact");
    {
      const auto info = placeholder(sparsePath);
      require(sha(sparsePath) == hashOf(sparse) && info.found && info.inSync &&
                  !online(sparsePath),
              "the uploaded sparse file is not an in-sync file on disk");
    }
    step("an edited sparse file uploads");
    sparse = editSparse(sparsePath, randomBytes(1048576), 60);
    require(cycleFor([&] {
              return dav.sha(remote.mid(1) + "/sparse-new.bin") ==
                     hashOf(sparse);
            }),
            "an edited sparse file did not upload");
    step("a server change to a sparse file downloads");
    {
      const auto server = randomBytes(2 * 1048576);
      require(dav.change(remote.mid(1) + "/sparse-new.bin", server) / 100 ==
                  2,
              "server change not written");
      require(cycleFor([&] { return sha(sparsePath) == hashOf(server); }),
              "a server change did not reach a sparse file");
      require(!online(sparsePath), "the downloaded file is online-only");
    }
    step("a sparse file changed on both sides is a conflict");
    {
      // The server first: a watcher cycle between a local write and a later
      // server change would upload the local one, and no conflict is left.
      const auto server = randomBytes(2 * 1048576);
      require(dav.change(remote.mid(1) + "/sparse-new.bin", server) / 100 ==
                  2,
              "server change not written");
      sparse = writeSparse(sparsePath, randomBytes(2 * 1048576));
      const auto copies = [&] {
        return QDir(root).entryList({"sparse-new (conflicted copy*"},
                                    QDir::Files);
      };
      const bool conflicted = cycleFor([&] { return !copies().isEmpty(); });
      if (!conflicted)
        std::fprintf(
            stderr, "STEP no conflict: local %s, server %s, entries %s\n",
            sha(sparsePath) == hashOf(sparse)   ? "kept"
            : sha(sparsePath) == hashOf(server) ? "server's"
                                                : "other",
            dav.sha(remote.mid(1) + "/sparse-new.bin") == hashOf(server)
                ? "kept"
                : "replaced",
            qPrintable(QDir(root).entryList(QDir::Files).join(',')));
      require(conflicted, "no conflict copy");
      require(sha(root + '/' + copies().first()) == hashOf(sparse) &&
                  sha(sparsePath) == hashOf(server),
              "the conflict lost one side");
    }
    stats();

    step("a switch waits for a running hydration (INV-05)");
    {
      // The child first lets a running cycle end (up to 30 s): the steps
      // before leave watcher cycles behind.
      require(child.command("hold", 40000) == "HOLDING", "hold not accepted");
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

    // Evidence only: SYNCD_TEST_HOLD names a marker file that exists for 30 s
    // while the root is still registered and served, after every check on it,
    // so the Explorer navigation entry can be photographed.
    if (const auto marker = qEnvironmentVariable("SYNCD_TEST_HOLD");
        !marker.isEmpty()) {
      step("holding the registered root for 30 s");
      QFile file(marker);
      require(file.open(QIODevice::WriteOnly), "hold marker not written");
      file.close();
      QThread::sleep(30);
      QFile::remove(marker);
    }
    step("removal unregisters the sync root");
    require(child.stop(), "runtime did not shut down cleanly");
    require(recordType(work + "/journal.sqlite", "sparse-new.bin") ==
                ItemTypeFile,
            "the journal records a sparse file as virtual");
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

    // v0.1.0 conversion: a folder synced with full files becomes on-demand
    // in place; nothing is downloaded again and the journal stays as it is.
    step("a v0.1.0 folder syncs with full files");
    require(dav.send("MKCOL", convertRemote.mid(1) + '/') == 201 &&
                dav.send("MKCOL", convertRemote.mid(1) + "/sub/") == 201,
            "MKCOL convert failed");
    convertCreated = true;
    QHash<QString, QByteArray> files;
    for (int i = 0; i < 24; ++i)
      files.insert(QString(i % 3 ? "f%1.txt" : "sub/f%1.txt").arg(i),
                   randomBytes(4096 + i));
    files.insert("keep.txt", randomBytes(8192));
    for (auto it = files.cbegin(); it != files.cend(); ++it)
      require(dav.send("PUT", convertRemote.mid(1) + '/' + it.key(),
                       it.value()) == 201,
              "PUT convert failed");
    require(QDir().mkpath(convertRoot), "convert root not created");
    // A sparse file the v0.1.0 folder uploaded: the conversion pass converts
    // it, and it keeps syncing both ways afterwards (patch 0021).
    const auto oldSparsePath = convertRoot + "/sparse-old.bin";
    auto oldSparse = writeSparse(oldSparsePath, randomBytes(2 * 1048576));
    const auto convertJournal = work + "/convert-journal.sqlite";
    const auto writeState = [&](const QString &path, bool full) {
      const auto remoteRoot = readCollections(base, verification.uid, user,
                                              reference, convertRemote, false);
      QFile file(path);
      require(file.open(QIODevice::WriteOnly), "convert state not written");
      file.write(QJsonDocument(QJsonObject{{"base", base.toString()},
                                           {"login", user},
                                           {"uid", verification.uid},
                                           {"reference", reference},
                                           {"local", convertRoot},
                                           {"remote", convertRemote},
                                           {"journal", convertJournal},
                                           {"remoteIdentity",
                                            remoteRoot.front().identity},
                                           {"folder", convertIdentity.folder},
                                           {"account", convertIdentity.account},
                                           {"full", full}})
                     .toJson());
    };
    const auto fullState = work + "/convert-full.json",
               onDemandState = work + "/convert-ondemand.json";
    writeState(fullState, true);
    writeState(onDemandState, false);
    convertChild.start(fullState);
    require(convertChild.downloaded == quint64(files.size()),
            "the full-file cycle did not download every file");
    require(convertChild.stop(), "full-file runtime did not stop");
    for (auto it = files.cbegin(); it != files.cend(); ++it)
      require(sha(convertRoot + '/' + it.key()) ==
                      QCryptographicHash::hash(it.value(),
                                               QCryptographicHash::Sha256) &&
                  !placeholder(convertRoot + '/' + it.key()).found,
              "the full-file folder does not hold its files");
    require(dav.sha(convertRemote.mid(1) + "/sparse-old.bin") ==
                    hashOf(oldSparse) &&
                !placeholder(oldSparsePath).found,
            "the full-file folder did not upload its sparse file");
    const auto before = journalDigest(convertJournal);

    step("the conversion pass makes the content in-sync placeholders");
    // An application deletes keep.txt during the pass, so it cannot be
    // opened and stays an ordinary file, as a pass's failure would leave it.
    auto deleting = CreateFileW(
        native(convertRoot + "/keep.txt").c_str(), DELETE | GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    require(deleting != INVALID_HANDLE_VALUE, "keep.txt not opened");
    const auto pend = [&](bool value) {
      FILE_DISPOSITION_INFO disposition{value};
      return SetFileInformationByHandle(deleting, FileDispositionInfo,
                                        &disposition, sizeof(disposition));
    };
    {
      auto release = qScopeGuard([&] {
        pend(false);
        CloseHandle(deleting);
      });
      require(pend(true), "keep.txt not marked for deletion");
      QProcess pass;
      pass.setProcessChannelMode(QProcess::ForwardedErrorChannel);
      pass.start(QCoreApplication::applicationFilePath(),
                 {"--convert", onDemandState});
      require(pass.waitForFinished(60000) && pass.exitCode() == 0,
              "conversion pass failed");
      const auto answer = pass.readAllStandardOutput().trimmed();
      require(answer.startsWith("CONVERTED "), "conversion pass answered");
      std::fprintf(stderr, "STEP conversion of %lld entries took %s ms\n",
                   static_cast<long long>(files.size() + 1),
                   answer.mid(10).constData());
    }
    require(journalDigest(convertJournal) == before,
            "the conversion pass changed the journal");
    const auto ids = fileIds(convertJournal);
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
      const auto path = convertRoot + '/' + it.key();
      const auto info = placeholder(path);
      require(sha(path) == QCryptographicHash::hash(it.value(),
                                                    QCryptographicHash::Sha256),
              "conversion changed a file's content");
      if (it.key() == "keep.txt") {
        require(!info.found, "a file being deleted was converted");
        continue;
      }
      require(info.found && info.inSync && !online(path) &&
                  info.identity == wideIdentity(ids.value(it.key())),
              "a converted file is not an in-sync hydrated placeholder with "
              "its journal file ID");
    }
    require(placeholder(convertRoot + "/sub").found,
            "a directory was not converted");
    {
      const auto info = placeholder(oldSparsePath);
      require(info.found && info.inSync && !online(oldSparsePath) &&
                  info.identity == wideIdentity(ids.value("sparse-old.bin")) &&
                  sha(oldSparsePath) == hashOf(oldSparse),
              "the sparse file is not an in-sync placeholder with its content");
    }

    step("the first on-demand cycle repairs a file the pass left, no download");
    convertChild.start(onDemandState);
    require(convertChild.downloaded == 0,
            "the first on-demand cycle downloaded a converted folder's file");
    {
      const auto info = placeholder(convertRoot + "/keep.txt");
      require(info.found && info.inSync &&
                  info.identity == wideIdentity(ids.value("keep.txt")) &&
                  sha(convertRoot + "/keep.txt") ==
                      QCryptographicHash::hash(files.value("keep.txt"),
                                               QCryptographicHash::Sha256),
              "the first cycle did not make the file left an in-sync "
              "placeholder");
    }

    step("free up space and re-open a converted file");
    const auto freed = convertRoot + "/f1.txt";
    require(bool(OCC::CfApiWrapper::setPinState(QDir::toNativeSeparators(freed),
                                                OCC::PinState::OnlineOnly,
                                                OCC::CfApiWrapper::NoRecurse)),
            "online-only pin not set on a converted file");
    bool dehydrated = false;
    for (int i = 0; i < 2 && !dehydrated; ++i) {
      require(convertChild.command("cycle", 60000) == "CYCLE ok",
              "cycle after free up space failed");
      dehydrated = online(freed);
    }
    require(dehydrated, "free up space kept a converted file's content");
    {
      Reader reader;
      reader.start(freed);
      require(reader.finish(60000) == 0, "re-opening a freed file failed");
    }
    require(!online(freed) && sha(freed) == QCryptographicHash::hash(
                                                files.value("f1.txt"),
                                                QCryptographicHash::Sha256),
            "a re-opened converted file differs from the server");

    step("a converted sparse file keeps syncing both ways");
    const auto convertCycle = [&](const std::function<bool()> &done) {
      for (int i = 0; i < 3 && !done(); ++i)
        require(convertChild.command("cycle", 60000) == "CYCLE ok",
                "converted folder cycle failed");
      return done();
    };
    require(sha(oldSparsePath) == hashOf(oldSparse),
            "the first on-demand cycles changed the sparse file");
    oldSparse = editSparse(oldSparsePath, randomBytes(1048576), 60);
    require(convertCycle([&] {
              return dav.sha(convertRemote.mid(1) + "/sparse-old.bin") ==
                     hashOf(oldSparse);
            }),
            "an edit of a converted sparse file did not upload");
    {
      const auto server = randomBytes(2 * 1048576);
      require(dav.change(convertRemote.mid(1) + "/sparse-old.bin",
                       server) /
                      100 ==
                  2,
              "server change not written");
      require(convertCycle([&] { return sha(oldSparsePath) == hashOf(server); }),
              "a server change did not reach a converted sparse file");
    }
    require(convertChild.stop(), "converted runtime did not shut down");
    require(recordType(convertJournal, "sparse-old.bin") == ItemTypeFile,
            "the journal records a converted sparse file as virtual");

    std::fprintf(stderr, "PASS ondemand acceptance %lld ms\n",
                 static_cast<long long>(total.elapsed()));
    result = 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    for (auto *serving : {&child, &convertChild})
      if (serving->process.state() == QProcess::Running)
        serving->command("dump", 15000);
  }
  step("cleanup");
  child.kill();
  convertChild.kill();
  // A root a failed run never created has no registration to end.
  const auto release = [](const QString &path,
                          const OnDemandRoot::Identity &owner) {
    return !QFileInfo::exists(path) ||
           OnDemandRoot::unregister(path, owner).isEmpty();
  };
  bool clean = release(root, identity) && release(convertRoot, convertIdentity);
  if (remoteCreated && dav.send("DELETE", remote.mid(1) + '/') != 204)
    clean = false;
  if (convertCreated && dav.send("DELETE", convertRemote.mid(1) + '/') != 204)
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
