// SPDX-License-Identifier: GPL-3.0-only
// windows.h and winternl.h (NTSTATUS) must precede cfapi.h.
#include <windows.h>
#include <winternl.h>

#include <cfapi.h>

#include "account.h"
#include "common/path.h"
#include "common/syncjournaldb.h"
#include "common/vfs.h"
#include "ondemand.h"
#include "vfs/cfapi/cfapiwrapper.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QScopeGuard>
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
} // namespace

bool OnDemandRoot::available() {
  return OCC::isVfsPluginAvailable(OCC::Vfs::WindowsCfApi);
}

OnDemandRoot::OnDemandRoot(const Identity &identity, const QString &localPath,
                           const QString &remotePath,
                           const OCC::AccountPtr &account,
                           OCC::SyncJournalDb *journal) {
  if (!ownershipError(localPath, identity).isEmpty())
    throw std::runtime_error("FOLDER_OWNED");
  auto plugin = OCC::createVfsFromPlugin(OCC::Vfs::WindowsCfApi);
  if (!plugin)
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  vfs_.reset(plugin.release());
  OCC::VfsSetupParams params;
  params.filesystemPath = QDir::fromNativeSeparators(nativeRoot(localPath));
  params.rootPath = OCC::FileSystem::Path{params.filesystemPath};
  params.displayName = identity.name;
  params.alias = identity.folder;
  params.remotePath = remotePath.endsWith('/') ? remotePath : remotePath + '/';
  params.account = account;
  params.journal = journal;
  params.providerName = provider;
  params.providerVersion = SYNCD_VERSION;
  params.syncRootAccountId = identity.account;
  vfs_->start(params);
  // Upstream only logs a failed registration; require the exact record.
  if (!sameRoot(registeredRoot(syncRootId(identity, userSid()), userSid()),
                localPath)) {
    vfs_->stop();
    vfs_.reset();
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  }
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

QString OnDemandRoot::ownershipError(const QString &localPath,
                                     const Identity &identity) {
  const auto native = nativeRoot(localPath);
  CF_SYNC_ROOT_PROVIDER_INFO info{};
  DWORD length = 0;
  const auto result = CfGetSyncRootInfoByPath(
      reinterpret_cast<LPCWSTR>(native.utf16()), CF_SYNC_ROOT_INFO_PROVIDER,
      &info, sizeof(info), &length);
  if (result == HRESULT_FROM_WIN32(ERROR_CLOUD_FILE_NOT_UNDER_SYNC_ROOT))
    return {};
  if (FAILED(result) ||
      QString::fromWCharArray(info.ProviderName) != QString(provider))
    return "FOLDER_OWNED";
  // Our provider name alone is not enough: the record must be this folder's,
  // for exactly this root, not another profile's or a parent root.
  const auto sid = userSid();
  return sameRoot(registeredRoot(syncRootId(identity, sid), sid), localPath)
             ? QString{}
             : "FOLDER_OWNED";
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
    return {};
  const bool released = releaseTree(nativeRoot(localPath).chopped(1));
  // Never upstream's unregisterFolder: when no root of this provider remains
  // it deletes shell-extension classes that may belong to the official client.
  if (!OCC::CfApiWrapper::unregisterSyncRoot(recorded, provider,
                                             identity.account))
    return "UNSUPPORTED_FOLDER_MODE";
  if (!registeredRoot(syncRootId(identity, sid), sid).isEmpty())
    return "UNSUPPORTED_FOLDER_MODE";
  return released ? QString{} : "LOCAL_ROOT_UNAVAILABLE";
}
