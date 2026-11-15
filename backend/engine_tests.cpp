// SPDX-License-Identifier: GPL-3.0-only
#include "engine_runtime.h"
#include "syncfileitem.h"
#include <QCoreApplication>
#include <cstdio>
#include <stdexcept>

namespace {
/// require keeps contract checks enabled in optimized builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

/// failureFilter: only a new, downloaded virtual file of an on-demand folder
/// that failed with a normal error is a placeholder another process may have
/// created first; a kept conflict copy or an ignored file fails nothing, and
/// everything else is a failure (EngineAdapter::Result::restarted).
void failureFilter() {
  using F = EngineAdapter::Failure;
  using I = OCC::SyncFileItem;
  struct Case {
    const char *name;
    bool onDemand;
    SyncInstructions instruction;
    I::Direction direction;
    ItemType type;
    I::Status status;
    F expected;
  };
  const Case cases[] = {
      {"placeholder", true, CSYNC_INSTRUCTION_NEW, I::Down, ItemTypeVirtualFile,
       I::NormalError, F::placeholder},
      {"full-file folder", false, CSYNC_INSTRUCTION_NEW, I::Down,
       ItemTypeVirtualFile, I::NormalError, F::other},
      {"existing file", true, CSYNC_INSTRUCTION_SYNC, I::Down,
       ItemTypeVirtualFile, I::NormalError, F::other},
      {"upload", true, CSYNC_INSTRUCTION_NEW, I::Up, ItemTypeVirtualFile,
       I::NormalError, F::other},
      {"hydrated file", true, CSYNC_INSTRUCTION_NEW, I::Down, ItemTypeFile,
       I::NormalError, F::other},
      {"directory", true, CSYNC_INSTRUCTION_NEW, I::Down, ItemTypeDirectory,
       I::NormalError, F::other},
      {"soft error", true, CSYNC_INSTRUCTION_NEW, I::Down, ItemTypeVirtualFile,
       I::SoftError, F::other},
      {"fatal error", true, CSYNC_INSTRUCTION_NEW, I::Down, ItemTypeVirtualFile,
       I::FatalError, F::other},
      {"conflict", true, CSYNC_INSTRUCTION_NEW, I::Down, ItemTypeVirtualFile,
       I::Conflict, F::none},
      {"conflict, full-file", false, CSYNC_INSTRUCTION_SYNC, I::Up,
       ItemTypeFile, I::Conflict, F::none},
      {"ignored", true, CSYNC_INSTRUCTION_IGNORE, I::None, ItemTypeFile,
       I::FileIgnored, F::none},
  };
  for (const auto &c : cases) {
    I item;
    item._instruction = c.instruction;
    item._direction = c.direction;
    item._type = c.type;
    item._status = c.status;
    require(EngineAdapter::failure(item, c.onDemand) == c.expected, c.name);
  }
}

/// excusedCycles: a cycle whose only failures are new placeholders held for
/// their own file IDs ends restarted, once in a row; anything else stays
/// SYNC_FAILED or what it was (EngineAdapter::excused). Notes: regression
/// cover for the first-cycle race with Explorer (2026-11-14), whose
/// once-in-a-row bound had never run.
void excusedCycles() {
  const auto held = [](const QString &path, const QByteArray &fileId) {
    return (path == "a.bin" && fileId == "id-a") ||
           (path == "b.bin" && fileId == "id-b");
  };
  const QList<EngineAdapter::Populated> both{{"a.bin", "id-a"},
                                             {"b.bin", "id-b"}};
  struct Case {
    const char *name;
    QString reason;
    bool stopping, otherFailure, before;
    QList<EngineAdapter::Populated> populated;
    bool expected;
  };
  const Case cases[] = {
      {"all failures excused", "SYNC_FAILED", false, false, false, both, true},
      {"one other failure", "SYNC_FAILED", false, true, false, both, false},
      {"placeholder not held", "SYNC_FAILED", false, false, false,
       {{"a.bin", "id-a"}, {"c.bin", "id-c"}}, false},
      {"held for another file ID", "SYNC_FAILED", false, false, false,
       {{"a.bin", "id-b"}}, false},
      {"second excused cycle in a row", "SYNC_FAILED", false, false, true,
       both, false},
      {"stopping", "SYNC_FAILED", true, false, false, both, false},
      {"cancelled", "CANCELLED", true, false, false, both, false},
      {"other reason", "DELETION_CONFIRMATION_REQUIRED", false, false, false,
       both, false},
      {"no failed placeholder", "SYNC_FAILED", false, false, false, {},
       false},
  };
  for (const auto &c : cases)
    require(EngineAdapter::excused(c.reason, c.stopping, c.otherFailure,
                                   c.before, c.populated,
                                   held) == c.expected,
            c.name);
  // The caller feeds the result back as the next cycle's flag: the second
  // racing cycle in a row is reported, a third is excused again.
  bool before = false;
  const bool expected[] = {true, false, true, false};
  for (const bool e : expected) {
    before = EngineAdapter::excused("SYNC_FAILED", false, false, before, both,
                                    held);
    require(before == e, "the once-in-a-row bound did not alternate");
  }
}

/// restartFollowUp: after a restarted cycle the follow-up is admitted at
/// exactly now + restartDelayMs and not before, the failure count stays and no
/// backoff or blocker applies; a real failure afterwards backs off from that
/// count as usual (EngineRuntime::settle, controllable clock).
void restartFollowUp() {
  Scheduler s;
  s.add("folder", "account");
  s.enqueue("folder", Scheduler::Reason::Recovery, 0);
  auto run = s.take(0);
  require(run.has_value(), "recovery not admitted");
  unsigned failures = 3; // a backoff of 16 s would apply to a failure
  EngineAdapter::Result restarted;
  restarted.restarted = restarted.followUp = true;
  qint64 now = 1000;
  EngineRuntime::settle(s, *run, restarted, failures, now);
  require(failures == 3, "a restarted cycle changed the failure count");
  require(s.blockers("folder").isEmpty(), "a restarted cycle added a blocker");
  require(s.pending("folder"), "a restarted cycle's work was dropped");
  require(!s.take(now + EngineRuntime::restartDelayMs - 1),
          "the follow-up ran before the restart delay");
  now += EngineRuntime::restartDelayMs;
  run = s.take(now);
  require(run.has_value(), "the follow-up waited past the restart delay");
  EngineAdapter::Result failed;
  failed.reason = "SYNC_FAILED";
  EngineRuntime::settle(s, *run, failed, failures, now);
  require(failures == 4, "a real failure was not counted");
  require(s.blockers("folder").isEmpty(), "SYNC_FAILED became a blocker");
  require(!s.take(now + 15999), "a real failure did not back off");
  run = s.take(now + 16000);
  require(run.has_value(), "the retry waited past its backoff");
  EngineAdapter::Result success;
  success.success = true;
  EngineRuntime::settle(s, *run, success, failures, now + 16000);
  require(failures == 0 && !s.pending("folder"),
          "success did not reset the failure count");
}

/// pausedFollowUp: neither a folder's own pause nor a global pause is undone
/// by a restarted cycle's follow-up; the work waits and runs after resuming
/// (INV-13/14).
void pausedFollowUp() {
  Scheduler s;
  s.add("folder", "account");
  s.enqueue("folder", Scheduler::Reason::Recovery, 0);
  auto run = s.take(0);
  require(run.has_value(), "recovery not admitted");
  s.block("folder", "userPaused", true);
  unsigned failures = 0;
  EngineAdapter::Result restarted;
  restarted.restarted = restarted.followUp = true;
  EngineRuntime::settle(s, *run, restarted, failures, 0);
  require(!s.take(600000), "the follow-up resumed a paused folder");
  require(s.blockers("folder") == QSet<QString>{"userPaused"},
          "the follow-up changed the folder's blockers");
  s.block("folder", "userPaused", false);
  run = s.take(600000);
  require(run.has_value(), "the follow-up was lost while paused");
  s.freeze(true);
  EngineRuntime::settle(s, *run, restarted, failures, 600000);
  require(!s.take(700000), "the follow-up ran during a global pause");
  s.freeze(false);
  require(s.take(700000).has_value(), "the follow-up was lost while frozen");
}
} // namespace

/// main runs the engine adapter's and runtime's pure cycle decisions without a
/// server, an engine or a sync root.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    failureFilter();
    excusedCycles();
    restartFollowUp();
    pausedFollowUp();
    std::puts("PASS failure filter; restarted classification; restart "
              "follow-up on a controllable clock; pause kept");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
