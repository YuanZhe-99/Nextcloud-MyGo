// SPDX-License-Identifier: GPL-3.0-only
#include "resume_context.h"
#include <QCoreApplication>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>

namespace {
/// require preserves contract assertions in release builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// bytes captures fixture content to detect accidental repair or resume-row
/// removal.
QByteArray bytes(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "fixture read failed");
  return file.readAll();
}
/// contexts covers absent, corrupt, incompatible, empty and each pending
/// context (INV-08/09).
void contexts() {
  QTemporaryDir root;
  require(root.isValid(), "temporary directory unavailable");
  require(resumeContextError(root.filePath("absent.sqlite")).isEmpty(),
          "absent journal blocked");
  require(resumeContextError(root.path()) == "JOURNAL_ERROR" &&
              resumeContextError(root.filePath(
                  "missing-parent/journal.sqlite")) == "JOURNAL_ERROR",
          "invalid journal location treated as absent state");
  const auto corrupt = root.filePath("corrupt.sqlite");
  {
    QFile file(corrupt);
    require(file.open(QIODevice::WriteOnly), "corrupt fixture open failed");
    file.write("owned invalid journal");
  }
  const auto original = bytes(corrupt);
  require(resumeContextError(corrupt) == "JOURNAL_ERROR" &&
              resumeContextError(corrupt, false) == "JOURNAL_ERROR" &&
              bytes(corrupt) == original,
          "corrupt journal was accepted or changed");
  for (const QString pending : {"", "async_poll", "uploadinfo", "downloadinfo",
                                "missing_schema", "missing_column"}) {
    const auto path = root.filePath(pending + "-context.sqlite");
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", "resume-fixture");
      db.setDatabaseName(path);
      require(db.open(), "fixture database open failed");
      {
        QSqlQuery q(db);
        for (const auto *schema :
             {"async_poll(path TEXT,pollpath TEXT)",
              "uploadinfo(path TEXT,transferid INTEGER,chunk INTEGER,size "
              "INTEGER,modtime INTEGER,contentChecksum TEXT)",
              "downloadinfo(path TEXT,tmpfile TEXT,etag TEXT)"})
          require(q.exec("CREATE TABLE " + QString(schema)),
                  "fixture schema failed");
        if (pending == "missing_schema")
          require(q.exec("DROP TABLE async_poll"),
                  "incompatible fixture failed");
        else if (pending == "missing_column")
          require(q.exec("ALTER TABLE downloadinfo DROP COLUMN tmpfile"),
                  "missing column fixture failed");
        else if (!pending.isEmpty())
          require(q.exec("INSERT INTO " + pending +
                         (pending == "downloadinfo" ? "(path,tmpfile)"
                          : pending == "uploadinfo" ? "(path,transferid)"
                                                    : "(path,pollpath)") +
                         " VALUES('owned',1)"),
                  "resume fixture failed");
      }
      db.close();
    }
    QSqlDatabase::removeDatabase("resume-fixture");
    const auto before = bytes(path);
    const auto expected =
        pending.isEmpty() ? QString{}
        : pending == "missing_schema" || pending == "missing_column"
            ? "JOURNAL_ERROR"
            : "RESUME_CONTEXT_UNSUPPORTED";
    require(resumeContextError(path) == expected,
            "resume context incorrectly classified");
    require(resumeContextError(path, false) ==
                (expected == "JOURNAL_ERROR" ? expected : QString{}),
            "same-origin recovery discarded supported upstream transfer state");
    require(bytes(path) == before, "resume inspection modified the journal");
  }
}
/// portableContexts checks the exact admitted classes and rejects
/// URL/session/escaping state without journal edits (INV-02/08/09).
void portableContexts() {
  QTemporaryDir root;
  require(root.isValid(), "portable profile unavailable");
  const auto path = root.filePath("portable.sqlite");
  auto db = QSqlDatabase::addDatabase("QSQLITE", "portable-fixture");
  db.setDatabaseName(path);
  require(db.open(), "portable journal fixture unavailable");
  QSqlQuery q(db);
  for (const auto *schema :
       {"async_poll(path TEXT,pollpath TEXT)",
        "uploadinfo(path TEXT,chunk INTEGER,transferid INTEGER,size "
        "INTEGER,modtime INTEGER,contentChecksum TEXT)",
        "downloadinfo(path TEXT,tmpfile TEXT,etag TEXT)"})
    require(q.exec("CREATE TABLE " + QString(schema)),
            "portable schema failed");
  struct Case {
    const char *sql;
    bool admitted;
  };
  for (const auto &test :
       {Case{"INSERT INTO downloadinfo "
             "VALUES('dir/file','.file.~owned','etag-one')",
             true},
        Case{"INSERT INTO uploadinfo "
             "VALUES('dir/file',0,0,16,123,'SHA1:owned-checksum')",
             true},
        Case{"INSERT INTO uploadinfo "
             "VALUES('dir/file',0,123,16,123,'SHA1:owned-checksum')",
             true},
        Case{"INSERT INTO uploadinfo "
             "VALUES('dir/file',0,4294967295,16,123,'SHA1:owned-checksum')",
             true},
        Case{"INSERT INTO uploadinfo VALUES('dir/file',0,123,16,123,'')",
             false},
        Case{"INSERT INTO uploadinfo "
             "VALUES('dir/file',0,-1,16,123,'SHA1:owned-checksum')",
             false},
        Case{"INSERT INTO uploadinfo "
             "VALUES('dir/file',0,4294967296,16,123,'SHA1:owned-checksum')",
             false},
        Case{"INSERT INTO uploadinfo "
             "VALUES('../file',0,123,16,123,'SHA1:owned-checksum')",
             false},
        Case{"INSERT INTO uploadinfo "
             "VALUES('dir/file',1,0,16,123,'SHA1:owned-checksum')",
             false},
        Case{"INSERT INTO uploadinfo VALUES('dir/file',0,0,16,123,'')", false},
        Case{"INSERT INTO uploadinfo "
             "VALUES('dir/file',0,0,-1,123,'SHA1:owned-checksum')",
             false},
        Case{"INSERT INTO uploadinfo "
             "VALUES('dir/file',0,0,16,0,'SHA1:owned-checksum')",
             false},
        Case{"INSERT INTO uploadinfo "
             "VALUES('dir/file',NULL,0,16,123,'SHA1:owned-checksum')",
             false},
        Case{"INSERT INTO downloadinfo VALUES('dir/file','../outside','etag')",
             false},
        Case{"INSERT INTO downloadinfo VALUES('dir/file','.. /outside','etag')",
             false},
        Case{"INSERT INTO downloadinfo VALUES('dir/file','dir/.. "
             "/outside','etag')",
             false},
        Case{"INSERT INTO downloadinfo VALUES('file?','.tmp','etag')", false},
        Case{"INSERT INTO downloadinfo VALUES('/absolute','.tmp','etag')",
             false},
        Case{"INSERT INTO downloadinfo VALUES('dir//file','.tmp','etag')",
             false},
        Case{"INSERT INTO downloadinfo VALUES('file','C:/outside','etag')",
             false},
        Case{"INSERT INTO downloadinfo VALUES('file','.tmp','')", false},
        Case{"INSERT INTO async_poll "
             "VALUES('file','https://old.example.invalid/poll')",
             false},
        Case{"INSERT INTO async_poll VALUES('file','relative-poll')", false}}) {
    for (const auto *table : {"async_poll", "uploadinfo", "downloadinfo"})
      require(q.exec("DELETE FROM " + QString(table)),
              "portable fixture reset failed");
    require(q.exec(test.sql), "portable context insert failed");
    q.finish();
    const auto before = bytes(path);
    require(resumeContextError(path) ==
                (test.admitted ? QString{}
                               : QStringLiteral("RESUME_CONTEXT_UNSUPPORTED")),
            "portable admission class drifted");
    require(
        resumeContextError(path, false).isEmpty() && bytes(path) == before,
        "portable inspection changed journal or blocked same-origin recovery");
  }
  require(q.exec("DELETE FROM async_poll") &&
              q.exec("INSERT INTO downloadinfo VALUES('good','.tmp','etag')") &&
              q.exec("INSERT INTO uploadinfo "
                     "VALUES('chunk',0,123,16,123,'SHA1:owned')"),
          "mixed portable fixture failed");
  q.finish();
  require(resumeContextError(path).isEmpty(),
          "portable download and chunk session were not admitted together");
  require(q.exec("INSERT INTO uploadinfo "
                 "VALUES('legacy',1,123,16,123,'SHA1:owned')"),
          "legacy chunk fixture failed");
  q.finish();
  require(resumeContextError(path) == "RESUME_CONTEXT_UNSUPPORTED",
          "one portable row bypassed a blocked legacy chunk index");
  q = QSqlQuery();
  db.close();
  db = {};
  QSqlDatabase::removeDatabase("portable-fixture");
}
} // namespace
/// main checks conservative read-only recovery admission without server access.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    contexts();
    portableContexts();
    std::puts(
        "PASS read-only resume context admission and journal preservation");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
