// SPDX-License-Identifier: GPL-3.0-only
#include "engine_runtime.h"
#include "namespace.h"
#include <QPointer>
#include <QRandomGenerator>
#include <algorithm>
#include <limits>
#include <stdexcept>

EngineRuntime::EngineRuntime() {
  clock_.start();
  timer_.setInterval(250);
  QObject::connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
  timer_.start();
}

EngineRuntime::~EngineRuntime() {
  if (!idle())
    qFatal("Engine runtime destroyed before shutdown completed");
}

void EngineRuntime::add(const QString &folder, const QString &account,
                        const EngineAdapter::Binding &binding) {
  if (stopping_ || folders_.contains(folder))
    throw std::logic_error("Cannot register folder runtime");
  auto bound = binding;
  bound.networkGeneration = networkGeneration_;
  auto adapter = std::make_unique<EngineAdapter>(bound);
  adapter->constrained = [owner = QPointer(this),
                          account](const QString &reason, qint64 delayMs) {
    if (owner && reason == "SERVER_THROTTLED")
      owner->throttleObserved(account, delayMs);
  };
  adapter->completed = [this, folder](const EngineAdapter::Result &result) {
    if (!active_ || active_->folder != folder)
      qFatal("Engine completion does not match active generation");
    auto &entry = folders_.at(folder);
    const auto now = clock_.elapsed();
    entry.failures = result.success ? 0 : std::min(entry.failures + 1, 8u);
    auto retryAt = now + std::min<qint64>(300000, 1000LL << entry.failures);
    if (result.reason == "REMOTE_LOCKED" || result.reason == "SERVER_THROTTLED")
      retryAt =
          result.retryAfterMs < 0
              ? std::numeric_limits<qint64>::max()
              : std::max(
                    retryAt,
                    now + std::min(result.retryAfterMs,
                                   std::numeric_limits<qint64>::max() - now));
    if (result.reason == "SERVER_THROTTLED" && result.retryAfterMs != 0)
      throttleObserved(entry.account, result.retryAfterMs);
    if (result.success)
      scheduler_.complete(*active_, true, retryAt);
    else
      scheduler_.fail(*active_, result.reason, retryAt);
    active_.reset();
    if (result.followUp)
      scheduler_.enqueue(folder, Scheduler::Reason::FollowUp, now);
    const bool completingShutdown = stopping_;
    if (cycle)
      cycle(folder, result);
    if (completingShutdown)
      finishShutdown();
  };
  const auto now = clock_.elapsed();
  auto watcher = std::make_unique<RootWatcher>(binding.localPath);
  watcher->changed = [this, folder](bool audit) {
    wake(folder, audit ? Scheduler::Reason::Audit : Scheduler::Reason::Local);
  };
  scheduler_.add(folder, account);
  folders_.emplace(folder,
                   Folder{account, std::move(adapter), std::move(watcher),
                          now + 60000, now + 86400000});
  scheduler_.enqueue(folder, Scheduler::Reason::Recovery, now);
}

bool EngineRuntime::remove(const QString &folder) {
  if (active_ && active_->folder == folder)
    return false;
  scheduler_.remove(folder);
  return folders_.erase(folder) != 0;
}

void EngineRuntime::wake(const QString &folder, Scheduler::Reason reason) {
  if (!stopping_) {
    if (reason == Scheduler::Reason::User)
      scheduler_.block(folder, "PERMISSION_DENIED", false);
    scheduler_.enqueue(folder, reason, clock_.elapsed());
  }
}

void EngineRuntime::block(const QString &folder, const QString &reason,
                          bool blocked) {
  scheduler_.block(folder, reason, blocked);
  if (blocked && active_ && active_->folder == folder)
    folders_.at(folder).adapter->stop();
}

void EngineRuntime::pause(bool value) {
  if (stopping_)
    return;
  scheduler_.freeze(value);
  if (value && active_)
    folders_.at(active_->folder).adapter->stop();
}

void EngineRuntime::authorize(const QString &folder, const QString &digest,
                              qint64 remainingMilliseconds) {
  folders_.at(folder).adapter->authorize(digest, remainingMilliseconds);
  scheduler_.block(folder, "awaitingDecision", false);
  scheduler_.enqueue(folder, Scheduler::Reason::User, clock_.elapsed());
}

void EngineRuntime::shutdown() {
  if (stopping_)
    return;
  stopping_ = true;
  shutdownStarted_ = clock_.elapsed();
  scheduler_.freeze(true);
  timer_.stop();
  if (active_)
    folders_.at(active_->folder).adapter->stop();
  else
    finishShutdown();
}

void EngineRuntime::finishShutdown() {
  if (shutdownFinished_ || active_)
    return;
  bool drained = true;
  for (auto &[id, entry] : folders_) {
    entry.watcher.reset();
    drained = entry.adapter->retire().networkDrained && drained;
  }
  if (!drained) {
    if (clock_.elapsed() - shutdownStarted_ >= 120000)
      qFatal("Network context failed to drain during bounded shutdown");
    QTimer::singleShot(10, this, [this] { finishShutdown(); });
    return;
  }
  shutdownFinished_ = true;
  if (stopped)
    stopped();
}

bool EngineRuntime::idle() const { return !active_; }

bool EngineRuntime::quiesceAccount(const QString &account, bool cancelActive) {
  for (const auto &[id, entry] : folders_)
    if (entry.account == account)
      scheduler_.block(id, "switchInProgress", true);
  if (active_ && folders_.at(active_->folder).account == account) {
    if (cancelActive)
      folders_.at(active_->folder).adapter->stop();
    return false;
  }
  return true;
}

QString EngineRuntime::retireAccount(const QString &account) {
  if (!quiesceAccount(account, false))
    return "QUIESCE_STALLED";
  QString reason;
  for (const auto &[id, entry] : folders_) {
    if (entry.account != account)
      continue;
    const auto proof = entry.adapter->retire();
    if (!proof.journalCommitted)
      reason = "JOURNAL_ERROR";
    else if (!proof.networkDrained && reason.isEmpty())
      reason = "QUIESCE_STALLED";
  }
  return reason;
}

void EngineRuntime::rebindAccount(const QString &account, const QUrl &server,
                                  quint64 epoch, const QString &login) {
  if (stopping_)
    throw std::runtime_error("CANCELLED");
  const auto reason = retireAccount(account);
  if (!reason.isEmpty())
    throw std::runtime_error(reason.toStdString());
  for (const auto &[id, entry] : folders_)
    if (entry.account == account) {
      entry.adapter->rebind(server, epoch, login);
      // Rebinding invalidates authorization; fresh discovery must produce a new
      // proposal.
      scheduler_.block(id, "awaitingDecision", false);
    }
}

void EngineRuntime::blockAccount(const QString &account, const QString &reason,
                                 bool blocked) {
  for (const auto &[id, entry] : folders_)
    if (entry.account == account)
      block(id, reason, blocked);
}

void EngineRuntime::throttleAccount(const QString &account, qint64 delayMs) {
  scheduler_.throttleAccount(account, delayMs, clock_.elapsed());
  if (active_ && folders_.at(active_->folder).account == account)
    folders_.at(active_->folder).adapter->stop();
}

void EngineRuntime::throttleObserved(const QString &account, qint64 delayMs) {
  try {
    if (serverThrottled)
      serverThrottled(account, delayMs);
  } catch (...) {
    qFatal("Server throttle persistence failed");
  }
  throttleAccount(account, delayMs);
}

void EngineRuntime::credentialsRenewed(const QString &account) {
  for (const auto &[id, entry] : folders_) {
    if (entry.account != account)
      continue;
    scheduler_.block(id, "AUTH_REQUIRED", false);
    scheduler_.block(id, "CREDENTIALS_LOCKED", false);
  }
}

void EngineRuntime::endpointVerified(const QString &account) {
  for (const auto &[id, entry] : folders_) {
    if (entry.account != account)
      continue;
    for (const auto &reason :
         {"TLS_ERROR", "ENDPOINT_UNTRUSTED", "ENDPOINT_IDENTITY_MISMATCH"})
      scheduler_.block(id, reason, false);
  }
}

QString EngineRuntime::rebindError(const QString &account,
                                   const QUrl &server) const {
  for (const auto &[id, entry] : folders_) {
    if (entry.account != account)
      continue;
    const auto reason = entry.adapter->rebindError(server);
    if (!reason.isEmpty())
      return reason;
  }
  return {};
}

void EngineRuntime::resumeAccount(const QString &account) {
  if (stopping_)
    return;
  for (const auto &[id, entry] : folders_) {
    if (entry.account == account) {
      scheduler_.enqueue(id, Scheduler::Reason::Recovery, clock_.elapsed());
      scheduler_.block(id, "switchInProgress", false);
    }
  }
}

EngineAdapter::Requests EngineRuntime::requests(const QString &folder) const {
  return folders_.at(folder).adapter->requests();
}

void EngineRuntime::tick() {
  if (stopping_)
    return;
  const auto now = clock_.elapsed();
  for (auto &[id, entry] : folders_) {
    if (now >= entry.pollAt) {
      scheduler_.enqueue(id, Scheduler::Reason::Remote, now);
      entry.pollAt = now + 48000 + QRandomGenerator::global()->bounded(24001);
    }
    if (now >= entry.auditAt) {
      scheduler_.enqueue(id, Scheduler::Reason::Audit, now);
      entry.auditAt = now + 86400000;
    }
  }
  if (active_)
    return;
  active_ = scheduler_.take(now);
  if (!active_)
    return;
  const auto id = active_->folder;
  try {
    if (started)
      started(id);
    folders_.at(id).adapter->start();
  } catch (const std::exception &error) {
    // Metadata exceptions carry an internal delay, not an IPC field. Preserve
    // it before converting to the sanitized completion reason.
    const auto *metadata = dynamic_cast<const MetadataError *>(&error);
    if (metadata && QString::fromUtf8(error.what()) == "SERVER_THROTTLED") {
      const auto account = folders_.at(id).account;
      throttleObserved(account, metadata->retryAfterMs);
    }
    const auto reason = QString::fromUtf8(error.what());
    const auto finishedAt = clock_.elapsed();
    const auto delay =
        metadata &&
                (reason == "REMOTE_LOCKED" || reason == "SERVER_THROTTLED") &&
                metadata->retryAfterMs != 0
            ? metadata->retryAfterMs
            : 60000;
    scheduler_.fail(
        *active_, reason,
        delay < 0
            ? std::numeric_limits<qint64>::max()
            : finishedAt + std::min(delay, std::numeric_limits<qint64>::max() -
                                               finishedAt));
    active_.reset();
    if (cycle)
      cycle(id, {false,
                 false,
                 reason,
                 {},
                 0,
                 0,
                 0,
                 folders_.at(id).adapter->epoch(),
                 0,
                 0,
                 0,
                 folders_.at(id).adapter->networkGeneration(),
                 metadata ? metadata->retryAfterMs : 0});
    if (stopping_)
      finishShutdown();
  }
}
