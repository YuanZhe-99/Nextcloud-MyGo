// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_policy.h"
#include <algorithm>

void EndpointPolicy::configure(const QString &id, const Config &config) {
  auto &e = entries_[id];
  if (e.config != config) {
    e.successes.clear();
    e.lastSuccess = -1;
    e.inFlight.reset();
  }
  e.config = config;
}
void EndpointPolicy::reviewed(const QString &id) {
  auto &e = entries_.at(id);
  if (!e.config.approved || !e.config.namespaceVerified || !e.config.tlsValid)
    return;
  e.quarantined = false;
  e.authBlocked = false;
  e.successes.clear();
  e.lastSuccess = -1;
  e.inFlight.reset();
}
void EndpointPolicy::remove(const QString &id) {
  if (id != active_)
    entries_.erase(id);
}
void EndpointPolicy::networkChanged(qint64 now) {
  if (changedFirst_ < 0)
    changedFirst_ = now;
  changedLast_ = now;
  ++network_;
  for (auto &[id, e] : entries_) {
    e.successes.clear();
    e.lastSuccess = -1;
    e.inFlight.reset();
    if (e.circuit == Circuit::HalfOpen)
      e.circuit = Circuit::Open;
  }
}
std::optional<EndpointPolicy::Ticket>
EndpointPolicy::beginProbe(const QString &id, qint64 now) {
  auto &e = entries_.at(id);
  if (changedFirst_ >= 0 &&
      now < std::min(changedFirst_ + 10000, changedLast_ + 2000))
    return {};
  changedFirst_ = -1;
  changedLast_ = -1;
  if (e.inFlight || !e.config.enabled || !e.config.approved ||
      !e.config.networkAllowed || e.quarantined || e.authBlocked)
    return {};
  if (e.circuit == Circuit::Open) {
    if (now < e.openUntil &&
        !(network_ != e.earlyGeneration && now - e.lastProbe >= 10000))
      return {};
    e.earlyGeneration = network_;
    e.circuit = Circuit::HalfOpen;
    e.successes.clear();
  }
  e.lastProbe = now;
  e.inFlight = Ticket{id,
                      ++serial_,
                      network_,
                      e.config.endpointRevision,
                      e.config.bindingRevision,
                      e.config.policyRevision};
  return e.inFlight;
}
bool EndpointPolicy::observe(const Ticket &ticket, Outcome outcome, qint64 now,
                             qint64 rtt, double jitter) {
  auto it = entries_.find(ticket.endpoint);
  if (it == entries_.end() || !it->second.inFlight ||
      *it->second.inFlight != ticket || ticket.network != network_)
    return false;
  auto &e = it->second;
  e.inFlight.reset();
  if (outcome == Outcome::TLSFailure || outcome == Outcome::IdentityFailure ||
      outcome == Outcome::AuthFailure) {
    hardFailure(ticket.endpoint, outcome);
    return true;
  }
  if (e.lastReliabilitySample < 0 || now - e.lastReliabilitySample >= 5000) {
    e.reliability =
        0.8 * e.reliability + 0.2 * (outcome == Outcome::Success ? 1 : 0);
    e.lastReliabilitySample = now;
  }
  if (outcome == Outcome::Success) {
    if (e.lastSuccess >= 0 && now - e.lastSuccess > 30000)
      e.successes.clear();
    e.rtt = 0.75 * e.rtt + 0.25 * std::clamp<qint64>(rtt, 1, 10000);
    e.lastSuccess = now;
    if (e.successes.empty() || now - e.successes.back() >= 5000)
      e.successes.push_back(now);
    while (e.successes.size() > 3)
      e.successes.erase(e.successes.begin() + 1);
    if (e.circuit == Circuit::HalfOpen && e.successes.size() >= 3 &&
        e.successes.back() - e.successes.front() >= 15000) {
      e.circuit = Circuit::Closed;
      e.round = 0;
      e.failures.clear();
    }
    return true;
  }
  e.successes.clear();
  e.lastSuccess = -1;
  if (now - e.lastFailure < 5000)
    return true;
  e.lastFailure = now;
  while (!e.failures.empty() && now - e.failures.front() > 30000)
    e.failures.pop_front();
  e.failures.push_back(now);
  while (e.failures.size() > 3)
    e.failures.pop_front();
  if (e.circuit == Circuit::HalfOpen || e.failures.size() >= 3) {
    const auto cap = std::min<qint64>(600000, 30000LL << std::min(e.round, 5u));
    e.openUntil = now + qint64(cap * (0.5 + 0.5 * jitter));
    e.round = std::min(e.round + 1, 5u);
    e.circuit = Circuit::Open;
    e.earlyGeneration = network_;
  }
  return true;
}
void EndpointPolicy::hardFailure(const QString &id, Outcome failure) {
  auto &e = entries_.at(id);
  e.inFlight.reset();
  e.successes.clear();
  e.lastSuccess = -1;
  if (failure == Outcome::AuthFailure)
    e.authBlocked = true;
  else
    e.quarantined = true;
}
bool EndpointPolicy::eligible(const Entry &e, qint64 now) const {
  const auto &c = e.config;
  return c.enabled && c.approved && c.namespaceVerified && c.tlsValid &&
         c.networkAllowed && !e.quarantined && !e.authBlocked &&
         e.circuit == Circuit::Closed && e.lastSuccess >= 0 &&
         now - e.lastSuccess <= 15000;
}
double EndpointPolicy::score(const Entry &e) const {
  return 45 * e.config.priority / 100.0 + 20 * e.reliability +
         15 / (1 + e.rtt / 100) + 10 * e.config.preference + 10 * e.config.cost;
}
EndpointPolicy::Proposal
EndpointPolicy::choose(qint64 now, const QString &fixedTarget) const {
  if (!fixedTarget.isEmpty()) {
    const auto it = entries_.find(fixedTarget);
    return it != entries_.end() && eligible(it->second, now) &&
                   fixedTarget != active_
               ? Proposal{fixedTarget, "userChoice"}
               : Proposal{};
  }
  auto best = entries_.end();
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (!eligible(it->second, now))
      continue;
    if (best == entries_.end() || score(it->second) > score(best->second) ||
        (score(it->second) == score(best->second) && it->first == active_))
      best = it;
  }
  if (best == entries_.end() || best->first == active_)
    return {};
  if (active_.isEmpty())
    return {best->first, "verified"};
  const auto current = entries_.find(active_);
  const bool failed =
      current == entries_.end() || current->second.circuit != Circuit::Closed ||
      current->second.quarantined || current->second.authBlocked ||
      !current->second.config.enabled || !current->second.config.approved ||
      !current->second.config.namespaceVerified ||
      !current->second.config.tlsValid ||
      !current->second.config.networkAllowed;
  const auto &candidate = best->second;
  if (failed)
    return candidate.successes.size() >= 2
               ? Proposal{best->first, "unreachable"}
               : Proposal{};
  if (!eligible(current->second, now))
    return {};
  if (candidate.successes.size() < 3 ||
      candidate.successes.back() - candidate.successes.front() < 15000 ||
      now - activatedAt_ < 120000 ||
      score(candidate) - score(current->second) < 12)
    return {};
  return {best->first, "faster"};
}
void EndpointPolicy::activated(const QString &id, qint64 now) {
  active_ = id;
  activatedAt_ = now;
}

EndpointPolicy::Health EndpointPolicy::health(const QString &id,
                                              qint64 now) const {
  const auto it = entries_.find(id);
  if (it == entries_.end())
    return {true};
  const auto &e = it->second;
  // Measured latency is reported only once a probe succeeded.
  return {e.circuit != Circuit::Closed || e.quarantined || e.authBlocked,
          eligible(e, now),
          e.circuit == Circuit::HalfOpen,
          int(e.successes.size()),
          e.successes.empty() ? 0 : now - e.successes.front(),
          e.successes.empty() ? 0 : qint64(e.rtt + 0.5),
          e.quarantined,
          e.authBlocked};
}

void EndpointPolicy::businessSuccess(const QString &id, qint64 now) {
  auto it = entries_.find(id);
  if (it == entries_.end())
    return;
  auto &e = it->second;
  if (e.quarantined || e.authBlocked || e.circuit != Circuit::Closed)
    return;
  e.lastSuccess = now;
  if (e.lastReliabilitySample < 0 || now - e.lastReliabilitySample >= 5000) {
    e.reliability = 0.8 * e.reliability + 0.2;
    e.lastReliabilitySample = now;
  }
}

bool EndpointPolicy::abandon(const Ticket &ticket) {
  auto it = entries_.find(ticket.endpoint);
  if (it != entries_.end() && it->second.inFlight &&
      *it->second.inFlight == ticket) {
    it->second.inFlight.reset();
    return true;
  }
  return false;
}

bool EndpointPolicy::current(const Ticket &ticket) const {
  const auto it = entries_.find(ticket.endpoint);
  return it != entries_.end() && it->second.inFlight &&
         *it->second.inFlight == ticket && ticket.network == network_;
}
