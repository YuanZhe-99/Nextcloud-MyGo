// SPDX-License-Identifier: GPL-3.0-only
#include "startup.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {
/// require keeps product assertions enabled in optimized test builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// bytes reads exact fixture bytes for preservation checks.
QByteArray bytes(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "fixture read failed");
  return file.readAll();
}
/// write creates only disposable fixture data.
void write(const QString &path, const QByteArray &data) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "fixture write failed");
  require(file.write(data) == data.size(), "fixture short write");
}
/// crash executes a real child that exits without clean shutdown or
/// destructors.
void crash(const QString &directory, qint64 now, bool stop = false) {
  QProcess child;
  child.start(
      QCoreApplication::applicationFilePath(),
      {stop ? "--stopped-crash" : "--crash", directory, QString::number(now)});
  require(child.waitForStarted(10000) && child.waitForFinished(10000),
          "crash child did not finish");
  require(child.exitCode() == 17, "crash child setup failed");
}
/// storm proves six actual prior unclean exits suppress engine admission (R02).
void storm() {
  QTemporaryDir root;
  require(root.isValid(), "temporary profile failed");
  for (int i = 0; i < 6; ++i)
    crash(root.path(), 1000000 + i * 1000);
  Startup startup(root.path(), 1006000);
  require(startup.decision().priorUnclean && startup.decision().crashes == 6,
          "actual prior crashes were not counted");
  require(startup.decision().diagnostic &&
              !startup.engineAllowed(1006000 + 3600000),
          "R02: repeated crashes did not suppress engine admission");
  require(startup.finishClean(), "diagnostic shutdown could not be recorded");
}
/// cleanAndBackoff checks clean shutdown, fixed delays and window expiry (R02).
void cleanAndBackoff() {
  QTemporaryDir root;
  {
    Startup startup(root.path(), 1000);
    require(!startup.decision().priorUnclean && startup.engineAllowed(1000),
            "first start was treated as a crash");
    require(startup.finishClean() && startup.finishClean(),
            "clean shutdown failed");
    require(!startup.engineAllowed(2000), "finished lifetime admitted engine");
  }
  {
    Startup startup(root.path(), 2000);
    require(!startup.decision().priorUnclean && startup.decision().crashes == 0,
            "clean shutdown counted as a crash");
    require(startup.finishClean(), "second clean shutdown failed");
  }
  constexpr qint64 delays[] = {2000, 5000, 15000, 60000, 60000};
  for (int i = 0; i < 5; ++i) {
    crash(root.path(), 3000 + i * 1000);
    Startup startup(root.path(), 3500 + i * 1000);
    require(startup.decision().crashes == i + 1 &&
                startup.decision().delayMs == delays[i],
            "crash restart backoff is not bounded 2/5/15/60 seconds");
    require(!startup.engineAllowed(3500 + i * 1000 + delays[i] - 1) &&
                startup.engineAllowed(3500 + i * 1000 + delays[i]),
            "restart backoff admitted too early or never expired");
    require(startup.finishClean(), "backoff clean finish failed");
  }
  {
    Startup startup(root.path(), 700000);
    require(startup.decision().crashes == 0 && startup.engineAllowed(700000),
            "old crash window never expired");
    require(startup.finishClean(), "expired-window clean finish failed");
  }
}
/// clocks preserves crash evidence across backwards monotonic discontinuity and
/// rejects early admission without consulting a mutable wall clock (PLAN
/// §14.2).
void clocks() {
  QTemporaryDir root;
  for (int i = 0; i < 6; ++i)
    crash(root.path(), 1000000 + i * 1000);
  Startup startup(root.path(), 100);
  require(startup.decision().diagnostic && startup.decision().crashes == 6 &&
              !startup.engineAllowed(99) &&
              !startup.engineAllowed(9000000000LL),
          "clock reset bypassed the crash gate");
  require(startup.finishClean(), "clock-reset clean finish failed");
}
/// stopIntent preserves user stop across clean and unclean exits (INV-14).
void stopIntent() {
  QTemporaryDir root;
  crash(root.path(), 1000, true);
  {
    Startup startup(root.path(), 2000);
    require(startup.decision().priorUnclean &&
                startup.decision().automaticRestartSuppressed &&
                !startup.engineAllowed(9000000),
            "explicit stop intent was lost after an unclean exit");
    require(startup.recordStopIntent(false) && startup.engineAllowed(4000),
            "explicit user resume could not clear its stop intent");
    require(startup.recordStopIntent() && !startup.engineAllowed(9000000),
            "explicit stop admitted new engine work in the stopping process");
    require(startup.finishClean(), "stop-intent clean finish failed");
  }
  {
    Startup startup(root.path(), 5000);
    require(!startup.decision().priorUnclean &&
                startup.decision().automaticRestartSuppressed &&
                !startup.engineAllowed(9000000),
            "clean shutdown erased the user's stopped intent");
    require(startup.finishClean(), "stopped clean finish failed");
  }
}
/// corruptLedger keeps original bytes and engine admission closed (INV-10).
void corruptLedger() {
  const QList<QByteArray> fixtures{
      "not json",
      "{}",
      QByteArray(5000, 'x'),
      R"({"crashes":[],"running":false,"started":"0","stopped":false,"version":2})",
      R"({"crashes":["bad"],"running":true,"started":"0","stopped":false,"version":1})",
      R"({"crashes":[],"running":true,"running":false,"started":"0","stopped":false,"version":1})"};
  for (const auto &fixture : fixtures) {
    QTemporaryDir root;
    const auto path = root.filePath("startup.json");
    write(path, fixture);
    {
      Startup startup(root.path(), 1000);
      require(startup.decision().diagnostic && !startup.engineAllowed(9000000),
              "corrupt ledger admitted an engine");
      require(!startup.finishClean(), "corrupt ledger was replaced as clean");
    }
    require(bytes(path) == fixture, "corrupt ledger bytes were overwritten");
  }
  QTemporaryDir root;
  require(QDir().mkdir(root.filePath("startup.json")),
          "unwritable ledger fixture failed");
  Startup startup(root.path(), 1000);
  require(startup.decision().diagnostic && !startup.engineAllowed(1000),
          "unwritable ledger admitted an engine");
}
/// singleWriter refuses a second live owner before it can touch the ledger
/// (INV-01); the subprocess uses the same production profile lock.
void singleWriter() {
  QTemporaryDir root;
  Startup startup(root.path(), 1000);
  const auto original = bytes(root.filePath("startup.json"));
  QProcess child;
  child.start(QCoreApplication::applicationFilePath(),
              {"--locked", root.path(), "2000"});
  require(child.waitForStarted(10000) && child.waitForFinished(10000) &&
              child.exitCode() == 0,
          "second process acquired the live profile lock");
  require(bytes(root.filePath("startup.json")) == original,
          "second process changed crash evidence");
  require(startup.finishClean(), "lock-owner clean finish failed");
}
/// sql runs only disposable SQLite fixture setup.
void sql(QSqlDatabase db, const QString &text) {
  QSqlQuery query(db);
  require(query.exec(text), "fixture SQL failed");
}
/// backup checks WAL-consistent migration backup and leaves journal bytes alone
/// (PLAN §14.2; INV-08/10).
void backup() {
  QTemporaryDir root;
  Startup startup(root.path(), 1000);
  const auto path = root.filePath("control.sqlite");
  require(backupControlBeforeMigration(path, 8).isEmpty(),
          "new profile created an unnecessary backup");
  const auto name = QUuid::createUuid().toString();
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(path);
    require(db.open(), "fixture database did not open");
    sql(db, "PRAGMA journal_mode=WAL");
    sql(db, "PRAGMA wal_autocheckpoint=0");
    sql(db, "CREATE TABLE retained(value TEXT)");
    sql(db, "INSERT INTO retained VALUES('before migration')");
    sql(db, "PRAGMA user_version=6");
    const auto journal = root.filePath("folder-journal.sqlite");
    write(journal, "unaltered journal bytes");
    const auto original = bytes(path);
    const auto wal = bytes(path + "-wal");
    const auto copy = backupControlBeforeMigration(path, 8);
    require(!copy.isEmpty() && QFileInfo::exists(copy),
            "migration backup was not published");
    require(bytes(path) == original && bytes(path + "-wal") == wal &&
                bytes(journal) == "unaltered journal bytes",
            "backup rewrote source, WAL or journal");
    const auto copyName = QUuid::createUuid().toString();
    {
      auto saved = QSqlDatabase::addDatabase("QSQLITE", copyName);
      saved.setDatabaseName(copy);
      saved.setConnectOptions("QSQLITE_OPEN_READONLY");
      require(saved.open(), "published backup did not reopen");
      QSqlQuery query(saved);
      require(query.exec("SELECT value FROM retained") && query.next() &&
                  query.value(0).toString() == "before migration",
              "backup lost committed WAL content");
      require(query.exec("PRAGMA user_version") && query.next() &&
                  query.value(0).toInt() == 6,
              "backup contains migrated rather than prior schema");
      query.finish();
      saved.close();
    }
    QSqlDatabase::removeDatabase(copyName);
    // A rejected transaction models a failed migration after the backup exists.
    require(db.transaction(), "migration fixture transaction failed");
    sql(db, "DELETE FROM retained");
    require(db.rollback(), "migration fixture rollback failed");
    require(QFileInfo::exists(copy), "failed migration removed its backup");
    sql(db, "PRAGMA user_version=8");
    require(backupControlBeforeMigration(path, 8).isEmpty(),
            "current schema unnecessarily backed up");
    sql(db, "PRAGMA user_version=9");
    bool refused = false;
    try {
      backupControlBeforeMigration(path, 8);
    } catch (const std::exception &) {
      refused = true;
    }
    require(refused, "newer schema was admitted for downgrade migration");
    db.close();
  }
  QSqlDatabase::removeDatabase(name);
  require(startup.finishClean(), "backup clean finish failed");
}
/// corruptDatabase preserves malformed Control DB bytes rather than recreating
/// an empty profile (PLAN §14.2; INV-10).
void corruptDatabase() {
  QTemporaryDir root;
  Startup startup(root.path(), 1000);
  const auto path = root.filePath("control.sqlite");
  const auto original =
      QByteArrayLiteral("not a SQLite database\0original evidence");
  write(path, original);
  for (int attempt = 0; attempt < 2; ++attempt) {
    bool refused = false;
    try {
      backupControlBeforeMigration(path, 8);
    } catch (const std::exception &) {
      refused = true;
    }
    require(refused && bytes(path) == original,
            "corrupt Control DB was admitted or overwritten");
  }
  require(
      QDir(root.path()).entryList({"*.pre-schema-*"}, QDir::Files).isEmpty(),
      "corrupt source was published as a valid migration backup");
  require(startup.finishClean(), "corrupt-database clean finish failed");
}
} // namespace

/// main runs isolated startup/storage regressions without credentials or
/// network.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if (args.size() == 4) {
    try {
      Startup startup(args[2], args[3].toLongLong());
      if (args[1] == "--locked")
        return 3;
      if (args[1] == "--stopped-crash" && !startup.recordStopIntent())
        return 4;
      std::_Exit(17);
    } catch (const std::exception &error) {
      return args[1] == "--locked" && QString::fromUtf8(error.what()) ==
                                          "SyncD profile is already owned"
                 ? 0
                 : 5;
    }
  }
  try {
    storm();
    cleanAndBackoff();
    clocks();
    stopIntent();
    corruptLedger();
    singleWriter();
    backup();
    corruptDatabase();
    std::puts("8 startup/storage contracts passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
