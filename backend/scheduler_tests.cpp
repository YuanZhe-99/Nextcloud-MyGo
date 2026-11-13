// SPDX-License-Identifier: GPL-3.0-only
#include "scheduler.h"
#include <cstdio>
#include <stdexcept>

namespace {
/// require keeps contract checks enabled in optimized builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

/// generations proves changes during a cycle survive completion and stale
/// completion is inert (§10.3).
void generations() {
  Scheduler s;
  s.add("one", "account");
  s.enqueue("one", Scheduler::Reason::Recovery, 0);
  const auto first = s.take(0);
  require(first.has_value(), "recovery not admitted");
  require(!s.take(0), "two cycles admitted");
  s.enqueue("one", Scheduler::Reason::Local, 100);
  require(s.complete(*first, true, 0), "completion refused");
  require(s.pending("one"), "in-flight change lost");
  require(!s.take(2099), "local debounce ignored");
  auto second = s.take(2100);
  require(second.has_value(), "new generation lost");
  require(!s.complete(*first, true, 0), "late completion accepted");
  require(s.complete(*second, false, 5000), "failure not recorded");
  s.enqueue("one", Scheduler::Reason::User, 2200);
  require(!s.take(4999), "manual request bypassed failure backoff");
  auto retry = s.take(5000);
  require(retry.has_value(), "failed work lost");
  require(s.complete(*retry, true, 0) && !s.pending("one"),
          "success left false dirty state");
}

/// blockers proves resume never clears independent blockers or a folder's own
/// pause (INV-13/14).
void blockers() {
  Scheduler s;
  s.add("one", "account");
  s.enqueue("one", Scheduler::Reason::User, 0);
  s.block("one", "userPaused", true);
  s.block("one", "identityMismatch", true);
  s.freeze(true);
  require(!s.take(0), "global pause ignored");
  s.freeze(false);
  require(!s.take(0), "resume all erased local pause");
  s.block("one", "userPaused", false);
  require(!s.take(0), "resume erased identity block");
  s.block("one", "identityMismatch", false);
  require(s.take(0).has_value(), "unblocked work disappeared");
}

/// fairness proves account/folder rotation and a ten-second cap on continuous
/// watcher debounce.
void fairness() {
  Scheduler s;
  s.add("a", "account-a");
  s.add("b", "account-b");
  s.add("c", "account-a");
  s.enqueue("a", Scheduler::Reason::Recovery, 0);
  s.enqueue("b", Scheduler::Reason::Audit, 0);
  s.enqueue("c", Scheduler::Reason::Remote, 0);
  auto first = s.take(0);
  require(first && first->folder == "a", "initial recovery priority lost");
  s.enqueue("a", Scheduler::Reason::Recovery, 1);
  s.complete(*first, true, 0);
  auto second = s.take(1);
  require(second && second->folder == "b", "other account starved");
  s.complete(*second, true, 0);
  auto third = s.take(1);
  require(third.has_value(), "account queue lost");
  s.complete(*third, true, 0);
  auto fourth = s.take(1);
  require(fourth && fourth->folder != third->folder,
          "same-account folder starved");
  s.complete(*fourth, true, 0);
  s.enqueue("a", Scheduler::Reason::Local, 100);
  for (qint64 now = 1100; now < 10100; now += 1000) {
    s.enqueue("a", Scheduler::Reason::Local, now);
    require(!s.take(now), "local debounce too short");
  }
  require(s.take(10100).has_value(), "continuous changes postponed forever");
}
/// recovery proves transient preflight errors and server constraints retry
/// while safety blocks and pauses survive (INV-10/13/14). Notes: a full server
/// quota once blocked its folder for good and held a switch open (2026-11-12).
void recovery() {
  for (const auto &reason :
       {"NETWORK_UNAVAILABLE", "LOCAL_ROOT_UNAVAILABLE",
        "REMOTE_ROOT_UNAVAILABLE", "DISCOVERY_INCOMPLETE", "REMOTE_LOCKED",
        "SERVER_THROTTLED", "REMOTE_QUOTA_EXCEEDED", "PERMISSION_DENIED",
        "TLS_ERROR",
        "ENDPOINT_IDENTITY_MISMATCH", "AUTH_REQUIRED", "FOLDER_ROOT_CHANGED",
        "JOURNAL_ERROR", "UNSUPPORTED_FOLDER_MODE",
        "DELETION_CONFIRMATION_REQUIRED"}) {
    Scheduler s;
    s.add("folder", "account");
    s.enqueue("folder", Scheduler::Reason::Recovery, 0);
    const auto run = s.take(0);
    require(run.has_value(), "initial recovery missing");
    require(s.fail(*run, reason, 1000), "current failure rejected");
    require(s.pending("folder") && !s.take(999), "failed work or backoff lost");
    s.block("folder", "userPaused", true);
    require(!s.take(1000), "failure retry bypassed user pause");
    s.block("folder", "userPaused", false);
    const bool transient = QString(reason).endsWith("UNAVAILABLE") ||
                           QString(reason) == "DISCOVERY_INCOMPLETE" ||
                           QString(reason) == "REMOTE_LOCKED" ||
                           QString(reason) == "SERVER_THROTTLED" ||
                           QString(reason) == "REMOTE_QUOTA_EXCEEDED";
    const auto retry = s.take(1000);
    require(retry.has_value() == transient,
            "transient recovery or safety gate incorrect");
    if (retry) {
      require(!s.fail(*run, "TLS_ERROR", 2000), "late failure accepted");
      require(s.complete(*retry, true, 0), "retry completion rejected");
      s.enqueue("folder", Scheduler::Reason::User, 2000);
      require(s.take(2000).has_value(),
              "late failure installed a safety blocker");
    }
  }
}
/// credentialRenewal clears only credential failures, preserving TLS and user
/// gates (INV-03/13/14).
void credentialRenewal() {
  Scheduler s;
  s.add("folder", "account");
  s.enqueue("folder", Scheduler::Reason::Recovery, 0);
  auto run = s.take(0);
  require(run && s.fail(*run, "AUTH_REQUIRED", 0),
          "authentication failure not recorded");
  s.block("folder", "TLS_ERROR", true);
  s.block("folder", "userPaused", true);
  s.block("folder", "AUTH_REQUIRED", false);
  require(!s.take(0), "credential renewal erased another gate");
  s.block("folder", "userPaused", false);
  require(!s.take(0), "credential renewal erased TLS failure");
  s.block("folder", "TLS_ERROR", false);
  require(s.take(0).has_value(),
          "verified credential renewal could not recover pending work");
}
/// accountThrottle proves Retry-After survives every wake source, new mappings,
/// account rebinding work and independent pause changes without blocking other
/// accounts (PLAN §12.1, N18, INV-13/14).
void accountThrottle() {
  Scheduler s;
  s.add("primary", "account");
  s.add("other", "other-account");
  s.throttleAccount("account", 90000, 1000);
  s.add("alternate", "account");
  for (const auto reason :
       {Scheduler::Reason::User, Scheduler::Reason::Recovery,
        Scheduler::Reason::Local, Scheduler::Reason::Remote,
        Scheduler::Reason::Audit, Scheduler::Reason::FollowUp}) {
    s.enqueue("primary", reason, 2000);
    s.enqueue("alternate", reason, 2000);
  }
  s.enqueue("other", Scheduler::Reason::User, 2000);
  auto other = s.take(2000);
  require(other && other->folder == "other", "throttle leaked across accounts");
  require(s.complete(*other, true, 0), "other account completion failed");
  s.freeze(true);
  s.freeze(false);
  s.block("primary", "userPaused", true);
  s.block("primary", "switchInProgress", true);
  s.block("primary", "switchInProgress", false);
  s.throttleAccount("account", 1000, 3000);
  require(!s.take(90999), "wake or shorter response bypassed account deadline");
  auto alternate = s.take(91000);
  require(alternate && alternate->folder == "alternate",
          "deadline did not release alternate work or erased folder pause");
  require(s.complete(*alternate, true, 0), "alternate completion failed");
  require(!s.take(92000), "throttle expiry erased user pause");
  s.block("primary", "userPaused", false);
  auto primary = s.take(92000);
  require(primary && primary->folder == "primary", "throttled work was lost");
  require(s.complete(*primary, true, 0), "primary completion failed");
  s.throttleAccount("account", 0, 100000);
  s.enqueue("primary", Scheduler::Reason::User, 100000);
  require(!s.take(159999) && s.take(160000).has_value(),
          "missing Retry-After lacked finite conservative fallback");

  Scheduler longDelay;
  longDelay.add("folder", "account");
  longDelay.enqueue("folder", Scheduler::Reason::User, 0);
  longDelay.throttleAccount("account", -1, 0);
  require(!longDelay.take(604800001),
          "unrepresentable server delay was silently shortened");
}
/// rebound proves a retry delay earned on an unreachable address no longer
/// holds back the folder once the account moved, while pauses and the
/// account's server throttle still do (PLAN §9.3 S5; sleep-resume regression).
void rebound() {
  Scheduler s;
  s.add("folder", "account");
  s.enqueue("folder", Scheduler::Reason::Recovery, 0);
  auto failed = s.take(0);
  require(failed && s.fail(*failed, "NETWORK_UNAVAILABLE", 60000),
          "unreachable-address failure fixture failed");
  s.enqueue("folder", Scheduler::Reason::Recovery, 15000);
  require(!s.take(15000), "failure backoff was not applied");
  s.block("folder", "userPaused", true);
  s.clearRetry("folder");
  require(!s.take(15000), "clearing the backoff erased a pause");
  s.block("folder", "userPaused", false);
  s.throttleAccount("account", 120000, 15000);
  require(!s.take(15000), "clearing the backoff bypassed the account throttle");
  require(s.take(135000).has_value(), "pending work lost after the throttle");
  Scheduler moved;
  moved.add("folder", "account");
  moved.enqueue("folder", Scheduler::Reason::Recovery, 0);
  auto again = moved.take(0);
  require(again && moved.fail(*again, "NETWORK_UNAVAILABLE", 60000),
          "second failure fixture failed");
  moved.clearRetry("folder");
  require(moved.take(15000).has_value(),
          "the moved account waited out the old address's backoff");
}
} // namespace

/// main verifies the pure scheduling contract without wall-clock sleeps or
/// filesystem fixtures.
int main() {
  try {
    generations();
    blockers();
    fairness();
    recovery();
    credentialRenewal();
    accountThrottle();
    rebound();
    std::puts("PASS scheduling generations, blockers, fairness, debounce and "
              "retry after an address change");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
