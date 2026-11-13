// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "endpoint_policy.h"
#include "login.h"
#include "scheduler.h"
#include "switch.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {
using Outcome = EndpointPolicy::Outcome;

/// require preserves contract assertions in optimized builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// trusted supplies caller-verified origin, identity and TLS inputs.
EndpointPolicy::Config trusted(int priority = 50) {
  EndpointPolicy::Config c;
  c.approved = c.namespaceVerified = c.tlsValid = true;
  c.priority = priority;
  return c;
}
/// probe begins one independent read-only probe and completes it at the same
/// injected time; it returns false when admission refused the probe.
bool probe(EndpointPolicy &p, const QString &id, qint64 now,
           Outcome outcome = Outcome::Success, double jitter = 1) {
  const auto ticket = p.beginProbe(id, now);
  if (!ticket)
    return false;
  require(p.observe(*ticket, outcome, now, 50, jitter),
          "a current probe result was refused");
  return true;
}

/// flapping switches the default interface every 400 ms for 20 s with results
/// arriving after the next change (late DNS), then alternates which address is
/// reachable every 3 s for five minutes (N08, INV-06): late results are
/// refused, nothing is proposed during the storm, and flaps shorter than the
/// two independent successes a failover needs never move the account.
void flapping() {
  EndpointPolicy p;
  p.configure("wan", trusted());
  p.configure("lan", trusted());
  p.activated("wan", 0);
  for (qint64 t = 0; t <= 15000; t += 5000) {
    require(probe(p, "wan", t) && probe(p, "lan", t), "warm-up refused");
  }
  std::optional<EndpointPolicy::Ticket> late;
  int refused = 0, admitted = 0;
  for (qint64 t = 16000; t < 36000; t += 400) {
    p.networkChanged(t);
    if (late) {
      refused += !p.observe(*late, Outcome::Success, t, 50, 1);
      late.reset();
    }
    late = p.beginProbe(t % 800 ? "wan" : "lan", t + 1);
    admitted += late.has_value();
    require(p.choose(t + 1).endpoint.isEmpty(),
            "a proposal was made during a notification storm");
  }
  require(admitted >= 1 && refused == admitted - (late ? 1 : 0),
          "a late result of an older network generation was accepted");
  require(!p.health("wan", 36000).fresh && !p.health("lan", 36000).fresh,
          "evidence of an older network survived the storm");
  // Interfaces alternate faster than two independent successes (5 s apart)
  // can be collected on either address.
  QString reachable = "lan";
  int switches = 0;
  for (qint64 t = 40000; t < 340000; t += 500) {
    if ((t - 40000) % 3000 == 0) {
      reachable = reachable == "lan" ? "wan" : "lan";
      p.networkChanged(t);
    }
    for (const QString id : {"wan", "lan"})
      probe(p, id, t,
            id == reachable ? Outcome::Success : Outcome::NetworkFailure,
            0.5);
    const auto chosen = p.choose(t);
    if (!chosen.endpoint.isEmpty()) {
      p.activated(chosen.endpoint, t);
      ++switches;
    }
  }
  require(switches == 0, "3-second interface flaps moved the account");
}

/// Margin is one N09 row: the alternate's priority advantage, the span of its
/// three successes, how long the current address has served, and the result.
struct Margin {
  const char *name;
  int advantage;
  qint64 span, served;
  bool proposed;
};
/// margins checks PLAN §8.5's optimization thresholds one by one on a caller
/// clock (N09): a 12-point score margin, three successes over at least 15 s,
/// 120 s of dwell and fresh evidence; equal RTT and reliability isolate the
/// priority term (0.45 points per priority step, so 27 steps clear 12 points
/// and 26 do not).
void margins() {
  const Margin rows[] = {
      {"all thresholds met", 27, 15000, 120000, true},
      {"margin 11.7 points", 26, 15000, 120000, false},
      {"dwell 1 ms short", 27, 15000, 119999, false},
      {"successes span 14.999 s", 27, 14999, 120000, false},
      {"home over internet (90 vs 50)", 40, 15000, 120000, true},
      {"vpn over internet (70 vs 50)", 20, 15000, 120000, false},
  };
  for (const auto &row : rows) {
    EndpointPolicy p;
    p.configure("wan", trusted(50));
    p.configure("lan", trusted(50 + row.advantage));
    const qint64 now = 200000;
    p.activated("wan", now - row.served);
    for (const qint64 at : {now - row.span, now - row.span + 5000, now}) {
      require(probe(p, "wan", at) && probe(p, "lan", at),
              "margin fixture probe refused");
    }
    const auto chosen = p.choose(now);
    if (row.proposed != (chosen.endpoint == "lan" && chosen.reason == "faster"))
      throw std::runtime_error(std::string("N09 row failed: ") + row.name);
  }
  // Freshness: the same winning evidence 15.001 s old proposes nothing.
  EndpointPolicy p;
  p.configure("wan", trusted(50));
  p.configure("lan", trusted(90));
  p.activated("wan", 0);
  for (const qint64 at : {150000, 155000, 165000})
    require(probe(p, "wan", at) && probe(p, "lan", at), "freshness fixture");
  require(p.choose(165000).endpoint == "lan", "fresh winner not proposed");
  require(p.choose(180001).endpoint.isEmpty(), "stale evidence proposed");
  // Cooldown: right after a committed switch even a far better previous
  // address waits out the dwell (which includes the 60 s cooldown).
  p.activated("lan", 165000);
  p.configure("wan", trusted(100));
  p.configure("lan", trusted(40));
  for (qint64 at = 170000; at <= 285000; at += 5000) {
    require(probe(p, "wan", at) && probe(p, "lan", at), "cooldown fixture");
    const auto chosen = p.choose(at);
    if (at < 285000)
      require(chosen.endpoint.isEmpty(), "switched back inside the cooldown");
    else
      require(chosen.endpoint == "wan" && chosen.reason == "faster",
              "better address not proposed after dwell and cooldown");
  }
}

/// independentFailures checks PLAN §8.5's soft-failure threshold (N10): three
/// failures 5 s or more apart within 30 s open the circuit; repeated
/// callbacks of one request, a burst of requests inside 5 s, or three failures
/// spread over more than 30 s do not.
void independentFailures() {
  {
    EndpointPolicy p;
    p.configure("one", trusted());
    const auto ticket = p.beginProbe("one", 0);
    require(ticket.has_value(), "fixture probe refused");
    require(p.observe(*ticket, Outcome::NetworkFailure, 0, 0, 1),
            "first failure refused");
    for (int i = 0; i < 5; ++i)
      require(!p.observe(*ticket, Outcome::NetworkFailure, 100 + i, 0, 1),
              "a second callback of one request was counted");
    require(!p.health("one", 200).failed, "one request opened the circuit");
  }
  {
    EndpointPolicy p;
    p.configure("one", trusted());
    for (const qint64 at : {0, 1000, 2000, 3000, 4000})
      probe(p, "one", at, Outcome::NetworkFailure);
    require(!p.health("one", 4500).failed,
            "failures inside 5 s counted as independent");
  }
  {
    EndpointPolicy p;
    p.configure("one", trusted());
    for (const qint64 at : {0, 16000, 32000})
      probe(p, "one", at, Outcome::NetworkFailure);
    require(!p.health("one", 32000).failed,
            "failures spread over more than 30 s opened the circuit");
  }
  EndpointPolicy p;
  p.configure("current", trusted());
  p.configure("backup", trusted());
  p.activated("current", 0);
  for (const qint64 at : {0, 6000, 12000}) {
    probe(p, "backup", at);
    require(!p.health("current", at).failed, "circuit opened early");
    probe(p, "current", at, Outcome::NetworkFailure);
  }
  require(p.health("current", 12000).failed,
          "three independent failures in 30 s did not open the circuit");
  const auto chosen = p.choose(12000);
  require(chosen.endpoint == "backup" && chosen.reason == "unreachable",
          "failure evaluation did not propose the healthy backup");
}

/// allDown fails both addresses for 30 minutes with a network notification
/// every 15 s (N15): nothing is ever proposed, and probing stays bounded by
/// the circuit's exponential backoff and the 10 s early half-open interval.
void allDown() {
  EndpointPolicy p;
  p.configure("wan", trusted());
  p.configure("lan", trusted());
  p.activated("wan", 0);
  std::map<QString, int> probes;
  // Control retries a failed address after 5 s; the circuit decides whether
  // it is admitted.
  for (qint64 t = 0; t <= 1800000; t += 2500) {
    if (t % 15000 == 0 && t > 0)
      p.networkChanged(t);
    for (const QString id : {"wan", "lan"})
      if (t % 5000 == 0 && probe(p, id, t, Outcome::NetworkFailure, 0.5))
        ++probes[id];
    require(p.choose(t).endpoint.isEmpty(),
            "a switch was proposed while every address was down");
  }
  // 3 to open, then at most one early half-open per network change (120 in
  // 30 min) plus the few backoff expiries.
  for (const auto &[id, count] : probes)
    require(count >= 4 && count <= 133,
            "all-down probing was not bounded by the circuit");
}

/// halfOpenStorm sends 50 network notifications in 10 s to an open circuit
/// (N22): at most one half-open probe is in flight, and early half-open
/// probes are at least 10 s apart.
void halfOpenStorm() {
  EndpointPolicy p;
  p.configure("one", trusted());
  for (const qint64 at : {0, 6000, 12000})
    probe(p, "one", at, Outcome::NetworkFailure, 1);
  require(p.health("one", 12000).failed, "circuit fixture did not open");
  std::optional<EndpointPolicy::Ticket> flight;
  std::vector<qint64> admitted;
  for (qint64 t = 12100; t < 80000; t += 100) {
    if (t < 70000 && t % 200 == 0)
      p.networkChanged(t);
    if (auto ticket = p.beginProbe("one", t)) {
      require(!flight, "two half-open probes were in flight");
      flight = ticket;
      admitted.push_back(t);
    }
    // Each half-open probe fails 100 ms later, before the next notification
    // (a result outliving a notification is stale and re-arms nothing).
    if (flight && t > admitted.back()) {
      p.observe(*flight, Outcome::NetworkFailure, t, 0, 1);
      flight.reset();
    }
  }
  require(admitted.size() >= 3, "too few half-open probes during the storm");
  for (size_t i = 1; i < admitted.size(); ++i)
    require(admitted[i] - admitted[i - 1] >= 10000,
            "early half-open probes closer than 10 s");
}

/// Shrinkable runs a randomized property: each failing seed is printed with
/// its minimal failing sequence (PLAN §20.5). Inputs: generate draws a
/// sequence; check returns the violated property or an empty string.
template <class Step>
void property(const char *name, unsigned seeds, int length,
              const std::function<std::vector<Step>(std::mt19937 &, int)> &generate,
              const std::function<std::string(const std::vector<Step> &)> &check,
              const std::function<std::string(const Step &)> &describe) {
  std::vector<unsigned> chosen;
  if (const auto fixed = qEnvironmentVariable("SYNCD_PROPERTY_SEED");
      !fixed.isEmpty())
    chosen.push_back(fixed.toUInt());
  else
    for (unsigned seed = 1; seed <= seeds; ++seed)
      chosen.push_back(seed);
  for (const auto seed : chosen) {
    std::mt19937 random(seed);
    auto steps = generate(random, length);
    auto failure = check(steps);
    if (failure.empty())
      continue;
    for (size_t i = 0; i < steps.size();) {
      auto fewer = steps;
      fewer.erase(fewer.begin() + qsizetype(i));
      if (auto again = check(fewer); !again.empty()) {
        steps = std::move(fewer);
        failure = std::move(again);
      } else
        ++i;
    }
    std::fprintf(stderr, "FAIL %s: %s; seed %u, minimal sequence of %zu:\n",
                 name, failure.c_str(), seed, steps.size());
    for (const auto &s : steps)
      std::fprintf(stderr, "  %s\n", describe(s).c_str());
    throw std::runtime_error(std::string(name) + " property failed");
  }
}

/// SchedulerStep is one random admission event over four folders in two
/// accounts.
struct SchedulerStep {
  enum Kind { Enqueue, Pause, Resume, Block, Unblock, Freeze, Take, Complete,
              Fail, Stale, Throttle, ClearRetry, Advance } kind;
  int folder = 0, reason = 0;
  qint64 dt = 0;
};
/// schedulerProperties runs random admission sequences (P01, P05, INV-01/13):
/// at most one engine cycle runs at a time, so no journal ever has a second
/// writer; a folder with a user pause is never admitted whatever recovery,
/// retry, throttle or unfreeze event arrives; stale completions change nothing.
void schedulerProperties() {
  const char *kinds[] = {"enqueue", "pause",  "resume",   "block",
                         "unblock", "freeze", "take",     "complete",
                         "fail",    "stale",  "throttle", "clearRetry",
                         "advance"};
  property<SchedulerStep>(
      "scheduler admission", 500, 300,
      [](std::mt19937 &random, int length) {
        const auto pick = [&](int n) {
          return std::uniform_int_distribution<int>(0, n - 1)(random);
        };
        std::vector<SchedulerStep> steps;
        for (int i = 0; i < length; ++i)
          steps.push_back({SchedulerStep::Kind(pick(13)), pick(4), pick(6),
                           qint64(pick(20000))});
        return steps;
      },
      [](const std::vector<SchedulerStep> &steps) -> std::string {
        Scheduler s;
        std::set<int> paused, frozen;
        bool freeze = false;
        std::optional<Scheduler::Run> active;
        std::vector<Scheduler::Run> old;
        const QString folders[] = {"f0", "f1", "f2", "f3"};
        for (int f = 0; f < 4; ++f)
          s.add(folders[f], f < 2 ? "a0" : "a1");
        qint64 now = 0;
        for (const auto &step : steps) {
          const auto &folder = folders[step.folder];
          switch (step.kind) {
          case SchedulerStep::Enqueue:
            s.enqueue(folder, Scheduler::Reason(step.reason), now);
            break;
          case SchedulerStep::Pause:
            s.block(folder, "userPause", true);
            paused.insert(step.folder);
            break;
          case SchedulerStep::Resume:
            s.block(folder, "userPause", false);
            paused.erase(step.folder);
            break;
          case SchedulerStep::Block:
            s.block(folder, "awaitingDecision", true);
            break;
          case SchedulerStep::Unblock:
            s.block(folder, "awaitingDecision", false);
            break;
          case SchedulerStep::Freeze:
            freeze = !freeze;
            s.freeze(freeze);
            break;
          case SchedulerStep::Take:
            if (const auto run = s.take(now)) {
              if (active)
                return "a second engine cycle was admitted";
              if (freeze)
                return "a cycle was admitted during global pause";
              const int index = run->folder.mid(1).toInt();
              if (paused.contains(index))
                return "a paused folder was admitted";
              active = run;
            }
            break;
          case SchedulerStep::Complete:
          case SchedulerStep::Fail:
            if (active) {
              const bool ok =
                  step.kind == SchedulerStep::Complete
                      ? s.complete(*active, step.reason % 2, now + step.dt)
                      : s.fail(*active, step.reason % 2 ? "NETWORK_UNAVAILABLE"
                                                         : "SYNC_FAILED",
                               now + step.dt);
              if (!ok)
                return "the running cycle's completion was refused";
              old.push_back(*active);
              active.reset();
            }
            break;
          case SchedulerStep::Stale:
            if (!old.empty() &&
                (s.complete(old[size_t(step.reason) % old.size()], true, 0) ||
                 s.fail(old[size_t(step.reason) % old.size()], "SYNC_FAILED",
                        0)))
              return "a stale completion was accepted";
            break;
          case SchedulerStep::Throttle:
            s.throttleAccount(step.folder < 2 ? "a0" : "a1", step.dt, now);
            break;
          case SchedulerStep::ClearRetry:
            s.clearRetry(folder);
            s.enqueue(folder, Scheduler::Reason::Recovery, now);
            break;
          case SchedulerStep::Advance:
            now += step.dt;
            break;
          }
        }
        return {};
      },
      [&kinds](const SchedulerStep &s) {
        return std::string(kinds[s.kind]) + " f" + std::to_string(s.folder) +
               " r" + std::to_string(s.reason) + " +" + std::to_string(s.dt);
      });
}

/// SwitchStep is one random switch-coordinator event.
struct SwitchStep {
  enum Kind { Begin, RunStart, Prove, Commit, Reconciled, Cancel } kind;
  int folder = 0, epochOffset = 0;
  unsigned flags = 0;
};
/// switchProperties runs random switch sequences (P03, P06, INV-05/06/08): a
/// cycle that started at an epoch ends at that epoch, because the epoch moves
/// only after every frozen folder proved its cycle over; it moves by exactly
/// one, only to a trusted target; a request or proof naming another epoch,
/// and a reconciliation of an older epoch, are refused.
void switchProperties() {
  const char *kinds[] = {"begin", "runStart", "prove",
                         "commit", "reconciled", "cancel"};
  property<SwitchStep>(
      "switch epochs", 500, 200,
      [](std::mt19937 &random, int length) {
        const auto pick = [&](int n) {
          return std::uniform_int_distribution<int>(0, n - 1)(random);
        };
        std::vector<SwitchStep> steps;
        for (int i = 0; i < length; ++i)
          steps.push_back({SwitchStep::Kind(pick(6)), pick(3), pick(3) - 1,
                           unsigned(pick(64)) | (pick(3) ? 63u : 0u)});
        return steps;
      },
      [](const std::vector<SwitchStep> &steps) -> std::string {
        SwitchCoordinator c;
        const QString folders[] = {"f0", "f1", "f2"};
        std::map<int, quint64> running; // folder -> epoch its cycle began at
        std::set<int> frozen, proved;
        QString pending;
        quint64 pendingEpoch = 0;
        for (const auto &step : steps) {
          const auto epoch = c.epoch();
          const auto stated = quint64(qint64(epoch) + step.epochOffset);
          const SwitchCoordinator::Target target{
              step.folder % 2 ? "lan" : "wan", bool(step.flags & 1),
              bool(step.flags & 2), bool(step.flags & 4), !(step.flags & 8)};
          const bool trusted = target.approved && target.identityVerified &&
                               target.tlsValid && !target.blocked;
          switch (step.kind) {
          case SwitchStep::Begin: {
            QSet<QString> members;
            for (const auto &f : folders)
              members.insert(f);
            const auto error = c.begin(target, stated, members);
            if (error.isEmpty()) {
              if (!pending.isEmpty() || stated != epoch || !trusted)
                return "an overlapping, stale or untrusted switch began";
              pending = target.id;
              pendingEpoch = epoch;
              frozen = {0, 1, 2};
              proved.clear();
            }
            break;
          }
          case SwitchStep::RunStart:
            // The engine admits no cycle of a folder that is quiescing.
            if (!frozen.contains(step.folder))
              running.emplace(step.folder, epoch);
            break;
          case SwitchStep::Prove: {
            const bool complete = (step.flags & 0x1f) == 0x1f;
            const bool accepted =
                c.observe(folders[step.folder],
                          {stated, complete, complete, complete,
                           bool(step.flags & 32) || complete, complete});
            if (accepted) {
              if (pending.isEmpty() || stated != epoch || !complete)
                return "an incomplete or other-epoch proof was accepted";
              proved.insert(step.folder);
              if (const auto run = running.find(step.folder);
                  run != running.end()) {
                if (run->second != epoch)
                  return "a cycle ended at a different epoch than it began";
                running.erase(run);
              }
            }
            break;
          }
          case SwitchStep::Commit: {
            if (c.commit(target).isEmpty()) {
              if (pending != target.id || !trusted || proved.size() != 3 ||
                  c.epoch() != pendingEpoch + 1 || c.active() != pending)
                return "the epoch moved without quiescence, trust or by one";
              for (const auto &[folder, began] : running)
                if (frozen.contains(folder))
                  return "a running cycle saw its epoch change";
              pending.clear();
              frozen.clear();
            } else if (c.epoch() != epoch) {
              return "a refused commit changed the epoch";
            }
            break;
          }
          case SwitchStep::Reconciled:
            if (c.reconciled(folders[step.folder], stated) && stated != epoch)
              return "an older epoch's reconciliation was accepted";
            break;
          case SwitchStep::Cancel:
            c.cancel();
            if (c.epoch() != epoch)
              return "cancel changed the epoch";
            pending.clear();
            frozen.clear();
            break;
          }
        }
        return {};
      },
      [&kinds](const SwitchStep &s) {
        return std::string(kinds[s.kind]) + " f" + std::to_string(s.folder) +
               " epoch" + (s.epochOffset < 0 ? "-1" : s.epochOffset ? "+1" : "") +
               " flags " + std::to_string(s.flags);
      });
}

/// originProperty draws 20000 URLs from hostile parts (P02, INV-03): every URL
/// the request gate admits is HTTPS on the approved host and port, without
/// user information or fragment, inside the installation path once decoded
/// and resolved; every plainly safe in-installation path is admitted.
void originProperty() {
  const auto base = canonicalServer("https://Cloud.Example:8443/nc");
  const char *schemes[] = {"https", "http", "HTTPS", "ftp"};
  const char *hosts[] = {"cloud.example", "CLOUD.EXAMPLE", "cloud.example.",
                         "evil.example", "cloud.example.evil", "127.0.0.1"};
  const char *ports[] = {":8443", "", ":443", ":8444"};
  const char *users[] = {"", "user@", "user:secret@"};
  const char *prefixes[] = {"/nc/", "/nc", "/ncx/", "/", "/NC/", "/nc//"};
  const char *parts[] = {"remote.php", "dav",  "files", "..",  ".",
                         "%2e%2e",     "a%2fb", "a%5cb", "a\\b", "%252f",
                         "x y",        "ü",    "",      "status.php"};
  const char *tails[] = {"", "?q=1", "#frag", "?a=%2f"};
  std::mt19937 random(qEnvironmentVariableIntValue("SYNCD_PROPERTY_SEED") + 1);
  // The first of every part is safe and drawn half the time, so admitted and
  // refused URLs both occur often.
  const auto pick = [&](int n) {
    return std::uniform_int_distribution<int>(0, 1)(random)
               ? 0
               : std::uniform_int_distribution<int>(0, n - 1)(random);
  };
  int admitted = 0;
  for (int i = 0; i < 20000; ++i) {
    QString text = QString(schemes[pick(4)]) + "://" + users[pick(3)] +
                   hosts[pick(6)] + ports[pick(4)] + prefixes[pick(6)];
    const int count = pick(5);
    for (int k = 0; k < count; ++k)
      text += QString(k ? "/" : "") + parts[pick(14)];
    text += tails[pick(4)];
    const QUrl url(text);
    if (!withinServer(base, url))
      continue;
    ++admitted;
    const auto decoded = url.path(QUrl::FullyDecoded);
    const auto resolved = QUrl("https://cloud.example:8443/")
                              .resolved(QUrl(url.path(QUrl::FullyEncoded)))
                              .path(QUrl::FullyDecoded);
    if (url.scheme() != "https" ||
        url.host().compare("cloud.example", Qt::CaseInsensitive) != 0 ||
        url.port(443) != 8443 || !url.userInfo().isEmpty() ||
        url.hasFragment() || !decoded.startsWith("/nc/") ||
        !resolved.startsWith("/nc/") || decoded.contains('\\')) {
      std::fprintf(stderr, "FAIL request gate admitted %s (seed %d, draw %d)\n",
                   qPrintable(text),
                   qEnvironmentVariableIntValue("SYNCD_PROPERTY_SEED") + 1, i);
      throw std::runtime_error("origin property failed");
    }
  }
  require(admitted > 100, "the generator produced too few admitted URLs");
  for (const auto *path : {"remote.php/dav/files/u/a.txt", "status.php",
                           "ocs/v2.php/cloud/user", "index.php/login/v2"})
    require(withinServer(base, QUrl("https://cloud.example:8443/nc/" +
                                    QString(path))),
            "a safe in-installation request was refused");
}

/// DurableStep is one random settings mutation, restart or receipt query.
struct DurableStep {
  enum Kind { Update, Replay, Conflict, Stale, Restart, Query } kind;
  int key = 0;
  bool paused = false;
};
/// durableProperty runs random mutation, replay, conflict, stale-revision and
/// restart sequences on a real Control database (P09, INV-12): every accepted
/// operation stays queryable with the same receipt after any number of
/// restarts, a replay never executes twice, a reused key with other
/// parameters conflicts, and the committed state survives.
void durableProperty() {
  const char *kinds[] = {"update", "replay", "conflict",
                         "stale",  "restart", "query"};
  property<DurableStep>(
      "durable receipts", 40, 60,
      [](std::mt19937 &random, int length) {
        const auto pick = [&](int n) {
          return std::uniform_int_distribution<int>(0, n - 1)(random);
        };
        std::vector<DurableStep> steps;
        for (int i = 0; i < length; ++i)
          steps.push_back({DurableStep::Kind(pick(6)), pick(8), bool(pick(2))});
        return steps;
      },
      [](const std::vector<DurableStep> &steps) -> std::string {
        QTemporaryDir dir;
        if (!dir.isValid())
          return "temporary database missing";
        const auto path = dir.filePath("control.sqlite");
        auto control = std::make_unique<Control>(path);
        bool hello = false, subscribed = false;
        const auto call = [&](const QString &method, const QJsonObject &params) {
          return control
              ->dispatch(QJsonObject{{"jsonrpc", "2.0"},
                                     {"id", 1},
                                     {"method", method},
                                     {"params", params}},
                         hello, subscribed)
              .toObject();
        };
        call("session.hello", {{"protocol", QJsonObject{{"major", 1}}}});
        struct Accepted {
          QString key, revision;
          bool paused;
          QJsonObject receipt;
        };
        std::vector<Accepted> accepted;
        quint64 revision = 0;
        bool paused = false;
        for (const auto &step : steps) {
          const auto revisionText = QString::number(revision);
          switch (step.kind) {
          case DurableStep::Update: {
            const auto key = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto reply = call("settings.update",
                                    {{"operationKey", key},
                                     {"expectedRevision", revisionText},
                                     {"allPaused", step.paused}});
            const auto receipt = reply.value("result").toObject();
            if (receipt.value("state") != "succeeded")
              return "a current mutation was not accepted";
            accepted.push_back({key, revisionText, step.paused, receipt});
            ++revision;
            paused = step.paused;
            break;
          }
          case DurableStep::Replay:
          case DurableStep::Conflict: {
            if (accepted.empty())
              break;
            const auto &a = accepted[size_t(step.key) % accepted.size()];
            const bool conflict = step.kind == DurableStep::Conflict;
            const auto reply =
                call("settings.update", {{"operationKey", a.key},
                                         {"expectedRevision", a.revision},
                                         {"allPaused", conflict != a.paused}});
            if (conflict
                    ? reply.value("error").toObject().value("data").toObject().value(
                          "reasonCode") != "OPERATION_KEY_CONFLICT"
                    : reply.value("result").toObject() != a.receipt)
              return conflict ? "a reused key with other parameters ran"
                              : "a replay did not return the original receipt";
            break;
          }
          case DurableStep::Stale: {
            const auto reply = call(
                "settings.update",
                {{"operationKey",
                  QUuid::createUuid().toString(QUuid::WithoutBraces)},
                 {"expectedRevision", QString::number(revision + 1 + step.key)},
                 {"allPaused", step.paused}});
            if (!reply.contains("error"))
              return "a stale revision was accepted";
            break;
          }
          case DurableStep::Restart:
            control.reset();
            control = std::make_unique<Control>(path);
            hello = true;
            if (control->projection()
                    .value("snapshot")
                    .toObject()
                    .value("daemon")
                    .toObject()
                    .value("allPaused")
                    .toBool() != paused)
              return "committed state was lost on restart";
            break;
          case DurableStep::Query: {
            for (const auto &a : accepted) {
              const auto reply = call(
                  "operations.get",
                  {{"operationId", a.receipt.value("operationId").toString()}});
              if (reply.value("result").toObject() != a.receipt)
                return "an accepted operation vanished or changed";
            }
            break;
          }
          }
        }
        return {};
      },
      [&kinds](const DurableStep &s) {
        return std::string(kinds[s.kind]) + " key" + std::to_string(s.key) +
               (s.paused ? " paused" : " running");
      });
}
} // namespace

/// main runs the switching contracts and properties on caller clocks and
/// temporary databases, without network access or sleeps.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    flapping();
    margins();
    independentFailures();
    allDown();
    halfOpenStorm();
    schedulerProperties();
    switchProperties();
    originProperty();
    durableProperty();
    std::puts("PASS N08 flapping and late results, N09 margin/stability/dwell/"
              "freshness/cooldown rows, N10 independent failures, N15 all-down "
              "bounded probing, N22 half-open storm; properties P01/P05 (500 "
              "seeds), P03/P06 (500 seeds), P02 (20000 URLs), P09 (40 seeds "
              "with restarts)");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
