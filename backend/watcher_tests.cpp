// SPDX-License-Identifier: GPL-3.0-only
#include "scheduler.h"
#include "watcher.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <stdexcept>

namespace {
/// require preserves filesystem contract assertions in release builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// write creates or modifies only files inside this fixture's temporary root.
void write(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::Append),
          "fixture open failed");
  require(file.write("change") == 6, "fixture write failed");
}
/// awaitChange waits for a real Windows notification, with a bounded failure
/// path.
void awaitChange(RootWatcher &watcher, const std::function<void()> &action,
                 const std::function<void(bool)> &observe = {}) {
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  bool notified = false;
  watcher.changed = [&](bool audit) {
    notified = true;
    if (observe)
      observe(audit);
    loop.quit();
  };
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  action();
  deadline.start(5000);
  loop.exec();
  watcher.changed = {};
  require(notified, "recursive notification timed out");
}
/// recursive verifies new subdirectories, edits, rename/delete, pause
/// preservation and dirty work arriving during an active cycle (INV-07/09).
void recursive() {
  QTemporaryDir root;
  require(root.isValid(), "temporary root unavailable");
  RootWatcher watcher(root.path());
  require(watcher.available(), "native watcher unavailable");
  const auto nested = root.path() + "/new/nested";
  awaitChange(watcher, [&] { require(QDir().mkpath(nested), "mkdir failed"); });
  Scheduler scheduler;
  scheduler.add("folder", "account");
  scheduler.block("folder", "userPaused", true);
  const auto file = nested + "/file";
  awaitChange(
      watcher, [&] { write(file); },
      [&](bool audit) {
        scheduler.enqueue(
            "folder",
            audit ? Scheduler::Reason::Audit : Scheduler::Reason::Local, 0);
      });
  require(!scheduler.take(10000), "notification bypassed user pause");
  scheduler.block("folder", "userPaused", false);
  const auto run = scheduler.take(10000);
  require(run.has_value(), "paused notification was lost");
  awaitChange(
      watcher, [&] { write(file); },
      [&](bool audit) {
        scheduler.enqueue(
            "folder",
            audit ? Scheduler::Reason::Audit : Scheduler::Reason::Local, 11000);
      });
  require(scheduler.complete(*run, true, 11000),
          "active generation completion rejected");
  require(scheduler.pending("folder"),
          "completion discarded concurrent local change");
  awaitChange(watcher, [&] {
    require(QFile::rename(file, file + "-renamed"), "rename failed");
  });
  awaitChange(watcher, [&] {
    require(QFile::remove(file + "-renamed"), "remove failed");
  });
}
/// overflow fills the bounded Windows buffer before dispatch and requires an
/// audit.
void overflow() {
  QTemporaryDir root;
  RootWatcher watcher(root.path());
  require(watcher.available(), "overflow watcher unavailable");
  bool audit = false;
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  watcher.changed = [&](bool lost) {
    audit |= lost;
    if (audit)
      loop.quit();
  };
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  for (int i = 0; i < 600; ++i)
    write(root.path() + '/' + QString(80, 'x') + QString::number(i));
  deadline.start(5000);
  loop.exec();
  watcher.changed = {};
  require(audit, "overflow did not request full audit");
  awaitChange(watcher, [&] { write(root.path() + "/after-overflow"); });
}
/// teardown verifies pending cancellation and unavailable-root fallback without
/// callbacks escaping the subscription's lifetime.
void teardown() {
  QTemporaryDir root;
  int callbacks = 0;
  for (int i = 0; i < 20; ++i) {
    RootWatcher watcher(root.path());
    require(watcher.available(), "teardown watcher unavailable");
    watcher.changed = [&](bool) { ++callbacks; };
    if (i % 2)
      write(root.path() + "/pending");
  }
  QCoreApplication::processEvents();
  require(callbacks == 0, "callback escaped destroyed subscription");
  RootWatcher absent(root.path() + "/absent");
  require(!absent.available(), "missing root reported armed watcher");
}
} // namespace

/// main exercises real local Windows subtree events without server or shell
/// integration.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    recursive();
    overflow();
    teardown();
    std::puts("PASS recursive changes, pause/generation isolation, overflow "
              "and teardown");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
