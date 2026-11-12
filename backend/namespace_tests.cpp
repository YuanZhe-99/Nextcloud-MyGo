// SPDX-License-Identifier: GPL-3.0-only
#include "namespace.h"
#include <QCoreApplication>
#include <QNetworkReply>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {
/// Reply supplies completed boundary inputs without issuing a network request.
class Reply final : public QNetworkReply {
public:
  /// Reply records the actual HTTP status, Qt error and optional retry header.
  Reply(int status, NetworkError error, const QByteArray &retry = {}) {
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    setError(error, "Synthetic metadata reply");
    if (!retry.isNull())
      setRawHeader("Retry-After", retry);
    setFinished(true);
  }
  /// abort is inert because this reply never owns a request.
  void abort() override {}

protected:
  /// readData returns EOF because classification never consumes a response
  /// body.
  qint64 readData(char *, qint64) override { return -1; }
};

/// require keeps contract assertions enabled in optimized builds.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}

/// replies preserves permission/lock/throttle semantics before generic Qt HTTP
/// errors, while retaining TLS, redirect and transport gates (PLAN §12.1/N18).
int replies() {
  /// Case specifies one completed HTTP boundary and its sanitized result.
  struct Case {
    const char *name;
    int status, expectedStatus;
    QNetworkReply::NetworkError error;
    QByteArray retry;
    const char *reason;
    qint64 delay = 0;
  };
  const auto now = QDateTime::fromString("2026-11-09T00:00:00Z", Qt::ISODate);
  const Case cases[]{
      {"JSON success", 200, 200, QNetworkReply::NoError, {}, ""},
      {"DAV success", 207, 207, QNetworkReply::NoError, {}, ""},
      {"JSON 401",
       401,
       200,
       QNetworkReply::AuthenticationRequiredError,
       {},
       "AUTH_REQUIRED"},
      {"DAV 401",
       401,
       207,
       QNetworkReply::AuthenticationRequiredError,
       {},
       "AUTH_REQUIRED"},
      {"JSON 403",
       403,
       200,
       QNetworkReply::ContentAccessDenied,
       {},
       "PERMISSION_DENIED"},
      {"DAV 403",
       403,
       207,
       QNetworkReply::ContentAccessDenied,
       {},
       "PERMISSION_DENIED"},
      {"JSON 423",
       423,
       200,
       QNetworkReply::UnknownContentError,
       {},
       "REMOTE_LOCKED",
       60000},
      {"DAV 423", 423, 207, QNetworkReply::UnknownContentError, "120",
       "REMOTE_LOCKED", 120000},
      {"JSON 429 missing",
       429,
       200,
       QNetworkReply::UnknownContentError,
       {},
       "SERVER_THROTTLED",
       60000},
      {"DAV 429 numeric", 429, 207, QNetworkReply::UnknownContentError, "3600",
       "SERVER_THROTTLED", 3600000},
      {"429 zero", 429, 207, QNetworkReply::UnknownContentError, "0",
       "SERVER_THROTTLED", 60000},
      {"429 seven days", 429, 207, QNetworkReply::UnknownContentError, "604800",
       "SERVER_THROTTLED", 604800000},
      {"429 too long", 429, 207, QNetworkReply::UnknownContentError, "604801",
       "SERVER_THROTTLED", -1},
      {"429 numeric overflow", 429, 207, QNetworkReply::UnknownContentError,
       "99999999999999999999999999", "SERVER_THROTTLED", -1},
      {"429 HTTP date", 429, 207, QNetworkReply::UnknownContentError,
       "Mon, 09 Nov 2026 01:00:00 GMT", "SERVER_THROTTLED", 3600000},
      {"429 long HTTP date", 429, 207, QNetworkReply::UnknownContentError,
       "Tue, 17 Nov 2026 00:00:00 GMT", "SERVER_THROTTLED", -1},
      {"429 obsolete HTTP date", 429, 207, QNetworkReply::UnknownContentError,
       "Monday, 09-Nov-26 01:00:00 GMT", "SERVER_THROTTLED", 3600000},
      {"429 long obsolete HTTP date", 429, 207,
       QNetworkReply::UnknownContentError, "Tuesday, 17-Nov-26 00:00:00 GMT",
       "SERVER_THROTTLED", -1},
      {"429 asctime HTTP date", 429, 207, QNetworkReply::UnknownContentError,
       "Mon Nov  9 01:00:00 2026", "SERVER_THROTTLED", 3600000},
      {"429 long asctime HTTP date", 429, 207,
       QNetworkReply::UnknownContentError, "Tue Nov 17 00:00:00 2026",
       "SERVER_THROTTLED", -1},
      {"429 past HTTP date", 429, 207, QNetworkReply::UnknownContentError,
       "Sun, 08 Nov 2026 00:00:00 GMT", "SERVER_THROTTLED", 60000},
      {"429 malformed", 429, 207, QNetworkReply::UnknownContentError,
       "not-a-date", "SERVER_THROTTLED", 60000},
      {"429 negative", 429, 207, QNetworkReply::UnknownContentError, "-1",
       "SERVER_THROTTLED", 60000},
      {"423 long deadline", 423, 207, QNetworkReply::UnknownContentError,
       "604801", "REMOTE_LOCKED", -1},
      {"TLS before throttle", 429, 207, QNetworkReply::SslHandshakeFailedError,
       "3600", "TLS_ERROR"},
      {"TLS before permission",
       403,
       200,
       QNetworkReply::SslHandshakeFailedError,
       {},
       "TLS_ERROR"},
      {"JSON redirect",
       302,
       200,
       QNetworkReply::NoError,
       {},
       "ENDPOINT_UNTRUSTED"},
      {"DAV redirect",
       307,
       207,
       QNetworkReply::NoError,
       {},
       "ENDPOINT_UNTRUSTED"},
      {"DAV 404",
       404,
       207,
       QNetworkReply::ContentNotFoundError,
       {},
       "REMOTE_ROOT_UNAVAILABLE"},
      {"connection refused",
       0,
       200,
       QNetworkReply::ConnectionRefusedError,
       {},
       "NETWORK_UNAVAILABLE"},
      {"timeout",
       200,
       200,
       QNetworkReply::TimeoutError,
       {},
       "NETWORK_UNAVAILABLE"},
      {"server unavailable",
       503,
       207,
       QNetworkReply::ServiceUnavailableError,
       {},
       "NETWORK_UNAVAILABLE"}};
  int failures = 0;
  for (const auto &test : cases) {
    Reply reply(test.status, test.error, test.retry);
    try {
      checkMetadataReply(reply, test.expectedStatus, now);
      require(QByteArray(test.reason).isEmpty(), "failure was accepted");
    } catch (const std::exception &error) {
      const auto *metadata = dynamic_cast<const MetadataError *>(&error);
      if (QByteArray(error.what()) != test.reason ||
          (test.delay != 0 &&
           (!metadata || metadata->retryAfterMs != test.delay))) {
        std::fprintf(
            stderr, "FAIL %s expected=%s delay=%lld actual=%s delay=%lld\n",
            test.name, test.reason, static_cast<long long>(test.delay),
            error.what(),
            static_cast<long long>(metadata ? metadata->retryAfterMs : 0));
        ++failures;
      }
    }
  }
  return failures;
}

/// propstat builds one independent DAV property-status group.
QByteArray propstat(const QByteArray &properties, int status = 200) {
  return "<d:propstat><d:prop>" + properties + "</d:prop><d:status>HTTP/1.1 " +
         QByteArray::number(status) + " Status</d:status></d:propstat>";
}

/// multistatus wraps one owned synthetic root with no user/server identifiers.
QByteArray multistatus(const QByteArray &properties) {
  return "<d:multistatus xmlns:d=\"DAV:\" "
         "xmlns:oc=\"http://owncloud.org/ns\"><d:response><d:href>"
         "/remote.php/dav/files/owned/root/</d:href>" +
         properties + "</d:response></d:multistatus>";
}

/// collections preserves independent successful properties, classifies only
/// unavailable required metadata, and rejects incomplete identity (INV-07/14).
int collections() {
  const QByteArray type = "<d:resourcetype><d:collection/></d:resourcetype>";
  const QByteArray id = "<oc:id>owned-root</oc:id>";
  const QByteArray permissions = "<oc:permissions>RMG</oc:permissions>";
  /// Case specifies raw DAV property groups and their availability outcome.
  struct Case {
    const char *name;
    QByteArray body;
    const char *reason;
  };
  const Case cases[]{
      {"valid metadata", multistatus(propstat(type + id + permissions)), ""},
      {"optional encryption unsupported",
       multistatus(propstat(type + id + permissions) +
                   propstat("<oc:is-encrypted/>", 404)),
       ""},
      {"optional encryption denied",
       multistatus(propstat(type + id + permissions) +
                   propstat("<oc:is-encrypted/>", 403)),
       ""},
      {"successful identity overrides denial",
       multistatus(propstat("<oc:id/>", 403) + propstat(type + permissions) +
                   propstat(id)),
       ""},
      {"successful permissions override lock",
       multistatus(propstat(type + id + permissions) +
                   propstat("<oc:permissions/>", 423)),
       ""},
      {"successful type overrides lock",
       multistatus(propstat("<d:resourcetype/>", 423) +
                   propstat(type + id + permissions)),
       ""},
      {"identity denied",
       multistatus(propstat(type + permissions) + propstat("<oc:id/>", 403)),
       "PERMISSION_DENIED"},
      {"identity locked",
       multistatus(propstat(type + permissions) + propstat("<oc:id/>", 423)),
       "REMOTE_LOCKED"},
      {"identity throttled",
       multistatus(propstat(type + permissions) + propstat("<oc:id/>", 429)),
       "SERVER_THROTTLED"},
      {"throttle survives another failed propstat",
       multistatus(propstat(type + permissions) + propstat("<oc:id/>", 429) +
                   propstat("<oc:id/>", 404)),
       "SERVER_THROTTLED"},
      {"successful identity overrides throttling",
       multistatus(propstat(type + permissions) + propstat("<oc:id/>", 429) +
                   propstat(id)),
       ""},
      {"permissions denied",
       multistatus(propstat(type + id) + propstat("<oc:permissions/>", 403)),
       "PERMISSION_DENIED"},
      {"permissions missing", multistatus(propstat(type + id)),
       "INVALID_SERVER_RESPONSE"},
      {"type locked",
       multistatus(propstat(id + permissions) +
                   propstat("<d:resourcetype/>", 423)),
       "REMOTE_LOCKED"},
      {"response locked",
       multistatus("<d:status>HTTP/1.1 423 Locked</d:status>"),
       "REMOTE_LOCKED"},
      {"identity missing", multistatus(propstat(type + permissions)),
       "INVALID_SERVER_RESPONSE"},
      {"identity empty", multistatus(propstat(type + "<oc:id/>" + permissions)),
       "INVALID_SERVER_RESPONSE"},
      {"identity unsupported",
       multistatus(propstat(type + permissions) + propstat("<oc:id/>", 404)),
       "INVALID_SERVER_RESPONSE"},
      {"malformed metadata", "<broken", "INVALID_SERVER_RESPONSE"},
      {"missing root", "<d:multistatus xmlns:d=\"DAV:\"/>",
       "INVALID_SERVER_RESPONSE"}};
  int failures = 0;
  for (const auto &test : cases) {
    try {
      const auto result =
          parseCollections(test.body, QUrl("https://owned.example.invalid/"),
                           "owned", "/root", false);
      require(QByteArray(test.reason).isEmpty(),
              "unavailable metadata accepted");
      require(result.size() == 1 && result.front().identity == "owned-root" &&
                  result.front().permissions == "RMG" &&
                  !result.front().encrypted,
              "valid metadata changed");
    } catch (const std::exception &error) {
      if (QByteArray(error.what()) != test.reason) {
        std::fprintf(stderr, "FAIL %s expected=%s actual=%s\n", test.name,
                     test.reason, error.what());
        ++failures;
      }
    }
  }
  return failures;
}
/// deadlines runs two-step health probes (status.php, then the authenticated
/// user check) on a controlled clock: a server that answers slowly but
/// answers is not NETWORK_UNAVAILABLE; one that never reaches status.php in
/// time, or never answers the credentialed step, still is.
/// Notes: the shared server took 2.3 s per fresh authentication and over 4 s
/// under concurrent sign-ins, which the former four-second total misread.
int deadlines() {
  struct Case {
    const char *name;
    qint64 status, user; // milliseconds each step takes
    bool reachable;
  };
  const Case cases[] = {
      {"fast server", 120, 300, true},
      {"fresh authentication", 300, 2300, true},
      {"loaded server", 3000, 4500, true},
      {"relay 1.5 s per flight", 3000, 1500, true},
      {"status beyond reach", 4000, 100, false},
      {"credentialed step never answered", 300, 15000, false},
  };
  int failures = 0;
  for (const auto &test : cases) {
    qint64 clock = 0; // the probe's monotonic time
    bool timedOut = false;
    for (const auto &[taken, credentialed] :
         {std::pair{test.status, false}, std::pair{test.user, true}}) {
      const auto sent = clock;
      clock += taken;
      if (clock - sent >= metadataDeadline(credentialed)) {
        timedOut = true;
        break;
      }
    }
    if (timedOut == test.reachable) {
      std::fprintf(stderr, "FAIL deadline %s\n", test.name);
      ++failures;
    }
  }
  return failures;
}
} // namespace

/// main exercises synthetic HTTP/DAV boundaries, not real DAV acceptance.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto failures = replies() + collections() + deadlines();
  if (failures) {
    std::fprintf(stderr, "FAIL namespace metadata cases=%d\n", failures);
    return 1;
  }
  std::puts("PASS namespace HTTP classification, bounded Retry-After, metadata "
              "deadlines, DAV "
            "property availability and valid metadata (synthetic only)");
  return 0;
}
