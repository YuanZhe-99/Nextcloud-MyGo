// SPDX-License-Identifier: GPL-3.0-only
#include "scheduler.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

void Scheduler::add(const QString &folder, const QString &account) {
  if (!entries_.emplace(folder, Entry{account}).second)
    throw std::logic_error("Folder already scheduled");
}

void Scheduler::remove(const QString &folder) {
  if (active_ && active_->folder == folder)
    throw std::logic_error("Folder still running");
  entries_.erase(folder);
}

void Scheduler::enqueue(const QString &folder, Reason reason, qint64 now) {
  auto &e = entries_.at(folder);
  if (e.reasons.isEmpty()) {
    e.queuedAt = now;
    e.eligible = now;
  }
  ++e.dirty;
  e.reasons.insert(reason);
  if (reason == Reason::Local) {
    if (e.firstLocal < 0)
      e.firstLocal = now;
    // A user/recovery request remains immediate even while watcher events
    // continue.
    if (e.reasons.size() == 1)
      e.eligible = std::min(now + 2000, e.firstLocal + 10000);
  } else
    e.eligible = now;
}

void Scheduler::block(const QString &folder, const QString &reason,
                      bool blocked) {
  auto &e = entries_.at(folder);
  if (blocked)
    e.blockers.insert(reason);
  else
    e.blockers.remove(reason);
}

void Scheduler::throttleAccount(const QString &account, qint64 delayMs,
                                qint64 now) {
  const auto delay = delayMs < 0   ? std::numeric_limits<qint64>::max() - now
                     : delayMs > 0 ? delayMs
                                   : 60000;
  const auto deadline =
      now + std::min(delay, std::numeric_limits<qint64>::max() - now);
  auto &until = throttleUntil_[account];
  until = std::max(until, deadline);
}

std::optional<Scheduler::Run> Scheduler::take(qint64 now) {
  if (frozen_ || active_)
    return {};
  auto eligible = [&](const auto &pair) {
    const auto &e = pair.second;
    const auto throttle = throttleUntil_.find(e.account);
    return e.dirty > e.completed && e.blockers.isEmpty() &&
           (throttle == throttleUntil_.end() || now >= throttle->second) &&
           now >= std::max(e.eligible, e.retryAt);
  };
  const bool otherAccount =
      std::any_of(entries_.begin(), entries_.end(), [&](const auto &p) {
        return eligible(p) && p.second.account != lastAccount_;
      });
  const bool otherFolder =
      std::any_of(entries_.begin(), entries_.end(), [&](const auto &p) {
        return eligible(p) && p.first != lastFolder_ &&
               (!otherAccount || p.second.account != lastAccount_);
      });
  auto selected = entries_.end();
  qint64 best = -1;
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (!eligible(*it) ||
        (otherAccount && it->second.account == lastAccount_) ||
        (otherFolder && it->first == lastFolder_))
      continue;
    const auto &e = it->second;
    int priority = 0;
    for (const auto reason : e.reasons) {
      const int value = reason == Reason::Recovery ? 90
                        : reason == Reason::User   ? 80
                        : reason == Reason::Local  ? 60
                        : reason == Reason::Audit  ? 20
                                                   : 50;
      priority = std::max(priority, value);
    }
    const auto score =
        priority + std::clamp<qint64>((now - e.queuedAt) / 30000, 0, 40);
    if (selected == entries_.end() || score > best ||
        (score == best && e.queuedAt < selected->second.queuedAt)) {
      selected = it;
      best = score;
    }
  }
  if (selected == entries_.end())
    return {};
  auto &e = selected->second;
  active_ = Run{selected->first, e.dirty, ++serial_};
  activeReasons_ = e.reasons;
  e.reasons.clear();
  e.firstLocal = -1;
  lastAccount_ = e.account;
  lastFolder_ = selected->first;
  return active_;
}

bool Scheduler::complete(const Run &run, bool success, qint64 retryAt) {
  if (!active_ || active_->serial != run.serial ||
      active_->folder != run.folder || active_->generation != run.generation)
    return false;
  auto &e = entries_.at(run.folder);
  if (success) {
    e.completed = run.generation;
    e.retryAt = 0;
  } else {
    e.reasons.unite(activeReasons_);
    e.retryAt = retryAt;
  }
  active_.reset();
  activeReasons_.clear();
  return true;
}

bool Scheduler::pending(const QString &folder) const {
  const auto &e = entries_.at(folder);
  return e.dirty > e.completed;
}

bool Scheduler::fail(const Run &run, const QString &reason, qint64 retryAt) {
  if (!complete(run, false, retryAt))
    return false;
  if (reason == "DELETION_CONFIRMATION_REQUIRED")
    block(run.folder, "awaitingDecision", true);
  else if (reason != "NETWORK_UNAVAILABLE" && reason != "SYNC_FAILED" &&
           reason != "CANCELLED" && reason != "DISCOVERY_INCOMPLETE" &&
           reason != "LOCAL_ROOT_UNAVAILABLE" &&
           reason != "REMOTE_ROOT_UNAVAILABLE" && reason != "REMOTE_LOCKED" &&
           reason != "SERVER_THROTTLED")
    block(run.folder, reason, true);
  return true;
}
