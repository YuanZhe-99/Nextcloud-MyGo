// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QSet>
#include <QString>
#include <map>
#include <optional>

/// Scheduler merges folder work and admits one control-thread cycle at a time
/// (PLAN §10). Inputs: monotonic milliseconds supplied by the runtime or a
/// deterministic test clock.
class Scheduler {
public:
  enum class Reason { Recovery, User, Local, Remote, FollowUp, Audit };
  /// Run captures the exact generation acknowledged by successful engine
  /// completion.
  struct Run {
    QString folder;
    quint64 generation;
    quint64 serial;
  };
  /// add registers a stable folder identity; duplicate registration is an
  /// internal error.
  void add(const QString &folder, const QString &account);
  /// remove forgets only an idle mapping; it never tears down a running engine.
  void remove(const QString &folder);
  /// enqueue coalesces reasons and retains changes arriving during a cycle.
  void enqueue(const QString &folder, Reason reason, qint64 now);
  /// block changes only the named blocker; pausing does not discard pending
  /// work.
  void block(const QString &folder, const QString &reason, bool blocked);
  /// throttleAccount extends an account-wide monotonic server deadline without
  /// clearing pending work or pauses; new folders inherit it (PLAN §12.1, N18).
  /// Notes: zero uses a finite 60-second default; negative rejects an oversized
  /// server deadline without shortening it.
  void throttleAccount(const QString &account, qint64 delayMs, qint64 now);
  /// freeze prevents admission during global pause or shutdown without changing
  /// folder blockers.
  void freeze(bool value) { frozen_ = value; }
  /// take admits one eligible folder, rotating accounts and folders before
  /// comparing priority.
  std::optional<Run> take(qint64 now);
  /// complete accepts only the active run, preserving newer changes and all
  /// failed work.
  bool complete(const Run &run, bool success, qint64 retryAt);
  /// fail retains failed work for retry, or adds an independent safety/decision
  /// blocker (INV-10/13/14). Notes: stale completions cannot add blockers to a
  /// newer cycle.
  bool fail(const Run &run, const QString &reason, qint64 retryAt);
  /// pending reports unfinished generations, including a currently running
  /// generation.
  bool pending(const QString &folder) const;
  /// blockers exposes independent sanitized admission reasons only to
  /// diagnostics.
  QSet<QString> blockers(const QString &folder) const {
    return entries_.at(folder).blockers;
  }

private:
  /// Entry stores scheduling metadata only; durable identity and journals
  /// belong to Control/engine.
  struct Entry {
    QString account;
    QSet<QString> blockers;
    QSet<Reason> reasons;
    quint64 dirty = 0, completed = 0;
    qint64 queuedAt = 0, eligible = 0, firstLocal = -1, retryAt = 0;
  };
  std::map<QString, Entry> entries_;
  std::map<QString, qint64> throttleUntil_;
  std::optional<Run> active_;
  QSet<Reason> activeReasons_;
  QString lastAccount_, lastFolder_;
  quint64 serial_ = 0;
  bool frozen_ = false;
};
