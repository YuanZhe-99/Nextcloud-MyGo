// SPDX-License-Identifier: GPL-3.0-only
// windows.h and winternl.h (NTSTATUS) must precede cfapi.h.
#include <windows.h>
#include <winternl.h>

#include <cfapi.h>

#include "account.h"
#include "common/filesystembase.h"
#include "common/path.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "common/vfs.h"
#include "ondemand.h"
#include "syncfileitem.h"
#include "vfs/cfapi/cfapiwrapper.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QUuid>
#include <QVariant>
#include <sddl.h>
#include <stdexcept>
#include <vector>

namespace {
constexpr auto syncRoots =
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager)";

/// nativeRoot is the root as upstream registers it: native separators and one
/// trailing backslash.
QString nativeRoot(const QString &path) {
  auto native = QDir::toNativeSeparators(QDir::cleanPath(path));
  if (!native.endsWith('\\'))
    native += '\\';
  return native;
}

/// sameRoot compares registered and leased roots case-insensitively, ignoring
/// trailing separators (NTFS names are case-insensitive for this user).
bool sameRoot(const QString &a, const QString &b) {
  return nativeRoot(a).compare(nativeRoot(b), Qt::CaseInsensitive) == 0;
}

/// userSid reads the process owner's string SID, the second part of every sync
/// root ID of this user.
QString userSid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  const auto close = qScopeGuard([&] { CloseHandle(token); });
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> data(size);
  LPWSTR text = nullptr;
  if (!GetTokenInformation(token, TokenUser, data.data(), size, &size) ||
      !ConvertSidToStringSidW(
          reinterpret_cast<const TOKEN_USER *>(data.data())->User.Sid, &text))
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  const auto sid = QString::fromWCharArray(text);
  LocalFree(text);
  return sid;
}

/// registeredRoot reads the local root recorded for one sync root ID.
/// Returns: empty when the ID is not registered.
QString registeredRoot(const QString &id, const QString &sid) {
  const auto key =
      QString::fromWCharArray(syncRoots) + '\\' + id + "\\UserSyncRoots";
  wchar_t value[MAX_PATH * 4]{};
  DWORD bytes = sizeof(value) - sizeof(wchar_t);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, reinterpret_cast<LPCWSTR>(key.utf16()),
                   reinterpret_cast<LPCWSTR>(sid.utf16()), RRF_RT_REG_SZ,
                   nullptr, value, &bytes) != ERROR_SUCCESS)
    return {};
  return QString::fromWCharArray(value);
}

/// syncRootId names this folder's registration (upstream's provider!SID!
/// account!alias format).
QString syncRootId(const OnDemandRoot::Identity &identity, const QString &sid) {
  return QString(OnDemandRoot::provider) + '!' + sid + '!' + identity.account +
         '!' + identity.folder;
}

/// placeholderState reads the Cloud Files state of a directory entry without
/// opening it, so no hydration or population is requested.
CF_PLACEHOLDER_STATE placeholderState(const WIN32_FIND_DATAW &entry) {
  return CfGetPlaceholderStateFromFindData(&entry);
}

/// revertPlaceholder turns a placeholder whose content is on disk into an
/// ordinary file or directory.
bool revertPlaceholder(const QString &path) {
  auto handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(path.utf16()),
      FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return false;
  const auto result = CfRevertPlaceholder(handle, CF_REVERT_FLAG_NONE, nullptr);
  CloseHandle(handle);
  return SUCCEEDED(result);
}

/// releaseTree reverts hydrated placeholders below root and deletes
/// online-only ones; it never deletes an entry whose content is on disk.
/// Returns: false when an entry could not be released.
bool releaseTree(const QString &root) {
  bool released = true;
  QStringList pending{root}, directories;
  while (!pending.isEmpty()) {
    const auto directory = pending.takeLast();
    directories.append(directory);
    const auto pattern = directory + "\\*";
    WIN32_FIND_DATAW entry{};
    auto search = FindFirstFileExW(reinterpret_cast<LPCWSTR>(pattern.utf16()),
                                   FindExInfoBasic, &entry,
                                   FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE) {
      if (GetLastError() != ERROR_FILE_NOT_FOUND)
        released = false;
      continue;
    }
    do {
      const auto name = QString::fromWCharArray(entry.cFileName);
      if (name == "." || name == "..")
        continue;
      const auto path = directory + '\\' + name;
      const auto state = placeholderState(entry);
      const bool placeholder = state != CF_PLACEHOLDER_STATE_INVALID &&
                               (state & CF_PLACEHOLDER_STATE_PLACEHOLDER);
      const bool directoryEntry =
          entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
      const bool online = state & (CF_PLACEHOLDER_STATE_PARTIAL |
                                   CF_PLACEHOLDER_STATE_PARTIALLY_ON_DISK);
      if (directoryEntry) {
        // An unpopulated directory has no local children to keep; it is
        // removed below only if it really is empty.
        if (!placeholder || !online)
          pending.append(path);
        else
          directories.append(path);
      } else if (placeholder && online) {
        if (!DeleteFileW(reinterpret_cast<LPCWSTR>(path.utf16())))
          released = false;
      } else if (placeholder && !revertPlaceholder(path)) {
        released = false;
      }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
  }
  // Children before parents; the root's own state ends with unregistration.
  for (auto it = directories.crbegin(); it != directories.crend(); ++it) {
    if (*it == root)
      continue;
    WIN32_FIND_DATAW entry{};
    auto search = FindFirstFileExW(reinterpret_cast<LPCWSTR>(it->utf16()),
                                   FindExInfoBasic, &entry,
                                   FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE)
      continue;
    FindClose(search);
    const auto state = placeholderState(entry);
    if (state == CF_PLACEHOLDER_STATE_INVALID ||
        !(state & CF_PLACEHOLDER_STATE_PLACEHOLDER))
      continue;
    if (state & (CF_PLACEHOLDER_STATE_PARTIAL |
                 CF_PLACEHOLDER_STATE_PARTIALLY_ON_DISK)) {
      if (!RemoveDirectoryW(reinterpret_cast<LPCWSTR>(it->utf16())) &&
          !revertPlaceholder(*it))
        released = false;
    } else if (!revertPlaceholder(*it)) {
      released = false;
    }
  }
  return released;
}

constexpr auto classesKey = LR"(Software\Classes\CLSID\)";
constexpr auto classesWow64Key = LR"(Software\Classes\Wow6432Node\CLSID\)";
constexpr auto namespaceKey =
    LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\)";
constexpr auto hiddenIconsKey =
    LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel)";
/// applicationName tags our navigation entries, as upstream tags its own, so
/// an uninstaller can find what is left (never the official client's name).
constexpr auto applicationName = L"Nextcloud-MyGo";

/// setValue writes one HKCU value, creating its key.
/// Returns: false when the value was not written.
bool setValue(const QString &key, const wchar_t *name, DWORD type,
              const QVariant &value) {
  LSTATUS result;
  if (type == REG_DWORD) {
    const DWORD number = value.toUInt();
    result = RegSetKeyValueW(HKEY_CURRENT_USER,
                             reinterpret_cast<LPCWSTR>(key.utf16()), name, type,
                             &number, sizeof(number));
  } else {
    const auto text = value.toString();
    result = RegSetKeyValueW(
        HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(key.utf16()), name, type,
        text.utf16(), DWORD((text.size() + 1) * sizeof(wchar_t)));
  }
  return result == ERROR_SUCCESS;
}

/// removeNavigationEntry deletes the Explorer navigation pane entry of a CLSID
/// (upstream's NavigationPaneHelper removal, HKCU only).
/// Returns: false when a part that exists could not be removed.
/// Notes: an empty CLSID names no entry; it must never reach RegDeleteTreeW,
/// which would then delete every class of this user.
bool removeNavigationEntry(const QString &clsid) {
  if (clsid.isEmpty())
    return true;
  bool removed = true;
  for (const auto *prefix : {classesKey, classesWow64Key, namespaceKey}) {
    const auto key = QString::fromWCharArray(prefix) + clsid;
    const auto result = RegDeleteTreeW(HKEY_CURRENT_USER,
                                       reinterpret_cast<LPCWSTR>(key.utf16()));
    removed &= result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
  }
  const auto result = RegDeleteKeyValueW(
      HKEY_CURRENT_USER, hiddenIconsKey,
      reinterpret_cast<LPCWSTR>(clsid.utf16()));
  return removed && (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND);
}

/// writeNavigationEntry pins a sync root to the Explorer navigation pane, as
/// upstream's NavigationPaneHelper does (Microsoft's "integrate a cloud storage
/// provider" steps), for the current user only.
/// Inputs: clsid in registry form ("{…}"); title as Explorer shows it;
/// targetFolderPath the root; iconPath the executable whose first icon is used.
/// Side effects: writes HKCU keys; the in-process server is the system's
/// shell32.dll, so no module of ours is loaded into Explorer.
/// Returns: false, with any partial entry removed, when a value failed.
bool writeNavigationEntry(const QString &clsid, const QString &title,
                          const QString &targetFolderPath,
                          const QString &iconPath) {
  struct Value {
    QString key;
    const wchar_t *name;
    DWORD type;
    QVariant value;
  };
  QList<Value> values;
  for (const auto *prefix : {classesKey, classesWow64Key}) {
    const auto key = QString::fromWCharArray(prefix) + clsid;
    values += {
        {key, nullptr, REG_SZ, title},
        {key + "\\DefaultIcon", nullptr, REG_SZ, iconPath},
        {key, L"System.IsPinnedToNameSpaceTree", REG_DWORD, 0x1},
        {key, L"SortOrderIndex", REG_DWORD, 0x41},
        {key + "\\InProcServer32", nullptr, REG_EXPAND_SZ,
         QStringLiteral(R"(%systemroot%\system32\shell32.dll)")},
        {key + "\\Instance", L"CLSID", REG_SZ,
         QStringLiteral("{0E5AAE11-A475-4c5b-AB00-C66DE400274E}")},
        {key + "\\Instance\\InitPropertyBag", L"Attributes", REG_DWORD, 0x11},
        {key + "\\Instance\\InitPropertyBag", L"TargetFolderPath", REG_SZ,
         targetFolderPath},
        {key + "\\ShellFolder", L"FolderValueFlags", REG_DWORD, 0x28},
        {key + "\\ShellFolder", L"Attributes", REG_DWORD, 0xF080004Du},
        {key, L"ApplicationName", REG_SZ, QString::fromWCharArray(applicationName)},
    };
  }
  const auto space = QString::fromWCharArray(namespaceKey) + clsid;
  values += {
      {space, nullptr, REG_SZ, title},
      {QString::fromWCharArray(hiddenIconsKey),
       reinterpret_cast<const wchar_t *>(clsid.utf16()), REG_DWORD, 0x1},
      {space, L"ApplicationName", REG_SZ,
       QString::fromWCharArray(applicationName)},
  };
  for (const auto &value : std::as_const(values))
    if (!setValue(value.key, value.name, value.type, value.value)) {
      removeNavigationEntry(clsid);
      return false;
    }
  return true;
}

/// unixTime converts a FILETIME as upstream's local discovery does
/// (csync_vio_local_win.cpp, rounding toward negative infinity), so a size
/// and time comparison with the journal agrees with what the next cycle sees.
qint64 unixTime(const FILETIME &time) {
  auto t = (qint64(time.dwHighDateTime) << 32) + qint64(time.dwLowDateTime) -
           116444736000000000LL;
  return t < 0 ? -1 - (-t - 1) / 10000000 : t / 10000000;
}
} // namespace

bool OnDemandRoot::available() {
  static const bool value = cloudFilesRunning() &&
                            OCC::isVfsPluginAvailable(OCC::Vfs::WindowsCfApi);
  return value;
}

bool OnDemandRoot::cloudFilesRunning() {
  const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager)
    return false;
  const auto closeManager = qScopeGuard([&] { CloseServiceHandle(manager); });
  const auto service = OpenServiceW(manager, L"CldFlt", SERVICE_QUERY_STATUS);
  if (!service)
    return false;
  const auto closeService = qScopeGuard([&] { CloseServiceHandle(service); });
  SERVICE_STATUS status{};
  return QueryServiceStatus(service, &status) &&
         status.dwCurrentState == SERVICE_RUNNING;
}

QString OnDemandRoot::ownershipReason(long queried, bool running,
                                      const QString &provider,
                                      bool ownRecord) {
  if (queried == HRESULT_FROM_WIN32(ERROR_CLOUD_FILE_NOT_UNDER_SYNC_ROOT))
    return {};
  if (SUCCEEDED(queried))
    return provider == QString(OnDemandRoot::provider) && ownRecord
               ? QString{}
               : "FOLDER_OWNED";
  for (const DWORD unavailable :
       {DWORD(ERROR_CLOUD_FILE_PROVIDER_NOT_RUNNING),
        DWORD(ERROR_CLOUD_FILE_NOT_SUPPORTED), DWORD(ERROR_NOT_SUPPORTED),
        DWORD(ERROR_INVALID_FUNCTION)})
    if (queried == HRESULT_FROM_WIN32(unavailable))
      return "UNSUPPORTED_FOLDER_MODE";
  return running ? "FOLDER_OWNED" : "UNSUPPORTED_FOLDER_MODE";
}

OnDemandRoot::OnDemandRoot(const Identity &identity, const QString &localPath,
                           const QString &remotePath,
                           const OCC::AccountPtr &account,
                           OCC::SyncJournalDb *journal) {
  if (const auto reason = ownershipError(localPath, identity); !reason.isEmpty())
    throw std::runtime_error(reason.toStdString());
  auto plugin = OCC::createVfsFromPlugin(OCC::Vfs::WindowsCfApi);
  if (!plugin)
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  vfs_.reset(plugin.release());
  OCC::VfsSetupParams params;
  params.filesystemPath = QDir::fromNativeSeparators(nativeRoot(localPath));
  root_ = params.filesystemPath;
  params.rootPath = OCC::FileSystem::Path{params.filesystemPath};
  params.displayName = identity.name;
  params.alias = identity.folder;
  params.remotePath = remotePath.endsWith('/') ? remotePath : remotePath + '/';
  params.account = account;
  params.journal = journal;
  params.providerName = provider;
  params.providerVersion = SYNCD_VERSION;
  params.syncRootAccountId = identity.account;
  // The entry is rewritten on every start: its icon is the running SyncD,
  // which lives in a versioned install directory. A missing entry only costs
  // the sidebar, so registration goes on without it.
  const auto sid = userSid();
  const bool registered =
      !registeredRoot(syncRootId(identity, sid), sid).isEmpty();
  const auto clsid = navigationClsid(identity);
  if (!clsid.isEmpty() &&
      writeNavigationEntry(
          clsid, identity.name,
          QDir::toNativeSeparators(QDir::cleanPath(localPath)),
          QDir::toNativeSeparators(QCoreApplication::applicationFilePath())))
    params.navigationPaneClsid = clsid;
  // A folder that was never registered keeps no entry when this fails.
  auto forget = qScopeGuard([&] {
    if (!registered && !params.navigationPaneClsid.isEmpty())
      removeNavigationEntry(clsid);
  });
  vfs_->start(params);
  // Upstream only logs a failed registration; require the exact record.
  if (!sameRoot(registeredRoot(syncRootId(identity, sid), sid), localPath)) {
    vfs_->stop();
    vfs_.reset();
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  }
  forget.dismiss();
}

QString OnDemandRoot::navigationClsid(const Identity &identity) {
  const QUuid id(identity.folder);
  return id.isNull() ? QString{} : id.toString(QUuid::WithBraces);
}

QString OnDemandRoot::conversionError(const QString &localPath,
                                      const Identity &identity) {
  if (!available() ||
      !OCC::Vfs::checkAvailability(localPath, OCC::Vfs::WindowsCfApi).isValid())
    return "UNSUPPORTED_FOLDER_MODE";
  return ownershipError(localPath, identity);
}

OnDemandRoot::~OnDemandRoot() {
  if (!vfs_)
    return;
  close();
  // A Cloud Files callback may be waiting for this thread; serve it rather
  // than disconnecting underneath it.
  const QDeadlineTimer deadline(60000);
  while (busy() && !deadline.hasExpired())
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  if (busy())
    qFatal("On-demand requests failed to drain before disconnect");
  vfs_->stop();
}

void OnDemandRoot::close() {
  closed_ = true;
  vfs_->refuseNewRequests();
}

bool OnDemandRoot::busy() const {
  return vfs_->isHydrating() || vfs_->hasRequestsInFlight();
}

bool OnDemandRoot::holds(const QString &relativePath,
                         const QByteArray &fileId) const {
  if (fileId.isEmpty() || relativePath.isEmpty())
    return false;
  const auto path = QDir::toNativeSeparators(root_ + relativePath);
  auto handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return false;
  std::vector<char> buffer(sizeof(CF_PLACEHOLDER_STANDARD_INFO) + 4096);
  DWORD length = 0;
  const auto result =
      CfGetPlaceholderInfo(handle, CF_PLACEHOLDER_INFO_STANDARD, buffer.data(),
                           DWORD(buffer.size()), &length);
  CloseHandle(handle);
  if (FAILED(result))
    return false;
  const auto *info =
      reinterpret_cast<const CF_PLACEHOLDER_STANDARD_INFO *>(buffer.data());
  const QByteArray identity(reinterpret_cast<const char *>(info->FileIdentity),
                            int(info->FileIdentityLength));
  // The listing's length counts UTF-16 units of the UTF-8 bytes it stores
  // (cfApiSendPlaceholdersTransferInfo): the file ID, its terminator, then as
  // many bytes again from past its end, which say nothing.
  return identity == fileId ||
         (identity.size() == 2 * (fileId.size() + 1) &&
          identity.startsWith(fileId + '\0'));
}

OnDemandRoot::Conversion
OnDemandRoot::convertExisting(OCC::SyncJournalDb &journal,
                              const QDeadlineTimer &budget) {
  struct Record {
    QString path;
    QByteArray fileId;
    ItemType type;
    qint64 modtime, size;
  };
  // Collected first: the journal holds its lock while it calls back.
  std::vector<Record> records;
  if (!journal.getFilesBelowPath(
          {}, [&](const OCC::SyncJournalFileRecord &record) {
            records.push_back({record.path(), record._fileId, record._type,
                               qint64(record._modtime), record._fileSize});
          }))
    throw std::runtime_error("JOURNAL_ERROR");
  Conversion done;
  for (const auto &record : records) {
    const bool directory = record.type == ItemTypeDirectory;
    if ((!directory && record.type != ItemTypeFile) || record.fileId.isEmpty() ||
        OCC::FileSystem::isExcludeFile(QFileInfo(record.path).fileName())) {
      ++done.skipped;
      continue;
    }
    if (!directory && budget.hasExpired()) {
      ++done.deferred;
      continue;
    }
    const auto local = root_ + record.path;
    // One directory lookup gives type, size, time and Cloud Files state
    // without opening the entry.
    const auto pattern =
        OCC::FileSystem::longWinPath(QDir::toNativeSeparators(local));
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileExW(
        reinterpret_cast<LPCWSTR>(pattern.utf16()), FindExInfoBasic, &entry,
        FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE) {
      ++done.skipped;
      continue;
    }
    FindClose(search);
    const auto state = placeholderState(entry);
    if (state != CF_PLACEHOLDER_STATE_INVALID &&
        (state & CF_PLACEHOLDER_STATE_PLACEHOLDER)) {
      ++done.present;
      continue;
    }
    const bool isDirectory = entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    const auto size =
        (qint64(entry.nFileSizeHigh) << 32) + qint64(entry.nFileSizeLow);
    // A sparse file is converted like any other: it keeps its content and
    // reads as hydrated (patch 0021).
    if (isDirectory != directory ||
        (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (!directory && (size != record.size ||
                        unixTime(entry.ftLastWriteTime) != record.modtime))) {
      ++done.skipped;
      continue;
    }
    // The journal's identity and type: an empty item (upstream's
    // switchToVirtualFiles) gives a directory a file's flags and every entry
    // an empty identity. The entry as its own replacement keeps its pin state.
    OCC::SyncFileItem item;
    item._file = record.path;
    item._type = record.type;
    item._fileId = record.fileId;
    const auto result = vfs_->convertToPlaceholder(
        local, item, local, OCC::Vfs::UpdateMetadataType::DatabaseMetadata);
    if (result && *result == OCC::Vfs::ConvertToPlaceholderResult::Ok)
      ++done.converted;
    else
      ++done.failed;
  }
  return done;
}

QString OnDemandRoot::ownershipError(const QString &localPath,
                                     const Identity &identity) {
  const auto native = nativeRoot(localPath);
  CF_SYNC_ROOT_PROVIDER_INFO info{};
  DWORD length = 0;
  const auto result = CfGetSyncRootInfoByPath(
      reinterpret_cast<LPCWSTR>(native.utf16()), CF_SYNC_ROOT_INFO_PROVIDER,
      &info, sizeof(info), &length);
  if (FAILED(result))
    return ownershipReason(result, cloudFilesRunning(), {}, false);
  // Our provider name alone is not enough: the record must be this folder's,
  // for exactly this root, not another profile's or a parent root.
  const auto name = QString::fromWCharArray(info.ProviderName);
  const auto sid = userSid();
  return ownershipReason(
      result, true, name,
      name == QString(provider) &&
          sameRoot(registeredRoot(syncRootId(identity, sid), sid), localPath));
}

QString OnDemandRoot::unregister(const QString &localPath,
                                 const Identity &identity) {
  const auto sid = userSid();
  const auto recorded = registeredRoot(syncRootId(identity, sid), sid);
  if (!recorded.isEmpty() && !sameRoot(recorded, localPath))
    return "FOLDER_OWNED";
  const auto owner = ownershipError(localPath, identity);
  if (!owner.isEmpty())
    return owner;
  // Ownership passed without a record: the folder is under no sync root, so
  // nothing of ours is left to release (removal is idempotent).
  if (recorded.isEmpty())
    return removeNavigationEntry(navigationClsid(identity))
               ? QString{}
               : "UNSUPPORTED_FOLDER_MODE";
  const bool released = releaseTree(nativeRoot(localPath).chopped(1));
  // Never upstream's unregisterFolder: when no root of this provider remains
  // it deletes shell-extension classes that may belong to the official client.
  if (!OCC::CfApiWrapper::unregisterSyncRoot(recorded, provider,
                                             identity.account))
    return "UNSUPPORTED_FOLDER_MODE";
  if (!registeredRoot(syncRootId(identity, sid), sid).isEmpty())
    return "UNSUPPORTED_FOLDER_MODE";
  if (!removeNavigationEntry(navigationClsid(identity)))
    return "UNSUPPORTED_FOLDER_MODE";
  return released ? QString{} : "LOCAL_ROOT_UNAVAILABLE";
}
