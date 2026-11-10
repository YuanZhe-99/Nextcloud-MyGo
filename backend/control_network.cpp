// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "namespace.h"
#include "switch_state.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QScopedValueRollback>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
/// execute refuses silent policy persistence failure without disclosing stored
/// data.
void execute(QSqlQuery &q) {
  if (!q.exec())
    throw std::runtime_error("INTERNAL_ERROR");
}
/// uuid creates one operation identity independently of endpoint and epoch.
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
/// revision derives an evidence revision from exact, nonsecret configuration
/// bytes.
quint64 revision(const QByteArray &bytes) {
  const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
  quint64 result = 0;
  for (int i = 0; i < 8; ++i)
    result = (result << 8) | static_cast<unsigned char>(hash[i]);
  return result;
}
/// outcome classifies only security/authentication failures as hard selection
/// blocks.
EndpointPolicy::Outcome outcome(const QString &reason) {
  if (reason.isEmpty())
    return EndpointPolicy::Outcome::Success;
  if (reason == "TLS_ERROR")
    return EndpointPolicy::Outcome::TLSFailure;
  if (reason == "ENDPOINT_IDENTITY_MISMATCH" || reason == "ENDPOINT_UNTRUSTED")
    return EndpointPolicy::Outcome::IdentityFailure;
  if (reason == "AUTH_REQUIRED" || reason == "CREDENTIALS_LOCKED")
    return EndpointPolicy::Outcome::AuthFailure;
  return EndpointPolicy::Outcome::NetworkFailure;
}
/// HealthProbe performs one public+approved authenticated sequence under one
/// total four-second deadline. Notes: no redirects, user content or writes;
/// QObject ownership cancels replies before callback storage dies.
class HealthProbe final : public QObject {
public:
  /// HealthProbe starts read-only checks after its complete callback and
  /// bounded lifetime are installed.
  HealthProbe(QObject *owner, QUrl base, QString uid, QString login,
              QString credential, std::function<void(QString, qint64)> done,
              std::function<bool()> allowed)
      : QObject(owner), base_(std::move(base)), uid_(std::move(uid)),
        login_(std::move(login)), credential_(std::move(credential)),
        done_(std::move(done)), allowed_(std::move(allowed)), network_(this) {
    clock_.start();
    deadline_.setSingleShot(true);
    QObject::connect(&deadline_, &QTimer::timeout, this,
                     [this] { finish("NETWORK_UNAVAILABLE"); });
    deadline_.start(4000);
    request(false);
  }

private:
  /// request issues one validated-origin request, only attaching credentials on
  /// the second approved step.
  void request(bool authenticated) {
    if (!allowed_()) {
      finish("CANCELLED");
      return;
    }
    const auto url = base_.resolved(QUrl(
        authenticated ? "ocs/v2.php/cloud/user?format=json" : "status.php"));
    if (!withinServer(base_, url)) {
      finish("ENDPOINT_UNTRUSTED");
      return;
    }
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("OCS-APIRequest", "true");
    if (authenticated) {
      if (login_.isEmpty() || login_.contains(':')) {
        finish("AUTH_REQUIRED");
        return;
      }
      try {
        auto secret = readCredential(credential_).toUtf8();
        req.setRawHeader("Authorization",
                         "Basic " +
                             (login_.toUtf8() + ':' + secret).toBase64());
        secret.fill(0);
      } catch (const std::exception &e) {
        finish(QString::fromUtf8(e.what()) == "AUTH_REQUIRED"
                   ? "AUTH_REQUIRED"
                   : "CREDENTIALS_LOCKED");
        return;
      }
    }
    body_.clear();
    reply_ = network_.get(req);
    reply_->setReadBufferSize(1048577);
    QObject::connect(reply_, &QIODevice::readyRead, this, [this] {
      if (finished_)
        return;
      body_.append(reply_->readAll());
      if (body_.size() > 1048576) {
        // Preserve status-based constraints even when an error response has an
        // oversized body; body limits cannot turn 429 into transport evidence.
        try {
          checkMetadataReply(*reply_, 200, QDateTime::currentDateTimeUtc());
        } catch (const MetadataError &error) {
          throttle_ = error.retryAfterMs;
          finish(QString::fromUtf8(error.what()));
          return;
        }
        finish("INVALID_SERVER_RESPONSE");
      }
    });
    QObject::connect(
        reply_, &QNetworkReply::finished, this, [this, authenticated] {
          if (finished_)
            return;
          auto *reply = reply_;
          body_.append(reply->readAll());
          try {
            checkMetadataReply(*reply, 200, QDateTime::currentDateTimeUtc());
          } catch (const MetadataError &error) {
            throttle_ = error.retryAfterMs;
            finish(QString::fromUtf8(error.what()));
            return;
          }
          if (body_.size() > 1048576 ||
              !reply->header(QNetworkRequest::ContentTypeHeader)
                   .toString()
                   .startsWith("application/json", Qt::CaseInsensitive)) {
            finish("INVALID_SERVER_RESPONSE");
            return;
          }
          QJsonParseError error;
          const auto doc = QJsonDocument::fromJson(body_, &error);
          if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            finish("INVALID_SERVER_RESPONSE");
            return;
          }
          if (!authenticated) {
            const auto status = doc.object();
            if (!status.value("installed").toBool() ||
                status.value("maintenance").toBool(true) ||
                status.value("needsDbUpgrade").toBool(true) ||
                !status.value("versionstring").isString()) {
              finish("INVALID_SERVER_RESPONSE");
              return;
            }
            reply_->deleteLater();
            reply_ = nullptr;
            request(true);
            return;
          }
          const auto ocs = doc.object().value("ocs").toObject();
          const auto code =
              ocs.value("meta").toObject().value("statuscode").toInt();
          if (code == 401 || code == 403 || code == 423 || code == 429) {
            finish(code == 401   ? "AUTH_REQUIRED"
                   : code == 403 ? "PERMISSION_DENIED"
                   : code == 423 ? "REMOTE_LOCKED"
                                 : "SERVER_THROTTLED");
            return;
          }
          if ((code != 100 && code != 200) ||
              ocs.value("data").toObject().value("id").toString() != uid_) {
            finish("ENDPOINT_IDENTITY_MISMATCH");
            return;
          }
          finish({});
        });
  }
  /// finish delivers exactly once and aborts only this read-only probe, never a
  /// sync request.
  void finish(const QString &reason) {
    if (finished_)
      return;
    finished_ = true;
    deadline_.stop();
    if (reply_ && !reply_->isFinished())
      reply_->abort();
    auto callback = std::move(done_);
    deleteLater();
    callback(reason, reason == "SERVER_THROTTLED" || reason == "REMOTE_LOCKED"
                         ? throttle_
                         : clock_.elapsed());
  }
  QUrl base_;
  QString uid_, login_, credential_;
  std::function<void(QString, qint64)> done_;
  std::function<bool()> allowed_;
  QNetworkAccessManager network_;
  QNetworkReply *reply_ = nullptr;
  QTimer deadline_;
  QElapsedTimer clock_;
  QByteArray body_;
  bool finished_ = false;
  qint64 throttle_ = 60000;
};
} // namespace

void Control::startNetwork() {
#ifdef SYNCD_WITH_ENGINE
  if (shutdown_ || storageFailed_ || (network_ && network_->monitor))
    return;
  if (!network_) {
    network_ = std::make_unique<NetworkState>();
    network_->clock.start();
  }
  network_->monitor = std::make_unique<NetworkMonitor>();
  network_->monitor->changed = [this](bool online) { networkChanged(online); };
  network_->timer.setInterval(250);
  QObject::connect(&network_->timer, &QTimer::timeout, &networkTasks_,
                   [this] { evaluateNetwork(); });
  network_->timer.start();
#endif
}

void Control::restoreAccountConstraints(qint64 utcNow, qint64 monotonicNow) {
  if (utcNow < 0)
    utcNow = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
  if (!constraintClock_.isValid())
    constraintClock_.start();
  const auto now = monotonicNow < 0 ? constraintClock_.elapsed() : monotonicNow;
  QSqlQuery q(db_);
  q.prepare("SELECT account_id,reason,record_version,received_utc,delay_ms,"
            "deadline_utc FROM account_constraints");
  execute(q);
  std::map<QString, qint64> restored;
  QStringList expired;
  while (q.next()) {
    const auto account = q.value(0).toString();
    const auto received = q.value(3).toLongLong(),
               delay = q.value(4).toLongLong();
    const bool indefinite = delay == -1;
    if (account.isEmpty() || q.value(1) != "SERVER_THROTTLED" ||
        q.value(2).toInt() != 1 || received <= 0 ||
        (!indefinite &&
         (delay < 60000 || delay > 604800000 ||
          received > std::numeric_limits<qint64>::max() - delay ||
          q.value(5).isNull() ||
          q.value(5).toLongLong() != received + delay)) ||
        (indefinite && !q.value(5).isNull()))
      throw std::runtime_error("INTERNAL_ERROR");
    // A clock rolled back before receipt cannot prove any of the requested
    // delay elapsed. Reapply the full bounded interval, never a shorter one.
    const auto remaining = indefinite ? -1
                           : utcNow < received
                               ? delay
                               : std::max<qint64>(0, received + delay - utcNow);
    if (remaining == 0)
      expired.append(account);
    else
      restored[account] = indefinite ? -1 : now + remaining;
  }
  q.finish();
  if (!expired.isEmpty()) {
    if (!db_.transaction())
      throw std::runtime_error("INTERNAL_ERROR");
    try {
      for (const auto &account : expired) {
        q.prepare("DELETE FROM account_constraints WHERE account_id=?");
        q.addBindValue(account);
        execute(q);
      }
      if (!db_.commit())
        throw std::runtime_error("INTERNAL_ERROR");
    } catch (...) {
      db_.rollback();
      throw;
    }
  }
  accountThrottleUntil_ = std::move(restored);
}

void Control::throttleAccount(const QString &account, qint64 delayMs) {
  if (storageFailed_)
    throw std::runtime_error("INTERNAL_ERROR");
  const auto now = constraintClock_.elapsed();
  const auto previous = accountThrottleUntil_.find(account);
  if (previous != accountThrottleUntil_.end() && previous->second < 0)
    return; // The already committed indefinite deadline cannot be shortened.
  const auto remaining = throttleRemaining(account, now);
  const auto delay = delayMs < 0 || delayMs > 604800000
                         ? -1
                         : std::max<qint64>(60000, delayMs);
  if (delay >= 0 && remaining >= delay)
    return; // Existing durable and monotonic evidence already covers it.
  const auto received = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
  try {
    if (!db_.transaction())
      throw std::runtime_error("INTERNAL_ERROR");
    QSqlQuery q(db_);
    q.prepare("SELECT 1 FROM accounts WHERE id=?");
    q.addBindValue(account);
    execute(q);
    if (!q.next()) {
      db_.rollback();
      return; // A late reply cannot recreate a removed account or its gate.
    }
    q.finish();
    q.prepare(
        "INSERT INTO account_constraints VALUES(?,'SERVER_THROTTLED',1,?,?,?) "
        "ON CONFLICT(account_id) DO UPDATE SET "
        "received_utc=excluded.received_utc,"
        "delay_ms=excluded.delay_ms,deadline_utc=excluded.deadline_utc");
    q.addBindValue(account);
    q.addBindValue(received);
    q.addBindValue(delay);
    q.addBindValue(delay < 0 ? QVariant{} : QVariant(received + delay));
    execute(q);
    if (!db_.commit())
      throw std::runtime_error("INTERNAL_ERROR");
  } catch (...) {
    db_.rollback();
    stopForStorageFailure();
    throw std::runtime_error("INTERNAL_ERROR");
  }
  // Never acknowledge or apply an uncommitted throttle as a successful busy
  // response. Admission is stopped instead if durable storage is unavailable.
  accountThrottleUntil_[account] = delay < 0 ? -1 : now + delay;
#ifdef SYNCD_WITH_ENGINE
  if (runtime_)
    runtime_->throttleAccount(account, delay);
#endif
}

qint64 Control::throttleRemaining(const QString &account, qint64 now) const {
  const auto found = accountThrottleUntil_.find(account);
  if (found == accountThrottleUntil_.end())
    return 0;
  if (found->second < 0)
    return std::numeric_limits<qint64>::max();
  if (now < 0)
    now = constraintClock_.elapsed();
  return std::max<qint64>(0, found->second - now);
}

void Control::metadataFailed(const QString &account,
                             const std::exception &error) {
  if (const auto *metadata = dynamic_cast<const MetadataError *>(&error);
      metadata && QString::fromUtf8(error.what()) == "SERVER_THROTTLED")
    throttleAccount(account, metadata->retryAfterMs);
}

void Control::networkChanged(bool online) {
  if (!network_)
    return;
  network_->online = online;
  ++network_->generation;
#ifdef SYNCD_WITH_ENGINE
  runtime_->networkChanged(network_->generation);
#endif
  const auto now = network_->clock.elapsed();
  for (auto &[account, state] : network_->accounts) {
    state.policy.networkChanged(now);
    state.businessSuccess = -30000;
#ifdef SYNCD_WITH_ENGINE
    for (auto &[folder, count] : state.businessCounts)
      if (runtime_->contains(folder))
        count = runtime_->requests(folder).successes;
#endif
    for (auto &[id, node] : state.nodes)
      node.next = now;
  }
}

void Control::refreshNetwork(const QString &account) {
  if (!network_)
    return;
  auto &state = network_->accounts[account];
  const auto binding = SwitchState(db_).binding(account);
  const auto now = network_->clock.elapsed();
  if (state.epoch != binding.epoch) {
    state.epoch = binding.epoch;
    state.businessSuccess = -30000;
    state.businessCounts.clear();
#ifdef SYNCD_WITH_ENGINE
    QSqlQuery folders(db_);
    folders.prepare("SELECT id FROM folders WHERE account_id=?");
    folders.addBindValue(account);
    execute(folders);
    while (folders.next()) {
      const auto id = folders.value(0).toString();
      if (runtime_->contains(id))
        state.businessCounts[id] = runtime_->requests(id).successes;
    }
#endif
  }
  if (state.active != binding.endpoint) {
    state.active = binding.endpoint;
    state.policy.activated(binding.endpoint, now);
  }
  QSqlQuery q(db_);
  q.prepare("SELECT mode,fixed_id,revision FROM endpoint_policies WHERE "
            "account_id=?");
  q.addBindValue(account);
  execute(q);
  state.mode = q.next() ? q.value(0).toString() : QStringLiteral("fixed");
  state.fixed = state.mode == "fixed"
                    ? (q.isValid() ? q.value(1).toString() : binding.endpoint)
                    : QString{};
  state.policyRevision = q.isValid() ? q.value(2).toULongLong() : 0;
  q.finish();
  q.prepare("SELECT id,remote_path,remote_identity,local_identity FROM folders "
            "WHERE account_id=? ORDER BY id");
  q.addBindValue(account);
  execute(q);
  QJsonArray roots;
  while (q.next())
    roots.append(QJsonArray{q.value(0).toString(), q.value(1).toString(),
                            q.value(2).toString(), q.value(3).toString()});
  q.finish();
  const auto identity = QJsonDocument(roots).toJson(QJsonDocument::Compact);
  q.prepare("SELECT endpoint_id,base_url,'verified' FROM accounts WHERE id=? "
            "AND state='ready' UNION ALL SELECT id,base_url,state FROM "
            "endpoints WHERE account_id=? AND approved=1");
  q.addBindValue(account);
  q.addBindValue(account);
  execute(q);
  QStringList present;
  while (q.next()) {
    const auto id = q.value(0).toString();
    present.append(id);
    auto &node = state.nodes[id];
    const auto server = canonicalServer(q.value(1).toString());
    const auto signature = identity + server.toEncoded();
    if (node.identity != signature) {
      node.identity = signature;
      node.verified = false;
      node.next = now;
    }
    node.server = server;
    QSqlQuery blocked(db_);
    blocked.prepare("SELECT reason FROM endpoint_quarantine WHERE account_id=? "
                    "AND endpoint_id=?");
    blocked.addBindValue(account);
    blocked.addBindValue(id);
    execute(blocked);
    node.config.approved = true;
    node.config.enabled =
        q.value(2).toString() != "rejected" && !blocked.next();
    node.config.tlsValid = node.verified;
    node.config.namespaceVerified = node.verified;
    node.config.endpointRevision = revision(signature);
    node.config.bindingRevision = quint64(binding.epoch);
    node.config.policyRevision = state.policyRevision;
    state.policy.configure(id, node.config);
  }
  for (auto it = state.nodes.begin(); it != state.nodes.end();) {
    if (!present.contains(it->first)) {
      state.policy.remove(it->first);
      it = state.nodes.erase(it);
    } else
      ++it;
  }
}

void Control::networkOutcome(const QString &account,
                             const EndpointPolicy::Ticket &ticket,
                             const QString &reason, qint64 rtt) {
#ifdef SYNCD_WITH_ENGINE
  if (!network_ || shutdown_ || storageFailed_ || !shutdownOperation_.isEmpty())
    return;
  try {
    refreshNetwork(account);
    auto &state = network_->accounts.at(account);
    const auto now = network_->clock.elapsed();
    if (reason == "SERVER_THROTTLED")
      throttleAccount(account, rtt);
    if (reauthenticating(account)) {
      state.policy.abandon(ticket);
      return;
    }
    if (reason == "SERVER_THROTTLED" || reason == "PERMISSION_DENIED" ||
        reason == "REMOTE_LOCKED" || reason == "CANCELLED") {
      if (state.policy.abandon(ticket)) {
        const auto delay = reason == "SERVER_THROTTLED"
                               ? throttleRemaining(account)
                           : reason == "REMOTE_LOCKED" && rtt > 0 ? rtt
                                                                  : 60000;
        const auto until =
            rtt < 0 && reason == "REMOTE_LOCKED"
                ? std::numeric_limits<qint64>::max()
                : now +
                      std::min(delay, std::numeric_limits<qint64>::max() - now);
        state.nodes.at(ticket.endpoint).next = until;
        state.nextSwitch = std::max(state.nextSwitch, until);
      }
      return;
    }
    if (!state.policy.observe(ticket, outcome(reason), now, rtt,
                              QRandomGenerator::global()->generateDouble()))
      return;
    if (reason == "TLS_ERROR" || reason == "ENDPOINT_IDENTITY_MISMATCH" ||
        reason == "ENDPOINT_UNTRUSTED") {
      accountChange(
          [&] { quarantineEndpoint(account, ticket.endpoint, reason); });
    }
    auto &node = state.nodes.at(ticket.endpoint);
    if (ticket.endpoint == state.active &&
        (reason == "AUTH_REQUIRED" || reason == "CREDENTIALS_LOCKED"))
      state.authFailed = ticket.endpoint;
    const auto health = state.policy.health(ticket.endpoint, now);
    // Candidate collection at approximately 0/5/15 seconds; healthy idle
    // polling thereafter has ±20% jitter.
    const bool collecting = health.successes < 3 || health.span < 15000 ||
                            health.halfOpen || !reason.isEmpty();
    node.next =
        now +
        (collecting
             ? (health.successes == 2 ? 10000 : 5000)
             : qint64(
                   (ticket.endpoint == state.active ? 30000 : 60000) *
                   (0.8 + 0.4 * QRandomGenerator::global()->generateDouble())));
    // Only a failed probe blocks. A success on a recovering (half-open) active
    // endpoint is recovery evidence: it must not fail the freshly verified
    // switch that activated it.
    if (ticket.endpoint == state.active && health.failed && !reason.isEmpty()) {
      state.networkBlocked = true;
      runtime_->quiesceAccount(account, true);
      accountChange([&] {
        QSqlQuery q(db_);
        q.prepare(
            "UPDATE folders SET status='offline',reason=? WHERE account_id=?");
        q.addBindValue(reason);
        q.addBindValue(account);
        execute(q);
      });
      const auto epoch = SwitchState(db_).binding(account).epoch;
      QTimer::singleShot(
          0, &networkTasks_,
          [this, account, reason, epoch, generation = network_->generation] {
            endpointResult(account, false, reason, quint64(epoch), generation);
          });
    }
  } catch (const std::exception &e) {
    if (QString::fromUtf8(e.what()) != "NOT_FOUND")
      stopForStorageFailure();
  }
#else
  Q_UNUSED(account);
  Q_UNUSED(ticket);
  Q_UNUSED(reason);
  Q_UNUSED(rtt);
#endif
}

void Control::reviewEndpoint(const QString &account, const QString &endpoint) {
  QSqlQuery q(db_);
  q.prepare(
      "DELETE FROM endpoint_quarantine WHERE account_id=? AND endpoint_id=?");
  q.addBindValue(account);
  q.addBindValue(endpoint);
  execute(q);
  if (!network_)
    return;
  refreshNetwork(account);
  auto &state = network_->accounts.at(account);
  auto &node = state.nodes.at(endpoint);
  node.verified = true;
  node.config.tlsValid = true;
  node.config.namespaceVerified = true;
  state.policy.configure(endpoint, node.config);
  state.policy.reviewed(endpoint);
  node.next = 0;
#ifdef SYNCD_WITH_ENGINE
  if (state.networkBlocked && state.active == endpoint) {
    QTimer::singleShot(0, &networkTasks_, [this, account, endpoint] {
      try {
        const QJsonObject params{
            {"accountId", account},
            {"endpointId", endpoint},
            {"operationKey", uuid()},
            {"expectedRevision", QString::number(revision_)}};
        switchMutation(params,
                       QCryptographicHash::hash(
                           QJsonDocument(params).toJson(QJsonDocument::Compact),
                           QCryptographicHash::Sha256),
                       false, "recovery");
      } catch (const std::exception &e) {
        if (QString::fromUtf8(e.what()) == "INTERNAL_ERROR")
          stopForStorageFailure();
      }
    });
  }
#endif
}

void Control::quarantineEndpoint(const QString &account,
                                 const QString &endpoint,
                                 const QString &reason) {
  QSqlQuery q(db_);
  q.prepare("INSERT OR REPLACE INTO endpoint_quarantine VALUES(?,?,?)");
  q.addBindValue(account);
  q.addBindValue(endpoint);
  q.addBindValue(reason);
  execute(q);
  if (network_) {
    refreshNetwork(account);
    auto &state = network_->accounts.at(account);
    if (state.active == endpoint)
      state.networkBlocked = true;
  }
#ifdef SYNCD_WITH_ENGINE
  if (SwitchState(db_).binding(account).endpoint == endpoint)
    runtime_->quiesceAccount(account, true);
#endif
}

void Control::evaluateNetwork() {
#ifdef SYNCD_WITH_ENGINE
  if (!network_ || network_->evaluating || dispatching_ || storageFailed_ ||
      shutdown_ || !shutdownOperation_.isEmpty())
    return;
  QScopedValueRollback guard(network_->evaluating, true);
  try {
    QSqlQuery q(db_);
    q.prepare("SELECT id FROM accounts WHERE state='ready' ORDER BY id");
    execute(q);
    QStringList accounts;
    while (q.next())
      accounts.append(q.value(0).toString());
    q.finish();
    for (const auto &account : accounts) {
      if (reauthenticating(account) || endpointChecks_.contains(account))
        continue;
      refreshNetwork(account);
      auto &state = network_->accounts.at(account);
      const auto now = network_->clock.elapsed();
      if (throttleRemaining(account) > 0)
        continue;
      q.prepare(
          "SELECT s.phase,o.receipt FROM switch_intents s JOIN operations o "
          "ON o.id=s.operation_id WHERE s.account_id=?");
      q.addBindValue(account);
      execute(q);
      const auto phase = q.next() ? q.value(0).toString() : QString{};
      const bool permissionBlocked =
          phase == "blocked" &&
          QJsonDocument::fromJson(q.value(1).toByteArray())
                  .object()
                  .value("error")
                  .toObject()
                  .value("data")
                  .toObject()
                  .value("reasonCode") == "PERMISSION_DENIED";
      q.finish();
      q.prepare("SELECT id FROM folders WHERE account_id=?");
      q.addBindValue(account);
      execute(q);
      while (q.next()) {
        const auto folder = q.value(0).toString();
        if (!runtime_->contains(folder))
          continue;
        const auto count = runtime_->requests(folder).successes;
        auto &previous = state.businessCounts[folder];
        if (count > previous) {
          state.businessSuccess = now;
          state.policy.businessSuccess(state.active, now);
        }
        previous = count;
      }
      q.finish();
      if (phase == "prepared")
        continue;
      // Offline hint suppresses fast work; native recovery or a low-frequency
      // fallback restarts it.
      if (!network_->online && now % 60000 >= 250)
        continue;
      QStringList probeIds;
      for (const auto &[id, node] : state.nodes)
        probeIds.append(id);
      for (const auto &id : probeIds) {
        if (throttleRemaining(account) > 0)
          break;
        const auto candidate = state.nodes.find(id);
        if (candidate == state.nodes.end())
          continue;
        auto &node = candidate->second;
        if (network_->probes >= 4 || state.probes >= 2)
          break;
        if (!node.config.enabled || node.next > now || node.inFlight)
          continue;
        if (!state.authFailed.isEmpty() && id != state.authFailed) {
          if (state.authAlternate.isEmpty())
            state.authAlternate = id;
          if (state.authAlternate != id)
            continue;
        }
        if (id == state.active && state.businessSuccess >= 0 &&
            now - state.businessSuccess < 30000 && node.verified)
          continue;
        auto ticket = state.policy.beginProbe(id, now);
        if (!ticket)
          continue;
        if (!node.verified) {
          const auto generation = network_->generation,
                     policy = state.policyRevision;
          const auto signature = node.identity;
          const auto epoch = node.config.bindingRevision;
          QString reason;
          qint64 retryAfterMs = 0;
          try {
            verifySwitchTarget(account, id);
          } catch (const std::exception &e) {
            if (const auto *metadata = dynamic_cast<const MetadataError *>(&e))
              retryAfterMs = metadata->retryAfterMs;
            reason = QString::fromUtf8(e.what());
          }
          refreshNetwork(account);
          const auto found = state.nodes.find(id);
          if (found == state.nodes.end())
            continue;
          auto &verified = found->second;
          if (generation != network_->generation ||
              policy != state.policyRevision ||
              signature != verified.identity ||
              epoch != verified.config.bindingRevision)
            continue;
          // Updating the verified config invalidates the old ticket; publish
          // only to a new matching one.
          if (reason.isEmpty()) {
            verified.verified = true;
            verified.config.tlsValid = true;
            verified.config.namespaceVerified = true;
            state.policy.configure(id, verified.config);
            verified.next = now;
            break; // A full-root check is not a lightweight RTT/independent
                   // stability sample.
          }
          if (reason == "REMOTE_ROOT_UNAVAILABLE" ||
              reason == "UNSUPPORTED_CAPABILITY" ||
              reason == "UNSUPPORTED_FOLDER_MODE") {
            state.policy.abandon(*ticket);
            verified.next = network_->clock.elapsed() + 60000;
            break;
          }
          if (ticket)
            networkOutcome(account, *ticket, reason, retryAfterMs);
          break; // full-root verification is serialized and never multiplied by
                 // nested event loops
        }
        q.prepare("SELECT uid,login_name,credential_ref FROM accounts WHERE "
                  "id=? AND state='ready'");
        q.addBindValue(account);
        execute(q);
        if (!q.next())
          break;
        const auto uid = q.value(0).toString(), login = q.value(1).toString(),
                   credential = q.value(2).toString();
        q.finish();
        if (credential != credentialReference(account)) {
          networkOutcome(account, *ticket, "AUTH_REQUIRED", 0);
          continue;
        }
        ++state.probes;
        ++network_->probes;
        node.inFlight = true;
        new HealthProbe(
            &networkTasks_, node.server, uid, login, credential,
            [this, account, ticket = *ticket](const QString &reason,
                                              qint64 rtt) {
              if (!network_)
                return;
              --network_->probes;
              auto it = network_->accounts.find(account);
              if (it != network_->accounts.end())
                --it->second.probes;
              if (it != network_->accounts.end() &&
                  it->second.nodes.contains(ticket.endpoint))
                it->second.nodes.at(ticket.endpoint).inFlight = false;
              // Permissions/rate limits do not become transport
              // evidence, and are retried conservatively.
              networkOutcome(account, ticket, reason, rtt);
            },
            [this, account, ticket = *ticket] {
              return probeAllowed(account, ticket);
            });
      }
      if (throttleRemaining(account) > 0 || permissionBlocked ||
          phase == "reconciling" || now < state.nextSwitch)
        continue;
      const auto proposal = state.policy.choose(
          now, state.mode == "fixed" ? state.fixed : QString{});
      QString target = proposal.endpoint;
      if (phase == "blocked" && target.isEmpty() &&
          state.policy.health(state.active, now).fresh &&
          state.policy.health(state.active, now).successes >= 2 &&
          (state.mode != "fixed" || state.fixed == state.active))
        target = state.active;
      if (target.isEmpty()) {
        if (state.networkBlocked && phase.isEmpty() &&
            state.policy.health(state.active, now).fresh &&
            !state.policy.health(state.active, now).failed) {
          runtime_->resumeAccount(account);
          state.networkBlocked = false;
        }
        continue;
      }
      const QJsonObject params{
          {"accountId", account},
          {"endpointId", target},
          {"operationKey", uuid()},
          {"expectedRevision", QString::number(revision_)}};
      state.nextSwitch = now + 60000;
      try {
        switchMutation(params,
                       QCryptographicHash::hash(
                           QJsonDocument(params).toJson(QJsonDocument::Compact),
                           QCryptographicHash::Sha256),
                       false,
                       proposal.reason == "faster"
                           ? QStringLiteral("preference")
                           : QStringLiteral("failover"));
      } catch (const std::exception &e) {
        if (QString::fromUtf8(e.what()) == "INTERNAL_ERROR")
          throw;
      }
    }
  } catch (const std::exception &e) {
    // A root/account may be removed during nested metadata work. Only storage
    // failures stop the daemon.
    const QString reason = QString::fromUtf8(e.what());
    if (reason == "INTERNAL_ERROR")
      stopForStorageFailure();
  }
#endif
}

bool Control::probeAllowed(const QString &account,
                           const EndpointPolicy::Ticket &ticket) {
  if (!network_ || storageFailed_ || shutdown_ ||
      !shutdownOperation_.isEmpty() || reauthenticating(account) ||
      throttleRemaining(account) > 0)
    return false;
  try {
    refreshNetwork(account);
    return network_->accounts.at(account).policy.current(ticket);
  } catch (const std::exception &e) {
    if (QString::fromUtf8(e.what()) == "INTERNAL_ERROR")
      stopForStorageFailure();
    return false;
  }
}

QJsonObject Control::policyMutation(const QJsonObject &params,
                                    const QByteArray &digest) {
#ifndef SYNCD_WITH_ENGINE
  Q_UNUSED(params);
  Q_UNUSED(digest);
  throw std::runtime_error("UNSUPPORTED_CAPABILITY");
#else
  if (params.value("expectedRevision").toString() != QString::number(revision_))
    throw std::runtime_error("REVISION_CONFLICT");
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "operationKey" && it.key() != "expectedRevision" &&
        it.key() != "accountId" && it.key() != "policy" &&
        it.key() != "fixedEndpointId")
      throw std::runtime_error("INVALID_PARAMS");
  const auto account = params.value("accountId").toString(),
             mode = params.value("policy").toString(),
             fixed = params.value("fixedEndpointId").toString();
  if (!params.value("accountId").isString() ||
      !params.value("policy").isString() ||
      (params.contains("fixedEndpointId") &&
       !params.value("fixedEndpointId").isString()))
    throw std::runtime_error("INVALID_PARAMS");
  SwitchState store(db_);
  store.binding(account);
  if ((mode != "auto" && mode != "fixed") ||
      (mode == "auto" && !fixed.isEmpty()) ||
      (mode == "fixed" && fixed.isEmpty()))
    throw std::runtime_error("INVALID_PARAMS");
  if (reauthenticating(account) || store.gated(account))
    throw std::runtime_error("QUIESCE_STALLED");
  if (mode == "fixed") {
    store.target(account, fixed);
    auto switchParams = params;
    switchParams.remove("policy");
    switchParams.remove("fixedEndpointId");
    switchParams.insert("endpointId", fixed);
    return switchMutation(switchParams, digest, false, "manual", true);
  }
  const auto operation = uuid();
  const QJsonObject receipt{{"operationId", operation},
                            {"state", "succeeded"},
                            {"result", QJsonObject{}}};
  accountChange([&] {
    QSqlQuery q(db_);
    q.prepare("INSERT INTO endpoint_policies VALUES(?,'auto','',1) ON "
              "CONFLICT(account_id) DO UPDATE SET "
              "mode='auto',fixed_id='',revision=revision+1");
    q.addBindValue(account);
    execute(q);
    q.prepare("INSERT INTO operations(key,id,digest,receipt,method) "
              "VALUES(?,?,?,?,'endpoints.setPolicy')");
    q.addBindValue(params.value("operationKey").toString());
    q.addBindValue(operation);
    q.addBindValue(digest);
    q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    execute(q);
  });
  refreshNetwork(account);
  return receipt;
#endif
}

void Control::completePolicy(const QString &operation) {
  QSqlQuery q(db_);
  q.prepare(
      "SELECT account_id,target_id FROM pending_policies WHERE operation_id=?");
  q.addBindValue(operation);
  execute(q);
  if (!q.next())
    return;
  const auto account = q.value(0).toString(), target = q.value(1).toString();
  q.finish();
  if (SwitchState(db_).binding(account).endpoint != target)
    throw std::runtime_error("REVISION_CONFLICT");
  q.prepare("INSERT INTO endpoint_policies VALUES(?,'fixed',?,1) ON "
            "CONFLICT(account_id) DO UPDATE SET "
            "mode='fixed',fixed_id=excluded.fixed_id,revision=revision+1");
  q.addBindValue(account);
  q.addBindValue(target);
  execute(q);
  q.prepare("DELETE FROM pending_policies WHERE operation_id=?");
  q.addBindValue(operation);
  execute(q);
}

void Control::switchEvidence(const QString &account, qint64 epoch) const {
  QSqlQuery q(db_);
  q.prepare("SELECT generation,policy_revision,binding_epoch FROM "
            "switch_context WHERE account_id=? AND epoch=?");
  q.addBindValue(account);
  q.addBindValue(epoch);
  execute(q);
  if (!q.next())
    throw std::runtime_error("REVISION_CONFLICT");
  if (network_ &&
      q.value(0).toString() != QString::number(network_->generation))
    throw std::runtime_error("REVISION_CONFLICT");
  const auto policy = q.value(1).toULongLong();
  const auto binding = q.value(2).toLongLong();
  q.finish();
  if (SwitchState(db_).binding(account).epoch != binding)
    throw std::runtime_error("REVISION_CONFLICT");
  q.prepare("SELECT revision FROM endpoint_policies WHERE account_id=?");
  q.addBindValue(account);
  execute(q);
  if ((q.next() ? q.value(0).toULongLong() : 0) != policy)
    throw std::runtime_error("REVISION_CONFLICT");
}

void Control::switchFailed(const QString &account, qint64 epoch,
                           const QString &reason) {
#ifdef SYNCD_WITH_ENGINE
  if (!network_ || storageFailed_ || shutdown_ || !shutdownOperation_.isEmpty())
    return;
  QSqlQuery q(db_);
  q.prepare("SELECT from_id,cause,operation_id FROM switch_context WHERE "
            "account_id=? AND epoch=?");
  q.addBindValue(account);
  q.addBindValue(epoch);
  execute(q);
  if (!q.next())
    return;
  auto target = q.value(0).toString();
  const auto cause = q.value(1).toString(), operation = q.value(2).toString();
  q.finish();
  accountChange([&] {
    q.prepare("DELETE FROM pending_policies WHERE operation_id=?");
    q.addBindValue(operation);
    execute(q);
  });
  const QStringList recoverable{
      "NETWORK_UNAVAILABLE", "INVALID_SERVER_RESPONSE",    "TLS_ERROR",
      "ENDPOINT_UNTRUSTED",  "ENDPOINT_IDENTITY_MISMATCH", "AUTH_REQUIRED",
      "REVISION_CONFLICT",   "RESUME_CONTEXT_UNSUPPORTED"};
  if (cause == "recovery" || !recoverable.contains(reason))
    return;
  q.prepare("SELECT mode,fixed_id FROM endpoint_policies WHERE account_id=?");
  q.addBindValue(account);
  execute(q);
  if (q.next() && q.value(0) == "fixed")
    target = q.value(1).toString();
  q.finish();
  if (target.isEmpty())
    return;
  // Recovery always creates a new intent/epoch and repeats all checks. A failed
  // recovery stays offline.
  QTimer::singleShot(0, &networkTasks_, [this, account, target] {
    try {
      const QJsonObject params{
          {"accountId", account},
          {"endpointId", target},
          {"operationKey", uuid()},
          {"expectedRevision", QString::number(revision_)}};
      switchMutation(params,
                     QCryptographicHash::hash(
                         QJsonDocument(params).toJson(QJsonDocument::Compact),
                         QCryptographicHash::Sha256),
                     false, "recovery");
    } catch (const std::exception &e) {
      if (QString::fromUtf8(e.what()) == "INTERNAL_ERROR")
        stopForStorageFailure();
    }
  });
#else
  Q_UNUSED(account);
  Q_UNUSED(epoch);
  Q_UNUSED(reason);
#endif
}

void Control::endpointResult(const QString &account, bool success,
                             const QString &reason, quint64 epoch,
                             quint64 generation) {
#ifdef SYNCD_WITH_ENGINE
  if (!network_ || network_->generation != generation)
    return;
  refreshNetwork(account);
  auto &state = network_->accounts.at(account);
  const auto now = network_->clock.elapsed();
  if (SwitchState(db_).binding(account).epoch != qint64(epoch))
    return;
  if (success) {
    return;
  }
  const QStringList affected{"NETWORK_UNAVAILABLE", "TLS_ERROR",
                             "ENDPOINT_IDENTITY_MISMATCH", "ENDPOINT_UNTRUSTED",
                             "AUTH_REQUIRED"};
  if (!affected.contains(reason))
    return;
  state.businessSuccess = -30000;
  for (auto &[id, node] : state.nodes)
    node.next = now;
  const bool security = reason == "TLS_ERROR" ||
                        reason == "ENDPOINT_IDENTITY_MISMATCH" ||
                        reason == "ENDPOINT_UNTRUSTED";
  if (security || reason == "AUTH_REQUIRED") {
    // Business hard failures cannot wait for a probe slot: that probe may
    // itself complete successfully late.
    state.policy.hardFailure(state.active, outcome(reason));
    state.networkBlocked = true;
    if (reason == "AUTH_REQUIRED")
      state.authFailed = state.active;
    runtime_->quiesceAccount(account, true);
    accountChange([&] {
      if (security)
        quarantineEndpoint(account, state.active, reason);
      QSqlQuery update(db_);
      update.prepare(
          "UPDATE folders SET status='offline',reason=? WHERE account_id=?");
      update.addBindValue(reason);
      update.addBindValue(account);
      execute(update);
    });
  } else if (!state.policy.health(state.active, now).failed) {
    auto ticket = state.policy.beginProbe(state.active, now);
    if (ticket)
      networkOutcome(account, *ticket, reason, 0);
  }
  QSqlQuery q(db_);
  q.prepare("SELECT operation_id FROM switch_intents WHERE account_id=? AND "
            "target_epoch=? AND phase='reconciling'");
  q.addBindValue(account);
  q.addBindValue(qint64(epoch));
  execute(q);
  if (!q.next())
    return;
  const auto operation = q.value(0).toString();
  q.finish();
  runtime_->quiesceAccount(account, true);
  accountChange([&] {
    SwitchState(db_).block(account, qint64(epoch));
    const QJsonObject receipt{
        {"operationId", operation},
        {"state", "failed"},
        {"error", QJsonObject{{"code", -32000},
                              {"message", "Endpoint recovery is required"},
                              {"data", QJsonObject{{"reasonCode", reason}}}}}};
    q.prepare("UPDATE operations SET receipt=? WHERE id=?");
    q.addBindValue(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    q.addBindValue(operation);
    execute(q);
  });
  switchFailed(account, qint64(epoch), reason);
#else
  Q_UNUSED(account);
  Q_UNUSED(success);
  Q_UNUSED(reason);
  Q_UNUSED(epoch);
  Q_UNUSED(generation);
#endif
}
