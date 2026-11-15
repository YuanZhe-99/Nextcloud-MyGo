// SPDX-License-Identifier: GPL-3.0-only
// On-demand folder contracts against the real Cloud Files API on temporary
// NTFS roots: registration identity, ownership, the request gate a switch
// waits on, and complete unregistration. No server is contacted; hydration
// requests fail against a closed loopback port.
// windows.h and winternl.h (NTSTATUS) must precede cfapi.h.
#include <windows.h>
#include <winternl.h>
#include <winioctl.h>

#include <cfapi.h>

#include "account.h"
#include "clientsideencryption.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "common/vfs.h"
#include "csync.h"
#include "creds/abstractcredentials.h"
#include "platform/windows/ondemand.h"
#include "root.h"
#include "vfs/cfapi/cfapiwrapper.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <cstdio>
#include <functional>
#include <sddl.h>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
/// require keeps contract checks active in release builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

/// step names each case so a failure says where it stopped.
void step(const char *name) { std::fprintf(stderr, "STEP %s\n", name); }

/// skipped is the exit code CTest reports as a skip (SKIP_RETURN_CODE).
constexpr int skipped = 77;

/// ownershipAnswers checks how simulated Cloud Files answers classify: an
/// unavailable Cloud Files (as on Windows Server runners) is unsupported, never
/// a folder owned by someone else, and an unexplained failure while the filter
/// runs still refuses as owned (fail closed).
void ownershipAnswers() {
  const auto free = HRESULT_FROM_WIN32(ERROR_CLOUD_FILE_NOT_UNDER_SYNC_ROOT);
  const struct {
    long result;
    bool running;
    QString provider;
    bool own;
    QString want;
  } cases[] = {
      {free, true, {}, false, {}},
      {free, false, {}, false, {}},
      {S_OK, true, OnDemandRoot::provider, true, {}},
      {S_OK, true, OnDemandRoot::provider, false, "FOLDER_OWNED"},
      {S_OK, true, "OneDrive", false, "FOLDER_OWNED"},
      {HRESULT_FROM_WIN32(ERROR_CLOUD_FILE_PROVIDER_NOT_RUNNING), true, {}, false,
       "UNSUPPORTED_FOLDER_MODE"},
      {HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), true, {}, false,
       "UNSUPPORTED_FOLDER_MODE"},
      {HRESULT_FROM_WIN32(ERROR_INVALID_FUNCTION), true, {}, false,
       "UNSUPPORTED_FOLDER_MODE"},
      {HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), false, {}, false,
       "UNSUPPORTED_FOLDER_MODE"},
      {HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), true, {}, false, "FOLDER_OWNED"},
  };
  for (const auto &c : cases)
    if (OnDemandRoot::ownershipReason(c.result, c.running, c.provider, c.own) !=
        c.want) {
      std::fprintf(stderr, "case 0x%08lx running=%d provider=%s\n",
                   static_cast<unsigned long>(c.result), c.running,
                   qPrintable(c.provider));
      throw std::runtime_error("a Cloud Files answer was misclassified");
    }
}

/// OfflineCredentials gives the account a network manager without secrets;
/// every request it creates goes to a closed loopback port.
class OfflineCredentials final : public OCC::AbstractCredentials {
public:
  QString authType() const override { return "http"; }
  QString user() const override { return "fixture"; }
  QString password() const override { return {}; }
  QNetworkAccessManager *createQNAM() const override {
    return new QNetworkAccessManager;
  }
  bool ready() const override { return true; }
  void fetchFromKeychain(const QString & = {}) override {
    _wasFetched = true;
    Q_EMIT fetched();
  }
  void askFromUser() override { Q_EMIT asked(); }
  bool stillValid(QNetworkReply *) override { return true; }
  void persist() override {}
  void invalidateToken() override {}
  void forgetSensitiveData() override {}
};

/// syncRootKey is the HKLM registration key of an identity.
QString syncRootKey(const OnDemandRoot::Identity &identity) {
  HANDLE token = nullptr;
  OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  QByteArray data(int(size), 0);
  GetTokenInformation(token, TokenUser, data.data(), size, &size);
  CloseHandle(token);
  LPWSTR sid = nullptr;
  ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid,
                         &sid);
  const auto text = QString::fromWCharArray(sid);
  LocalFree(sid);
  return QString(
             R"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager\)") +
         OnDemandRoot::provider + '!' + text + '!' + identity.account + '!' +
         identity.folder;
}

/// hasValue reports whether a registration value exists.
bool hasValue(const QString &key, const wchar_t *name) {
  return RegGetValueW(HKEY_LOCAL_MACHINE,
                      reinterpret_cast<LPCWSTR>(key.utf16()), name, RRF_RT_ANY,
                      nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

/// hasKey reports whether a registration key exists.
bool hasKey(const QString &key) {
  HKEY handle = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, reinterpret_cast<LPCWSTR>(key.utf16()),
                    0, KEY_READ, &handle) != ERROR_SUCCESS)
    return false;
  RegCloseKey(handle);
  return true;
}

/// userValue reads an HKCU string value (name nullptr is the default value).
/// Returns: null when the key or value is missing.
QString userValue(const QString &key, const wchar_t *name) {
  wchar_t value[MAX_PATH * 4]{};
  DWORD bytes = sizeof(value) - sizeof(wchar_t);
  if (RegGetValueW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(key.utf16()),
                   name, RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
    return {};
  return QString::fromWCharArray(value);
}

/// hasUserValue reports whether an HKCU value of any type exists.
bool hasUserValue(const QString &key, const wchar_t *name) {
  return RegGetValueW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(key.utf16()),
                      name, RRF_RT_ANY | RRF_NOEXPAND, nullptr, nullptr,
                      nullptr) == ERROR_SUCCESS;
}

/// hasUserKey reports whether an HKCU key exists.
bool hasUserKey(const QString &key) {
  HKEY handle = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(key.utf16()), 0,
                    KEY_READ, &handle) != ERROR_SUCCESS)
    return false;
  RegCloseKey(handle);
  return true;
}

const QString classKey = R"(Software\Classes\CLSID\)";
const QString classWow64Key = R"(Software\Classes\Wow6432Node\CLSID\)";
const QString namespaceKey =
    R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\)";
const QString hiddenIconsKey =
    R"(Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel)";

/// forgetNavigationEntry deletes whatever navigation entry a failed case left
/// for a CLSID; it never runs with an empty one.
void forgetNavigationEntry(const QString &clsid) {
  if (clsid.isEmpty())
    return;
  for (const auto &prefix : {classKey, classWow64Key, namespaceKey})
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   reinterpret_cast<LPCWSTR>((prefix + clsid).utf16()));
  RegDeleteKeyValueW(HKEY_CURRENT_USER,
                     reinterpret_cast<LPCWSTR>(hiddenIconsKey.utf16()),
                     reinterpret_cast<LPCWSTR>(clsid.utf16()));
}

/// navigationEntry checks a root's Explorer navigation pane entry: the sync
/// root record names it, and the HKCU entry opens this root under its title.
void navigationEntry(const OnDemandRoot::Identity &identity,
                     const QString &root) {
  const auto clsid = OnDemandRoot::navigationClsid(identity);
  require(clsid == '{' + identity.folder + '}',
          "navigation CLSID is not the folder ID");
  wchar_t recorded[64]{};
  DWORD bytes = sizeof(recorded) - sizeof(wchar_t);
  require(RegGetValueW(HKEY_LOCAL_MACHINE,
                       reinterpret_cast<LPCWSTR>(syncRootKey(identity).utf16()),
                       L"NamespaceCLSID", RRF_RT_REG_SZ, nullptr, recorded,
                       &bytes) == ERROR_SUCCESS &&
              QString::fromWCharArray(recorded) == clsid,
          "sync root does not name its navigation entry");
  for (const auto &prefix : {classKey, classWow64Key}) {
    require(userValue(prefix + clsid, nullptr) == identity.name,
            "navigation entry has no title");
    require(userValue(prefix + clsid + R"(\Instance\InitPropertyBag)",
                      L"TargetFolderPath") == QDir::toNativeSeparators(root),
            "navigation entry does not open the root");
    require(userValue(prefix + clsid + R"(\Instance)", L"CLSID")
                    .compare("{0E5AAE11-A475-4c5b-AB00-C66DE400274E}",
                             Qt::CaseInsensitive) == 0,
            "navigation entry is not a file folder");
    require(userValue(prefix + clsid, L"ApplicationName") == "Nextcloud-MyGo",
            "navigation entry is not tagged");
  }
  require(userValue(namespaceKey + clsid, nullptr) == identity.name &&
              userValue(namespaceKey + clsid, L"ApplicationName") ==
                  "Nextcloud-MyGo",
          "navigation entry not in the namespace root");
  require(hasUserValue(hiddenIconsKey, reinterpret_cast<const wchar_t *>(
                                           clsid.utf16())),
          "navigation entry shown on the desktop");
}

/// noNavigationEntry checks that nothing of a CLSID's entry is left.
void noNavigationEntry(const QString &clsid) {
  require(!clsid.isEmpty(), "no navigation CLSID");
  for (const auto &prefix : {classKey, classWow64Key, namespaceKey})
    require(!hasUserKey(prefix + clsid), "navigation entry key left behind");
  require(!hasUserValue(hiddenIconsKey,
                        reinterpret_cast<const wchar_t *>(clsid.utf16())),
          "desktop icon value left behind");
}

/// underSyncRoot reports whether Cloud Files still knows a path.
bool underSyncRoot(const QString &path) {
  CF_SYNC_ROOT_PROVIDER_INFO info{};
  DWORD length = 0;
  return CfGetSyncRootInfoByPath(
             reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
             CF_SYNC_ROOT_INFO_PROVIDER, &info, sizeof(info), &length) !=
         HRESULT_FROM_WIN32(ERROR_CLOUD_FILE_NOT_UNDER_SYNC_ROOT);
}

/// attributes reads a path's attributes without opening its content.
DWORD attributes(const QString &path) {
  return GetFileAttributesW(
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
}

/// unsupported reports a lease refusal for an unsupported folder mode.
bool unsupported(const std::function<void()> &lease) {
  try {
    lease();
    return false;
  } catch (const std::exception &e) {
    return QByteArray(e.what()) == "UNSUPPORTED_FOLDER_MODE";
  }
}

/// startReader opens a placeholder from another process, as an application
/// would; the provider never hydrates for its own process.
void startReader(QProcess &reader, const QString &path) {
  reader.setProgram("cmd.exe");
  reader.setNativeArguments("/d /c copy /b /y \"" +
                            QDir::toNativeSeparators(path) + "\" nul > nul");
  reader.start();
  require(reader.waitForStarted(10000), "reader did not start");
}

/// finish serves the control thread until the reader exits.
/// Returns: its exit code, or -1 after the deadline.
int finish(QProcess &reader, int milliseconds) {
  const QDeadlineTimer deadline(milliseconds);
  while (reader.state() != QProcess::NotRunning && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    reader.waitForFinished(10);
  }
  if (reader.state() != QProcess::NotRunning) {
    reader.kill();
    reader.waitForFinished(10000);
    return -1;
  }
  return reader.exitStatus() == QProcess::NormalExit ? reader.exitCode() : -1;
}

/// virtualRecord registers a placeholder in the journal as upstream discovery
/// would, so a hydration request reaches the network.
void virtualRecord(OCC::SyncJournalDb &journal, const QByteArray &path,
                   qint64 size) {
  OCC::SyncJournalFileRecord record;
  record._path = path;
  record._type = ItemTypeVirtualFile;
  record._fileId = "fixture-" + path;
  record._etag = "etag";
  record._modtime = 1700000000;
  record._fileSize = size;
  record._remotePerm = OCC::RemotePermissions::fromDbValue("WDNVR");
  require(bool(journal.setFileRecord(record)), "journal record not written");
}

/// convertInPlace makes an ordinary file with content a hydrated placeholder.
void convertInPlace(const QString &path) {
  auto handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
      GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
      nullptr);
  require(handle != INVALID_HANDLE_VALUE, "hydrated fixture not opened");
  const QByteArray id = "fixture-kept";
  const auto result =
      CfConvertToPlaceholder(handle, id.constData(), DWORD(id.size()),
                             CF_CONVERT_FLAG_MARK_IN_SYNC, nullptr, nullptr);
  CloseHandle(handle);
  require(SUCCEEDED(result), "hydrated placeholder not created");
}

/// writeSparse writes a sparse file whose first half is a hole and whose
/// second half is content: an NTFS sparse file with all of it on disk.
void writeSparse(const QString &path) {
  constexpr DWORD size = 1048576;
  auto handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
      GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "sparse fixture not created");
  const auto close = qScopeGuard([&] { CloseHandle(handle); });
  DWORD returned = 0;
  const QByteArray content(size, 'S');
  FILE_ZERO_DATA_INFORMATION hole{};
  hole.BeyondFinalZero.QuadPart = size / 2;
  require(DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                          &returned, nullptr) &&
              WriteFile(handle, content.constData(), size, &returned,
                        nullptr) &&
              DeviceIoControl(handle, FSCTL_SET_ZERO_DATA, &hole, sizeof(hole),
                              nullptr, 0, &returned, nullptr),
          "sparse fixture not written");
}

/// statVirtual asks upstream's plugin how local discovery reads an entry.
/// Returns: the item type, or ItemTypeFile when the plugin reads an ordinary
/// file (statTypeVirtualFile false).
ItemType statVirtual(OCC::Vfs &vfs, const QString &path) {
  WIN32_FIND_DATAW entry{};
  const auto search = FindFirstFileExW(
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
      FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
  require(search != INVALID_HANDLE_VALUE, "stat fixture missing");
  FindClose(search);
  csync_file_stat_t stat;
  stat.path = QFileInfo(path).fileName().toUtf8();
  return vfs.statTypeVirtualFile(&stat, &entry) ? stat.type : ItemTypeFile;
}

/// findState reads a path's Cloud Files state from its directory entry, as
/// OnDemandRoot::unregister's release does.
CF_PLACEHOLDER_STATE findState(const QString &path) {
  WIN32_FIND_DATAW entry{};
  const auto search = FindFirstFileExW(
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
      FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
  if (search == INVALID_HANDLE_VALUE)
    return CF_PLACEHOLDER_STATE_INVALID;
  FindClose(search);
  return CfGetPlaceholderStateFromFindData(&entry);
}

/// Placeholder is what Cloud Files records for one converted entry.
struct Placeholder {
  bool found = false, inSync = false;
  CF_PIN_STATE pin = CF_PIN_STATE_UNSPECIFIED;
  qint64 onDisk = -1;
  QByteArray identity;
};

/// placeholderInfo reads an entry's placeholder record without requesting
/// any content. Returns: found false for an ordinary file.
Placeholder placeholderInfo(const QString &path) {
  auto handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return {};
  std::vector<char> buffer(sizeof(CF_PLACEHOLDER_STANDARD_INFO) + 4096);
  DWORD length = 0;
  const auto result = CfGetPlaceholderInfo(
      handle, CF_PLACEHOLDER_INFO_STANDARD, buffer.data(), DWORD(buffer.size()),
      &length);
  CloseHandle(handle);
  if (FAILED(result))
    return {};
  const auto *info =
      reinterpret_cast<const CF_PLACEHOLDER_STANDARD_INFO *>(buffer.data());
  return {true, info->InSyncState == CF_IN_SYNC_STATE_IN_SYNC, info->PinState,
          info->OnDiskDataSize.QuadPart,
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

/// journalDigest hashes every journal record field a conversion must leave
/// alone. Returns: the record count and the digest.
std::pair<int, QByteArray> journalDigest(OCC::SyncJournalDb &journal) {
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
  return {count, hash.result()};
}

/// conversionCase converts a full-file folder's content in place: the
/// journal's file IDs become placeholder identities, directories are
/// converted even when the budget is gone, files only while it lasts; a
/// second pass after an interruption completes the first; unconverted,
/// changed and missing entries are left; a sparse file is converted and
/// upstream reads it as hydrated (patch 0021); the journal is unchanged;
/// unregistering afterwards (the constructor-failure rollback) leaves every
/// file as an ordinary file with its content and deletes nothing (v0.1.0
/// conversion, INV-01/06).
void conversionCase(const QString &fixture, const OCC::AccountPtr &account) {
  const auto root = QDir::cleanPath(fixture + "/convert");
  const OnDemandRoot::Identity identity{
      QUuid::createUuid().toString(QUuid::WithoutBraces),
      QUuid::createUuid().toString(QUuid::WithoutBraces),
      "Nextcloud MyGo - convert"};
  const auto cleanup = qScopeGuard([&] {
    if (underSyncRoot(root))
      OnDemandRoot::unregister(root, identity);
    forgetNavigationEntry(OnDemandRoot::navigationClsid(identity));
  });
  require(QDir().mkpath(root + "/dir/deep"), "conversion fixture missing");
  const QStringList files{"a.txt", "dir/b.txt", "dir/deep/c.txt",
                          "changed.txt", "open.txt", "deleting.txt",
                          "sparse.bin"};
  for (const auto &name : files) {
    QFile file(root + '/' + name);
    require(file.open(QIODevice::WriteOnly) &&
                file.write("content of " + name.toUtf8()) > 0,
            "conversion file fixture missing");
  }
  writeSparse(root + "/sparse.bin");
  OCC::SyncJournalDb journal(fixture + "/convert-journal.db");
  const auto record = [&](const QString &path, ItemType type) {
    const QFileInfo info(root + '/' + path);
    OCC::SyncJournalFileRecord entry;
    entry._path = path.toUtf8();
    entry._type = type;
    entry._fileId = "00000" + path.toUtf8().toHex().left(12) + "ocfixture";
    entry._etag = "etag-" + path.toUtf8();
    entry._modtime = info.exists() ? info.lastModified().toSecsSinceEpoch()
                                   : 1700000000;
    entry._fileSize = type == ItemTypeDirectory ? 0 : info.size();
    entry._remotePerm = OCC::RemotePermissions::fromDbValue("WDNVR");
    if (path == "changed.txt")
      entry._fileSize += 1; // changed here since its last sync
    require(bool(journal.setFileRecord(entry)), "journal fixture not written");
    return entry._fileId;
  };
  QHash<QString, QByteArray> ids;
  for (const auto &path : {"dir", "dir/deep"})
    ids.insert(path, record(path, ItemTypeDirectory));
  for (const auto &name : files)
    ids.insert(name, record(name, ItemTypeFile));
  record("gone.txt", ItemTypeFile); // deleted here while SyncD was stopped
  const auto before = journalDigest(journal);
  QHash<QString, QByteArray> contents;
  for (const auto &name : files) {
    QFile file(root + '/' + name);
    require(file.open(QIODevice::ReadOnly), "fixture unreadable");
    contents.insert(name, file.readAll());
  }
  require(OnDemandRoot::conversionError(root, identity).isEmpty(),
          "conversion fixture refused");
  // An application holds open.txt without sharing during the pass (it is
  // still converted: upstream opens with no access), and another one is
  // deleting deleting.txt, so no one can open it (that conversion fails).
  auto open = CreateFileW(
      reinterpret_cast<LPCWSTR>(
          QDir::toNativeSeparators(root + "/open.txt").utf16()),
      GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  auto deleting = CreateFileW(
      reinterpret_cast<LPCWSTR>(
          QDir::toNativeSeparators(root + "/deleting.txt").utf16()),
      DELETE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
      FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  require(open != INVALID_HANDLE_VALUE && deleting != INVALID_HANDLE_VALUE,
          "open fixtures not opened");
  const auto pend = [&](bool value) {
    FILE_DISPOSITION_INFO disposition{value};
    return SetFileInformationByHandle(deleting, FileDispositionInfo,
                                      &disposition, sizeof(disposition));
  };
  auto unlock = qScopeGuard([&] {
    pend(false);
    CloseHandle(deleting);
    CloseHandle(open);
  });
  require(pend(true), "delete-pending fixture not set");
  const QStringList converted{"dir",       "dir/deep", "a.txt",
                              "dir/b.txt", "dir/deep/c.txt", "open.txt",
                              "sparse.bin"};
  {
    OnDemandRoot connected(identity, root, "/", account, &journal);
    step("a pass whose budget is gone converts only directories");
    auto done = connected.convertExisting(journal, QDeadlineTimer(0));
    std::fprintf(stderr, "first pass converted %llu deferred %llu\n",
                 done.converted, done.deferred);
    require(done.converted == 2 && done.deferred == 8 && done.failed == 0,
            "an expired budget still converted files, or skipped a directory");
    require(placeholderInfo(root + "/dir/deep").found &&
                !placeholderInfo(root + "/a.txt").found,
            "the interrupted pass left the wrong entries");

    step("a second pass completes an interrupted one");
    QElapsedTimer pass;
    pass.start();
    done = connected.convertExisting(journal, QDeadlineTimer(60000));
    std::fprintf(stderr,
                 "second pass %lld ms: converted %llu present %llu failed "
                 "%llu skipped %llu\n",
                 static_cast<long long>(pass.elapsed()), done.converted,
                 done.present, done.failed, done.skipped);
    require(done.converted == 5 && done.present == 2 && done.deferred == 0,
            "the second pass did not finish the folder");
    require(done.failed == 1 && done.skipped == 2,
            "a changed, missing or deleting entry was not left");
    for (const auto &path : converted) {
      const auto info = placeholderInfo(root + '/' + path);
      require(info.found && info.inSync,
              "a converted entry is not an in-sync placeholder");
      require(info.identity == wideIdentity(ids.value(path)),
              "a placeholder's identity is not its journal file ID");
      require(info.pin == CF_PIN_STATE_UNSPECIFIED,
              "conversion changed a pin state");
      if (path.contains('.'))
        require(info.onDisk == contents.value(path).size(),
                "a converted file is not fully on disk");
    }
    for (const auto &path : {"changed.txt", "deleting.txt"})
      require(!placeholderInfo(root + '/' + QString(path)).found,
              "a file that must stay ordinary was converted");
    step("a converted sparse file reads as hydrated, never virtual");
    {
      const auto sparse = root + "/sparse.bin";
      require(attributes(sparse) & FILE_ATTRIBUTE_SPARSE_FILE,
              "the converted fixture is no longer sparse");
      require(!connected.vfs()->isDehydratedPlaceholder(sparse) &&
                  statVirtual(*connected.vfs(), sparse) == ItemTypeFile,
              "upstream reads a converted sparse file as dehydrated");
      QFile file(sparse);
      require(file.open(QIODevice::ReadOnly) &&
                  file.readAll() == contents.value("sparse.bin"),
              "a converted sparse file lost content");
    }
    step("conversion is idempotent");
    done = connected.convertExisting(journal, QDeadlineTimer(60000));
    require(done.converted == 0 && done.present == 7 && done.failed == 1,
            "a repeated pass changed converted entries");
  }
  require(journalDigest(journal) == before,
          "conversion changed the journal");
  // A process that is not the connected provider sees placeholders
  // disguised; the exposed view shows what is really on disk.
  using Mode = CHAR(NTAPI *)(CHAR);
  const auto setMode = reinterpret_cast<Mode>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                     "RtlSetProcessPlaceholderCompatibilityMode"));
  require(setMode, "placeholder compatibility mode unavailable");
  const auto exposed = [&](const std::function<void()> &body) {
    const auto previous = setMode(2); // PHCM_EXPOSE_PLACEHOLDERS
    body();
    setMode(previous);
  };
  // unregister's release deletes a file it reads as an online-only
  // placeholder; a converted file must never read so, in either view.
  const auto deletable = [&](const QString &path) {
    const auto state = findState(root + '/' + path);
    return state != CF_PLACEHOLDER_STATE_INVALID &&
           (state & CF_PLACEHOLDER_STATE_PLACEHOLDER) &&
           (state & (CF_PLACEHOLDER_STATE_PARTIAL |
                     CF_PLACEHOLDER_STATE_PARTIALLY_ON_DISK));
  };
  for (const auto &path : converted)
    require(!deletable(path), "removal would read a converted file as online");
  exposed([&] {
    for (const auto &path : converted) {
      require(attributes(root + '/' + path) & FILE_ATTRIBUTE_REPARSE_POINT,
              "a converted entry is no placeholder on disk");
      require(!deletable(path),
              "removal would read a converted file as online (exposed)");
    }
  });
  unlock.dismiss();
  require(pend(false), "delete-pending fixture not cancelled");
  CloseHandle(deleting);
  CloseHandle(open);
  step("rollback: unregistering a converted folder keeps every file");
  require(OnDemandRoot::unregister(root, identity).isEmpty(),
          "rollback unregistration failed");
  require(!underSyncRoot(root), "rollback left the sync root");
  for (const auto &name : files) {
    QFile file(root + '/' + name);
    require(file.open(QIODevice::ReadOnly) &&
                file.readAll() == contents.value(name),
            "rollback lost or changed a file");
  }
  exposed([&] {
    for (const auto &path : converted + files)
      require(!(attributes(root + '/' + path) & FILE_ATTRIBUTE_REPARSE_POINT),
              "rollback left a placeholder on disk");
  });
  {
    RootLease lease(root);
    require(lease.validateTree(), "rolled-back folder is not a full-file root");
  }
  require(journalDigest(journal) == before, "rollback changed the journal");
}
} // namespace

/// main runs the cases in order; every exit path unregisters what it created.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QElapsedTimer total;
  total.start();
  QTemporaryDir fixture;
  QString root, foreign;
  OnDemandRoot::Identity identity{
      QUuid::createUuid().toString(QUuid::WithoutBraces),
      QUuid::createUuid().toString(QUuid::WithoutBraces),
      "Nextcloud MyGo - contract"};
  const auto cleanup = qScopeGuard([&] {
    // Unregister whatever a failed case left, never another provider's root.
    if (!root.isEmpty() && underSyncRoot(root))
      OnDemandRoot::unregister(root, identity);
    forgetNavigationEntry(OnDemandRoot::navigationClsid(identity));
    if (!foreign.isEmpty() && underSyncRoot(foreign))
      CfUnregisterSyncRoot(
          reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(foreign).utf16()));
  });
  try {
    require(fixture.isValid(), "fixture directory missing");
    root = QDir::cleanPath(fixture.filePath("root"));
    foreign = QDir::cleanPath(fixture.filePath("foreign"));
    require(QDir().mkpath(root + "/populated") && QDir().mkpath(foreign),
            "fixture roots missing");
    {
      QFile plain(root + "/plain.txt"), kept(root + "/kept.txt");
      require(plain.open(QIODevice::WriteOnly) && plain.write("plain") == 5,
              "plain fixture missing");
      require(kept.open(QIODevice::WriteOnly) && kept.write("kept") == 4,
              "kept fixture missing");
    }
    step("ownership answers (simulated Cloud Files results)");
    ownershipAnswers();

    // The registration cases need a Cloud Files that answers for this volume.
    // Where it cannot, on-demand must read as unsupported and the cases are
    // reported as skipped, never as passed.
    const auto running = OnDemandRoot::cloudFilesRunning();
    CF_SYNC_ROOT_PROVIDER_INFO probeInfo{};
    DWORD probeLength = 0;
    const auto probe = CfGetSyncRootInfoByPath(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(root).utf16()),
        CF_SYNC_ROOT_INFO_PROVIDER, &probeInfo, sizeof(probeInfo), &probeLength);
    if (probe != HRESULT_FROM_WIN32(ERROR_CLOUD_FILE_NOT_UNDER_SYNC_ROOT))
      std::fprintf(stderr, "NOTE CfGetSyncRootInfoByPath(%s) 0x%08lx, CldFlt %s\n",
                   qPrintable(QDir::toNativeSeparators(root)),
                   static_cast<unsigned long>(probe),
                   running ? "running" : "not running");
    if (OnDemandRoot::ownershipReason(probe, running, {}, false) ==
        "UNSUPPORTED_FOLDER_MODE") {
      require(OnDemandRoot::ownershipError(root, identity) ==
                  "UNSUPPORTED_FOLDER_MODE",
              "unavailable Cloud Files read as a folder owner");
      require(running || !OnDemandRoot::available(),
              "on-demand offered without the Cloud Files filter");
      std::fprintf(stderr,
                   "SKIP Cloud Files registration cases: Cloud Files cannot "
                   "answer here (CfGetSyncRootInfoByPath 0x%08lx, CldFlt %s)\n",
                   static_cast<unsigned long>(probe),
                   running ? "running" : "not running");
      return skipped;
    }
    require(OnDemandRoot::available(),
            "CFAPI plugin did not load or the Cloud Files filter is not running");

    step("ownership before registration");
    require(OnDemandRoot::ownershipError(root, identity).isEmpty(),
            "an unregistered folder was refused");
    step("a plain folder may be converted");
    require(OnDemandRoot::conversionError(root, identity).isEmpty(),
            "a plain NTFS folder was refused for conversion");
    require(!hasUserKey(classKey + OnDemandRoot::navigationClsid(identity)),
            "conversion check wrote a navigation entry");

    auto account = OCC::Account::create(false);
    const auto releaseAccount =
        qScopeGuard([&] { account->e2e()->setAccount({}); });
    account->setUrl(QUrl("https://127.0.0.1:9"));
    account->setCredentials(new OfflineCredentials);
    account->setDavUser("fixture");
    OCC::SyncJournalDb journal(fixture.filePath("journal.db"));
    {
      step("registration identity (PLAN §2.3, INV-02)");
      OnDemandRoot connected(identity, root, "/", account, &journal);
      const auto key = syncRootKey(identity);
      require(hasKey(key), "sync root not registered under this folder's ID");
      require(!hasValue(key, L"CustomStateHandler") &&
                  !hasValue(key, L"ThumbnailProvider"),
              "sync root names a shell handler that was not registered");
      step("navigation pane entry");
      navigationEntry(identity, root);
      require(OnDemandRoot::ownershipError(root, identity).isEmpty(),
              "own registration refused");
      auto other = identity;
      other.folder = QUuid::createUuid().toString(QUuid::WithoutBraces);
      require(OnDemandRoot::ownershipError(root, other) == "FOLDER_OWNED",
              "another folder ID adopted this sync root");
      require(OnDemandRoot::ownershipError(root + "/populated", identity) ==
                  "FOLDER_OWNED",
              "a folder inside the sync root was accepted");
      require(OnDemandRoot::conversionError(root + "/populated", other) ==
                  "FOLDER_OWNED",
              "a folder inside another sync root may be converted");

      step("leases admit only the own cloud root (INV-01)");
      {
        RootLease lease(root, true);
        require(lease.verify(), "on-demand lease refused its own root");
        require(lease.validateTree(), "on-demand tree refused");
      }
      require(unsupported([&] { RootLease lease(root); }) ||
                  !(attributes(root) & FILE_ATTRIBUTE_REPARSE_POINT),
              "full-file lease accepted a cloud root");

      step("placeholders are admitted only on demand");
      require(bool(OCC::CfApiWrapper::createPlaceholderInfo(
                  QDir::toNativeSeparators(root + "/online.bin"), 1700000000,
                  4096, "fixture-online.bin")),
              "placeholder not created");
      virtualRecord(journal, "online.bin", 4096);
      require(attributes(root + "/online.bin") &
                  FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS,
              "placeholder is not online-only");
      step("a placeholder is recognized by its file ID in both identity forms");
      {
        // A cycle's failed placeholder creation counts as done only when the
        // entry is this server file's placeholder: a directory listing by
        // another process had created it first (2026-11-14).
        require(connected.holds("online.bin", "fixture-online.bin"),
                "a cycle's placeholder was not recognized");
        // The listing's form: the UTF-8 file ID, its terminator and as many
        // bytes again (cfApiSendPlaceholdersTransferInfo).
        const QByteArray id = "fixture-listed.bin";
        const auto identity = id + '\0' + QByteArray(id.size() + 1, '\x5a');
        CF_PLACEHOLDER_CREATE_INFO listed{};
        listed.RelativeFileName = L"listed.bin";
        listed.FsMetadata.BasicInfo.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        listed.FsMetadata.FileSize.QuadPart = 4096;
        listed.FileIdentity = identity.constData();
        listed.FileIdentityLength = DWORD(identity.size());
        listed.Flags = CF_PLACEHOLDER_CREATE_FLAG_MARK_IN_SYNC;
        DWORD created = 0;
        require(SUCCEEDED(CfCreatePlaceholders(
                    reinterpret_cast<LPCWSTR>(
                        QDir::toNativeSeparators(root).utf16()),
                    &listed, 1, CF_CREATE_FLAG_NONE, &created)) &&
                    created == 1,
                "listing-style placeholder not created");
        require(connected.holds("listed.bin", "fixture-listed.bin"),
                "a listing's placeholder was not recognized");
        require(!connected.holds("online.bin", "fixture-listed.bin") &&
                    !connected.holds("listed.bin", "fixture-listed") &&
                    !connected.holds("online.bin", {}) &&
                    !connected.holds("plain.txt", "fixture-plain.txt") &&
                    !connected.holds("missing.bin", "fixture-missing.bin"),
                "another file, an ordinary file or a missing one was taken "
                "for the placeholder");
        require(DeleteFileW(reinterpret_cast<LPCWSTR>(
                    QDir::toNativeSeparators(root + "/listed.bin").utf16())),
                "listing-style placeholder not removed");
      }
      convertInPlace(root + "/kept.txt");
      {
        RootLease lease(root, true);
        require(lease.validateTree(), "own placeholders refused");
      }

      step("only a placeholder without its content reads as dehydrated");
      {
        // A sparse file with its content, ordinary or made an in-sync
        // placeholder, is a file: upstream once wiped a new one and stopped
        // uploading an existing one (patch 0021).
        writeSparse(root + "/sparse.bin");
        writeSparse(root + "/sparse-kept.bin");
        convertInPlace(root + "/sparse-kept.bin");
        auto &vfs = *connected.vfs();
        const struct {
          const char *name;
          bool dehydrated;
          ItemType type;
        } cases[] = {{"online.bin", true, ItemTypeVirtualFile},
                     {"kept.txt", false, ItemTypeFile},
                     {"plain.txt", false, ItemTypeFile},
                     {"sparse.bin", false, ItemTypeFile},
                     {"sparse-kept.bin", false, ItemTypeFile}};
        for (const auto &c : cases) {
          const auto path = root + '/' + c.name;
          if (vfs.isDehydratedPlaceholder(path) != c.dehydrated ||
              statVirtual(vfs, path) != c.type)
            std::fprintf(stderr, "case %s attributes 0x%08lx\n", c.name,
                         static_cast<unsigned long>(attributes(path)));
          require(vfs.isDehydratedPlaceholder(path) == c.dehydrated,
                  "dehydration misread");
          require(statVirtual(vfs, path) == c.type,
                  "discovery type misread");
        }
        // A pinned placeholder without content is one to download.
        require(bool(OCC::CfApiWrapper::createPlaceholderInfo(
                    QDir::toNativeSeparators(root + "/pinned.bin"), 1700000000,
                    4096, "fixture-pinned.bin")),
                "pinned placeholder not created");
        require(bool(OCC::CfApiWrapper::setPinState(
                    QDir::toNativeSeparators(root + "/pinned.bin"),
                    OCC::PinState::AlwaysLocal, OCC::CfApiWrapper::NoRecurse)),
                "pin not set");
        require(vfs.isDehydratedPlaceholder(root + "/pinned.bin") &&
                    statVirtual(vfs, root + "/pinned.bin") ==
                        ItemTypeVirtualFileDownload,
                "a pinned placeholder is not read for download");
        // Free up space on a hydrated sparse placeholder is a dehydration.
        require(bool(OCC::CfApiWrapper::setPinState(
                    QDir::toNativeSeparators(root + "/sparse-kept.bin"),
                    OCC::PinState::OnlineOnly, OCC::CfApiWrapper::NoRecurse)),
                "online-only pin not set");
        require(statVirtual(vfs, root + "/sparse-kept.bin") ==
                    ItemTypeVirtualFileDehydration,
                "free up space on a sparse placeholder is not a dehydration");
        require(DeleteFileW(reinterpret_cast<LPCWSTR>(
                    QDir::toNativeSeparators(root + "/pinned.bin").utf16())),
                "pinned fixture not removed");
      }

      step("a running hydration keeps the epoch busy (§2.3, INV-05)");
      QProcess reader;
      // Do not serve the control thread yet: the request stays in flight.
      startReader(reader, root + "/online.bin");
      const QDeadlineTimer arrival(15000);
      while (!connected.busy() && !arrival.hasExpired())
        QThread::msleep(5);
      require(connected.busy(), "hydration request never reached SyncD");
      const auto code = finish(reader, 60000);
      std::fprintf(stderr, "reader exit %d: %s\n", code,
                   reader.readAllStandardError().trimmed().constData());
      require(code > 0, "hydration from an unreachable server succeeded");
      require(!connected.busy(), "finished hydration kept the epoch busy");
      require(attributes(root + "/online.bin") &
                  FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS,
              "failed hydration left local content");

      step("a closed root refuses requests without network");
      connected.close();
      QProcess refused;
      startReader(refused, root + "/online.bin");
      const auto started = total.elapsed();
      require(finish(refused, 30000) > 0, "closed root served a hydration");
      require(total.elapsed() - started < 10000,
              "refusal waited for a request");
      require(!connected.busy(), "refused request kept the epoch busy");
    }

    step("reconnecting a registered root rewrites its navigation entry");
    {
      const auto clsid = OnDemandRoot::navigationClsid(identity);
      require(RegDeleteKeyValueW(
                  HKEY_CURRENT_USER,
                  reinterpret_cast<LPCWSTR>(
                      (classKey + clsid + R"(\Instance\InitPropertyBag)")
                          .utf16()),
                  L"TargetFolderPath") == ERROR_SUCCESS,
              "fixture could not change the navigation entry");
      OnDemandRoot again(identity, root, "/", account, &journal);
      navigationEntry(identity, root);
    }

    step("a foreign provider's root is never adopted (INV-01)");
    {
      const std::wstring name = L"SyncD-Contract-Foreign", version = L"1";
      CF_SYNC_REGISTRATION registration{};
      registration.StructSize = sizeof(registration);
      registration.ProviderName = name.c_str();
      registration.ProviderVersion = version.c_str();
      CF_SYNC_POLICIES policies{};
      policies.StructSize = sizeof(policies);
      policies.Hydration.Primary = CF_HYDRATION_POLICY_FULL;
      policies.Population.Primary = CF_POPULATION_POLICY_ALWAYS_FULL;
      require(SUCCEEDED(CfRegisterSyncRoot(
                  reinterpret_cast<LPCWSTR>(
                      QDir::toNativeSeparators(foreign).utf16()),
                  &registration, &policies, CF_REGISTER_FLAG_NONE)),
              "foreign fixture root not registered");
      auto stranger = identity;
      stranger.folder = QUuid::createUuid().toString(QUuid::WithoutBraces);
      require(OnDemandRoot::ownershipError(foreign, stranger) == "FOLDER_OWNED",
              "a foreign sync root was accepted");
      require(OnDemandRoot::unregister(foreign, stranger) == "FOLDER_OWNED",
              "a foreign sync root was unregistered");
      require(underSyncRoot(foreign), "foreign sync root was touched");
    }

    step("unregistration releases everything it owns");
    require(OnDemandRoot::unregister(root, identity).isEmpty(),
            "unregistration failed");
    require(!hasKey(syncRootKey(identity)), "registration key left behind");
    require(!underSyncRoot(root), "Cloud Files still knows the root");
    noNavigationEntry(OnDemandRoot::navigationClsid(identity));
    require(!QFileInfo::exists(root + "/online.bin"),
            "online-only placeholder left without a provider");
    QFile kept(root + "/kept.txt"), plain(root + "/plain.txt");
    require(
        kept.open(QIODevice::ReadOnly) && kept.readAll() == "kept" &&
            !(attributes(root + "/kept.txt") & FILE_ATTRIBUTE_REPARSE_POINT),
        "hydrated content not kept as an ordinary file");
    require(plain.open(QIODevice::ReadOnly) && plain.readAll() == "plain",
            "ordinary file changed");
    for (const auto *name : {"sparse.bin", "sparse-kept.bin"}) {
      QFile sparse(root + '/' + name);
      require(sparse.open(QIODevice::ReadOnly) &&
                  sparse.readAll() ==
                      QByteArray(524288, '\0') + QByteArray(524288, 'S') &&
                  !(attributes(root + '/' + name) &
                    FILE_ATTRIBUTE_REPARSE_POINT),
              "a sparse file was not kept as an ordinary file");
    }
    require(QFileInfo(root + "/populated").isDir(), "directory removed");
    step("removal without a registration still removes a left entry");
    {
      const auto clsid = OnDemandRoot::navigationClsid(identity);
      const DWORD one = 1;
      for (const auto &key : {classKey + clsid, namespaceKey + clsid})
        require(RegSetKeyValueW(HKEY_CURRENT_USER,
                                reinterpret_cast<LPCWSTR>(key.utf16()),
                                L"ApplicationName", REG_SZ, L"Nextcloud-MyGo",
                                sizeof(L"Nextcloud-MyGo")) == ERROR_SUCCESS,
                "fixture entry not written");
      require(RegSetKeyValueW(HKEY_CURRENT_USER,
                              reinterpret_cast<LPCWSTR>(hiddenIconsKey.utf16()),
                              reinterpret_cast<LPCWSTR>(clsid.utf16()),
                              REG_DWORD, &one, sizeof(one)) == ERROR_SUCCESS,
              "fixture desktop value not written");
    }
    require(OnDemandRoot::unregister(root, identity).isEmpty(),
            "repeated unregistration failed");
    noNavigationEntry(OnDemandRoot::navigationClsid(identity));
    {
      RootLease lease(root);
      require(lease.validateTree(), "released folder is not a full-file root");
    }
    conversionCase(fixture.path(), account);
    std::fprintf(stderr, "PASS ondemand contracts %lld ms\n",
                 static_cast<long long>(total.elapsed()));
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
  }
}
