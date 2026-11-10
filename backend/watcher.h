// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

/// RootWatcher marks recursive local changes without interpreting them as sync
/// operations (INV-07/09). All callbacks run on the owning control thread.
class RootWatcher final : public QObject {
public:
  /// RootWatcher subscribes to an already leased local root; unavailable
  /// notifications fall back to the runtime's periodic discovery.
  explicit RootWatcher(const QString &path);
  /// ~RootWatcher cancels and drains overlapped I/O before freeing its buffer.
  ~RootWatcher() override;
  /// available reports whether a subtree notification request is armed.
  bool available() const;
  /// changed requests ordinary dirty work or a full audit after notification
  /// loss.
  std::function<void(bool audit)> changed;

private:
  /// arm reissues one bounded recursive request without retaining file names.
  bool arm();
  /// ready consumes the signalled request and rearms before notifying the
  /// owner.
  void ready();
  /// State owns the Windows handles, notification buffer and event-loop
  /// notifier.
  struct State;
  std::unique_ptr<State> state_;
};
