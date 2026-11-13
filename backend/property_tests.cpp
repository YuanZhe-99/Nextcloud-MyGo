// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_policy.h"
#include <QByteArray>
#include <QtGlobal>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {
/// Step is one randomized policy event with every random input fixed, so a
/// sequence replays exactly and can be shrunk.
struct Step {
  enum Kind {
    Configure,
    Reviewed,
    NetworkChanged,
    BeginProbe,
    Observe,
    HardFailure,
    Activate,
    Abandon,
    BusinessSuccess,
    Advance
  } kind;
  int endpoint = 0, ticket = 0, outcome = 0, priority = 50;
  unsigned flags = 0;
  qint64 dt = 0, rtt = 50;
  double jitter = 0.5;
};

constexpr int endpoints = 3;
const char *names[endpoints] = {"wan", "lan", "vpn"};
const char *kinds[] = {"configure", "reviewed", "networkChanged",
                       "beginProbe", "observe",  "hardFailure",
                       "activate",   "abandon",  "businessSuccess",
                       "advance"};

/// generate draws a sequence from one seed; flags set each trust bit with
/// probability 0.8 so eligible and ineligible endpoints both occur often.
std::vector<Step> generate(unsigned seed, int length) {
  std::mt19937 random(seed);
  const auto pick = [&](int n) {
    return std::uniform_int_distribution<int>(0, n - 1)(random);
  };
  std::vector<Step> steps;
  for (int i = 0; i < length; ++i) {
    Step s{static_cast<Step::Kind>(pick(10))};
    s.endpoint = pick(endpoints);
    s.ticket = pick(8);
    s.outcome = pick(5);
    s.priority = pick(101);
    for (int bit = 0; bit < 5; ++bit)
      s.flags |= (pick(10) < 8 ? 1u : 0u) << bit;
    s.dt = pick(4) == 0 ? pick(60000) : pick(6000);
    s.rtt = 1 + pick(400);
    s.jitter = pick(1001) / 1000.0;
    steps.push_back(s);
  }
  return steps;
}

/// Ticket is an issued probe ticket with the test model's own verdict on
/// whether it is still current; the model (configuration, and the security
/// blocks only reviewed() lifts) is kept by run(), never read from the policy.
struct Ticket {
  EndpointPolicy::Ticket ticket;
  int endpoint;
  bool stale = false;
};

/// same compares every reported health field.
bool same(const EndpointPolicy::Health &a, const EndpointPolicy::Health &b) {
  return a.failed == b.failed && a.fresh == b.fresh &&
         a.halfOpen == b.halfOpen && a.successes == b.successes &&
         a.span == b.span && a.latencyMs == b.latencyMs &&
         a.quarantined == b.quarantined && a.authBlocked == b.authBlocked;
}

/// run replays a sequence and returns the first violated property, or empty.
/// P04: a ticket from an older network generation, an older configuration, a
/// hard failure or a review, or one already consumed, is refused and changes
/// nothing. P08: no proposal, automatic or fixed, names an endpoint that is
/// disabled, unapproved, unverified, without valid TLS, not allowed on this
/// network, quarantined or blocked for authentication, and a fixed target never
/// falls back to another endpoint.
std::string run(const std::vector<Step> &steps, size_t *failedAt = nullptr) {
  EndpointPolicy p;
  EndpointPolicy::Config config[endpoints];
  bool quarantined[endpoints] = {}, authBlocked[endpoints] = {};
  std::vector<Ticket> tickets;
  quint64 revision = 0;
  qint64 now = 0;
  for (int i = 0; i < endpoints; ++i)
    p.configure(names[i], config[i]);
  const auto staleFor = [&](int endpoint) {
    for (auto &t : tickets)
      if (endpoint < 0 || t.endpoint == endpoint)
        t.stale = true;
  };
  const auto forbidden = [&](const QString &id) {
    for (int i = 0; i < endpoints; ++i)
      if (id == names[i]) {
        const auto &c = config[i];
        return !c.enabled || !c.approved || !c.namespaceVerified ||
               !c.tlsValid || !c.networkAllowed || quarantined[i] ||
               authBlocked[i];
      }
    return true;
  };
  for (size_t index = 0; index < steps.size(); ++index) {
    const auto &s = steps[index];
    const QString id = names[s.endpoint];
    const auto fail = [&](const char *property) {
      if (failedAt)
        *failedAt = index;
      return std::string(property);
    };
    switch (s.kind) {
    case Step::Configure: {
      EndpointPolicy::Config c;
      c.endpointRevision = ++revision;
      c.enabled = s.flags & 1;
      c.approved = s.flags & 2;
      c.namespaceVerified = s.flags & 4;
      c.tlsValid = s.flags & 8;
      c.networkAllowed = s.flags & 16;
      c.priority = s.priority;
      p.configure(id, c);
      config[s.endpoint] = c;
      staleFor(s.endpoint);
      break;
    }
    case Step::Reviewed: {
      p.reviewed(id);
      const auto &c = config[s.endpoint];
      if (c.approved && c.namespaceVerified && c.tlsValid) {
        quarantined[s.endpoint] = authBlocked[s.endpoint] = false;
        staleFor(s.endpoint);
      }
      break;
    }
    case Step::NetworkChanged:
      p.networkChanged(now);
      staleFor(-1);
      break;
    case Step::BeginProbe:
      if (const auto t = p.beginProbe(id, now))
        tickets.push_back({*t, s.endpoint});
      break;
    case Step::Observe:
    case Step::Abandon: {
      if (tickets.empty())
        break;
      auto &t = tickets[s.ticket % tickets.size()];
      const QString target = names[t.endpoint];
      const auto before = p.health(target, now);
      const auto outcome = static_cast<EndpointPolicy::Outcome>(s.outcome);
      const bool accepted =
          s.kind == Step::Observe
              ? p.observe(t.ticket, outcome, now, s.rtt, s.jitter)
              : p.abandon(t.ticket);
      if (t.stale) {
        if (accepted)
          return fail("P04 stale or consumed ticket accepted");
        if (!same(before, p.health(target, now)))
          return fail("P04 refused ticket changed the endpoint's health");
      }
      if (accepted) {
        t.stale = true;
        if (s.kind == Step::Observe &&
            (outcome == EndpointPolicy::Outcome::TLSFailure ||
             outcome == EndpointPolicy::Outcome::IdentityFailure))
          quarantined[t.endpoint] = true;
        if (s.kind == Step::Observe &&
            outcome == EndpointPolicy::Outcome::AuthFailure)
          authBlocked[t.endpoint] = true;
      }
      break;
    }
    case Step::HardFailure: {
      const auto outcome = static_cast<EndpointPolicy::Outcome>(
          int(EndpointPolicy::Outcome::TLSFailure) + s.outcome % 3);
      p.hardFailure(id, outcome);
      (outcome == EndpointPolicy::Outcome::AuthFailure
           ? authBlocked
           : quarantined)[s.endpoint] = true;
      staleFor(s.endpoint);
      break;
    }
    case Step::Activate:
      if (const auto proposal = p.choose(now); !proposal.endpoint.isEmpty())
        p.activated(proposal.endpoint, now);
      break;
    case Step::BusinessSuccess:
      p.businessSuccess(id, now);
      break;
    case Step::Advance:
      now += s.dt;
      break;
    }
    if (const auto e = p.choose(now).endpoint; !e.isEmpty() && forbidden(e))
      return fail("P08 automatic proposal names a filtered endpoint");
    for (int i = 0; i < endpoints; ++i) {
      const auto e = p.choose(now, names[i]).endpoint;
      if (!e.isEmpty() && (e != names[i] || forbidden(e)))
        return fail("P08 fixed proposal names a filtered or other endpoint");
    }
  }
  return {};
}

/// shrink removes steps one at a time while the sequence still fails, giving a
/// minimal failing sequence for the record.
std::vector<Step> shrink(std::vector<Step> steps) {
  size_t at = 0;
  run(steps, &at);
  steps.resize(at + 1);
  for (bool removed = true; removed;) {
    removed = false;
    for (size_t i = 0; i < steps.size(); ++i) {
      auto shorter = steps;
      shorter.erase(shorter.begin() + i);
      if (!run(shorter).empty()) {
        steps = shorter;
        removed = true;
        break;
      }
    }
  }
  return steps;
}
} // namespace

/// main runs randomized policy sequences from fixed seeds (or the one in
/// SYNCD_PROPERTY_SEED) and prints the seed and minimal sequence of a failure
/// (PLAN §20.5 P04, P08; INV-04/06/14).
int main() {
  std::vector<unsigned> seeds;
  if (const auto chosen = qEnvironmentVariable("SYNCD_PROPERTY_SEED");
      !chosen.isEmpty())
    seeds.push_back(chosen.toUInt());
  else
    for (unsigned seed = 1; seed <= 500; ++seed)
      seeds.push_back(seed);
  constexpr int length = 400;
  for (const auto seed : seeds) {
    const auto steps = generate(seed, length);
    const auto failure = run(steps);
    if (failure.empty())
      continue;
    const auto minimal = shrink(steps);
    std::fprintf(stderr, "FAIL %s; seed %u, minimal sequence of %zu steps:\n",
                 failure.c_str(), seed, minimal.size());
    for (const auto &s : minimal)
      std::fprintf(stderr,
                   "  %s endpoint=%s ticket=%d outcome=%d flags=%u dt=%lld\n",
                   kinds[s.kind], names[s.endpoint], s.ticket, s.outcome,
                   s.flags, static_cast<long long>(s.dt));
    return 1;
  }
  std::printf("PASS %zu seeded policy sequences of %d steps: stale tickets "
              "refused without effect (P04), no filtered endpoint proposed "
              "(P08)\n",
              seeds.size(), length);
  return 0;
}
