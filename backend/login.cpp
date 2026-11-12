#include "login.h"
#include "namespace.h"
#include "wire.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <algorithm>
#include <memory>
#include <stdexcept>

QUrl canonicalServer(const QString &input) {
  auto text = input.trimmed();
  if (!text.contains("://"))
    text.prepend("https://");
  QUrl url(text, QUrl::StrictMode);
  if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
      !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
    throw std::runtime_error("Invalid server address");
  auto path = url.path(QUrl::FullyDecoded);
  if (path.contains('\\') || path.contains("//") || path.contains(QChar::Null))
    throw std::runtime_error("Ambiguous server path");
  for (const auto &part : path.split('/'))
    if (part == "." || part == "..")
      throw std::runtime_error("Ambiguous server path");
  const auto encoded = url.path(QUrl::FullyEncoded).toLower();
  if (encoded.contains("%2f") || encoded.contains("%5c") ||
      encoded.contains("%25"))
    throw std::runtime_error("Ambiguous server path");
  if (!path.endsWith('/'))
    path += '/';
  url.setPath(path);
  url.setHost(url.host().toLower());
  if (url.port() == 443)
    url.setPort(-1);
  return url;
}

bool withinServer(const QUrl &base, const QUrl &target) {
  if (!target.isValid() || target.scheme() != "https" ||
      target.host().compare(base.host(), Qt::CaseInsensitive) != 0 ||
      target.port(443) != base.port(443) || !target.userInfo().isEmpty() ||
      target.hasFragment())
    return false;
  const auto path = target.path(QUrl::FullyDecoded);
  if (!path.startsWith(base.path()) || path.contains('\\') ||
      path.contains("//"))
    return false;
  for (const auto &part : path.split('/'))
    if (part == "." || part == "..")
      return false;
  const auto encoded = target.path(QUrl::FullyEncoded).toLower();
  return !encoded.contains("%2f") && !encoded.contains("%5c") &&
         !encoded.contains("%25");
}

LoginFlow::LoginFlow(const QUrl &base, const QString &session,
                     const QString &account)
    : id(session), accountId(account),
      expires(QDateTime::currentDateTimeUtc().addSecs(20 * 60)), base_(base) {
  elapsed_.start();
  deadline_ = now() + 20 * 60 * 1000;
  timer_.setInterval(1500);
  QObject::connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
  timer_.start();
}

void LoginFlow::tick() {
  if (state_ == "failed" || state_ == "cancelled" || state_ == "succeeded")
    return;
  if (state_ == "discarding") {
    // Retiring an acquired password is not acceptance, so it is not bounded
    // by session validity; only the server's own delay holds it back.
    if (!reply_ && !gated())
      revoke();
    return;
  }
  if (now() >= deadline_) {
    fail("LOGIN_SESSION_EXPIRED");
    return;
  }
  if (state_ == "waiting" && !reply_ && !gated())
    poll();
}

void LoginFlow::constrained(const QString &reason, qint64 retryAfterMs) {
  const bool delay = reason != "PERMISSION_DENIED";
  if (delay) {
    // Close the local gate first: a failed commit must admit nothing further.
    gateUntil_ = retryAfterMs < 0 || gateUntil_ < 0
                     ? -1
                     : std::max(gateUntil_,
                                now() + std::max<qint64>(retryAfterMs, 60000));
    if (reason == "SERVER_THROTTLED" && throttled && !throttled(retryAfterMs))
      gateUntil_ = -1;
  }
  if (state_ == "discarding") {
    // The server refused the DELETE, so the password still exists and the
    // request is not uncertain. Keep it owned while a bounded delay permits a
    // later attempt; otherwise report the unconfirmed revocation explicitly.
    if (!delay || gateUntil_ < 0)
      fail("APP_PASSWORD_REVOCATION_UNCONFIRMED");
    else
      hold();
    return;
  }
  // Polling may resume only when the delay ends inside the unextended session.
  if (state_ == "waiting" && delay && gateUntil_ > 0 && gateUntil_ < deadline_)
    return;
  fail(reason);
}

LoginFlow::~LoginFlow() {
  if (reply_) {
    QObject::disconnect(reply_, nullptr, this, nullptr);
    reply_->abort();
  }
  token_.fill(0);
  authorization_.fill(0);
}

void LoginFlow::request(const QUrl &url, const QByteArray &method,
                        const QByteArray &body, const QByteArray &authorization,
                        std::function<void(int, const QByteArray &)> done) {
  if (!withinServer(base_, url)) {
    fail("ENDPOINT_UNTRUSTED");
    return;
  }
  if (gated()) {
    fail("SERVER_THROTTLED");
    return;
  }
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::ManualRedirectPolicy);
  req.setTransferTimeout(30000);
  req.setRawHeader("User-Agent", "Nextcloud-MyGo/" SYNCD_VERSION);
  req.setRawHeader("Accept", "application/json");
  if (!authorization.isEmpty())
    req.setRawHeader("Authorization", authorization);
  if (method == "POST")
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
  if (method == "PROPFIND") {
    req.setRawHeader("Depth", "0");
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/xml; charset=utf-8");
  }
  if (url.path().contains("/ocs/"))
    req.setRawHeader("OCS-APIRequest", "true");
  auto reply = send_ ? send_(req, method, body)
                     : network_.sendCustomRequest(req, method, body);
  reply_ = reply;
  reply->setReadBufferSize(1048577);
  auto bytes = std::make_shared<QByteArray>();
  QObject::connect(reply, &QIODevice::readyRead, this, [this, reply, bytes] {
    bytes->append(reply->readAll());
    if (bytes->size() > 1048576)
      fail("INVALID_SERVER_RESPONSE");
  });
  QObject::connect(
      reply, &QNetworkReply::finished, this,
      [this, reply, bytes, method, done = std::move(done)] {
        reply->deleteLater();
        if (reply_ != reply)
          return;
        reply_ = nullptr;
        bytes->append(reply->readAll());
        if (state_ == "failed" || state_ == "cancelled" ||
            state_ == "succeeded")
          return;
        if (bytes->size() > 1048576) {
          fail("INVALID_SERVER_RESPONSE");
          return;
        }
        const auto code =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
          fail("TLS_ERROR");
          return;
        }
        if (code >= 300 && code < 400) {
          fail("ENDPOINT_UNTRUSTED");
          return;
        }
        if (!code) {
          fail("NETWORK_UNAVAILABLE");
          return;
        }
        if (code == 403 || code == 423 || code == 429) {
          // Same typed outcomes and Retry-After bounds as every other metadata
          // request; never an authentication or identity verdict.
          try {
            checkMetadataReply(*reply, 0, QDateTime::currentDateTimeUtc());
          } catch (const MetadataError &error) {
            constrained(QString::fromUtf8(error.what()), error.retryAfterMs);
          }
          return;
        }
        if (code == 200 && method != "PROPFIND") {
          QJsonDocument parsed;
          if (!decodeMessage(*bytes, parsed) || !parsed.isObject()) {
            fail("INVALID_SERVER_RESPONSE");
            return;
          }
        }
        done(code, *bytes);
      });
}

void LoginFlow::start() {
  if (state_ != "starting")
    return;
  request(base_.resolved(QUrl("index.php/login/v2")), "POST", {}, {},
          [this](int code, const QByteArray &body) {
            const auto obj = QJsonDocument::fromJson(body).object();
            const auto poll = obj.value("poll").toObject();
            auto browser = QUrl(obj.value("login").toString());
            auto endpoint = QUrl(poll.value("endpoint").toString());
            const auto token = poll.value("token").toString().toUtf8();
            if (code != 200 || token.isEmpty() || token.size() > 4096 ||
                !withinServer(base_, browser) ||
                !withinServer(base_, endpoint)) {
              fail("INVALID_SERVER_RESPONSE");
              return;
            }
            loginUrl = browser.toString(QUrl::FullyEncoded);
            pollUrl_ = endpoint;
            token_ = token;
            state_ = "waiting";
            publish();
          });
}

void LoginFlow::poll() {
  const auto body =
      "token=" + QUrl::toPercentEncoding(QString::fromUtf8(token_));
  request(pollUrl_, "POST", body, {}, [this](int code, const QByteArray &body) {
    if (code == 404)
      return;
    const auto obj = QJsonDocument::fromJson(body).object();
    if (code != 200 || obj.value("loginName").toString().isEmpty() ||
        obj.value("appPassword").toString().isEmpty()) {
      fail("AUTH_REQUIRED");
      return;
    }
    try {
      if (canonicalServer(obj.value("server").toString()) != base_) {
        fail("ENDPOINT_UNTRUSTED");
        return;
      }
    } catch (...) {
      fail("ENDPOINT_UNTRUSTED");
      return;
    }
    token_.fill(0);
    token_.clear();
    ownedPassword_ = true;
    verifyCredentials(obj.value("loginName").toString(),
                      obj.value("appPassword").toString());
  });
}

void LoginFlow::verifyCredentials(const QString &user,
                                  const QString &password) {
  if (user.isEmpty() || user.contains(':') || password.isEmpty() ||
      (state_ != "starting" && state_ != "waiting")) {
    fail("AUTH_REQUIRED");
    return;
  }
  loginName = user;
  authorization_ =
      "Basic " + (user.toUtf8() + ':' + password.toUtf8()).toBase64();
  if (!saveCredential || !saveCredential(user, password)) {
    fail("CREDENTIALS_LOCKED");
    return;
  }
  state_ = "verifying";
  request(
      base_.resolved(QUrl("ocs/v2.php/cloud/user?format=json")), "GET", {},
      authorization_, [this](int code, const QByteArray &body) {
        const auto ocs =
            QJsonDocument::fromJson(body).object().value("ocs").toObject();
        const auto data = ocs.value("data").toObject();
        uid = data.value("id").toString();
        displayName = data.value("display-name")
                          .toString(data.value("displayname").toString(uid));
        if (code != 200 ||
            ocs.value("meta").toObject().value("status").toString() != "ok" ||
            uid.isEmpty() || uid.contains('/') || uid.contains('\\')) {
          fail("AUTH_REQUIRED");
          return;
        }
        verifyRoot();
      });
}

void LoginFlow::verifyRoot() {
  const auto root =
      QUrl::fromEncoded(base_.toEncoded() + "remote.php/dav/files/" +
                        QUrl::toPercentEncoding(uid) + '/');
  const QByteArray xml =
      "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\" "
      "xmlns:oc=\"http://owncloud.org/ns\"><d:prop><d:resourcetype/><oc:id/></"
      "d:prop></d:propfind>";
  request(root, "PROPFIND", xml, authorization_,
          [this, root](int code, const QByteArray &body) {
            if (code != 207) {
              fail("ENDPOINT_IDENTITY_MISMATCH");
              return;
            }
            QXmlStreamReader xml(body);
            QString href, candidate, status;
            bool collection = false, matched = false;
            while (!xml.atEnd()) {
              xml.readNext();
              if (xml.isStartElement() && xml.name() == "response" &&
                  xml.namespaceUri() == "DAV:") {
                href.clear();
                candidate.clear();
                collection = false;
              } else if (xml.isStartElement() && xml.name() == "href" &&
                         xml.namespaceUri() == "DAV:")
                href = xml.readElementText();
              else if (xml.isStartElement() && xml.name() == "propstat" &&
                       xml.namespaceUri() == "DAV:") {
                candidate.clear();
                status.clear();
                collection = false;
              } else if (xml.isStartElement() && xml.name() == "status" &&
                         xml.namespaceUri() == "DAV:")
                status = xml.readElementText();
              else if (xml.isStartElement() && xml.name() == "id" &&
                       xml.namespaceUri() == "http://owncloud.org/ns")
                candidate = xml.readElementText();
              else if (xml.isStartElement() && xml.name() == "collection" &&
                       xml.namespaceUri() == "DAV:")
                collection = true;
              else if (xml.isEndElement() && xml.name() == "propstat" &&
                       xml.namespaceUri() == "DAV:" &&
                       status.split(' ').value(1) == "200" && collection &&
                       !candidate.isEmpty() &&
                       root.resolved(QUrl(href)) == root) {
                if (matched) {
                  fail("INVALID_SERVER_RESPONSE");
                  return;
                }
                matched = true;
                rootId = candidate;
              }
            }
            if (xml.hasError() || !matched) {
              fail("ENDPOINT_IDENTITY_MISMATCH");
              return;
            }
            rootVerified_ = true;
            if (verified)
              verified(*this);
            else
              accept();
          });
}

void LoginFlow::accept(bool installed) {
  if (!rootVerified_ || state_ != "verifying")
    return;
  if (installed)
    ownedPassword_ = false;
  state_ = "succeeded";
  publish();
  if (state_ == "succeeded") {
    timer_.stop(); // A refused publication still needs tick to retire it.
    ownedPassword_ = false;
    authorization_.fill(0);
    authorization_.clear();
  }
}

void LoginFlow::reject(const QString &reason) { fail(reason); }

void LoginFlow::publish() {
  if (changed && !changed(*this)) {
    if (state_ == "succeeded") {
      state_ = "verifying";
      fail("INTERNAL_ERROR");
      return;
    }
    state_ = "failed";
    reason_ = "INTERNAL_ERROR";
    timer_.stop();
    token_.fill(0);
    token_.clear();
    authorization_.fill(0);
    authorization_.clear();
    auto reply = reply_;
    reply_ = nullptr;
    if (reply)
      reply->abort();
  }
}

void LoginFlow::fail(const QString &reason) {
  if (state_ == "failed" || state_ == "cancelled" || state_ == "succeeded")
    return;
  if (ownedPassword_ && !authorization_.isEmpty() && state_ != "discarding") {
    // An acquired app password is ours to retire; borrowed acceptance
    // credentials are never revoked here.
    discardReason_ = reason;
    state_ = "discarding";
    auto pending = reply_;
    reply_ = nullptr;
    if (pending)
      pending->abort();
    // A bounded server delay defers the DELETE to tick; the vault slot and
    // the owner's reservation stay until the outcome is known.
    if (gateUntil_ < 0)
      fail("APP_PASSWORD_REVOCATION_UNCONFIRMED");
    else if (!gated())
      revoke();
    else
      hold();
    return;
  }
  if (state_ == "discarding" && ownedPassword_) {
    ownedPassword_ = false;
    discardReason_.clear();
    fail("APP_PASSWORD_REVOCATION_UNCONFIRMED");
    return;
  }
  state_ = "failed";
  reason_ = reason;
  timer_.stop();
  auto reply = reply_;
  reply_ = nullptr;
  if (reply)
    reply->abort();
  token_.fill(0);
  token_.clear();
  authorization_.fill(0);
  authorization_.clear();
  if (reason == "LOGIN_CANCELLED")
    state_ = "cancelled";
  publish();
}

void LoginFlow::revoke() {
  // From here the DELETE's outcome is unknown until answered: the durable
  // "only delayed" record must be gone first, or a restart would send another.
  if (heldRecorded_ && !held(false)) {
    fail("APP_PASSWORD_REVOCATION_UNCONFIRMED");
    return;
  }
  heldRecorded_ = false;
  request(base_.resolved(QUrl("ocs/v2.php/core/apppassword?format=json")),
          "DELETE", {}, authorization_,
          [this](int code, const QByteArray &body) {
            const bool confirmed = code == 200 && QJsonDocument::fromJson(body)
                                                          .object()
                                                          .value("ocs")
                                                          .toObject()
                                                          .value("meta")
                                                          .toObject()
                                                          .value("status")
                                                          .toString() == "ok";
            const auto original = discardReason_;
            ownedPassword_ = false;
            discardReason_.clear();
            fail(confirmed ? original : "APP_PASSWORD_REVOCATION_UNCONFIRMED");
          });
}

void LoginFlow::cancel() {
  if (state_ == "succeeded" || state_ == "failed" || state_ == "cancelled")
    return;
  if (state_ != "discarding")
    fail("LOGIN_CANCELLED");
}
