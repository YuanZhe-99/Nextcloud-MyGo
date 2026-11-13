// SPDX-License-Identifier: GPL-3.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "network_acceptance.h"

#include "control.h"
#include "namespace.h"
#include "network_tunnel.h"
#include "switch_state.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QNetworkProxyFactory>
#include <QProcess>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUuid>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

/// NetworkTestAccess observes the real coordinator while keeping fault controls
/// out of production IPC.
struct NetworkTestAccess {
  /// removeDuringVerification exercises endpoint removal while the real
  /// full-verification loop is nested.
  static void
  removeDuringVerification(Control &c, const QString &account,
                           const QString &target,
                           const std::function<quint64()> &capabilityRequests) {
    auto &state = c.network_->accounts.at(account);
    const auto now = c.network_->clock.elapsed();
    for (auto &[id, node] : state.nodes)
      node.next = now + 600000;
    auto &node = state.nodes.at(target);
    node.verified = false;
    node.config.namespaceVerified = false;
    node.config.tlsValid = false;
    node.next = 0;
    state.policy.configure(target, node.config);
    bool removed = false;
    const auto before = capabilityRequests();
    QTimer::singleShot(0, &c.networkTasks_, [&] {
      bool negotiated = true, subscribed = false;
      const QJsonObject params{
          {"endpointId", target},
          {"operationKey", QUuid::createUuid().toString(QUuid::WithoutBraces)},
          {"expectedRevision", c.projection().value("revision")}};
      const auto reply = c.dispatch(QJsonObject{{"jsonrpc", "2.0"},
                                                {"id", 1},
                                                {"method", "endpoints.remove"},
                                                {"params", params}},
                                    negotiated, subscribed)
                             .toObject();
      removed = reply.contains("result");
      // A different completed health probe can refresh this same state during
      // the nested request.
      c.refreshNetwork(account);
    });
    c.evaluateNetwork();
    if (capabilityRequests() != before)
      throw std::runtime_error(
          "removed endpoint received a new credentialed capability request");
    if (!removed || c.shutdownRequested() || state.nodes.contains(target))
      throw std::runtime_error(
          "endpoint removal invalidated the in-use verification lifetime");
  }
  /// flow accesses only this test's borrowed-credential login session.
  static LoginFlow &flow(Control &c, const QString &session) {
    return *c.logins_.at(session);
  }
  /// changed injects the same generation invalidation used by native
  /// notifications after a local proxy fault.
  static void changed(Control &c) { c.networkChanged(true); }
  /// ready requires full-root verification and fresh independent health on
  /// every approved endpoint.
  static bool ready(Control &c, const QString &account) {
    if (!c.network_ || !c.network_->accounts.contains(account))
      return false;
    const auto &state = c.network_->accounts.at(account);
    if (state.nodes.size() != 2)
      return false;
    for (const auto &[id, node] : state.nodes) {
      const auto h = state.policy.health(id, c.network_->clock.elapsed());
      if (!node.verified || !h.fresh || h.successes < 2)
        return false;
    }
    return true;
  }
  /// failed requires observed independent failure/circuit evidence, not merely
  /// an offline hint.
  /// age makes the active address count as serving ms longer and the switch
  /// cooldown as ms older; the thresholds themselves are proven on a caller
  /// clock (syncd_switching_tests).
  static void age(Control &c, const QString &account, qint64 ms) {
    auto &state = c.network_->accounts.at(account);
    state.policy.activated(state.active, c.network_->clock.elapsed() - ms);
    state.nextSwitch -= ms;
  }
  /// fresh requires full-root verification and fresh repeated health of one
  /// endpoint.
  static bool fresh(Control &c, const QString &account,
                    const QString &endpoint) {
    if (!c.network_ || !c.network_->accounts.contains(account))
      return false;
    const auto &state = c.network_->accounts.at(account);
    const auto node = state.nodes.find(endpoint);
    const auto h = state.policy.health(endpoint, c.network_->clock.elapsed());
    return node != state.nodes.end() && node->second.verified && h.fresh &&
           h.successes >= 2;
  }
  static bool failed(Control &c, const QString &account,
                     const QString &endpoint) {
    return c.network_ && c.network_->accounts.contains(account) &&
           c.network_->accounts.at(account)
               .policy.health(endpoint, c.network_->clock.elapsed())
               .failed;
  }
  /// binding returns the committed epoch without inspecting or changing the
  /// engine journal.
  static SwitchState::Binding binding(Control &c, const QString &a) {
    return SwitchState(c.db_).binding(a);
  }
  /// stable reports actual switch reconciliation, not merely committed binding
  /// publication.
  static bool stable(Control &c, const QString &a) {
    return !SwitchState(c.db_).gated(a);
  }
  /// facts reads the durable switch identities through the Control connection.
  static SwitchFacts facts(Control &c, const QString &account) {
    SwitchFacts facts;
    const auto bound = SwitchState(c.db_).binding(account);
    facts.endpoint = bound.endpoint;
    facts.epoch = bound.epoch;
    QSqlQuery q(c.db_);
    const auto run = [&](const QString &sql) {
      q.prepare(sql);
      q.addBindValue(account);
      if (!q.exec())
        throw std::runtime_error("owned switch facts unavailable");
    };
    run("SELECT phase FROM switch_intents WHERE account_id=?");
    if (q.next())
      facts.phase = q.value(0).toString();
    run("SELECT credential_ref FROM accounts WHERE id=?");
    if (q.next())
      facts.credential = q.value(0).toString();
    run("SELECT id,journal_path,local_identity,remote_identity FROM folders "
        "WHERE account_id=? ORDER BY id");
    while (q.next())
      for (int column = 0; column < 4; ++column)
        facts.folders.append(q.value(column).toString());
    q.finish();
    if (!q.exec("SELECT receipt FROM operations WHERE method IN "
                "('endpoints.requestSwitch','endpoints.setPolicy')"))
      throw std::runtime_error("owned switch receipts unavailable");
    while (q.next()) {
      const auto receipt =
          QJsonDocument::fromJson(q.value(0).toByteArray()).object();
      if (receipt.value("state") == "failed")
        facts.interrupted.append(receipt.value("error")
                                     .toObject()
                                     .value("data")
                                     .toObject()
                                     .value("reasonCode")
                                     .toString());
    }
    return facts;
  }
};

SwitchFacts switchFacts(Control &c, const QString &account) {
  return NetworkTestAccess::facts(c, account);
}

namespace {
/// require keeps real acceptance checks enabled in optimized builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
/// call uses the production JSON-RPC path with optimistic revision and
/// operation IDs.
QJsonObject call(Control &c, const QString &method, QJsonObject params = {}) {
  params.insert("operationKey",
                QUuid::createUuid().toString(QUuid::WithoutBraces));
  params.insert("expectedRevision", c.projection().value("revision"));
  bool negotiated = true, subscribed = false;
  return c
      .dispatch(QJsonObject{{"jsonrpc", "2.0"},
                            {"id", 1},
                            {"method", method},
                            {"params", params}},
                negotiated, subscribed)
      .toObject();
}
/// waitFor drives real Qt networking with an explicit overall deadline and no
/// retry of mutating requests.
void waitFor(const std::function<bool()> &condition, const char *message) {
  // The parent names the last step a killed child was in; the text is fixed.
  static QElapsedTimer clock;
  if (!clock.isValid())
    clock.start();
  std::printf("STEP %lld %s\n", static_cast<long long>(clock.elapsed()),
              message);
  std::fflush(stdout);
  QEventLoop loop;
  QTimer poll, deadline;
  poll.setInterval(25);
  deadline.setSingleShot(true);
  bool done = false;
  std::exception_ptr error;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    try {
      if (condition()) {
        done = true;
        loop.quit();
      }
    } catch (...) {
      error = std::current_exception();
      loop.quit();
    }
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  deadline.start(180000);
  loop.exec();
  if (error)
    std::rethrow_exception(error);
  require(done, message);
}
/// settled waits for an accepted operation and returns only its sanitized
/// terminal result.
QJsonObject settled(Control &c, const QJsonObject &accepted) {
  const auto operation =
      accepted.value("result").toObject().value("operationId").toString();
  require(!operation.isEmpty(), "network acceptance operation refused");
  QJsonObject receipt;
  waitFor(
      [&] {
        receipt = call(c, "operations.get", {{"operationId", operation}})
                      .value("result")
                      .toObject();
        return receipt.value("state") != "running" &&
               receipt.value("state") != "accepted";
      },
      "network acceptance operation deadline");
  return receipt;
}
/// FaultState contains only approved origin names and test-local refusal
/// switches, never credentials.
struct FaultState {
  QString primary, alternate;
  std::atomic_bool failPrimary{false}, failAlternate{false};
  quint16 port = 0;
  std::atomic<quint64> refused{0};
  std::atomic<quint64> capabilityRequests{0};
  std::atomic_bool holdPrimaryResponses{false};
  bool tunnel = false;
  // Requests to the primary whose path ends with this suffix lose their reply.
  QMutex withholdLock;
  QString withholdSuffix;
  quint16 withholdPort = 0;
  std::atomic<quint64> withheld{0};
  // Control-thread observer of requests about to be created for the alternate.
  std::function<void(const QString &)> targetRequest;
  // Switching scenarios: host names served by a loopback TLS-terminating
  // proxy, and alternate requests refused by path (both under withholdLock).
  std::map<QString, quint16> routes;
  QString refuseAlternate;
  quint16 abortPort = 0; // a listener that aborts every connection
};
/// FaultProxy refuses selected HTTPS tunnels through a process-local closed
/// proxy without TLS interception.
class FaultProxy final : public QNetworkProxyFactory {
public:
  /// FaultProxy shares only nonsecret fault state with its acceptance fixture.
  explicit FaultProxy(std::shared_ptr<FaultState> state)
      : state_(std::move(state)) {}
  /// queryProxy routes healthy addresses directly; failed ones reach the owned
  /// rejecting socket.
  QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override {
    const auto host = query.url().host();
    if (query.url().path().contains("/cloud/capabilities"))
      ++state_->capabilityRequests;
    if (host == state_->alternate && state_->targetRequest &&
        QThread::currentThread() == QCoreApplication::instance()->thread())
      state_->targetRequest(query.url().path());
    if ((host == state_->primary && state_->failPrimary) ||
        (host == state_->alternate && state_->failAlternate)) {
      ++state_->refused;
      return {
          QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", state_->port)};
    }
    {
      QMutexLocker lock(&state_->withholdLock);
      if (host == state_->alternate && !state_->refuseAlternate.isEmpty() &&
          query.url().path().contains(state_->refuseAlternate)) {
        ++state_->refused;
        return {QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1",
                              state_->abortPort)};
      }
      if (const auto route = state_->routes.find(host);
          route != state_->routes.end())
        return {QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1",
                              route->second)};
    }
    if (host == state_->primary) {
      QMutexLocker lock(&state_->withholdLock);
      if (!state_->withholdSuffix.isEmpty() &&
          query.url().path().endsWith(state_->withholdSuffix)) {
        ++state_->withheld;
        return {QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1",
                              state_->withholdPort)};
      }
    }
    if (state_->tunnel)
      return {
          QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", state_->port)};
    return {QNetworkProxy(QNetworkProxy::NoProxy)};
  }

private:
  std::shared_ptr<FaultState> state_;
};
/// ProxyScope restores process-local proxy configuration even when acceptance
/// fails.
struct ProxyScope {
  /// ~ProxyScope removes the owned factory without changing any system proxy
  /// setting.
  ~ProxyScope() { QNetworkProxyFactory::setApplicationProxyFactory(nullptr); }
};
/// borrowedLogin adds one more account with a fixture user's own credentials
/// through the production login path; its vault slot enters the parent's
/// cleanup manifest before it exists. Returns: the terminal login status.
QJsonObject borrowedLogin(Control &c, const QString &root, const QUrl &base,
                          const QString &user, const QString &password) {
  const auto operation =
      call(c, "accounts.login.start", {{"canonicalBaseUrl", base.toString()}})
          .value("result")
          .toObject()
          .value("operationId")
          .toString();
  require(!operation.isEmpty(), "owned additional login reservation failed");
  const auto connection = QUuid::createUuid().toString();
  QString session;
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(QDir(root).filePath("control.sqlite"));
    require(db.open(), "owned additional login DB unavailable");
    QSqlQuery q(db);
    q.prepare("SELECT session_id,credential_ref FROM accounts WHERE "
              "operation_id=?");
    q.addBindValue(operation);
    require(q.exec() && q.next(), "owned additional reservation absent");
    session = q.value(0).toString();
    const auto reference = q.value(1).toString();
    q.finish();
    db.close();
    QFile manifest(QDir(root).filePath("owned-refs.json"));
    require(manifest.open(QIODevice::ReadOnly), "owned manifest unreadable");
    auto refs = QJsonDocument::fromJson(manifest.readAll()).array();
    manifest.close();
    refs.append(reference);
    require(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                manifest.write(
                    QJsonDocument(refs).toJson(QJsonDocument::Compact)) > 0 &&
                manifest.flush(),
            "owned manifest update failed");
  }
  QSqlDatabase::removeDatabase(connection);
  NetworkTestAccess::flow(c, session).verifyCredentials(user, password);
  QJsonObject status;
  waitFor(
      [&] {
        status = call(c, "accounts.login.status", {{"loginSessionId", session}})
                     .value("result")
                     .toObject();
        return status.value("state") == "succeeded" ||
               status.value("state") == "failed";
      },
      "owned additional login did not finish");
  return status;
}
/// hold drives a bounded observation interval to prove no bad-endpoint
/// ping-pong or fixed fallback.
void hold(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}
/// relayCommand sends one line to the out-of-process relay and returns its
/// one-line answer, driving this process's event loop until it arrives.
QByteArray relayCommand(QProcess &relay, const QByteArray &line,
                        int milliseconds) {
  relay.write(line + '\n');
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  QObject::connect(&relay, &QProcess::readyReadStandardOutput, &loop, [&] {
    if (relay.canReadLine())
      loop.quit();
  });
  QObject::connect(&relay, &QProcess::finished, &loop, &QEventLoop::quit);
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  deadline.start(milliseconds);
  if (!relay.canReadLine())
    loop.exec();
  require(relay.canReadLine(), "owned sleep relay did not answer");
  return relay.readLine().trimmed();
}
/// Suspension holds one process suspended and resumes it when it ends, so no
/// path out of the relay leaves its parent suspended.
struct Suspension {
  using Call = LONG(NTAPI *)(HANDLE);
  HANDLE process = nullptr;
  Call resume = nullptr;
  /// ~Suspension resumes the process and releases its handle.
  ~Suspension() {
    if (process) {
      resume(process);
      CloseHandle(process);
    }
  }
};
} // namespace

int sleepRelay(quint32 parent) {
  const auto base = canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_URL")),
             alternate = canonicalServer(
                 qEnvironmentVariable("NEXTCLOUD_TEST_ALTERNATE_URL"));
  if (qEnvironmentVariableIsSet("NEXTCLOUD_TEST_CLEANUP_URL"))
    NetworkTunnel::setTarget(
        QUrl(qEnvironmentVariable("NEXTCLOUD_TEST_CLEANUP_URL")).host());
  bool failPrimary = false, failAlternate = false;
  QTcpServer listener;
  require(listener.listen(QHostAddress::LocalHost, 0),
          "owned sleep relay unavailable");
  QObject::connect(&listener, &QTcpServer::newConnection, &listener, [&] {
    while (auto *socket = listener.nextPendingConnection())
      new NetworkTunnel(socket, base, alternate, [&](bool primary) {
        return primary ? failPrimary : failAlternate;
      });
  });
  const auto ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto suspend = reinterpret_cast<Suspension::Call>(
      GetProcAddress(ntdll, "NtSuspendProcess"));
  const auto resume = reinterpret_cast<Suspension::Call>(
      GetProcAddress(ntdll, "NtResumeProcess"));
  require(suspend && resume, "process suspension unavailable");
  std::unique_ptr<Suspension> suspended;
  QElapsedTimer clock;
  std::printf("PORT %u\n", unsigned(listener.serverPort()));
  std::fflush(stdout);
  const auto answer = [](const QByteArray &text) {
    std::printf("%s\n", text.constData());
    std::fflush(stdout);
  };
  const auto handle = [&](const QByteArray &line) {
    const auto words = line.trimmed().split(' ');
    const auto value = words.value(1).toLongLong();
    if (words.size() == 2 && words[0] == "stall" && value >= 0) {
      NetworkTunnel::setStallAfter(value);
      answer("ok");
    } else if (words.size() == 2 && words[0] == "sleep" && value > 0 &&
               value <= 60000 && !suspended) {
      const auto process = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, parent);
      if (!process || suspend(process) < 0) {
        if (process)
          CloseHandle(process);
        answer("failed");
        return;
      }
      suspended.reset(new Suspension{process, resume});
      clock.start();
      // Woken on another network: every connection is gone and only the
      // alternate answers from now on.
      failPrimary = failAlternate = true;
      NetworkTunnel::setStallAfter(0);
      QTimer::singleShot(500, &listener, [&] { failAlternate = false; });
      QTimer::singleShot(int(value), Qt::PreciseTimer, &listener, [&] {
        suspended.reset();
        answer("awake " + QByteArray::number(clock.elapsed()));
      });
    } else
      answer("refused");
  };
  // Standard input is read on its own thread; its end ends the relay.
  std::jthread reader([&] {
    std::string line;
    while (std::getline(std::cin, line))
      QMetaObject::invokeMethod(
          &listener, [&, text = QByteArray::fromStdString(line)] { handle(text); },
          Qt::QueuedConnection);
    QMetaObject::invokeMethod(QCoreApplication::instance(),
                              &QCoreApplication::quit, Qt::QueuedConnection);
  });
  return QCoreApplication::exec();
}

void networkAcceptance(const QString &scenario) {
#ifndef SYNCD_WITH_ENGINE
  Q_UNUSED(scenario);
  throw std::runtime_error("network acceptance requires an engine build");
#else
  const bool lifetimeOnly = scenario == "network-lifetime",
             wrongInstance = scenario == "wrong-instance",
             killing = scenario.startsWith("switch-kill="),
             recovering = scenario.startsWith("switch-recover="),
             sleeping = scenario == "sleep-resume",
             activeTransfers =
                 killing || recovering || sleeping ||
                 scenario.section('=', 0, 0) == "network-active" ||
                 scenario == "network-chunked" ||
                 scenario.section('=', 0, 0) == "network-uncertain" ||
                 scenario == "network-large" ||
                 scenario.startsWith("file-safety");
  const bool multi =
                 scenario == "multi-account" || scenario == "switch-quota",
             slow = scenario == "slow-server",
             dataSafety = scenario.startsWith("data-safety="),
             dataRecover = scenario.startsWith("data-recover=");
  QString alternateKind = QStringLiteral("internet");
  const bool switching = switchingScenario(scenario, &alternateKind);
  const auto stage = scenario.section('=', 1);
  // One transfer case per process: each starts on healthy endpoints, with no
  // circuit history (and its growing open time) left by an earlier fault.
  if (!killing && !recovering)
    selectTransferCase(stage);
  // An isolated loopback fixture is addressed through reserved names that
  // only the relay maps back; its direct origin is the cleanup address. The
  // relay then carries every request, also those of the network scenario.
  const bool relayed =
      activeTransfers || multi || slow || dataSafety || dataRecover ||
      switching ||
      (scenario == "network" &&
       qEnvironmentVariableIsSet("NEXTCLOUD_TEST_CLEANUP_URL"));
  if (relayed && qEnvironmentVariableIsSet("NEXTCLOUD_TEST_CLEANUP_URL"))
    NetworkTunnel::setTarget(
        QUrl(qEnvironmentVariable("NEXTCLOUD_TEST_CLEANUP_URL")).host());
  const auto root = QDir::cleanPath(QDir::fromNativeSeparators(
      qEnvironmentVariable("SYNCD_REAUTH_LIFETIME_ROOT")));
  require(QDir::isAbsolutePath(root) && QDir(root).exists() &&
              QRegularExpression("^reauth-lifetime-[a-f0-9]{32}$")
                  .match(QFileInfo(root).fileName())
                  .hasMatch(),
          "owned network acceptance profile missing");
  const auto base = canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_URL")),
             alternate = canonicalServer(
                 qEnvironmentVariable("NEXTCLOUD_TEST_ALTERNATE_URL"));
  require(wrongInstance ? base != alternate : base.host() != alternate.host(),
          "network acceptance requires distinct approved HTTPS origin hosts");
  auto faults = std::make_shared<FaultState>();
  faults->primary = base.host();
  faults->alternate = alternate.host();
  faults->tunnel = relayed;
  QTcpServer rejecting;
  require(rejecting.listen(QHostAddress::LocalHost, 0),
          "owned rejecting proxy unavailable");
  faults->port = rejecting.serverPort();
  QObject::connect(&rejecting, &QTcpServer::newConnection, &rejecting, [&] {
    while (auto *socket = rejecting.nextPendingConnection()) {
      if (relayed)
        new NetworkTunnel(
            socket, base, alternate,
            [faults](bool primary) {
              return primary ? faults->failPrimary.load()
                             : faults->failAlternate.load();
            },
            [faults](bool primary) {
              return primary && faults->holdPrimaryResponses.load();
            });
      else {
        socket->abort();
        socket->deleteLater();
      }
    }
  });
  QTcpServer withholding;
  require(withholding.listen(QHostAddress::LocalHost, 0),
          "owned withholding proxy unavailable");
  faults->withholdPort = withholding.serverPort();
  QObject::connect(&withholding, &QTcpServer::newConnection, &withholding, [&] {
    while (auto *socket = withholding.nextPendingConnection())
      new NetworkTunnel(
          socket, base, alternate,
          [faults](bool primary) {
            return primary ? faults->failPrimary.load()
                           : faults->failAlternate.load();
          },
          {}, true);
  });
  QTcpServer aborting;
  require(aborting.listen(QHostAddress::LocalHost, 0),
          "owned aborting proxy unavailable");
  faults->abortPort = aborting.serverPort();
  QObject::connect(&aborting, &QTcpServer::newConnection, &aborting, [&] {
    while (auto *socket = aborting.nextPendingConnection()) {
      socket->abort();
      socket->deleteLater();
    }
  });
  // A suspended process cannot drop its own connections: for the simulated
  // sleep every tunnel runs in a child relay that keeps running meanwhile.
  QProcess relay;
  if (sleeping) {
    relay.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    relay.start(QCoreApplication::applicationFilePath(),
                {"--sleep-relay=" +
                 QString::number(QCoreApplication::applicationPid())});
    require(relay.waitForStarted(10000), "owned sleep relay did not start");
    QElapsedTimer started;
    started.start();
    while (!relay.canReadLine() && started.elapsed() < 10000)
      relay.waitForReadyRead(1000);
    const auto line = relay.readLine().trimmed();
    bool parsed = false;
    const auto port = line.mid(5).toUShort(&parsed);
    require(line.startsWith("PORT ") && parsed && port,
            "owned sleep relay port missing");
    faults->port = port;
  }
  ProxyScope proxy;
  QNetworkProxyFactory::setApplicationProxyFactory(new FaultProxy(faults));
  // Switching scenarios put their TLS-terminating proxies on the route before
  // the sign-in, so every request of the run can pass them.
  const auto fixture =
      switching ? switchingFixture(scenario, root,
                                   [faults](const QString &host, quint16 port) {
                                     QMutexLocker lock(&faults->withholdLock);
                                     if (port)
                                       faults->routes[host] = port;
                                     else
                                       faults->routes.erase(host);
                                   })
                : nullptr;
  if (dataRecover)
    dataSafetyBeforeRestart(root, stage);
  Control c(QDir(root).filePath("control.sqlite"));
  const AcceptanceCalls calls{
      [&](const QString &method, const QJsonObject &params) {
        return call(c, method, params);
      },
      [&](const QJsonObject &accepted) { return settled(c, accepted); },
      [](const std::function<bool()> &condition, const char *message) {
        waitFor(condition, message);
      },
      [&](const QString &user, const QString &password) {
        return borrowedLogin(c, root, base, user, password);
      },
      [&](const QString &id) {
        const auto bound = NetworkTestAccess::binding(c, id);
        return std::make_pair(bound.endpoint, bound.epoch);
      },
      [&](const QString &id) { return NetworkTestAccess::stable(c, id); }};
  if (dataRecover) {
    try {
      dataSafetyRecover(c, root, stage, calls);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s\n", error.what());
      settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
      throw;
    }
    settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
    return;
  }
  if (recovering) {
    // The restarted daemon gets no hint: only its own storage and the server.
    try {
      switchRecoverAcceptance(c, root, stage);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s\n", error.what());
      settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
      throw;
    }
    settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
    return;
  }
  const auto accepted =
      call(c, "accounts.login.start", {{"canonicalBaseUrl", base.toString()}});
  const auto operation =
      accepted.value("result").toObject().value("operationId").toString();
  require(!operation.isEmpty(), "network borrowed login reservation failed");
  // The cleanup manifest is reserved before copying the borrowed password into
  // the native vault.
  const auto connection = QUuid::createUuid().toString();
  QString credential;
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(QDir(root).filePath("control.sqlite"));
    require(db.open(), "owned network fixture DB unavailable");
    QSqlQuery q(db);
    require(q.exec("SELECT id,session_id,credential_ref FROM accounts") &&
                q.next(),
            "owned network account reservation absent");
    const auto session = q.value(1).toString(),
               reference = q.value(2).toString();
    credential = reference;
    q.finish();
    QFile manifest(QDir(root).filePath("owned-refs.json"));
    require(manifest.open(QIODevice::WriteOnly),
            "network cleanup manifest unavailable");
    require(manifest.write(QJsonDocument(QJsonArray{reference})
                               .toJson(QJsonDocument::Compact)) > 0 &&
                manifest.flush(),
            "network cleanup reservation failed");
    manifest.close();
    NetworkTestAccess::flow(c, session)
        .verifyCredentials(qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                           qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD"));
    waitFor(
        [&] {
          return call(c, "accounts.login.status", {{"loginSessionId", session}})
                     .value("result")
                     .toObject()
                     .value("state") == "succeeded";
        },
        "network borrowed credential verification failed");
    q.finish();
    db.close();
  }
  QSqlDatabase::removeDatabase(connection);
  const auto accounts =
      c.projection().value("snapshot").toObject().value("accounts").toArray();
  require(accounts.size() == 1, "network account identity absent");
  const auto account = accounts[0].toObject().value("id").toString();
  const auto primary = NetworkTestAccess::binding(c, account).endpoint;
  auto receipt = settled(c, call(c, "endpoints.stage",
                                 {{"accountId", account},
                                  {"name", "Owned alternate"},
                                  {"baseUrl", alternate.toString()},
                                  {"kind", alternateKind}}));
  require(receipt.value("state") == "succeeded",
          "real alternate public probe failed");
  const auto staging =
      receipt.value("result").toObject().value("id").toString();
  receipt =
      settled(c, call(c, "endpoints.approveOrigin", {{"stagingId", staging}}));
  if (wrongInstance) {
    require(
        receipt.value("state") == "failed" &&
            receipt.value("error").toObject().value("data").toObject().value(
                "reasonCode") == "ENDPOINT_IDENTITY_MISMATCH",
        "owned wrong instance did not fail namespace verification");
    const auto user = [&](const QUrl &origin) {
      return readServerJson(origin, "ocs/v2.php/cloud/user?format=json",
                            qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                            credential)
          .value("ocs")
          .toObject()
          .value("data")
          .toObject()
          .value("id")
          .toString();
    };
    const auto uid = user(base);
    require(!uid.isEmpty() && user(alternate) == uid,
            "owned wrong-instance authentication was not the same user");
    const auto left =
        readCollections(base, uid, qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                        credential, "/", false);
    const auto right = readCollections(
        alternate, uid, qEnvironmentVariable("NEXTCLOUD_TEST_USER"), credential,
        "/", false);
    require(left.size() == 1 && right.size() == 1 &&
                left.first().identity != right.first().identity,
            "owned independent instances did not have distinct namespaces");
    bool rejected = false;
    for (const auto &entry : c.projection()
                                 .value("snapshot")
                                 .toObject()
                                 .value("endpoints")
                                 .toArray()) {
      const auto endpoint = entry.toObject();
      if (endpoint.value("id") == staging)
        rejected = endpoint.value("approved").toBool() &&
                   endpoint.value("blocked").toBool() &&
                   endpoint.value("verified") == "failed" &&
                   !endpoint.value("inUse").toBool();
    }
    require(rejected, "owned wrong instance did not remain quarantined");
    const auto refused =
        call(c, "endpoints.requestSwitch",
             {{"accountId", account}, {"endpointId", staging}});
    require(refused.value("error").toObject().value("data").toObject().value(
                "reasonCode") == "ENDPOINT_IDENTITY_MISMATCH" &&
                NetworkTestAccess::binding(c, account).endpoint == primary &&
                NetworkTestAccess::binding(c, account).epoch == 0,
            "owned wrong instance was activated or changed canonical binding");
    settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
    std::puts("PASS real independent instances with matching authenticated "
              "UID, distinct DAV namespace, durable quarantine and unchanged "
              "canonical binding");
    return;
  }
  require(receipt.value("state") == "succeeded",
          "real approved alternate root verification failed");
  const auto target =
      receipt.value("result").toObject().value("endpointId").toString();
  require(!target.isEmpty(), "approved alternate ID absent");
  if (multi || dataSafety) {
    try {
      if (dataSafety)
        // A plain network loss refuses both addresses: nowhere to fail over.
        dataSafetyAcceptance(c, root, account, stage, calls, [&](bool cut) {
          faults->failPrimary = cut;
          faults->failAlternate = cut;
          NetworkTestAccess::changed(c);
        });
      else if (scenario == "switch-quota")
        switchQuotaAcceptance(c, root, base, calls);
      else
        multiAccountAcceptance(c, root, account, target, base, calls);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s\n", error.what());
      settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
      throw;
    }
    settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
    return;
  }
  if (switching) {
    const SwitchingControls controls{
        calls,
        account,
        primary,
        target,
        credential,
        base,
        alternate,
        [&](bool failed) { faults->failPrimary = failed; },
        [&](bool failed) { faults->failAlternate = failed; },
        [&](const QString &path) {
          QMutexLocker lock(&faults->withholdLock);
          faults->refuseAlternate = path;
        },
        [&](const std::function<void(const QString &)> &observer) {
          faults->targetRequest = observer;
        },
        [&] { NetworkTestAccess::changed(c); },
        [&] { return NetworkTestAccess::ready(c, account); },
        [&](const QString &endpoint) {
          return NetworkTestAccess::failed(c, account, endpoint);
        },
        [&](const QString &endpoint) {
          return NetworkTestAccess::fresh(c, account, endpoint);
        },
        [&](qint64 ms) { NetworkTestAccess::age(c, account, ms); },
        [&] { return NetworkTestAccess::facts(c, account); }};
    try {
      switchingAcceptance(scenario, fixture.get(), controls);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s\n", error.what());
      faults->failPrimary = false;
      faults->failAlternate = false;
      faults->targetRequest = {};
      settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
      throw;
    }
    faults->targetRequest = {};
    settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
    return;
  }
  require(settled(c, call(c, "endpoints.setPolicy",
                          {{"accountId", account}, {"policy", "auto"}}))
                  .value("state") == "succeeded",
          "automatic policy failed");
  waitFor([&] { return NetworkTestAccess::ready(c, account); },
          "initial real health/root evidence did not become ready");
  if (slow) {
    // A reachable server that answers every client flight 1.5 s late (a
    // probe on a fresh connection then takes about 4.5 s, like the shared
    // server under concurrent sign-ins) is slow, not unavailable.
    NetworkTunnel::setReplyDelay(1500);
    NetworkTestAccess::changed(c);
    bool failed = false;
    waitFor(
        [&] {
          failed = failed || NetworkTestAccess::failed(c, account, primary) ||
                   NetworkTestAccess::failed(c, account, target);
          return failed || NetworkTestAccess::ready(c, account);
        },
        "slow server health evidence did not refresh");
    NetworkTunnel::setReplyDelay(0);
    require(!failed, "a slow but reachable server was classified unavailable");
    settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
    std::puts("PASS a server answering 1.5 s late per request stays healthy "
              "on both addresses");
    return;
  }
  if (lifetimeOnly) {
    NetworkTestAccess::removeDuringVerification(
        c, account, target, [&] { return faults->capabilityRequests.load(); });
    std::puts("PASS endpoint removal during real nested full verification "
              "retains safe selection/session lifetime");
    return;
  }
  const auto original = NetworkTestAccess::binding(c, account);
  if (activeTransfers) {
    const TransferFaults controls{
        [&](bool failed) {
          faults->failPrimary = failed;
          NetworkTestAccess::changed(c);
        },
        [&] { return NetworkTestAccess::ready(c, account); },
        [&](bool held) { faults->holdPrimaryResponses = held; },
        [&](const QString &suffix) {
          QMutexLocker lock(&faults->withholdLock);
          faults->withholdSuffix = suffix;
          faults->withheld = 0;
        },
        [&] { return faults->withheld.load(); },
        [&](const std::function<void(const QString &)> &observer) {
          faults->targetRequest = observer;
        },
        [&](qint64 bytes) {
          require(relayCommand(relay, "stall " + QByteArray::number(bytes),
                               10000) == "ok",
                  "owned sleep relay refused the stall");
        },
        [&](qint64 milliseconds) {
          // Both a control-thread timer and an independent thread must stand
          // still: the whole process was suspended, not one thread.
          std::atomic<qint64> threadPause{0};
          std::atomic<int> beats{0};
          std::jthread beating([&](std::stop_token stop) {
            auto last = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(20));
              const auto now = std::chrono::steady_clock::now();
              threadPause = std::max<qint64>(
                  threadPause,
                  std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                        last)
                      .count());
              last = now;
              ++beats;
            }
          });
          // A thread not yet scheduled when the process is suspended would
          // only start after it and see no pause.
          for (int i = 0; i < 200 && beats < 2; ++i)
            hold(10);
          require(beats >= 2, "owned heartbeat thread did not start");
          QElapsedTimer beat;
          beat.start();
          qint64 timerPause = 0;
          QTimer ticks;
          ticks.setInterval(20);
          QObject::connect(&ticks, &QTimer::timeout, &ticks, [&] {
            timerPause = std::max(timerPause, beat.restart());
          });
          ticks.start();
          const auto awake = relayCommand(
              relay, "sleep " + QByteArray::number(milliseconds),
              int(milliseconds) + 20000);
          hold(100); // the overdue tick
          require(awake.startsWith("awake "),
                  "owned sleep relay did not suspend this process");
          return std::pair(awake.mid(6).toLongLong(),
                           std::min(timerPause, threadPause.load()));
        }};
    try {
      if (killing)
        switchKillAcceptance(c, root, account, primary, target, credential,
                             stage, controls);
      else
        networkTransferAcceptance(c, root, account, primary, target, credential,
                                  controls);
    } catch (const std::exception &error) {
      // Preserve the actual assertion before draining; Control must never be
      // destroyed with an active writer.
      std::fprintf(stderr, "%s\n", error.what());
      faults->failPrimary = false;
      faults->failAlternate = false;
      faults->holdPrimaryResponses = false;
      faults->targetRequest = {};
      controls.withhold({});
      settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
      throw;
    }
    settled(c, call(c, "daemon.shutdown", {{"reason", "user"}}));
    return;
  }
  faults->failAlternate = true;
  const auto rejected = call(c, "endpoints.setPolicy",
                             {{"accountId", account},
                              {"policy", "fixed"},
                              {"fixedEndpointId", target}});
  const auto rejectedOperation =
      rejected.value("result").toObject().value("operationId").toString();
  receipt = settled(c, rejected);
  require(receipt.value("state") == "failed",
          "failed target verification committed a fixed policy");
  waitFor(
      [&] {
        const auto b = NetworkTestAccess::binding(c, account);
        return b.endpoint == primary && b.epoch > original.epoch &&
               NetworkTestAccess::stable(c, account);
      },
      "failed target did not freshly recover previous origin with a new epoch");
  require(call(c, "operations.get", {{"operationId", rejectedOperation}})
                  .value("result")
                  .toObject()
                  .value("state") == "failed",
          "successful fallback changed failed request into success");
  require(c.projection()
                  .value("snapshot")
                  .toObject()
                  .value("networks")
                  .toArray()[0]
                  .toObject()
                  .value("policy") == "auto",
          "failed fixed target replaced prior policy");
  faults->failAlternate = false;
  NetworkTestAccess::changed(c);
  waitFor([&] { return NetworkTestAccess::ready(c, account); },
          "real healthy origins did not refresh after failed-target recovery");
  faults->failPrimary = true;
  NetworkTestAccess::changed(c);
  waitFor(
      [&] {
        const auto b = NetworkTestAccess::binding(c, account);
        return b.endpoint == target && b.epoch > 0;
      },
      "automatic failover did not activate real approved alternate");
  const auto failover = NetworkTestAccess::binding(c, account);
  require(faults->refused > 0, "process-local proxy fault was not exercised");
  faults->failAlternate = true;
  NetworkTestAccess::changed(c);
  waitFor(
      [&] {
        return NetworkTestAccess::failed(c, account, primary) &&
               NetworkTestAccess::failed(c, account, target);
      },
      "both real endpoint failures were not classified");
  hold(16000);
  require(NetworkTestAccess::binding(c, account).epoch == failover.epoch,
          "all-bad endpoints ping-ponged bindings");
  faults->failAlternate = false;
  NetworkTestAccess::changed(c);
  waitFor(
      [&] {
        return NetworkTestAccess::ready(c, account) ||
               !NetworkTestAccess::failed(c, account, target);
      },
      "alternate half-open recovery failed");
  receipt = settled(c, call(c, "endpoints.setPolicy",
                            {{"accountId", account},
                             {"policy", "fixed"},
                             {"fixedEndpointId", target}}));
  require(receipt.value("state") == "succeeded",
          "verified fixed alternate policy did not commit");
  const auto fixed = NetworkTestAccess::binding(c, account);
  faults->failPrimary = false;
  faults->failAlternate = true;
  NetworkTestAccess::changed(c);
  waitFor(
      [&] {
        return NetworkTestAccess::failed(c, account, target) &&
               !NetworkTestAccess::failed(c, account, primary);
      },
      "fixed-failure/healthy-backup evidence missing");
  hold(16000);
  require(NetworkTestAccess::binding(c, account).epoch == fixed.epoch,
          "fixed policy silently fell back to healthy alternate");
  const auto network = c.projection()
                           .value("snapshot")
                           .toObject()
                           .value("networks")
                           .toArray()[0]
                           .toObject();
  require(network.value("policy") == "fixed" &&
              network.value("fixedEndpointId") == target &&
              !c.shutdownRequested(),
          "fixed failure changed configuration or stopped daemon");
  call(c, "daemon.shutdown", {{"reason", "user"}});
  std::puts(
      "PASS real approved HTTPS probes, process-local transport failure, "
      "automatic failover, both-bad waiting, half-open recovery and "
      "fixed-target refusal; no system settings or remote content changed");
#endif
}
