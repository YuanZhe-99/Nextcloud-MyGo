// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QString>
#include <deque>
#include <map>
#include <optional>

/// EndpointPolicy proposes, but never commits, generation-bound endpoint
/// changes (PLAN §8, INV-03/04/06/14). Inputs: all times are monotonic
/// milliseconds; callers provide verified trust and bounded jitter samples.
class EndpointPolicy {
public:
  /// Config captures endpoint/binding/policy revisions and independent hard
  /// eligibility conditions.
  struct Config {
    quint64 endpointRevision = 0, bindingRevision = 0, policyRevision = 0;
    bool enabled = true, approved = false, namespaceVerified = false,
         tlsValid = false, networkAllowed = true;
    int priority = 50;
    double preference = 0.5, cost = 0.5;
    bool operator==(const Config &) const = default;
  };
  /// Ticket prevents a late probe from modifying a different network or binding
  /// generation.
  struct Ticket {
    QString endpoint;
    quint64 serial = 0, network = 0, endpointRevision = 0, bindingRevision = 0,
            policyRevision = 0;
    bool operator==(const Ticket &) const = default;
  };
  /// Outcome separates transport failure from security/authentication failures,
  /// which never recover by scoring.
  enum class Outcome {
    Success,
    NetworkFailure,
    TLSFailure,
    IdentityFailure,
    AuthFailure
  };
  /// Proposal is an eligible target plus a stable explanation; an empty target
  /// means keep/wait.
  struct Proposal {
    QString endpoint, reason;
  };
  /// configure changes eligibility and invalidates evidence without clearing
  /// security quarantine or backoff.
  void configure(const QString &id, const Config &config);
  /// reviewed clears quarantine only after an explicit full trust/credential
  /// verification has succeeded.
  void reviewed(const QString &id);
  /// remove forgets a nonactive endpoint's selection metadata.
  void remove(const QString &id);
  /// networkChanged invalidates all old evidence and coalesces notification
  /// storms for 2–10 seconds.
  void networkChanged(qint64 now);
  /// beginProbe admits at most one read-only probe per endpoint, respecting
  /// circuit cooldown and debounce.
  std::optional<Ticket> beginProbe(const QString &id, qint64 now);
  /// observe consumes exactly the matching ticket and classifies an independent
  /// probe outcome. Inputs: jitter is uniform in [0,1]; it chooses a
  /// half-to-full exponential backoff cap. Notes: a transport failure
  /// invalidates prior healthy evidence even before the independent-failure
  /// circuit opens.
  bool observe(const Ticket &ticket, Outcome outcome, qint64 now, qint64 rtt,
               double jitter);
  /// hardFailure immediately invalidates the outstanding ticket, success
  /// samples and last success independently of circuit state or probe admission
  /// (INV-04/06/14). Inputs: callers supply only TLSFailure, IdentityFailure or
  /// AuthFailure. Side effects: security failures quarantine; authentication
  /// failures block authentication without clearing existing circuit/backoff.
  void hardFailure(const QString &id, Outcome failure);
  /// choose applies hard gates, stable-success thresholds, deterministic ties,
  /// dwell and cooldown. Inputs: fixedTarget requests only that endpoint, never
  /// fallback to another address.
  Proposal choose(qint64 now, const QString &fixedTarget = {}) const;
  /// activated records a target only after the switch coordinator has committed
  /// it.
  void activated(const QString &id, qint64 now);
  /// Health exposes scheduling evidence without exposing credentials or network
  /// identifiers.
  /// Notes: latencyMs is the smoothed probe round trip once a probe has
  /// succeeded (0 before); quarantined and authBlocked name why failed is set
  /// when it is not the circuit.
  struct Health {
    bool failed = false, fresh = false, halfOpen = false;
    int successes = 0;
    qint64 span = 0;
    qint64 latencyMs = 0;
    bool quarantined = false, authBlocked = false;
  };
  /// health reports current circuit/security and sample freshness to the
  /// bounded probe scheduler.
  Health health(const QString &id, qint64 now) const;
  /// businessSuccess refreshes active liveness without mixing transfer timing
  /// into probe RTT.
  void businessSuccess(const QString &id, qint64 now);
  /// abandon releases only the matching read-only ticket without scoring
  /// permissions or rate limits as network failure.
  bool abandon(const Ticket &ticket);
  /// current reports whether a ticket still owns the exact current
  /// generation/revision probe slot.
  bool current(const Ticket &ticket) const;

private:
  /// NetworkTestAccess lets the real-network soak run the dwell and circuit
  /// timers faster; no product path uses it.
  friend struct NetworkTestAccess;
  enum class Circuit { Closed, Open, HalfOpen };
  /// Entry retains circuit/security history independently of fresh network
  /// evidence.
  struct Entry {
    Config config;
    Circuit circuit = Circuit::Closed;
    bool quarantined = false, authBlocked = false;
    unsigned round = 0;
    qint64 openUntil = 0, lastProbe = -10000, lastSuccess = -1,
           lastReliabilitySample = -1, lastFailure = -10000;
    quint64 earlyGeneration = 0;
    double reliability = 0.5, rtt = 100;
    std::deque<qint64> successes, failures;
    std::optional<Ticket> inFlight;
  };
  /// eligible enforces every hard condition before any score is consulted.
  bool eligible(const Entry &entry, qint64 now) const;
  /// score applies the fixed, explainable PLAN §8.4 weights.
  double score(const Entry &entry) const;
  std::map<QString, Entry> entries_;
  QString active_;
  quint64 network_ = 0, serial_ = 0;
  qint64 changedFirst_ = -1, changedLast_ = -1, activatedAt_ = 0;
};
