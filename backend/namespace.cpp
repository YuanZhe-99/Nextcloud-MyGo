// SPDX-License-Identifier: GPL-3.0-only
#include "namespace.h"
#include "login.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTimeZone>
#include <QTimer>
#include <QVersionNumber>
#include <QXmlStreamReader>
#include <algorithm>
#include <stdexcept>

MetadataError::MetadataError(const char *reason, qint64 retryAfterMs)
    : std::runtime_error(reason), retryAfterMs(retryAfterMs) {}

namespace {
/// retryDelay preserves valid bounded server deadlines and rejects longer ones
/// without converting them into an earlier retry (PLAN §12.1/N18).
qint64 retryDelay(const QByteArray &header, const QDateTime &now) {
  constexpr qint64 fallback = 60000, maximum = 7 * 86400000LL;
  const auto value = header.trimmed();
  if (!value.isEmpty() && std::all_of(value.begin(), value.end(), [](char c) {
        return c >= '0' && c <= '9';
      })) {
    bool valid = false;
    const auto seconds = value.toULongLong(&valid);
    if (!valid || seconds > quint64(maximum / 1000))
      return -1;
    return std::max(fallback, qint64(seconds * 1000));
  }
  const auto text = QString::fromLatin1(value);
  const auto locale = QLocale::c();
  auto date = locale.toDateTime(text, "ddd, dd MMM yyyy HH:mm:ss 'GMT'");
  if (!date.isValid())
    date = locale.toDateTime(text, "dddd, dd-MMM-yy HH:mm:ss 'GMT'",
                             now.date().year() - 49);
  if (!date.isValid())
    date = locale.toDateTime(text.simplified(), "ddd MMM d HH:mm:ss yyyy");
  if (date.isValid())
    date.setTimeZone(QTimeZone::UTC);
  else
    date = QDateTime::fromString(text, Qt::RFC2822Date);
  if (!date.isValid())
    return fallback;
  const auto delay = now.msecsTo(date);
  return delay > maximum ? -1 : std::max(fallback, delay);
}

/// unavailableProperties classifies only required properties still missing
/// after all successful propstats have been merged. Notes: unavailable metadata
/// is not evidence of a different server (INV-07).
void unavailableProperties(const QList<int> &statuses) {
  if (statuses.contains(401))
    throw MetadataError("AUTH_REQUIRED");
  if (statuses.contains(429))
    throw MetadataError("SERVER_THROTTLED", 60000);
  if (statuses.contains(423))
    throw MetadataError("REMOTE_LOCKED", 60000);
  if (statuses.contains(403))
    throw MetadataError("PERMISSION_DENIED");
  throw MetadataError("INVALID_SERVER_RESPONSE");
}
} // namespace

void checkMetadataReply(const QNetworkReply &reply, int expectedStatus,
                        const QDateTime &now) {
  const auto status =
      reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (reply.error() == QNetworkReply::SslHandshakeFailedError)
    throw MetadataError("TLS_ERROR");
  if (status == 401)
    throw MetadataError("AUTH_REQUIRED");
  if (status == 403)
    throw MetadataError("PERMISSION_DENIED");
  if (status == 423)
    throw MetadataError("REMOTE_LOCKED",
                        retryDelay(reply.rawHeader("Retry-After"), now));
  if (status == 429)
    throw MetadataError("SERVER_THROTTLED",
                        retryDelay(reply.rawHeader("Retry-After"), now));
  if (status >= 300 && status < 400)
    throw MetadataError("ENDPOINT_UNTRUSTED");
  if (expectedStatus == 207 && status == 404)
    throw MetadataError("REMOTE_ROOT_UNAVAILABLE");
  if (reply.error() != QNetworkReply::NoError || status != expectedStatus)
    throw MetadataError("NETWORK_UNAVAILABLE");
}

ServerCapabilities syncCapabilities(const QJsonObject &response) {
  const auto ocs = response.value("ocs").toObject();
  const auto code = ocs.value("meta").toObject().value("statuscode").toInt();
  const auto data = ocs.value("data").toObject();
  const auto caps = data.value("capabilities").toObject();
  const auto version =
      data.value("version").toObject().value("string").toString();
  qsizetype suffix = 0;
  const auto parsed = QVersionNumber::fromString(version, &suffix);
  if ((code != 100 && code != 200) ||
      ocs.value("meta").toObject().value("status") != "ok" ||
      !caps.value("files").isObject() || parsed.isNull() ||
      suffix != version.size())
    throw std::runtime_error("UNSUPPORTED_CAPABILITY");
  const auto chunking =
      caps.value("dav").toObject().value("chunking").toString();
  const auto chunkVersion = QVersionNumber::fromString(chunking, &suffix);
  if (chunkVersion.isNull() || suffix != chunking.size() ||
      chunkVersion < QVersionNumber(1, 0))
    throw std::runtime_error("UNSUPPORTED_CAPABILITY");
  QVariantMap sync;
  for (const auto &key :
       {"files", "dav", "checksums", "files_sharing", "files_lock"}) {
    if (caps.contains(key)) {
      if (!caps.value(key).isObject())
        throw std::runtime_error("INVALID_SERVER_RESPONSE");
      sync.insert(key, caps.value(key).toObject().toVariantMap());
    }
  }
  return {version, sync};
}

QJsonObject readServerJson(const QUrl &base, const QString &path,
                           const QString &login, const QString &credential,
                           QJsonObject *certificate) {
  const auto url = base.resolved(QUrl(path));
  if (!withinServer(base, url))
    throw std::runtime_error("ENDPOINT_UNTRUSTED");
  QNetworkAccessManager network;
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setRawHeader("Accept", "application/json");
  request.setRawHeader("OCS-APIRequest", "true");
  if (!credential.isEmpty()) {
    if (login.isEmpty() || login.contains(':'))
      throw std::runtime_error("AUTH_REQUIRED");
    auto secret = readCredential(credential).toUtf8();
    request.setRawHeader("Authorization",
                         "Basic " + (login.toUtf8() + ':' + secret).toBase64());
    secret.fill(0);
  }
  auto reply = network.get(request);
  reply->setReadBufferSize(1048577);
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  QByteArray body;
  QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
    body += reply->readAll();
    if (body.size() > 1048576)
      reply->abort();
  });
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { reply->abort(); });
  deadline.start(metadataDeadline(!credential.isEmpty()));
  loop.exec();
  body += reply->readAll();
  checkMetadataReply(*reply, 200, QDateTime::currentDateTimeUtc());
  if (body.size() > 1048576 ||
      !reply->header(QNetworkRequest::ContentTypeHeader)
           .toString()
           .startsWith("application/json", Qt::CaseInsensitive))
    throw std::runtime_error("INVALID_SERVER_RESPONSE");
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(body, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    throw std::runtime_error("INVALID_SERVER_RESPONSE");
  if (certificate) {
    const auto peer = reply->sslConfiguration().peerCertificate();
    *certificate = {{"secureTLS", true},
                    {"certSubject",
                     peer.subjectInfo(QSslCertificate::CommonName).join(", ")},
                    {"certIssuer",
                     peer.issuerInfo(QSslCertificate::CommonName).join(", ")}};
  }
  return document.object();
}

QString remotePath(const QString &path) {
  if (!path.startsWith('/') || path.contains('\\') ||
      path.contains(QChar::Null) || path.contains("//"))
    throw std::runtime_error("INVALID_PARAMS");
  for (const auto &part : path.split('/'))
    if (part == "." || part == "..")
      throw std::runtime_error("INVALID_PARAMS");
  auto result = path;
  if (result.size() > 1 && result.endsWith('/'))
    result.chop(1);
  return result;
}

QList<DavCollection> readCollections(const QUrl &server, const QString &uid,
                                     const QString &login,
                                     const QString &credential,
                                     const QString &path, bool children) {
  if (uid.isEmpty() || uid.contains('/') || uid.contains('\\') ||
      login.isEmpty() || login.contains(':'))
    throw std::runtime_error("AUTH_REQUIRED");
  const auto relative = remotePath(path);
  const auto home = server.toEncoded() + "remote.php/dav/files/" +
                    QUrl::toPercentEncoding(uid) + '/';
  const auto url =
      QUrl::fromEncoded(home + QUrl::toPercentEncoding(relative.mid(1), "/") +
                        (relative == "/" ? "" : "/"));
  if (!withinServer(server, url))
    throw std::runtime_error("ENDPOINT_UNTRUSTED");
  QNetworkAccessManager network;
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setRawHeader("Depth", children ? "1" : "0");
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    "application/xml; charset=utf-8");
  auto secret = readCredential(credential).toUtf8();
  request.setRawHeader("Authorization",
                       "Basic " + (login.toUtf8() + ':' + secret).toBase64());
  secret.fill(0);
  const QByteArray xml =
      "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\" "
      "xmlns:oc=\"http://owncloud.org/ns\"><d:prop><d:resourcetype/><oc:id/"
      "><oc:is-encrypted/><oc:permissions/></d:prop></d:propfind>";
  auto reply = network.sendCustomRequest(request, "PROPFIND", xml);
  reply->setReadBufferSize(4 * 1024 * 1024 + 1);
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  QByteArray body;
  QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
    body += reply->readAll();
    if (body.size() > 4 * 1024 * 1024)
      reply->abort();
  });
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { reply->abort(); });
  deadline.start(30000);
  loop.exec();
  body += reply->readAll();
  checkMetadataReply(*reply, 207, QDateTime::currentDateTimeUtc());
  try {
    return parseCollections(body, server, uid, relative, children);
  } catch (const MetadataError &error) {
    // A 207 can carry the delay for a locked/throttled required property.
    if (error.retryAfterMs == 0)
      throw;
    throw MetadataError(error.what(),
                        retryDelay(reply->rawHeader("Retry-After"),
                                   QDateTime::currentDateTimeUtc()));
  }
}

QList<DavCollection> parseCollections(const QByteArray &body,
                                      const QUrl &server, const QString &uid,
                                      const QString &path, bool children) {
  const auto relative = remotePath(path);
  const auto home = server.toEncoded() + "remote.php/dav/files/" +
                    QUrl::toPercentEncoding(uid) + '/';
  const auto url =
      QUrl::fromEncoded(home + QUrl::toPercentEncoding(relative.mid(1), "/") +
                        (relative == "/" ? "" : "/"));
  if (body.size() > 4 * 1024 * 1024)
    throw MetadataError("INVALID_SERVER_RESPONSE");
  QList<DavCollection> result;
  QXmlStreamReader reader(body);
  if (!reader.readNextStartElement() || reader.name() != "multistatus" ||
      reader.namespaceUri() != "DAV:")
    throw MetadataError("INVALID_SERVER_RESPONSE");
  while (!reader.atEnd()) {
    reader.readNext();
    if (!reader.isStartElement() || reader.name() != "response" ||
        reader.namespaceUri() != "DAV:")
      continue;
    QString href, identity, permissions;
    bool collection = false, encrypted = false, typeRead = false,
         permissionsRead = false;
    QList<int> identityFailures, typeFailures, permissionsFailures;
    int responseFailure = 0;
    while (reader.readNextStartElement()) {
      if (reader.name() == "href" && reader.namespaceUri() == "DAV:")
        href = reader.readElementText();
      else if (reader.name() == "status" && reader.namespaceUri() == "DAV:")
        responseFailure = reader.readElementText().split(' ').value(1).toInt();
      else if (reader.name() == "propstat" && reader.namespaceUri() == "DAV:") {
        QString statusLine, candidate, candidatePermissions;
        bool directory = false, crypt = false, hasIdentity = false,
             hasType = false, hasPermissions = false;
        while (reader.readNextStartElement()) {
          if (reader.name() == "status" && reader.namespaceUri() == "DAV:")
            statusLine = reader.readElementText();
          else if (reader.name() == "prop" && reader.namespaceUri() == "DAV:") {
            while (reader.readNextStartElement()) {
              if (reader.name() == "id" &&
                  reader.namespaceUri() == "http://owncloud.org/ns") {
                hasIdentity = true;
                candidate = reader.readElementText();
              } else if (reader.name() == "permissions" &&
                         reader.namespaceUri() == "http://owncloud.org/ns") {
                hasPermissions = true;
                candidatePermissions = reader.readElementText();
              } else if (reader.name() == "is-encrypted" &&
                         reader.namespaceUri() == "http://owncloud.org/ns") {
                const auto value = reader.readElementText();
                crypt = value == "1" || value == "true";
              } else if (reader.name() == "resourcetype" &&
                         reader.namespaceUri() == "DAV:") {
                hasType = true;
                while (reader.readNextStartElement()) {
                  if (reader.name() == "collection" &&
                      reader.namespaceUri() == "DAV:")
                    directory = true;
                  reader.skipCurrentElement();
                }
              } else
                reader.skipCurrentElement();
            }
          } else
            reader.skipCurrentElement();
        }
        const auto status = statusLine.split(' ').value(1).toInt();
        if (status == 200) {
          if (!candidate.isEmpty())
            identity = candidate;
          if (hasPermissions) {
            permissions = candidatePermissions;
            permissionsRead = true;
          }
          typeRead |= hasType;
          collection |= directory;
          encrypted |= crypt;
        } else {
          if (hasIdentity)
            identityFailures.append(status);
          if (hasType)
            typeFailures.append(status);
          if (hasPermissions)
            permissionsFailures.append(status);
        }
      } else
        reader.skipCurrentElement();
    }
    const auto target = url.resolved(QUrl(href));
    if (href.isEmpty() || target.hasQuery() || !withinServer(server, target) ||
        !target.toEncoded().startsWith(home))
      throw MetadataError("INVALID_SERVER_RESPONSE");
    if (reader.hasError())
      throw MetadataError("INVALID_SERVER_RESPONSE");
    if ((responseFailure && responseFailure != 200) || !typeRead ||
        (collection && (identity.isEmpty() || !permissionsRead))) {
      QList<int> failures;
      if (responseFailure && responseFailure != 200)
        failures.append(responseFailure);
      if (!typeRead)
        failures.append(typeFailures);
      if (!typeRead || collection) {
        if (identity.isEmpty())
          failures.append(identityFailures);
        if (!permissionsRead)
          failures.append(permissionsFailures);
      }
      unavailableProperties(failures);
    }
    if (!collection)
      continue;
    const auto decoded =
        QUrl::fromPercentEncoding(target.toEncoded().mid(home.size()));
    const auto itemPath = remotePath('/' + decoded);
    if (itemPath != relative &&
        (!children ||
         !(itemPath.startsWith(relative == "/" ? "/" : relative + '/') &&
           !itemPath.mid(relative == "/" ? 1 : relative.size() + 1)
                .contains('/'))))
      throw MetadataError("INVALID_SERVER_RESPONSE");
    for (const auto &existing : result)
      if (existing.path == itemPath)
        throw MetadataError("INVALID_SERVER_RESPONSE");
    result.append({itemPath, identity, encrypted, permissions});
    if (result.size() > 1000)
      throw MetadataError("RATE_LIMITED");
  }
  if (reader.hasError() ||
      std::none_of(result.begin(), result.end(),
                   [&](const auto &item) { return item.path == relative; }))
    throw MetadataError("INVALID_SERVER_RESPONSE");
  return result;
}
