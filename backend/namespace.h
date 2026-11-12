// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <stdexcept>

class QNetworkReply;

/// MetadataError preserves a sanitized metadata failure and an internal retry
/// delay across synchronous verification loops (INV-07/09/14).
struct MetadataError : std::runtime_error {
  /// MetadataError records only the caller-supplied stable reason, never reply
  /// bodies, URLs, headers or credentials.
  MetadataError(const char *reason, qint64 retryAfterMs = 0);
  /// retryAfterMs is zero when unspecified, or a delay up to seven days for
  /// lock/throttle responses. Minus one rejects a valid longer deadline:
  /// callers must keep automatic retry blocked instead of shortening that
  /// deadline.
  qint64 retryAfterMs;
};

/// checkMetadataReply classifies a completed JSON (200) or DAV (207) reply
/// before its body is trusted. Inputs: now supplies UTC wall time only for
/// HTTP-date Retry-After; callers retain the resulting relative delay with
/// their monotonic deadline. Notes: TLS/redirect failures remain hard failures.
void checkMetadataReply(const QNetworkReply &reply, int expectedStatus,
                        const QDateTime &now);

/// ServerCapabilities contains only parsed sync metadata for one approved
/// runtime binding.
struct ServerCapabilities {
  QString version;
  QVariantMap sync;
};
/// syncCapabilities validates the OCS response and excludes
/// interactive/background feature groups.
ServerCapabilities syncCapabilities(const QJsonObject &response);
/// metadataDeadline is how long one metadata request may take before it counts
/// as NETWORK_UNAVAILABLE: four seconds without credentials (reaching the
/// server), fifteen with them. Notes: a reachable server spends seconds on
/// every request that authenticates afresh, more under concurrent sign-ins;
/// that is load, not an unreachable address.
constexpr int metadataDeadline(bool credentialed) {
  return credentialed ? 15000 : 4000;
}
/// readServerJson reads one bounded JSON response without redirects or
/// unapproved credential forwarding. Notes: runs a control-thread event loop
/// bounded by metadataDeadline; callers retain context and recheck consent
/// between steps (INV-03/06).
QJsonObject readServerJson(const QUrl &base, const QString &path,
                           const QString &login = {},
                           const QString &credential = {},
                           QJsonObject *certificate = nullptr);

/// DavCollection is successful Depth-0/1 metadata, never a substitute for
/// upstream discovery.
struct DavCollection {
  QString path, identity;
  bool encrypted = false;
  QString permissions;
};
/// remotePath validates a decoded account-relative collection path without
/// normalizing ambiguity away.
QString remotePath(const QString &path);
/// parseCollections decodes bounded successful DAV metadata without treating
/// missing/denied identity evidence as a different server (INV-07). Inputs:
/// server, uid and path identify the exact request; children selects Depth-0/1.
/// Returns: validated metadata, never an empty successful root listing.
QList<DavCollection> parseCollections(const QByteArray &body,
                                      const QUrl &server, const QString &uid,
                                      const QString &path, bool children);
/// readCollections reads bounded DAV metadata over verified TLS and refuses
/// redirects (INV-03/04). Notes: runs a bounded Qt event loop on the control
/// thread; callers must not hold a DB transaction.
QList<DavCollection> readCollections(const QUrl &server, const QString &uid,
                                     const QString &login,
                                     const QString &credential,
                                     const QString &path, bool children);
