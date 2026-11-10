// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_policy.h"
#include <cstdio>
#include <stdexcept>

namespace {
/// require preserves contract assertions in optimized builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// trusted supplies caller-verified origin, identity and TLS inputs for a pure
/// policy fixture.
EndpointPolicy::Config trusted(int priority) {
  EndpointPolicy::Config c;
  c.approved = true;
  c.namespaceVerified = true;
  c.tlsValid = true;
  c.priority = priority;
  return c;
}
/// sample creates an independent ticket and completes it with an injected
/// monotonic time.
void sample(
    EndpointPolicy &p, const QString &id, qint64 now,
    EndpointPolicy::Outcome outcome = EndpointPolicy::Outcome::Success) {
  const auto ticket = p.beginProbe(id, now);
  require(ticket.has_value(), "probe unexpectedly blocked");
  require(p.observe(*ticket, outcome, now, 50, 1), "current sample rejected");
}
/// selection checks trust filters, score margin, stable-success span, dwell,
/// freshness and fixed-mode refusal (INV-04/14).
void selection() {
  EndpointPolicy p;
  p.configure("wan", trusted(50));
  p.configure("lan", trusted(90));
  p.activated("wan", 0);
  for (const qint64 now : {1000, 6000, 16000}) {
    sample(p, "wan", now);
    sample(p, "lan", now);
  }
  require(p.choose(16000).endpoint.isEmpty(), "preference bypassed dwell");
  for (const qint64 now : {130000, 135000, 145000}) {
    sample(p, "wan", now);
    sample(p, "lan", now);
  }
  require(p.choose(145000).endpoint == "lan",
          "stable improvement not proposed");
  require(p.choose(160001).endpoint.isEmpty(), "stale evidence selected");
  auto bad = trusted(100);
  bad.tlsValid = false;
  p.configure("lan", bad);
  sample(p, "lan", 165000);
  sample(p, "wan", 165000);
  require(p.choose(165000, "lan").endpoint.isEmpty(),
          "manual choice bypassed TLS");
  bad = trusted(50);
  p.configure("lan", bad);
  sample(p, "lan", 170000);
  sample(p, "wan", 170000);
  require(p.choose(170000).endpoint.isEmpty(),
          "equal score displaced current endpoint");
  EndpointPolicy tied;
  tied.configure("a", trusted(50));
  tied.configure("b", trusted(50));
  sample(tied, "a", 0);
  sample(tied, "b", 0);
  require(tied.choose(0).endpoint == "a",
          "equal-score endpoint ID order changed");
  tied.activated("b", 0);
  require(tied.choose(0).endpoint.isEmpty(),
          "equal-score current endpoint was displaced");
}
/// generations checks stale results, bounded notification debounce and revision
/// invalidation (INV-06).
void generations() {
  EndpointPolicy p;
  p.configure("one", trusted(50));
  const auto old = p.beginProbe("one", 0);
  require(old.has_value(), "initial probe missing");
  for (qint64 now = 0; now <= 10000; now += 1000) {
    p.networkChanged(now);
    if (now < 10000)
      require(!p.beginProbe("one", now), "storm debounce ended early");
  }
  const auto fresh = p.beginProbe("one", 10000);
  require(fresh.has_value(), "notification storm postponed forever");
  require(!p.observe(*old, EndpointPolicy::Outcome::TLSFailure, 10000, 0, 1),
          "old network result applied");
  require(p.observe(*fresh, EndpointPolicy::Outcome::Success, 10000, 50, 1),
          "old result consumed fresh ticket");
  const auto prior = p.beginProbe("one", 15000);
  auto c = trusted(50);
  c.bindingRevision = 1;
  p.configure("one", c);
  require(!p.observe(*prior, EndpointPolicy::Outcome::Success, 15000, 50, 1),
          "old binding result applied");
  require(p.choose(15000).endpoint.isEmpty(),
          "binding change retained freshness");
}
/// circuits checks three-failure opening, bounded early half-open, single probe
/// and security quarantine.
void circuits() {
  EndpointPolicy p;
  p.configure("one", trusted(50));
  for (const qint64 now : {0, 5000, 10000})
    sample(p, "one", now, EndpointPolicy::Outcome::NetworkFailure);
  require(!p.beginProbe("one", 15000), "open circuit admitted early traffic");
  p.networkChanged(16000);
  require(!p.beginProbe("one", 18000),
          "network event reset backoff rate limit");
  const auto half = p.beginProbe("one", 20000);
  require(half.has_value(),
          "fresh network did not allow bounded half-open check");
  require(!p.beginProbe("one", 20001), "half-open admitted concurrent probes");
  require(p.observe(*half, EndpointPolicy::Outcome::Success, 20000, 50, 1),
          "half-open success rejected");
  sample(p, "one", 25000);
  require(p.choose(25000).endpoint.isEmpty(),
          "circuit closed before recovery threshold");
  sample(p, "one", 35000);
  require(p.choose(35000).endpoint == "one",
          "healthy half-open circuit did not recover");
  sample(p, "one", 40000, EndpointPolicy::Outcome::IdentityFailure);
  p.networkChanged(50000);
  require(!p.beginProbe("one", 70000),
          "network change cleared identity quarantine");
  p.reviewed("one");
  sample(p, "one", 70000);
  require(p.choose(70000).endpoint == "one",
          "explicit full review did not release quarantine");
}
/// failover refuses a single lucky sample and never substitutes another
/// endpoint for a fixed selection.
void failover() {
  EndpointPolicy p;
  p.configure("current", trusted(90));
  p.configure("backup", trusted(50));
  p.activated("current", 0);
  for (const qint64 now : {0, 5000, 10000})
    sample(p, "current", now, EndpointPolicy::Outcome::NetworkFailure);
  sample(p, "backup", 10000);
  require(p.choose(10000).endpoint.isEmpty(),
          "failover accepted only one successful check");
  sample(p, "backup", 15000);
  require(p.choose(15000).endpoint == "backup" &&
              p.choose(15000).reason == "unreachable",
          "failure did not bypass preference dwell and score margin");
  require(p.choose(15000, "current").endpoint.isEmpty(),
          "fixed endpoint failure silently selected a backup");
}
/// failedFreshness rejects prior healthy evidence after a newer transport
/// failure, without opening the circuit on one lost request (PLAN §8.3/N10).
void failedFreshness() {
  EndpointPolicy p;
  p.configure("current", trusted(50));
  p.configure("backup", trusted(90));
  p.activated("current", 0);
  sample(p, "backup", 0);
  require(p.choose(0, "backup").endpoint == "backup",
          "healthy manual target was unavailable");
  sample(p, "backup", 5000, EndpointPolicy::Outcome::NetworkFailure);
  require(!p.health("backup", 5000).failed,
          "single transport failure opened the circuit");
  require(!p.health("backup", 5000).fresh &&
              p.choose(5000, "backup").endpoint.isEmpty(),
          "failed probe retained prior-success selection eligibility");
  sample(p, "backup", 10000);
  require(p.health("backup", 10000).fresh &&
              p.choose(10000, "backup").endpoint == "backup",
          "fresh success did not restore manual target eligibility");
  sample(p, "backup", 10001, EndpointPolicy::Outcome::NetworkFailure);
  sample(p, "backup", 10002);
  sample(p, "backup", 10003, EndpointPolicy::Outcome::NetworkFailure);
  require(!p.health("backup", 10003).failed &&
              !p.health("backup", 10003).fresh &&
              p.choose(10003, "backup").endpoint.isEmpty(),
          "correlated failure retained a newer healthy selection sample");
}
/// hardFailures preserves security/authentication blocks despite a busy probe
/// or open circuit, and rejects late successful completions (INV-04/06/14,
/// N04/N13).
void hardFailures() {
  for (const auto failure : {EndpointPolicy::Outcome::TLSFailure,
                             EndpointPolicy::Outcome::IdentityFailure,
                             EndpointPolicy::Outcome::AuthFailure}) {
    for (const bool open : {false, true}) {
      EndpointPolicy p;
      p.configure("one", trusted(50));
      for (const qint64 now : {0, 5000, 15000})
        sample(p, "one", now);
      const auto old = p.beginProbe("one", 20000);
      require(old.has_value(), "hard-failure fixture ticket missing");
      if (open) {
        require(p.observe(*old, EndpointPolicy::Outcome::NetworkFailure, 20000,
                          0, 1),
                "open-circuit fixture failure rejected");
        sample(p, "one", 25000, EndpointPolicy::Outcome::NetworkFailure);
        sample(p, "one", 30000, EndpointPolicy::Outcome::NetworkFailure);
      }
      const qint64 now = open ? 30001 : 20000;
      require(!p.beginProbe("one", now),
              "busy/open fixture unexpectedly admitted a probe");
      p.hardFailure("one", failure);
      const auto blocked = p.health("one", now);
      require(blocked.failed && !blocked.fresh && blocked.successes == 0 &&
                  blocked.span == 0 && !p.current(*old),
              "hard failure retained a ticket or healthy evidence");
      require(!p.observe(*old, EndpointPolicy::Outcome::Success, now, 50, 1) &&
                  !p.abandon(*old),
              "old successful completion survived hard failure");
      p.businessSuccess("one", now + 1);
      require(!p.health("one", now + 1).fresh,
              "business success cleared a hard failure");
      auto config = trusted(50);
      config.bindingRevision = 1;
      p.configure("one", config);
      p.networkChanged(now + 2);
      require(!p.beginProbe("one", 1000000) &&
                  p.choose(1000000, "one").endpoint.isEmpty(),
              "time or configuration changes cleared a hard failure");
      p.reviewed("one");
      sample(p, "one", 1000000);
      if (open) {
        require(p.health("one", 1000000).halfOpen &&
                    !p.health("one", 1000000).fresh,
                "hard failure or review erased circuit recovery requirements");
        sample(p, "one", 1005000);
        sample(p, "one", 1015000);
      }
      require(p.health("one", 1015000).fresh,
              "explicit review and circuit recovery did not release the block");
    }
  }
}
} // namespace
/// main verifies PLAN §8 selection contracts without network access, wall-clock
/// sleeps or user state.
int main() {
  try {
    selection();
    generations();
    circuits();
    failover();
    failedFreshness();
    hardFailures();
    std::puts("PASS endpoint scoring, generation isolation, debounce, circuit "
              "recovery, trust gates and independent hard failures");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
