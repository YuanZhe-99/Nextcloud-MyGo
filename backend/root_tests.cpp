// SPDX-License-Identifier: GPL-3.0-only
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "root.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>
#include <windows.h>

namespace {
/// require keeps root ownership checks active in release builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
/// refused checks ownership refusal without creating or deleting a user
/// journal.
bool refused(const QString &path) {
  try {
    RootLease duplicate(path);
    return false;
  } catch (const std::exception &e) {
    return QByteArray(e.what()) == "FOLDER_OWNED";
  }
}

/// line reads one bounded child protocol line without leaving a process alive
/// on timeout; each caller owns child cleanup with a scope guard.
QByteArray line(QProcess &child) {
  while (!child.canReadLine()) {
    if (!child.waitForReadyRead(10000))
      return {};
  }
  return child.readLine().trimmed();
}

/// stop reaps only the parent's own child, including assertion and crash paths.
void stop(QProcess &child) {
  if (child.state() != QProcess::NotRunning) {
    child.kill();
    child.waitForFinished(10000);
  }
}

/// startContender reserves a child that waits for its parent's simultaneous
/// admission gate. Distinct profile settings must not partition root ownership.
void startContender(QProcess &child, const QString &path,
                    const QString &profile) {
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("NEXTCLOUD_MYGO_PROFILE", profile);
  child.setProcessEnvironment(environment);
  child.start(QCoreApplication::applicationFilePath(), {"--contend", path});
  require(child.waitForStarted(10000) && line(child) == "READY",
          "root contender did not reach parent-owned gate");
}

/// race verifies S19 simultaneous equal/nested exclusion and independent
/// sibling admission while successful children retain their leases until
/// released.
void race(const QString &first, const QString &second, bool siblings) {
  QProcess a, b;
  const auto cleanup = qScopeGuard([&] {
    stop(a);
    stop(b);
  });
  startContender(a, first, "root-fixture-a");
  startContender(b, second, "root-fixture-b");
  a.write("go\n");
  b.write("go\n");
  require(a.waitForBytesWritten(10000) && b.waitForBytesWritten(10000),
          "root contender gate write failed");
  const auto resultA = line(a), resultB = line(b);
  require((resultA == "ACQUIRED" || resultA == "REFUSED") &&
              (resultB == "ACQUIRED" || resultB == "REFUSED"),
          "root contender returned an unexpected failure");
  const auto admitted = (resultA == "ACQUIRED") + (resultB == "ACQUIRED");
  require(admitted == (siblings ? 2 : 1),
          siblings ? "S19 unrelated siblings were serialized"
                   : "S19 simultaneous overlapping roots admitted");
  for (auto *child : {&a, &b}) {
    if (child->state() != QProcess::NotRunning) {
      child->write("release\n");
      child->closeWriteChannel();
      require(child->waitForFinished(10000), "root contender did not exit");
    }
    require(child->exitStatus() == QProcess::NormalExit &&
                child->exitCode() == 0,
            "root contender cleanup failed");
  }
}

/// contend waits at the parent's gate and retains a successfully admitted root
/// until release, so two sequential successes cannot masquerade as a race pass.
int contend(const QString &path) {
  std::puts("READY");
  std::fflush(stdout);
  char command[16]{};
  if (!std::fgets(command, sizeof(command), stdin))
    return 2;
  try {
    RootLease lease(path);
    std::puts("ACQUIRED");
    std::fflush(stdout);
    return std::fgets(command, sizeof(command), stdin) ? 0 : 2;
  } catch (const std::exception &e) {
    if (QByteArray(e.what()) != "FOLDER_OWNED")
      return 2;
    std::puts("REFUSED");
    std::fflush(stdout);
    return 0;
  }
}
} // namespace

/// main verifies equal/nested ownership across processes/profiles, simultaneous
/// admission, crash cleanup, replacement prevention and upstream ownership
/// (S19, INV-01/02/10). --expect-unsupported inspects only a parent-owned
/// fixture.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().size() == 3 && app.arguments().at(1) == "--expect-owned")
    return refused(app.arguments().at(2)) ? 0 : 1;
  if (app.arguments().size() == 3 && app.arguments().at(1) == "--contend")
    return contend(app.arguments().at(2));
  if (app.arguments().size() == 3 &&
      app.arguments().at(1) == "--expect-unsupported") {
    try {
      RootLease lease(app.arguments().at(2));
      return 1;
    } catch (const std::exception &e) {
      return QByteArray(e.what()) == "UNSUPPORTED_FOLDER_MODE" ? 0 : 2;
    }
  }
  try {
    QTemporaryDir fixture;
    require(fixture.isValid(), "fixture directory missing");
    const auto root = fixture.filePath("root");
    require(QDir().mkpath(root + "/child") && QDir().mkpath(root + "/sibling"),
            "nested fixtures missing");
    QString identity;
    {
      RootLease lease(root);
      identity = lease.identity();
      require(lease.verify(), "fresh root invalid");
      require(lease.validateTree(), "ordinary NTFS tree refused");
      {
        // S17 regression: a file whose path passes MAX_PATH is an ordinary
        // file for upstream; the scan refused it as an unsupported tree.
        QString deep = root + "/child";
        for (int i = 0; i < 6; ++i)
          deep += '/' + QString(60, QChar('d' + i));
        QFile file(deep + "/long-" + QString(60, 'f') + ".txt");
        require(QDir().mkpath(deep) && file.open(QIODevice::WriteOnly) &&
                    file.write("x") == 1 && file.fileName().size() > 400,
                "long-path fixture missing");
        file.close();
        require(lease.validateTree(), "S17 tree with a long path refused");
        require(QDir(root + "/child/" + QString(60, 'd')).removeRecursively(),
                "long-path fixture not removed");
      }
      require(refused(root),
              "same-thread recursive mutex allowed a second writer");
      require(refused(root + "/child"), "nested mapping accepted");
      require(refused(fixture.path()), "ancestor mapping accepted");
      QProcess child;
      const auto cleanup = qScopeGuard([&] { stop(child); });
      child.start(QCoreApplication::applicationFilePath(),
                  {"--expect-owned", root});
      require(child.waitForFinished(10000) &&
                  child.exitStatus() == QProcess::NormalExit &&
                  child.exitCode() == 0,
              "cross-process writer admitted");
      child.start(QCoreApplication::applicationFilePath(),
                  {"--expect-owned", root + "/child"});
      require(child.waitForFinished(10000) &&
                  child.exitStatus() == QProcess::NormalExit &&
                  child.exitCode() == 0,
              "S19 cross-process descendant root admitted");
      child.start(QCoreApplication::applicationFilePath(),
                  {"--expect-owned", fixture.path()});
      require(child.waitForFinished(10000) &&
                  child.exitStatus() == QProcess::NormalExit &&
                  child.exitCode() == 0,
              "S19 cross-process ancestor root admitted");
      require(!QDir().rename(root, fixture.filePath("moved")),
              "leased root could be replaced");
      const auto moved = fixture.path() + "-moved";
      const bool ancestorMoved = QDir().rename(fixture.path(), moved);
      if (ancestorMoved)
        require(QDir().rename(moved, fixture.path()),
                "owned ancestor fixture could not be restored");
      require(!ancestorMoved, "leased root's ancestor could be replaced");
    }
    {
      RootLease reopened(root);
      require(reopened.identity() == identity, "stable root identity changed");
    }
    {
      // A malformed kernel namespace entry must not be interpreted as an empty
      // ownership registry. Only the fixture's identity-named object is
      // touched.
      const auto name =
          "Global\\Nextcloud-MyGo-root-" +
          QString::fromLatin1(QCryptographicHash::hash(
                                  identity.toUtf8(), QCryptographicHash::Sha256)
                                  .toHex());
      auto collision = CreateEventW(nullptr, TRUE, FALSE,
                                    reinterpret_cast<LPCWSTR>(name.utf16()));
      require(collision != nullptr, "kernel collision fixture unavailable");
      const auto cleanup = qScopeGuard([&] { CloseHandle(collision); });
      require(refused(root) && refused(root + "/child"),
              "malformed kernel ownership entry did not fail closed");
    }
    for (int i = 0; i < 12; ++i) {
      race(root, root.toUpper(), false);
      race(root, root + "/child", false);
      race(root + "/child", root, false);
      race(root + "/child", root + "/sibling", true);
    }
    {
      QProcess child;
      const auto cleanup = qScopeGuard([&] { stop(child); });
      startContender(child, root + "/child", "root-fixture-crash");
      child.write("go\n");
      require(child.waitForBytesWritten(10000) && line(child) == "ACQUIRED",
              "crash fixture did not acquire root");
      require(refused(root), "child lease did not exclude its parent");
      stop(child);
      RootLease recovered(root);
      require(recovered.verify(), "crashed descendant left stale ownership");
    }
    {
      RootLease recovered(root + "/child");
      require(recovered.verify(), "released ancestor left stale ownership");
    }
    QFile foreign(root + "/.sync_fixture.db");
    require(foreign.open(QIODevice::WriteOnly),
            "fixture journal creation failed");
    foreign.close();
    require(refused(root) && refused(root + "/child"),
            "existing upstream journal ignored");
    std::puts(
        "PASS S19/INV-01/02/10 NTFS identity, cross-process/profile nested "
        "exclusion, 48 simultaneous races, siblings, crash cleanup, "
        "malformed ownership refusal, replacement prevention and "
        "existing journal refusal");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
