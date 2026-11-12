// SPDX-License-Identifier: GPL-3.0-only
#include "engine.h"
#include "account.h"
#include "clientsideencryption.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "configfile.h"
#include "creds/abstractcredentials.h"
#include "csync_exclude.h"
#include "deletion.h"
#include "login.h"
#include "namespace.h"
#include "platform/windows/ondemand.h"
#include "request_lifetime.h"
#include "resume_context.h"
#include "root.h"
#include "syncengine.h"
#include "syncoptions.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QEvent>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
/// deletionGroups summarizes a held deletion proposal by top-level entry:
/// a directory counts the files the journal knows below it, loose files are
/// one group. Returns: at most eight groups, largest first, as compact JSON.
QByteArray deletionGroups(const QStringList &baseline,
                          const QList<DeleteAction> &actions) {
  std::map<QString, qint64> files;
  for (const auto &action : actions) {
    const bool loose = !action.path.contains('/') && !action.directory;
    const auto name = loose ? QString{} : action.path.section('/', 0, 0);
    qint64 count = action.directory ? 0 : 1;
    if (action.directory)
      for (const auto &path : baseline)
        count += path.startsWith(action.path + '/');
    files[name] += count;
  }
  std::vector<std::pair<QString, qint64>> sorted(files.begin(), files.end());
  std::stable_sort(
      sorted.begin(), sorted.end(),
      [](const auto &a, const auto &b) { return a.second > b.second; });
  QJsonArray groups;
  for (const auto &[name, count] : sorted) {
    if (groups.size() == 8)
      break;
    groups.append(QJsonObject{
        {"name", name}, {"files", count}, {"loose", name.isEmpty()}});
  }
  return QJsonDocument(groups).toJson(QJsonDocument::Compact);
}
/// RejectedReply completes a denied destination without creating a socket or
/// following redirects.
class RejectedReply final : public QNetworkReply {
public:
  /// RejectedReply exposes a bounded policy error without including the denied
  /// URL in its text.
  RejectedReply(const QNetworkRequest &request, QObject *parent)
      : QNetworkReply(parent) {
    setRequest(request);
    setUrl(request.url());
    open(QIODevice::ReadOnly);
    QTimer::singleShot(0, this, [this] {
      setError(QNetworkReply::ContentAccessDenied,
               "Server address is not approved");
      setFinished(true);
      Q_EMIT errorOccurred(error());
      Q_EMIT finished();
    });
  }
  /// abort is inert because no network operation exists.
  void abort() override {}

protected:
  /// readData reports an empty error body.
  qint64 readData(char *, qint64) override { return -1; }
};

/// BoundNetwork checks every upstream request before credentials are attached
/// (INV-03).
class BoundNetwork final : public QNetworkAccessManager {
public:
  /// BoundNetwork binds a manager to one immutable explicitly approved
  /// installation.
  BoundNetwork(QUrl base, QByteArray authorization,
               std::shared_ptr<EngineAdapter::Requests> requests,
               std::shared_ptr<RequestLifetime> lifetime,
               std::shared_ptr<quint64> networkGeneration)
      : base_(std::move(base)), authorization_(std::move(authorization)),
        requests_(std::move(requests)), lifetime_(std::move(lifetime)),
        networkGeneration_(std::move(networkGeneration)) {
    ++requests_->managers;
  }
  /// ~BoundNetwork erases its retained authorization bytes.
  ~BoundNetwork() override {
    authorization_.fill(0);
    --requests_->managers;
  }

protected:
  /// createRequest refuses off-installation URLs and overrides caller
  /// redirect/credential headers.
  QNetworkReply *createRequest(Operation operation,
                               const QNetworkRequest &input,
                               QIODevice *data) override {
    if (!withinServer(base_, input.url())) {
      ++requests_->denied;
      auto *reply = new RejectedReply(input, this);
      lifetime_->observe(reply, false);
      return reply;
    }
    auto request = input;
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    // Preserve AccessManager's stable HTTP/1.1 default; this origin-bound
    // manager must not silently enable an upstream-disabled transport mode.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setRawHeader("Authorization", {});
    if (auto *reply = lifetime_->denyThrottled(request, this)) {
      // A server gate is not an unapproved origin and must never contribute to
      // endpoint quarantine, transport failure or another HTTP 429 sample.
      lifetime_->observe(reply, false);
      return reply;
    }
    if (!request
             .attribute(OCC::AbstractCredentials::DontAddCredentialsAttribute)
             .toBool())
      request.setRawHeader("Authorization", authorization_);
    ++requests_->total;
    const auto verb =
        request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
    if (verb == "PROPFIND")
      ++requests_->discovery;
    if (operation == GetOperation &&
        input.url().path().contains("/remote.php/dav/"))
      ++requests_->downloads;
    const bool mutating = operation != GetOperation &&
                          operation != HeadOperation && verb != "PROPFIND" &&
                          verb != "REPORT" && verb != "OPTIONS";
    if (mutating)
      ++requests_->writes;
    auto *reply =
        QNetworkAccessManager::createRequest(operation, request, data);
    lifetime_->observe(reply, mutating,
                       operation == PutOperation ||
                           (operation == GetOperation &&
                            input.url().path().contains("/remote.php/dav/")),
                       networkGeneration_);
    if (operation == PutOperation) {
      QObject::connect(reply, &QNetworkReply::uploadProgress, reply,
                       [counts = requests_,
                        previous = qint64(0)](qint64 bytes, qint64) mutable {
                         if (bytes > previous) {
                           counts->uploadBytes += quint64(bytes - previous);
                           previous = bytes;
                         }
                       });
    } else if (operation == GetOperation &&
               input.url().path().contains("/remote.php/dav/")) {
      QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                       [counts = requests_,
                        previous = qint64(0)](qint64 bytes, qint64) mutable {
                         if (bytes > previous) {
                           counts->downloadBytes += quint64(bytes - previous);
                           previous = bytes;
                         }
                       });
    }
    QObject::connect(
        reply, &QNetworkReply::finished, reply, [reply, counts = requests_] {
          const auto status =
              reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                  .toInt();
          if (status == 401)
            ++counts->authenticationFailures;
          if (reply->error() == QNetworkReply::SslHandshakeFailedError)
            ++counts->tlsFailures;
          if (RequestLifetime::transportFailure(reply->error()))
            ++counts->transportFailures;
        });
    return reply;
  }

private:
  QUrl base_;
  QByteArray authorization_;
  std::shared_ptr<EngineAdapter::Requests> requests_;
  std::shared_ptr<RequestLifetime> lifetime_;
  std::shared_ptr<quint64> networkGeneration_;
};

/// VaultCredentials replaces URL-derived upstream keychain ownership with one
/// stable vault reference.
class VaultCredentials final : public OCC::AbstractCredentials {
public:
  /// VaultCredentials reads only the already reserved credential; no GUI or
  /// persistence is invoked.
  VaultCredentials(const EngineAdapter::Binding &b,
                   std::shared_ptr<EngineAdapter::Requests> requests,
                   std::shared_ptr<RequestLifetime> lifetime)
      : base_(b.server), user_(b.login),
        password_(readCredential(b.credentialReference)),
        requests_(std::move(requests)), lifetime_(std::move(lifetime)),
        networkGeneration_(b.networkGeneration ? b.networkGeneration
                                               : std::make_shared<quint64>(0)) {
  }
  /// ~VaultCredentials clears its password buffer.
  ~VaultCredentials() override { password_.fill(QChar::Null); }
  /// authType identifies the upstream Basic authentication scheme.
  QString authType() const override { return "http"; }
  /// user returns the login name, distinct from the DAV UID.
  QString user() const override { return user_; }
  /// password is available only inside the upstream runtime, never IPC.
  QString password() const override { return password_; }
  /// createQNAM builds the strict per-runtime request boundary.
  QNetworkAccessManager *createQNAM() const override {
    return new BoundNetwork(
        base_,
        "Basic " + (user_.toUtf8() + ':' + password_.toUtf8()).toBase64(),
        requests_, lifetime_, networkGeneration_);
  }
  /// ready reflects whether the account still has usable in-memory credentials.
  bool ready() const override { return !password_.isEmpty(); }
  /// fetchFromKeychain reports the already completed native-vault fetch without
  /// upstream migration.
  void fetchFromKeychain(const QString & = {}) override {
    _wasFetched = true;
    Q_EMIT fetched();
  }
  /// askFromUser reports completion without opening an upstream authentication
  /// dialog.
  void askFromUser() override { Q_EMIT asked(); }
  /// stillValid marks authentication failures without retrying through a GUI
  /// path.
  bool stillValid(QNetworkReply *reply) override {
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
           401;
  }
  /// persist is intentionally inert: the control layer alone owns credential
  /// persistence.
  void persist() override { Q_EMIT credentialsPersisted(); }
  /// invalidateToken prevents subsequent cycles from reusing an invalid
  /// password.
  void invalidateToken() override { forgetSensitiveData(); }
  /// forgetSensitiveData clears memory without deleting the durable vault
  /// entry.
  void forgetSensitiveData() override {
    password_.fill(QChar::Null);
    password_.clear();
  }

private:
  QUrl base_;
  QString user_, password_;
  std::shared_ptr<EngineAdapter::Requests> requests_;
  std::shared_ptr<RequestLifetime> lifetime_;
  std::shared_ptr<quint64> networkGeneration_;
};
} // namespace

/// State owns the account before the journal and engine so reverse destruction
/// preserves dependencies.
struct EngineAdapter::State {
  /// ~State also breaks the unused E2EE back-reference when construction fails
  /// before the adapter's destructor can run.
  ~State() {
    if (account)
      account->e2e()->setAccount({});
  }
  Binding binding;
  std::unique_ptr<RootLease> root;
  OCC::AccountPtr account;
  std::unique_ptr<OCC::SyncJournalDb> journal;
  /// onDemand borrows account and journal, so it is destroyed before them.
  std::unique_ptr<OnDemandRoot> onDemand;
  std::unique_ptr<OCC::SyncEngine> engine;
  bool active = false, stopping = false, retired = false;
  QString reason;
  QStringList baseline;
  QByteArray baselineDigest;
  QString authorization, proposal;
  QDeadlineTimer authorizationExpiry{0};
  DeleteAssessment deletion;
  bool upstreamGuard = false;
  std::shared_ptr<Requests> requests = std::make_shared<Requests>();
  std::shared_ptr<RequestLifetime> lifetime =
      std::make_shared<RequestLifetime>();
  Requests requestBaseline;
  RequestLifetime::Counts lifetimeBaseline;
  quint64 uploaded = 0, downloaded = 0, deletedRemote = 0;
  quint64 deletedLocal = 0, renamedRemote = 0, renamedLocal = 0;
  // The held proposal's summary by top-level entry (protocol 1.9).
  QByteArray groups;
  // Files the current cycle completed so far (protocol 1.9 progress).
  quint64 done = 0;
  quint64 cycleNetworkGeneration = 0;
  quint64 cycle = 0;
  bool capabilitiesInstalled = false;
};

EngineAdapter::EngineAdapter(const Binding &binding)
    : state_(std::make_unique<State>()) {
  if (canonicalServer(binding.server.toString()) != binding.server ||
      binding.login.isEmpty() || binding.login.contains(':') ||
      binding.uid.isEmpty() || !QFileInfo(binding.localPath).isDir() ||
      !QFileInfo(binding.journalPath).isAbsolute() ||
      !QFileInfo(binding.excludePath).isFile())
    throw std::runtime_error("Invalid engine binding");
  if (binding.onDemand &&
      (binding.folderId.isEmpty() || binding.accountId.isEmpty()))
    throw std::runtime_error("Invalid engine binding");
  state_->binding = binding;
  state_->root =
      std::make_unique<RootLease>(binding.localPath, binding.onDemand);
  if (!binding.localIdentity.isEmpty() &&
      binding.localIdentity != state_->root->identity())
    throw std::runtime_error("FOLDER_ROOT_CHANGED");
  state_->binding.localPath = state_->root->path();
  // Upstream diagnostics can contain raw URLs; do not enable them in the daemon
  // runtime.
  QLoggingCategory::setFilterRules(
      "nextcloud.*.debug=false\nnextcloud.*.info=false\nnextcloud.*.warning="
      "false\nnextcloud.*.critical=false");
  OCC::ConfigFile::setConfDir(QFileInfo(binding.journalPath).absolutePath());
  createAccount();
  state_->journal = std::make_unique<OCC::SyncJournalDb>(binding.journalPath);
  connectOnDemand();
}

void EngineAdapter::connectOnDemand() {
  auto &s = *state_;
  if (!s.binding.onDemand)
    return;
  s.onDemand = std::make_unique<OnDemandRoot>(
      OnDemandRoot::Identity{s.binding.accountId, s.binding.folderId,
                             QString(OnDemandRoot::provider) + " - " +
                                 QFileInfo(s.binding.localPath).fileName()},
      s.binding.localPath, s.binding.remotePath, s.account, s.journal.get());
}

void EngineAdapter::closeRequests() {
  if (state_->onDemand)
    state_->onDemand->close();
}

bool EngineAdapter::quiescent() const {
  return !state_->active && !(state_->onDemand && state_->onDemand->busy());
}

void EngineAdapter::createAccount() {
  state_->capabilitiesInstalled = false;
  const auto &binding = state_->binding;
  state_->account = OCC::Account::create(false);
  ++state_->requests->accounts;
  QObject::connect(state_->account.data(), &QObject::destroyed,
                   [counts = state_->requests] { --counts->accounts; });
  try {
    state_->account->setUrl(binding.server);
    state_->account->setCredentials(
        new VaultCredentials(binding, state_->requests, state_->lifetime));
    state_->account->setDavUser(binding.uid);
  } catch (...) {
    state_->account->e2e()->setAccount({});
    state_->account.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    throw;
  }
}

EngineAdapter::~EngineAdapter() {
  if (state_->active)
    qFatal("Active engine destroyed before completion");
  retire();
}

bool EngineAdapter::active() const { return state_->active; }
quint64 EngineAdapter::epoch() const { return state_->binding.epoch; }
quint64 EngineAdapter::networkGeneration() const {
  return state_->cycleNetworkGeneration;
}
EngineAdapter::Requests EngineAdapter::requests() const {
  auto counts = *state_->requests;
  const auto lifetime = state_->lifetime->counts();
  counts.active = lifetime.active;
  counts.alive = lifetime.alive;
  counts.uncertainWrites = lifetime.uncertainWrites;
  counts.activeUploads = lifetime.activeUploads;
  counts.activeDownloads = lifetime.activeDownloads;
  counts.successes = lifetime.currentNetworkSuccesses;
  counts.permissionFailures = lifetime.permissionFailures;
  counts.lockFailures = lifetime.lockFailures;
  counts.throttleFailures = lifetime.throttleFailures;
  return counts;
}

EngineAdapter::Retirement EngineAdapter::retire() {
  if (state_->active)
    throw std::logic_error("Cannot retire an active engine binding");
  state_->retired = true;
  // A hydration holds this epoch's account; disconnect only once it ended.
  if (state_->onDemand) {
    state_->onDemand->close();
    if (!state_->onDemand->busy())
      state_->onDemand.reset();
  }
  state_->journal->close();
  // Account::create installs a strong back-reference in its E2EE helper. This
  // adapter never initializes E2EE; release that cycle after engine teardown.
  if (state_->account)
    state_->account->e2e()->setAccount({});
  state_->account.reset();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  const auto counts = requests();
  return {!state_->onDemand && counts.accounts == 0 && counts.managers == 0 &&
              counts.active == 0 && counts.alive == 0,
          !state_->journal->hasCommitError(), counts.uncertainWrites};
}

void EngineAdapter::rebind(const QUrl &server, quint64 epoch,
                           const QString &login) {
  if (!state_->retired || state_->active)
    throw std::logic_error("Rebinding requires retirement");
  if (epoch <= state_->binding.epoch)
    throw std::runtime_error("REVISION_CONFLICT");
  if (canonicalServer(server.toString()) != server)
    throw std::runtime_error("ENDPOINT_UNTRUSTED");
  const auto proof = retire();
  if (!proof.networkDrained)
    throw std::runtime_error("QUIESCE_STALLED");
  if (!proof.journalCommitted)
    throw std::runtime_error("JOURNAL_ERROR");
  const auto resumeError = rebindError(server);
  if (!resumeError.isEmpty())
    throw std::runtime_error(resumeError.toStdString());
  if (QFileInfo::exists(state_->binding.journalPath)) {
    if (!state_->journal->open())
      throw std::runtime_error("JOURNAL_ERROR");
    state_->journal->forceRemoteDiscoveryNextSync();
    if (!state_->journal->isOpen())
      throw std::runtime_error("JOURNAL_ERROR");
    state_->journal->close();
    if (state_->journal->hasCommitError())
      throw std::runtime_error("JOURNAL_ERROR");
  }
  state_->authorization.clear();
  state_->authorizationExpiry = QDeadlineTimer(0);
  state_->binding.server = server;
  state_->binding.epoch = epoch;
  if (!login.isEmpty())
    state_->binding.login = login;
  createAccount();
  connectOnDemand();
  state_->retired = false;
}

QString EngineAdapter::rebindError(const QUrl &server) const {
  if (!state_->retired || state_->active)
    return "QUIESCE_STALLED";
  if (state_->journal->hasCommitError())
    return "JOURNAL_ERROR";
  if (!state_->root->verify())
    return "FOLDER_ROOT_CHANGED";
  return resumeContextError(state_->binding.journalPath,
                            state_->binding.server != server);
}

void EngineAdapter::authorize(const QString &digest,
                              qint64 remainingMilliseconds) {
  state_->authorization = digest;
  state_->authorizationExpiry = QDeadlineTimer(remainingMilliseconds);
}

void EngineAdapter::start() {
  if (state_->retired)
    throw std::logic_error("Cannot restart a retired engine binding");
  if (state_->active)
    throw std::logic_error("Engine cycle already active");
  if (state_->journal->hasCommitError())
    throw std::runtime_error("JOURNAL_ERROR");
  if (!state_->root->verify())
    throw std::runtime_error("FOLDER_ROOT_CHANGED");
  if (!state_->root->validateTree())
    throw std::runtime_error("UNSUPPORTED_FOLDER_MODE");
  auto &s = *state_;
  // A quiesce that ended without rebinding left the root refusing requests;
  // reconnect it once its last request is done.
  if (s.onDemand && s.onDemand->closed()) {
    if (s.onDemand->busy())
      throw std::runtime_error("QUIESCE_STALLED");
    s.onDemand.reset();
    connectOnDemand();
  }
  s.active = true;
  s.requestBaseline = *s.requests;
  s.lifetimeBaseline = s.lifetime->counts();
  const auto cycle = ++s.cycle;
  const auto epoch = s.binding.epoch;
  s.lifetime->constrained = [owner = QPointer(this), cycle,
                             epoch](const QString &reason, qint64) {
    if (!owner || !owner->state_->active || owner->state_->cycle != cycle ||
        owner->epoch() != epoch)
      return;
    // Never abort an upstream job from inside QNetworkReply::finished.
    // The lifetime observer has already closed all of this adapter's QNAMs.
    QTimer::singleShot(0, owner, [owner, cycle, epoch, reason] {
      if (!owner || !owner->state_->active || owner->state_->cycle != cycle ||
          owner->epoch() != epoch)
        return;
      const auto delay =
          reason == "PERMISSION_DENIED"
              ? 0
              : owner->state_->lifetime->remainingServerRetry(reason);
      if (reason == "SERVER_THROTTLED" && delay == 0)
        return;
      try {
        if (owner->constrained)
          owner->constrained(reason, delay);
      } catch (...) {
        qFatal("Server constraint coordination failed");
      }
      if (owner && reason == "SERVER_THROTTLED")
        owner->stop();
    });
  };
  s.uploaded = s.downloaded = s.deletedRemote = s.done = 0;
  s.deletedLocal = s.renamedRemote = s.renamedLocal = 0;
  s.cycleNetworkGeneration =
      s.binding.networkGeneration ? *s.binding.networkGeneration : 0;
  s.stopping = false;
  try {
    // Preflight helpers own separate short-lived managers. A fresh cycle must
    // honor the same server deadline before issuing even those metadata reads.
    if (s.lifetime->throttled())
      throw MetadataError("SERVER_THROTTLED",
                          s.lifetime->remainingServerRetry("SERVER_THROTTLED"));
    if (!s.capabilitiesInstalled) {
      const auto capabilities = syncCapabilities(readServerJson(
          s.binding.server, "ocs/v2.php/cloud/capabilities?format=json",
          s.binding.login, s.binding.credentialReference));
      s.account->setServerVersion(capabilities.version);
      s.account->setCapabilities(capabilities.sync);
      s.capabilitiesInstalled = true;
    }
    if (!s.stopping && !s.binding.remoteIdentity.isEmpty()) {
      const auto roots = readCollections(
          s.binding.server, s.binding.uid, s.binding.login,
          s.binding.credentialReference, s.binding.remotePath, false);
      if (roots.front().identity != s.binding.remoteIdentity ||
          roots.front().encrypted)
        throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
    }
  } catch (...) {
    s.active = false;
    throw;
  }
  if (s.stopping) {
    s.active = false;
    QTimer::singleShot(0, this, [this] {
      if (completed)
        completed({false,
                   false,
                   "CANCELLED",
                   {},
                   0,
                   0,
                   0,
                   state_->binding.epoch,
                   0,
                   0,
                   0,
                   state_->cycleNetworkGeneration});
    });
    return;
  }
  s.baseline.clear();
  s.proposal.clear();
  s.groups.clear();
  s.deletion = {};
  s.upstreamGuard = false;
  QCryptographicHash baselineHash(QCryptographicHash::Sha256);
  if (QFileInfo::exists(s.binding.journalPath) &&
      !s.journal->getFilesBelowPath(
          {}, [&](const OCC::SyncJournalFileRecord &record) {
            s.baseline.append(QString::fromUtf8(record._path));
            baselineHash.addData(
                QJsonDocument(QJsonArray{QString::fromUtf8(record._path),
                                         QString::fromUtf8(record._fileId),
                                         QString::fromUtf8(record._etag)})
                    .toJson(QJsonDocument::Compact));
          })) {
    s.active = false;
    throw std::runtime_error("JOURNAL_ERROR");
  }
  s.baselineDigest = baselineHash.result();
  OCC::SyncOptions options; // Keep upstream defaults and mass-deletion
                            // protection; never setIsCmd(true).
  if (s.onDemand)
    options._vfs = s.onDemand->vfs();
  try {
    s.engine = std::make_unique<OCC::SyncEngine>(s.account, s.binding.localPath,
                                                 options, s.binding.remotePath,
                                                 s.journal.get());
  } catch (...) {
    s.active = false;
    throw;
  }
  s.engine->excludedFiles().addExcludeFilePath(s.binding.excludePath);
  if (!s.engine->excludedFiles().reloadExcludeFiles()) {
    s.engine.reset();
    s.active = false;
    throw std::runtime_error("Cannot load sync exclusions");
  }
  s.active = true;
  s.stopping = false;
  s.reason.clear();
  QObject::connect(s.engine.get(), &OCC::SyncEngine::aboutToRemoveAllFiles,
                   this, [this](auto, auto answer) {
                     // Keep the upstream trigger, but decide at the verified
                     // veto checkpoint with a complete plan.
                     state_->upstreamGuard = true;
                     answer(false);
                   });
  QObject::connect(
      s.engine.get(), &OCC::SyncEngine::aboutToPropagate, this,
      [this](OCC::SyncFileItemVector &items) {
        auto &s = *state_;
        if (!s.root->validateTree()) {
          s.reason = "UNSUPPORTED_FOLDER_MODE";
          s.engine->abort();
          return;
        }
        QList<DeleteAction> actions;
        QStringList plan;
        for (const auto &item : items) {
          if (item->isEncrypted()) {
            s.reason = "UNSUPPORTED_FOLDER_MODE";
            s.engine->abort();
            return;
          }
          if (item->_instruction == CSYNC_INSTRUCTION_ERROR) {
            s.reason = "DISCOVERY_INCOMPLETE";
            s.engine->abort();
            return;
          }
          if (item->_instruction == CSYNC_INSTRUCTION_REMOVE &&
              !item->_isSelectiveSync)
            actions.append({item->_file,
                            item->_direction == OCC::SyncFileItem::Down,
                            item->isDirectory()});
          if (item->_instruction != CSYNC_INSTRUCTION_NONE &&
              item->_instruction != CSYNC_INSTRUCTION_UPDATE_METADATA)
            plan.append(QString::fromUtf8(
                QJsonDocument(QJsonArray{item->_file, item->_renameTarget,
                                         int(item->_instruction),
                                         int(item->_direction),
                                         QString::fromUtf8(item->_fileId),
                                         QString::fromUtf8(item->_etag),
                                         QString::number(item->_size),
                                         QString::number(item->_modtime)})
                    .toJson(QJsonDocument::Compact)));
        }
        std::sort(plan.begin(), plan.end());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(s.baselineDigest);
        hash.addData(QJsonDocument(QJsonArray{s.binding.localIdentity,
                                              s.binding.remoteIdentity,
                                              s.binding.server.toString(),
                                              QString::number(s.binding.epoch),
                                              "deletion-policy-1"})
                         .toJson(QJsonDocument::Compact));
        for (const auto &entry : plan) {
          hash.addData(entry.toUtf8());
          hash.addData("\n");
        }
        const auto digest = QString::fromLatin1(hash.result().toHex());
        s.deletion = assessDeletion(s.baseline, actions, s.upstreamGuard);
        const bool approved = !s.authorization.isEmpty() &&
                              !s.authorizationExpiry.hasExpired() &&
                              s.authorization == digest;
        const bool changedApproval = !s.authorization.isEmpty() && !approved;
        s.authorization.clear();
        if ((s.deletion.blocked || changedApproval) && !approved) {
          s.proposal = digest;
          s.groups = deletionGroups(s.baseline, actions);
          s.reason = "DELETION_CONFIRMATION_REQUIRED";
          s.engine->abort();
        }
      },
      Qt::DirectConnection);
  QObject::connect(s.engine.get(), &OCC::SyncEngine::itemCompleted, this,
                   [this](const OCC::SyncFileItemPtr &item, auto) {
                     if (item->_status != OCC::SyncFileItem::Success)
                       return;
                     if (!item->isDirectory())
                       ++state_->done;
                     const bool up =
                         item->_direction == OCC::SyncFileItem::Up;
                     const bool down =
                         item->_direction == OCC::SyncFileItem::Down;
                     if (item->_instruction == CSYNC_INSTRUCTION_REMOVE && up)
                       ++state_->deletedRemote;
                     else if (item->_instruction == CSYNC_INSTRUCTION_REMOVE &&
                              down)
                       ++state_->deletedLocal;
                     else if (item->_instruction == CSYNC_INSTRUCTION_RENAME &&
                              up)
                       ++state_->renamedRemote;
                     else if (item->_instruction == CSYNC_INSTRUCTION_RENAME &&
                              down)
                       ++state_->renamedLocal;
                     else if (!item->isDirectory() &&
                              (item->_instruction == CSYNC_INSTRUCTION_NEW ||
                               item->_instruction == CSYNC_INSTRUCTION_SYNC)) {
                       if (item->_direction == OCC::SyncFileItem::Up)
                         ++state_->uploaded;
                       else if (item->_direction == OCC::SyncFileItem::Down)
                         ++state_->downloaded;
                     }
                   });
  QObject::connect(
      s.engine.get(), &OCC::SyncEngine::syncError, this,
      [this](const QString &, OCC::ErrorCategory category) {
        // A full server quota is the server's constraint, not a
        // failed endpoint: name it (protocol 1.9).
        if (category == OCC::ErrorCategory::InsufficientRemoteStorage &&
            (state_->reason.isEmpty() || state_->reason == "SYNC_FAILED"))
          state_->reason = "REMOTE_QUOTA_EXCEEDED";
        else if (state_->reason.isEmpty())
          state_->reason = "SYNC_FAILED";
      });
  QObject::connect(
      s.engine.get(), &OCC::SyncEngine::finished, this, [this](bool success) {
        if (state_->reason.isEmpty() && (!success || state_->stopping))
          state_->reason = state_->stopping ? "CANCELLED" : "SYNC_FAILED";
        if (state_->requests->denied > state_->requestBaseline.denied)
          state_->reason = "ENDPOINT_UNTRUSTED";
        else if (state_->requests->tlsFailures >
                 state_->requestBaseline.tlsFailures)
          state_->reason = "TLS_ERROR";
        else if (state_->requests->authenticationFailures >
                 state_->requestBaseline.authenticationFailures)
          state_->reason = "AUTH_REQUIRED";
        else if (state_->reason.isEmpty() || state_->reason == "SYNC_FAILED" ||
                 state_->reason == "CANCELLED" ||
                 state_->reason == "DISCOVERY_INCOMPLETE" ||
                 state_->reason == "NETWORK_UNAVAILABLE") {
          const auto constraint = state_->lifetime->constraintSince(
              state_->lifetimeBaseline, success && !state_->stopping);
          if (!constraint.isEmpty())
            state_->reason = constraint;
          else if (!state_->stopping &&
                   state_->requests->transportFailures >
                       state_->requestBaseline.transportFailures)
            state_->reason = "NETWORK_UNAVAILABLE";
        }
        const auto retryAfter =
            state_->reason == "REMOTE_LOCKED" ||
                    state_->reason == "SERVER_THROTTLED"
                ? state_->lifetime->remainingServerRetry(state_->reason)
                : 0;
        Result result{
            success && !state_->stopping && state_->reason.isEmpty(),
            state_->engine->isAnotherSyncNeeded() != OCC::NoFollowUpSync,
            state_->reason,
            state_->proposal,
            state_->deletion.local,
            state_->deletion.remote,
            state_->deletion.baseline,
            state_->binding.epoch,
            state_->uploaded,
            state_->downloaded,
            state_->deletedRemote,
            state_->cycleNetworkGeneration,
            retryAfter,
            state_->proposal.isEmpty() ? QByteArray{} : state_->groups};
        result.deletedLocal = state_->deletedLocal;
        result.renamedRemote = state_->renamedRemote;
        result.renamedLocal = state_->renamedLocal;
        // finalize clears its propagator after emitting finished. Never destroy
        // from inside that signal.
        QTimer::singleShot(0, this, [this, result]() mutable {
          state_->engine.reset();
          // Patched discovery owns its worker pool; its deferred destructor
          // joins those workers.
          QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
          // Conflict copies upstream kept, for the user to look at (1.9).
          QJsonArray conflicts;
          // Reading never creates a journal a cancelled first cycle lacks.
          if (QFileInfo::exists(state_->binding.journalPath))
            for (const auto &path : state_->journal->conflictRecordPaths()) {
              const auto record = state_->journal->conflictRecord(path);
              conflicts.append(
                  QJsonArray{QString::fromUtf8(path),
                             QString::fromUtf8(record.initialBasePath)});
            }
          result.conflicts =
              QJsonDocument(conflicts).toJson(QJsonDocument::Compact);
          state_->journal->close();
          if (state_->journal->hasCommitError()) {
            result.success = false;
            result.followUp = false;
            result.reason = "JOURNAL_ERROR";
            result.proposalDigest.clear();
            result.retryAfterMs = 0;
          }
          state_->active = false;
          if (completed)
            completed(result);
        });
      });
  QTimer::singleShot(0, this, [this] {
    if (state_->stopping) {
      state_->engine.reset();
      state_->active = false;
      if (completed)
        completed({false, false, "CANCELLED"});
    } else
      state_->engine->startSync();
  });
}

void EngineAdapter::stop() {
  if (!state_->active || state_->stopping)
    return;
  state_->stopping = true;
  if (state_->engine && state_->engine->isSyncRunning())
    state_->engine->abort();
}

quint64 EngineAdapter::filesDone() const { return state_->done; }
