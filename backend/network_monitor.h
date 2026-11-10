// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>

/// NetworkMonitor coalesces native notifications onto its owning Qt thread.
/// Notes: callbacks touch only an atomic bit; destruction drains native
/// registrations (INV-06).
class NetworkMonitor : public QObject {
public:
  /// NetworkMonitor registers interface/address/route/resume notifications and
  /// a DNS fallback poll.
  NetworkMonitor();
  /// ~NetworkMonitor cancels and drains callbacks before releasing their
  /// context.
  ~NetworkMonitor() override;
  /// changed invalidates policy evidence; online is only an attempt hint, never
  /// trust.
  std::function<void(bool online)> changed;

private:
  /// State owns OS handles and callback memory independently of Qt scheduling.
  struct State;
  /// poll delivers native changes and detects DNS/adapter changes without
  /// persisting network identifiers.
  void poll();
  std::unique_ptr<State> state_;
  QTimer timer_;
};
