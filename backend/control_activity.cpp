// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include <QDateTime>
#include <QJsonArray>
#include <QSqlQuery>
#include <cmath>
#include <limits>
#include <stdexcept>

QJsonArray Control::activityProjection(const QJsonObject &params) const {
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "cursor" && it.key() != "limit" &&
        it.key() != "accountId" && it.key() != "folderId")
      throw std::runtime_error("INVALID_PARAMS");
  const auto limit = params.value("limit").toDouble(100);
  if (params.contains("limit") && !params.value("limit").isDouble())
    throw std::runtime_error("INVALID_PARAMS");
  if (!std::isfinite(limit) || std::floor(limit) != limit || limit < 1 ||
      limit > 500)
    throw std::runtime_error("INVALID_PARAMS");
  qint64 cursor = std::numeric_limits<qint64>::max();
  if (params.contains("cursor")) {
    bool valid = false;
    const auto text = params.value("cursor").toString();
    cursor = text.toLongLong(&valid);
    if (!valid || cursor <= 0 || QString::number(cursor) != text)
      throw std::runtime_error("INVALID_PARAMS");
  }
  QString statement =
      "SELECT cursor,account_id,folder_id,at,kind,count,from_id,to_id,reason "
      "FROM activity WHERE cursor<?";
  for (const auto &field : {"accountId", "folderId"})
    if (params.contains(field)) {
      if (!params.value(field).isString() ||
          params.value(field).toString().isEmpty())
        throw std::runtime_error("INVALID_PARAMS");
      statement +=
          QString(field == QStringLiteral("accountId") ? " AND account_id=?"
                                                       : " AND folder_id=?");
    }
  statement += " ORDER BY cursor DESC LIMIT ?";
  QSqlQuery q(db_);
  q.prepare(statement);
  q.addBindValue(cursor);
  for (const auto &field : {"accountId", "folderId"})
    if (params.contains(field))
      q.addBindValue(params.value(field).toString());
  q.addBindValue(int(limit));
  if (!q.exec())
    throw std::runtime_error("INTERNAL_ERROR");
  QJsonArray result;
  while (q.next()) {
    QJsonObject item{{"cursor", QString::number(q.value(0).toLongLong())},
                     {"accountId", q.value(1).toString()},
                     {"folderId", q.value(2).toString()},
                     {"at", q.value(3).toString()},
                     {"kind", q.value(4).toString()},
                     {"count", q.value(5).toInt()},
                     {"fromId", q.value(6).toString()},
                     {"toId", q.value(7).toString()},
                     {"reason", q.value(8).toString()}};
    if (q.value(4) == "switched" || q.value(4) == "switchFailed") {
      QSqlQuery endpoint(db_);
      endpoint.prepare(
          "SELECT name FROM endpoints WHERE account_id=? AND id=? UNION ALL "
          "SELECT base_url FROM accounts WHERE id=? AND endpoint_id=?");
      endpoint.addBindValue(q.value(1));
      endpoint.addBindValue(q.value(7));
      endpoint.addBindValue(q.value(1));
      endpoint.addBindValue(q.value(7));
      if (!endpoint.exec())
        throw std::runtime_error("INTERNAL_ERROR");
      const auto text = endpoint.next() ? endpoint.value(0).toString()
                                        : QStringLiteral("Nextcloud-MyGo");
      item.insert("name",
                  text.startsWith("https://") ? QUrl(text).host() : text);
    }
    result.append(item);
  }
  return result;
}

void Control::recordActivity(const QString &account, const QString &folder,
                             const QString &kind, quint64 count,
                             const QString &from, const QString &to,
                             const QString &reason) {
  if (!count)
    return;
  QSqlQuery q(db_);
  q.prepare("INSERT INTO "
            "activity(account_id,folder_id,at,kind,count,from_id,to_id,reason) "
            "VALUES(?,?,?,?,?,?,?,?)");
  q.addBindValue(account);
  q.addBindValue(folder.isNull() ? QStringLiteral("") : folder);
  q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  q.addBindValue(kind);
  q.addBindValue(qint64(count));
  for (const auto &text : {from, to, reason})
    q.addBindValue(text.isNull() ? QStringLiteral("") : text);
  if (!q.exec() ||
      !q.exec("DELETE FROM activity WHERE cursor NOT IN (SELECT cursor FROM "
              "activity ORDER BY cursor DESC LIMIT 1000)"))
    throw std::runtime_error("INTERNAL_ERROR");
}
