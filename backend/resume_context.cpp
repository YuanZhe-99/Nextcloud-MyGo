// SPDX-License-Identifier: GPL-3.0-only
#include "resume_context.h"
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <windows.h>

namespace {
/// relativeFile rejects origin-bearing and escaping paths before upstream can
/// reuse local transfer state.
bool relativeFile(const QString &path) {
  if (path.isEmpty() || path.startsWith('/') || path.contains('\\') ||
      path.contains(':') || path.contains(QChar::Null))
    return false;
  for (const auto &part : path.split('/'))
    if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') ||
        part.endsWith(' '))
      return false;
  for (const auto character : path)
    if (character.unicode() < 32 ||
        QStringLiteral("<>\"|?*").contains(character))
      return false;
  return true;
}
/// portable inspects all transfer rows without mutating or interpreting remote
/// outcomes (INV-08/09).
QString portable(QSqlDatabase db) {
  QSqlQuery q(db);
  if (!q.exec("SELECT path,pollpath FROM async_poll"))
    return "JOURNAL_ERROR";
  if (q.next())
    return "RESUME_CONTEXT_UNSUPPORTED";
  if (q.lastError().isValid())
    return "JOURNAL_ERROR";
  if (!q.exec("SELECT path,chunk,transferid,size,modtime,contentChecksum FROM "
              "uploadinfo"))
    return "JOURNAL_ERROR";
  while (q.next()) {
    bool chunk = false, transfer = false, size = false, mtime = false;
    const auto c = q.value(1).toLongLong(&chunk),
               t = q.value(2).toLongLong(&transfer),
               s = q.value(3).toLongLong(&size),
               m = q.value(4).toLongLong(&mtime);
    if (!relativeFile(q.value(0).toString()) || !chunk || c != 0 || !transfer ||
        t != 0 || !size || s < 0 || !mtime || m <= 0 ||
        q.value(5).toByteArray().isEmpty())
      return "RESUME_CONTEXT_UNSUPPORTED";
  }
  if (q.lastError().isValid())
    return "JOURNAL_ERROR";
  if (!q.exec("SELECT path,tmpfile,etag FROM downloadinfo"))
    return "JOURNAL_ERROR";
  while (q.next())
    if (!relativeFile(q.value(0).toString()) ||
        !relativeFile(q.value(1).toString()) ||
        q.value(2).toByteArray().isEmpty())
      return "RESUME_CONTEXT_UNSUPPORTED";
  return q.lastError().isValid() ? QStringLiteral("JOURNAL_ERROR") : QString{};
}
} // namespace

QString resumeContextError(const QString &journalPath, bool endpointChanged) {
  const auto attributes =
      GetFileAttributesW(reinterpret_cast<LPCWSTR>(journalPath.utf16()));
  if (attributes == INVALID_FILE_ATTRIBUTES)
    return GetLastError() == ERROR_FILE_NOT_FOUND ? QString{} : "JOURNAL_ERROR";
  if (attributes & FILE_ATTRIBUTE_DIRECTORY)
    return "JOURNAL_ERROR";
  const auto name = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto db = QSqlDatabase::addDatabase("QSQLITE", name);
  db.setDatabaseName(journalPath);
  db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=0");
  QString reason = "JOURNAL_ERROR";
  if (db.open()) {
    QSqlQuery integrity(db);
    if (integrity.exec("PRAGMA quick_check") && integrity.next() &&
        integrity.value(0).toString() == "ok" && !integrity.next() &&
        !integrity.lastError().isValid()) {
      QSqlQuery resume(db);
      if (resume.exec("SELECT path,pollpath FROM async_poll UNION ALL "
                      "SELECT path,transferid FROM uploadinfo UNION ALL "
                      "SELECT path,tmpfile FROM downloadinfo LIMIT 1")) {
        resume.next();
        if (!resume.lastError().isValid())
          reason = endpointChanged ? portable(db) : QString{};
      }
    }
  }
  db.close();
  db = {};
  QSqlDatabase::removeDatabase(name);
  return reason;
}
