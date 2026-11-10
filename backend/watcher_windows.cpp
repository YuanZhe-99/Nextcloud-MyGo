// SPDX-License-Identifier: GPL-3.0-only
#include "watcher.h"
#include <QDir>
#include <QWinEventNotifier>
#include <array>
#include <windows.h>

/// State keeps the overlapped storage alive until cancellation has completed.
struct RootWatcher::State {
  HANDLE directory = INVALID_HANDLE_VALUE;
  OVERLAPPED operation{};
  alignas(DWORD) std::array<char, 64 * 1024> buffer{};
  std::unique_ptr<QWinEventNotifier> notifier;
  bool pending = false;
};

RootWatcher::RootWatcher(const QString &path)
    : state_(std::make_unique<State>()) {
  auto &s = *state_;
  const auto native = QDir::toNativeSeparators(path);
  s.directory = CreateFileW(
      reinterpret_cast<LPCWSTR>(native.utf16()), FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED |
          FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (s.directory == INVALID_HANDLE_VALUE)
    return;
  s.operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!s.operation.hEvent)
    return;
  s.notifier = std::make_unique<QWinEventNotifier>(s.operation.hEvent);
  QObject::connect(s.notifier.get(), &QWinEventNotifier::activated, this,
                   [this] { ready(); });
  arm();
}

RootWatcher::~RootWatcher() {
  auto &s = *state_;
  s.notifier.reset();
  if (s.pending) {
    CancelIoEx(s.directory, &s.operation);
    DWORD bytes = 0;
    GetOverlappedResult(s.directory, &s.operation, &bytes, TRUE);
  }
  if (s.directory != INVALID_HANDLE_VALUE)
    CloseHandle(s.directory);
  if (s.operation.hEvent)
    CloseHandle(s.operation.hEvent);
}

bool RootWatcher::available() const { return state_->pending; }

bool RootWatcher::arm() {
  auto &s = *state_;
  ResetEvent(s.operation.hEvent);
  s.pending = ReadDirectoryChangesW(
      s.directory, s.buffer.data(), DWORD(s.buffer.size()), TRUE,
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
          FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SECURITY,
      nullptr, &s.operation, nullptr);
  s.notifier->setEnabled(s.pending);
  return s.pending;
}

void RootWatcher::ready() {
  auto &s = *state_;
  DWORD bytes = 0;
  const bool ok = GetOverlappedResult(s.directory, &s.operation, &bytes, FALSE);
  if (!ok && GetLastError() == ERROR_IO_INCOMPLETE)
    return;
  s.pending = false;
  const bool armed = arm();
  if (changed)
    changed(!ok || bytes == 0 || !armed);
}
