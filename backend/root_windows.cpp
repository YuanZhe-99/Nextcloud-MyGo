// SPDX-License-Identifier: GPL-3.0-only
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "root.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <stdexcept>
#include <vector>
#include <windows.h>

namespace {
/// identityOf reads a handle's stable filesystem identity without touching file
/// content.
QString identityOf(HANDLE handle) {
  FILE_ID_INFO id{};
  if (!GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)))
    return {};
  return QString::number(id.VolumeSerialNumber, 16) + ':' +
         QByteArray(reinterpret_cast<const char *>(id.FileId.Identifier),
                    sizeof(id.FileId.Identifier))
             .toHex();
}

/// objectName binds kernel ownership to native identity rather than path
/// aliases or a profile. Global objects also cover separate sessions of the
/// same user.
QString objectName(const QString &kind, const QString &identity) {
  return "Global\\Nextcloud-MyGo-root-" + kind +
         QString::fromLatin1(QCryptographicHash::hash(
                                 identity.toUtf8(), QCryptographicHash::Sha256)
                                 .toHex());
}

/// userScope reads the process owner's SID for profile-independent ownership
/// within one Windows user, without storing account names or process IDs.
QString userScope() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    throw std::runtime_error("FOLDER_OWNED");
  const auto close = qScopeGuard([&] { CloseHandle(token); });
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> data(size);
  if (!GetTokenInformation(token, TokenUser, data.data(), size, &size))
    throw std::runtime_error("FOLDER_OWNED");
  const auto user = reinterpret_cast<const TOKEN_USER *>(data.data());
  return QString::fromLatin1(
      QByteArray(static_cast<const char *>(user->User.Sid),
                 GetLengthSid(user->User.Sid))
          .toHex());
}
} // namespace

/// Handles retains ancestor directories, shared descendant-presence events and
/// the exact-root mutex. No event payload or on-disk registry is trusted.
struct RootLease::Handles {
  std::vector<HANDLE> directories, presence;
  HANDLE mutex = nullptr;
  /// ~Handles releases partial construction and normal leases alike. Kernel
  /// handle cleanup provides the same ownership cleanup after a process crash.
  ~Handles() {
    if (mutex) {
      ReleaseMutex(mutex);
      CloseHandle(mutex);
    }
    for (auto handle : directories)
      CloseHandle(handle);
    for (auto handle : presence)
      CloseHandle(handle);
  }
};

RootLease::RootLease(const QString &path)
    : handles_(std::make_unique<Handles>()) {
  const QFileInfo info(path);
  if (!info.isAbsolute() || !info.isDir())
    throw std::runtime_error("LOCAL_ROOT_UNAVAILABLE");
  path_ = QDir::fromNativeSeparators(info.absoluteFilePath());
  if (path_.startsWith("//") || path_.size() < 3 || path_.at(1) != ':' ||
      path_.size() == 3)
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  const auto drive = QDir::toNativeSeparators(path_.left(3));
  if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(drive.utf16())) != DRIVE_FIXED)
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  QStringList identities;
  for (QString current = path_;;) {
    const auto native = QDir::toNativeSeparators(current);
    auto directory = CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()),
        FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
      throw std::runtime_error("LOCAL_ROOT_UNAVAILABLE");
    handles_->directories.push_back(directory);
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(directory, FileAttributeTagInfo,
                                      &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
      throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
    if (current == path_) {
      wchar_t filesystem[MAX_PATH + 1]{};
      if (!GetVolumeInformationByHandleW(directory, nullptr, 0, nullptr,
                                         nullptr, nullptr, filesystem,
                                         MAX_PATH + 1) ||
          QString::fromWCharArray(filesystem) != "NTFS")
        throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
    }
    const auto identity = identityOf(directory);
    if (identity.isEmpty())
      throw std::runtime_error("LOCAL_ROOT_UNAVAILABLE");
    identities.append(identity);
    if (current == path_)
      identity_ = identity;
    // Never acquire a root already covered by an existing upstream journal in
    // an ancestor.
    if (!QDir(current)
             .entryList({".sync_*.db", ".csync_journal.db"},
                        QDir::Files | QDir::Hidden | QDir::System)
             .isEmpty())
      throw std::runtime_error("FOLDER_OWNED");
    QDir parent(current);
    if (!parent.cdUp() || parent.absolutePath() == current)
      break;
    current = parent.absolutePath();
  }
  // A substituted drive can hide ancestors outside its apparent drive root.
  // Only admit drive letters whose held root is the actual NTFS volume root;
  // otherwise the hierarchical presence chain would be incomplete (S19).
  wchar_t volumeRoot[MAX_PATH + 1]{};
  const auto length = GetFinalPathNameByHandleW(
      handles_->directories.back(), volumeRoot, MAX_PATH + 1,
      FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
  if (!length || length >= MAX_PATH + 1)
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  const auto volumePath = QString::fromWCharArray(volumeRoot, length);
  if (!volumePath.startsWith("\\\\?\\Volume{") ||
      volumePath.indexOf('\\', 4) != volumePath.size() - 1)
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  // All ancestors are now held without delete sharing. Reopening the original
  // path closes the race in which an ancestor changed during handle
  // acquisition.
  if (!verify())
    throw std::runtime_error("LOCAL_ROOT_UNAVAILABLE");

  // Serialize admission only, never a sync cycle. Presence on a common ancestor
  // is shared; exclusively locking ancestors would incorrectly reject siblings.
  const auto user = userScope();
  const auto admissionName = objectName("admission-", user);
  auto admission = CreateMutexW(
      nullptr, FALSE, reinterpret_cast<LPCWSTR>(admissionName.utf16()));
  if (!admission)
    throw std::runtime_error("FOLDER_OWNED");
  bool acquired = false;
  const auto unlock = qScopeGuard([&] {
    if (acquired)
      ReleaseMutex(admission);
    CloseHandle(admission);
  });
  const auto wait = WaitForSingleObject(admission, 10000);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    throw std::runtime_error("FOLDER_OWNED");
  acquired = true;
  for (qsizetype i = 0; i < identities.size(); ++i) {
    // An exact-root mutex may exist only for an admitted root. Keeping the old
    // name preserves same-root exclusion with the previous implementation too.
    const auto exactName = objectName({}, identities.at(i));
    auto exact = CreateMutexW(nullptr, FALSE,
                              reinterpret_cast<LPCWSTR>(exactName.utf16()));
    const auto exactError = GetLastError();
    if (!exact || exactError == ERROR_ALREADY_EXISTS) {
      if (exact)
        CloseHandle(exact);
      throw std::runtime_error("FOLDER_OWNED");
    }
    if (i == 0) {
      const auto owned = WaitForSingleObject(exact, 0);
      if (owned != WAIT_OBJECT_0) {
        CloseHandle(exact);
        throw std::runtime_error("FOLDER_OWNED");
      }
      handles_->mutex = exact;
    } else {
      CloseHandle(exact);
    }
    const auto presenceName =
        objectName("tree-", user + ':' + identities.at(i));
    auto presence = CreateEventW(
        nullptr, TRUE, FALSE, reinterpret_cast<LPCWSTR>(presenceName.utf16()));
    const auto presenceError = GetLastError();
    if (!presence)
      throw std::runtime_error("FOLDER_OWNED");
    handles_->presence.push_back(presence);
    // Existing presence on this root means an equal or descendant lease. An
    // ancestor's presence is deliberately shared by unrelated sibling leases.
    if (i == 0 && presenceError == ERROR_ALREADY_EXISTS)
      throw std::runtime_error("FOLDER_OWNED");
  }
}

RootLease::~RootLease() = default;

bool RootLease::verify() const {
  if (!handles_ || handles_->directories.empty() ||
      identityOf(handles_->directories.front()) != identity_)
    return false;
  for (auto directory : handles_->directories) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(directory, FileAttributeTagInfo,
                                      &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
      return false;
  }
  const auto native = QDir::toNativeSeparators(path_);
  auto probe = CreateFileW(
      reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (probe == INVALID_HANDLE_VALUE)
    return false;
  const bool same = identityOf(probe) == identity_;
  CloseHandle(probe);
  return same;
}

bool RootLease::validateTree() const {
  if (!verify())
    return false;
  QStringList pending{path_};
  while (!pending.isEmpty()) {
    const auto path = QDir::toNativeSeparators(pending.takeLast());
    const auto attributes =
        GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      return false;
    const auto pattern = path + "\\*";
    WIN32_FIND_DATAW entry{};
    auto search =
        FindFirstFileW(reinterpret_cast<LPCWSTR>(pattern.utf16()), &entry);
    if (search == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_NOT_FOUND)
        continue;
      return false;
    }
    bool valid = true;
    do {
      const auto name = QString::fromWCharArray(entry.cFileName);
      if (name == "." || name == "..")
        continue;
      if ((name.startsWith(".sync_") && name.endsWith(".db")) ||
          name == ".csync_journal.db") {
        valid = false;
        break;
      }
      if (entry.dwFileAttributes &
          (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
           FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) {
        valid = false;
        break;
      }
      if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        pending.append(path + '\\' + name);
    } while (FindNextFileW(search, &entry));
    if (valid && GetLastError() != ERROR_NO_MORE_FILES)
      valid = false;
    FindClose(search);
    if (!valid)
      return false;
  }
  return verify();
}
