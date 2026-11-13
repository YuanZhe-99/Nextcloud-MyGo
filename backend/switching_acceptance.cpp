// SPDX-License-Identifier: GPL-3.0-only
#include "login.h"
#include "namespace.h"
#include "network_acceptance.h"
#include "network_tunnel.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTimer>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
/// require keeps real acceptance checks enabled in optimized builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
/// hold runs the control thread's event loop for a bounded time.
void hold(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

/// Identity is one throwaway server certificate and its key.
struct Identity {
  QSslCertificate certificate;
  QSslKey key;
};

/// Message is one HTTP/1.1 request or response head and its decoded body.
struct Message {
  QByteArray start;
  QList<std::pair<QByteArray, QByteArray>> headers;
  QByteArray body;
  /// header returns one header's value, case-insensitively.
  QByteArray header(const QByteArray &name) const {
    for (const auto &[key, value] : headers)
      if (key.compare(name, Qt::CaseInsensitive) == 0)
        return value;
    return {};
  }
  /// set replaces or adds one header.
  void set(const QByteArray &name, const QByteArray &value) {
    remove(name);
    headers.append({name, value});
  }
  /// remove drops one header.
  void remove(const QByteArray &name) {
    headers.removeIf([&](const auto &h) {
      return h.first.compare(name, Qt::CaseInsensitive) == 0;
    });
  }
  /// serialize writes the message with a Content-Length body.
  QByteArray serialize(bool withLength = true) const {
    QByteArray out = start + "\r\n";
    for (const auto &[key, value] : headers)
      out += key + ": " + value + "\r\n";
    if (withLength)
      out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    return out + "\r\n" + body;
  }
};
/// parse reads one complete message from data. Returns: the bytes consumed,
/// or -1 while incomplete. Inputs: bodyless says the message has no body
/// (HEAD replies, 204, 304); closed says the peer ended the stream.
qsizetype parse(const QByteArray &data, Message &m, bool response,
                bool bodyless, bool closed) {
  const auto end = data.indexOf("\r\n\r\n");
  if (end < 0)
    return -1;
  m = {};
  const auto lines = data.left(end).split('\n');
  m.start = lines.first().trimmed();
  for (qsizetype i = 1; i < lines.size(); ++i) {
    const auto colon = lines[i].indexOf(':');
    if (colon > 0)
      m.headers.append(
          {lines[i].left(colon).trimmed(), lines[i].mid(colon + 1).trimmed()});
  }
  qsizetype at = end + 4;
  if (bodyless)
    return at;
  if (m.header("Transfer-Encoding").toLower().contains("chunked")) {
    for (;;) {
      const auto line = data.indexOf("\r\n", at);
      if (line < 0)
        return -1;
      bool ok = false;
      const auto size = data.mid(at, line - at).split(';').first().trimmed()
                            .toLongLong(&ok, 16);
      require(ok, "TLS proxy fixture read a malformed chunk");
      at = line + 2;
      if (size == 0) {
        const auto trailer = data.indexOf("\r\n", at);
        return trailer < 0 ? -1 : trailer + 2;
      }
      if (data.size() < at + size + 2)
        return -1;
      m.body += data.mid(at, size);
      at += size + 2;
    }
  }
  if (const auto length = m.header("Content-Length"); !length.isEmpty()) {
    const auto size = length.toLongLong();
    if (data.size() < at + size)
      return -1;
    m.body = data.mid(at, size);
    return at + size;
  }
  if (!response)
    return at;
  if (!closed)
    return -1;
  m.body = data.mid(at);
  return data.size();
}

class TlsProxy;
/// Session is one client connection to a TlsProxy: the CONNECT request, the
/// fixture's own TLS server side, and a separate verified TLS connection to
/// the real server for each forwarded request stream.
class Session final : public QObject {
public:
  Session(TlsProxy *proxy, qintptr descriptor);
  ~Session() override;

private:
  void clientData();
  void next();
  void upstreamData(bool closed);
  void answer(int status, const QByteArray &reason,
              const QList<std::pair<QByteArray, QByteArray>> &headers);
  QPointer<TlsProxy> proxy_;
  QSslSocket client_, upstream_;
  QByteArray in_, reply_, method_, queued_;
  QString host_;
  bool tunnel_ = false, encrypted_ = false, awaiting_ = false, dav_ = false;
};

/// TlsProxy is a loopback CONNECT proxy that terminates TLS with a throwaway
/// certificate and forwards HTTP/1.1 to the real server over verified TLS
/// (test process only). Notes: it can strip an installation path, answer
/// every request with one status, or refuse connections; it counts requests
/// and the ones that carried credentials, never their values.
class TlsProxy final : public QTcpServer {
public:
  enum class Mode { Forward, Refuse, Status };
  /// TlsProxy serves identity and forwards to upstreamName at the loopback
  /// server port.
  TlsProxy(Identity identity, QString upstreamName, quint16 port)
      : identity_(std::move(identity)), upstream_(std::move(upstreamName)),
        port_(port) {
    clock_.start();
    require(listen(QHostAddress::LocalHost, 0),
            "TLS proxy fixture could not listen");
  }
  /// setIdentity presents another certificate and ends every connection.
  void setIdentity(Identity identity) {
    identity_ = std::move(identity);
    reset();
  }
  /// setMode changes what every new request gets and ends every connection.
  void setMode(Mode mode, int status = 0,
               QList<std::pair<QByteArray, QByteArray>> headers = {}) {
    mode_ = mode;
    status_ = status;
    statusHeaders_ = std::move(headers);
    reset();
  }
  /// setStrip forwards only paths under prefix, without it, and adds it back
  /// to the hrefs and Location of the server's answers.
  void setStrip(const QByteArray &prefix) { strip_ = prefix; }
  quint64 connections = 0, requests = 0, credentialed = 0, plain = 0;
  /// davInFlight counts forwarded DAV requests not yet answered; delay holds
  /// every forwarded answer back this many milliseconds.
  qint64 davInFlight = 0, delay = 0;
  QList<qint64> attempts; // ms since the proxy started, one per CONNECT

private:
  friend class Session;
  void incomingConnection(qintptr descriptor) override {
    ++connections;
    attempts.append(clock_.elapsed());
    sessions_.append(new Session(this, descriptor));
  }
  void reset() {
    for (const auto &session : std::exchange(sessions_, {}))
      if (session)
        session->deleteLater();
  }
  Identity identity_;
  QString upstream_;
  quint16 port_;
  QByteArray strip_;
  Mode mode_ = Mode::Forward;
  int status_ = 0;
  QList<std::pair<QByteArray, QByteArray>> statusHeaders_;
  QElapsedTimer clock_;
  QList<QPointer<Session>> sessions_;
};

Session::Session(TlsProxy *proxy, qintptr descriptor)
    : QObject(proxy), proxy_(proxy) {
  client_.setSocketDescriptor(descriptor);
  QObject::connect(&client_, &QSslSocket::readyRead, this,
                   [this] { clientData(); });
  QObject::connect(&client_, &QSslSocket::disconnected, this,
                   [this] { deleteLater(); });
  QObject::connect(&client_, &QSslSocket::encrypted, this, [this] {
    encrypted_ = true;
    next();
  });
  // A client that refuses the certificate just goes away.
  QObject::connect(&client_, &QSslSocket::errorOccurred, this,
                   [this] { deleteLater(); });
  QObject::connect(&upstream_, &QSslSocket::encrypted, this, [this] {
    upstream_.write(queued_);
    queued_.clear();
  });
  QObject::connect(&upstream_, &QSslSocket::readyRead, this,
                   [this] { upstreamData(false); });
  QObject::connect(&upstream_, &QSslSocket::disconnected, this,
                   [this] { upstreamData(true); });
  // An upstream that never connected ends the client's request too.
  QObject::connect(&upstream_, &QSslSocket::errorOccurred, this, [this] {
    if (awaiting_ && upstream_.state() == QAbstractSocket::UnconnectedState)
      client_.disconnectFromHost();
  });
}

Session::~Session() {
  if (awaiting_ && dav_ && proxy_)
    --proxy_->davInFlight;
}

void Session::clientData() {
  in_ += client_.readAll();
  if (tunnel_) {
    next();
    return;
  }
  const auto end = in_.indexOf("\r\n\r\n");
  if (end < 0)
    return;
  const auto line = in_.left(in_.indexOf("\r\n")).split(' ');
  if (line.value(0) != "CONNECT") {
    // A plain-HTTP request through the proxy: counted, never forwarded.
    ++proxy_->plain;
    ++proxy_->requests;
    if (in_.toLower().contains("\nauthorization:") ||
        in_.toLower().contains("\ncookie:"))
      ++proxy_->credentialed;
    client_.write("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n"
                  "Connection: close\r\n\r\n");
    client_.disconnectFromHost();
    return;
  }
  host_ = QString::fromLatin1(line.value(1)).section(':', 0, 0);
  in_.clear();
  tunnel_ = true;
  if (proxy_->mode_ == TlsProxy::Mode::Refuse) {
    client_.abort();
    deleteLater();
    return;
  }
  client_.write("HTTP/1.1 200 Connection established\r\n\r\n");
  client_.flush();
  client_.setLocalCertificate(proxy_->identity_.certificate);
  client_.setPrivateKey(proxy_->identity_.key);
  client_.setPeerVerifyMode(QSslSocket::VerifyNone);
  client_.startServerEncryption();
}

void Session::next() {
  if (!encrypted_ || awaiting_)
    return;
  Message request;
  const auto used = parse(in_, request, false, false, false);
  if (used < 0)
    return;
  in_.remove(0, used);
  ++proxy_->requests;
  if (!request.header("Authorization").isEmpty() ||
      !request.header("Cookie").isEmpty())
    ++proxy_->credentialed;
  if (proxy_->mode_ == TlsProxy::Mode::Status) {
    answer(proxy_->status_, "Fixture", proxy_->statusHeaders_);
    return;
  }
  auto parts = request.start.split(' ');
  method_ = parts.value(0);
  dav_ = parts.value(1).contains("/remote.php/dav/");
  if (dav_)
    ++proxy_->davInFlight;
  const auto &strip = proxy_->strip_;
  if (!strip.isEmpty()) {
    if (!parts.value(1).startsWith(strip + '/')) {
      answer(404, "Not Found", {});
      return;
    }
    parts[1] = parts[1].mid(strip.size());
    if (const auto destination = request.header("Destination");
        destination.startsWith(strip + '/')) {
      // Qt's engine sends an absolute path.
      request.set("Destination", destination.mid(strip.size()));
    } else if (!destination.isEmpty()) {
      QUrl url(QString::fromLatin1(destination));
      url.setHost(proxy_->upstream_);
      url.setPath(url.path(QUrl::FullyEncoded).mid(strip.size()),
                  QUrl::TolerantMode);
      request.set("Destination", url.toEncoded());
    }
  }
  request.start = parts.join(' ');
  request.set("Host", proxy_->upstream_.toLatin1() + ':' +
                          QByteArray::number(proxy_->port_));
  request.set("Accept-Encoding", "identity");
  request.remove("Transfer-Encoding");
  request.remove("Content-Length"); // serialize writes the decoded length
  awaiting_ = true;
  reply_.clear();
  const auto bytes = request.serialize();
  if (upstream_.state() == QAbstractSocket::UnconnectedState) {
    queued_ = bytes;
    upstream_.setProxy(QNetworkProxy::NoProxy);
    upstream_.connectToHostEncrypted("127.0.0.1", proxy_->port_,
                                     proxy_->upstream_);
  } else if (!upstream_.isEncrypted())
    queued_ += bytes;
  else
    upstream_.write(bytes);
}

void Session::upstreamData(bool closed) {
  reply_ += upstream_.readAll();
  if (!awaiting_)
    return;
  Message response;
  const auto status = reply_.mid(9, 3).toInt();
  const auto used = parse(reply_, response, true,
                          method_ == "HEAD" || status == 204 || status == 304,
                          closed);
  if (used < 0) {
    if (closed)
      client_.disconnectFromHost();
    return;
  }
  reply_.remove(0, used);
  const auto &strip = proxy_->strip_;
  if (!strip.isEmpty()) {
    response.body.replace("<d:href>/", "<d:href>" + strip + '/');
    if (const auto location = response.header("Location");
        location.startsWith('/'))
      response.set("Location", strip + location);
  }
  response.remove("Transfer-Encoding");
  response.remove("Content-Length");
  const bool close = closed || response.header("Connection").toLower() ==
                                   "close";
  const auto deliver = [this, bytes = response.serialize(), close] {
    client_.write(bytes);
    awaiting_ = false;
    if (dav_)
      --proxy_->davInFlight;
    if (close) {
      client_.disconnectFromHost();
      upstream_.abort();
      return;
    }
    next();
  };
  // A reply held back models a slow answer that arrives late.
  if (proxy_->delay > 0)
    QTimer::singleShot(proxy_->delay, this, deliver);
  else
    deliver();
}

void Session::answer(int status, const QByteArray &reason,
                     const QList<std::pair<QByteArray, QByteArray>> &headers) {
  Message m;
  m.start = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason;
  m.headers = headers;
  m.set("Connection", "close");
  client_.write(m.serialize());
  client_.disconnectFromHost();
}

/// Run holds a scenario's controls and the read helpers every case uses.
struct Run {
  const SwitchingControls &k;
  QString root;
  QUrl cleanup;
  QString uid, collection;
  /// snapshot reads the daemon's current projection over the real dispatcher.
  QJsonObject snapshot() const {
    return k.calls.call("state.bootstrap", {})
        .value("result")
        .toObject()
        .value("snapshot")
        .toObject();
  }
  /// item finds one entry of a projection array by its id.
  QJsonObject item(const char *array, const QString &id) const {
    for (const auto &entry : snapshot().value(array).toArray())
      if (entry.toObject().value("id") == id)
        return entry.toObject();
    return {};
  }
  /// network is the account's network-policy projection.
  QJsonObject network(const QString &account = {}) const {
    for (const auto &entry : snapshot().value("networks").toArray())
      if (entry.toObject().value("accountId") ==
          (account.isEmpty() ? k.account : account))
        return entry.toObject();
    return {};
  }
  /// settle submits one command and waits for its terminal receipt.
  QJsonObject settle(const QString &method, const QJsonObject &params) const {
    return k.calls.settled(k.calls.call(method, params));
  }
  /// reason is a failed receipt's reason code.
  static QString reason(const QJsonObject &receipt) {
    return receipt.value("error")
        .toObject()
        .value("data")
        .toObject()
        .value("reasonCode")
        .toString();
  }
  /// bound is the account's committed endpoint and epoch.
  std::pair<QString, qint64> bound(const QString &account = {}) const {
    return k.calls.binding(account.isEmpty() ? k.account : account);
  }
  /// steady holds for ms and fails as soon as invariant breaks.
  void steady(int ms, const std::function<bool()> &invariant,
              const char *message) const {
    QElapsedTimer t;
    t.start();
    for (;;) {
      if (!invariant()) {
        // Sanitized state for the record: which address, epoch, phase.
        const auto facts = k.facts();
        std::fprintf(stderr, "state: bound %s epoch %lld phase %s failed %s\n",
                     facts.endpoint == k.primary  ? "primary"
                     : facts.endpoint == k.target ? "alternate"
                                                  : "other",
                     facts.epoch, qPrintable(facts.phase),
                     qPrintable(facts.interrupted.join(',')));
        throw std::runtime_error(message);
      }
      if (t.elapsed() >= ms)
        return;
      hold(200);
    }
  }
  /// policy sets the account's endpoint policy.
  void policy(const QString &mode) const {
    require(settle("endpoints.setPolicy",
                   {{"accountId", k.account}, {"policy", mode}})
                    .value("state") == "succeeded",
            "endpoint policy could not be set");
  }
  /// reserve creates the run's one owned remote collection; the parent's
  /// cleanup removes it even after a crash.
  void reserve() {
    uid = readServerJson(k.base, "ocs/v2.php/cloud/user?format=json",
                         qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                         k.credential)
              .value("ocs")
              .toObject()
              .value("data")
              .toObject()
              .value("id")
              .toString();
    require(!uid.isEmpty(), "fixture user identity unavailable");
    collection = "mygo-network-" + QFileInfo(root).fileName().right(32);
    QSaveFile manifest(root + "/owned-content.json");
    const auto reservation =
        QJsonDocument(QJsonObject{{"uid", uid}, {"collection", collection}})
            .toJson(QJsonDocument::Compact);
    require(manifest.open(QIODevice::WriteOnly) &&
                manifest.write(reservation) == reservation.size() &&
                manifest.commit(),
            "owned content cleanup reservation failed");
    require(admin("MKCOL", "") == 201,
            "owned reserved collection could not be created");
  }
  /// admin sends one request as the fixture user to the direct loopback
  /// origin, inside the reserved collection.
  int admin(const QByteArray &method, const QString &path,
            const QByteArray &content = {}, QByteArray *body = nullptr) const {
    return davRequest(cleanup, method,
                      home(uid) + collection + (path.isEmpty() ? "" : "/") +
                          path,
                      qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                      qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD"), content,
                      body);
  }
  /// remote reads one file of the reserved collection; empty when absent.
  /// Notes: reads the whole entry (davRequest keeps only its first MiB).
  QByteArray remote(const QString &path) const {
    const auto url =
        cleanup.resolved(QUrl(home(uid) + collection + '/' + path));
    require(withinServer(cleanup, url), "owned read escaped the origin");
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    request.setRawHeader(
        "Authorization",
        "Basic " + (qEnvironmentVariable("NEXTCLOUD_TEST_USER").toUtf8() + ':' +
                    qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD").toUtf8())
                       .toBase64());
    auto *reply = manager.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
      loop.exec();
    const auto body = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                                  .toInt() == 200
                          ? reply->readAll()
                          : QByteArray{};
    reply->deleteLater();
    return body;
  }
  /// Mapped is one mapping of a subcollection of the reserved one.
  struct Mapped {
    QString folder, local, name;
  };
  /// map creates a subcollection, maps it to a new local folder and waits for
  /// its first reconciliation.
  Mapped map(const QString &name, bool reconcile = true) const {
    Mapped m{{}, root + "/sw-" + name, name};
    require(admin("MKCOL", name) == 201 && QDir().mkpath(m.local),
            "owned subcollection could not be created");
    const auto token = k.calls
                           .call("folders.inspect",
                                 {{"accountId", k.account},
                                  {"remotePath", '/' + collection + '/' + name},
                                  {"localPath", m.local}})
                           .value("result")
                           .toObject()
                           .value("inspectToken")
                           .toString();
    require(!token.isEmpty() &&
                settle("folders.add", {{"inspectToken", token}})
                        .value("state") == "succeeded",
            "owned folder could not be added");
    for (const auto &entry : snapshot().value("folders").toArray())
      if (entry.toObject().value("localPath") == QDir::cleanPath(m.local))
        m.folder = entry.toObject().value("id").toString();
    require(!m.folder.isEmpty(), "owned mapping absent");
    if (reconcile)
      k.calls.wait([&] { return state(m) == "upToDate"; },
                   "owned folder did not reconcile");
    return m;
  }
  /// counter reads one sanitized diagnostics counter of a mapping.
  quint64 counter(const QString &folder, const char *name) const {
    for (const auto &entry : k.calls.call("diagnostics.snapshot", {})
                                 .value("result")
                                 .toObject()
                                 .value("folders")
                                 .toArray())
      if (entry.toObject().value("folderId") == folder)
        return entry.toObject().value(name).toString().toULongLong();
    return 0;
  }
  /// state is a mapping's projected state.
  QString state(const Mapped &m) const {
    return item("folders", m.folder).value("state").toString();
  }
  /// write puts one local file into a mapping and asks for a cycle.
  void write(const Mapped &m, const QString &name,
             const QByteArray &content) const {
    QFile file(m.local + '/' + name);
    require(file.open(QIODevice::WriteOnly) &&
                file.write(content) == content.size(),
            "owned local file could not be written");
    file.close();
    k.calls.call("folders.syncNow", {{"folderId", m.folder}});
  }
  /// uploaded waits until the server holds exactly this content.
  void uploaded(const Mapped &m, const QString &name, const QByteArray &content,
                const char *message) const {
    k.calls.wait(
        [&] {
          return QCryptographicHash::hash(remote(m.name + '/' + name),
                                          QCryptographicHash::Sha256) ==
                 QCryptographicHash::hash(content, QCryptographicHash::Sha256);
        },
        message);
  }
};
/// random returns fresh content of the given size.
QByteArray random(int size) {
  QByteArray bytes(size, Qt::Uninitialized);
  QRandomGenerator::global()->fillRange(
      reinterpret_cast<quint32 *>(bytes.data()), size / 4);
  return bytes;
}
/// endpointCase is the alternate's endpoint projection field set used by
/// several cases.
bool blockedFor(const QJsonObject &endpoint, const QString &reason) {
  return endpoint.value("blocked").toBool() &&
         endpoint.value("blockedReason") == reason;
}
} // namespace

/// SwitchingFixture owns one scenario's proxies, certificates and profile.
struct SwitchingFixture {
  QString root, name;
  QDir tls;
  std::map<QString, std::unique_ptr<TlsProxy>> proxies;
  int serial = 0;
  /// openssl runs the throwaway certificate tool with bounded time.
  void openssl(const QStringList &arguments) {
    auto program = QStandardPaths::findExecutable("openssl");
    if (program.isEmpty())
      program = QStandardPaths::findExecutable(
          "openssl", {"C:/Program Files/Git/usr/bin"});
    require(!program.isEmpty(), "the TLS proxy fixture needs openssl");
    QProcess p;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("MSYS2_ARG_CONV_EXCL", "*");
    p.setProcessEnvironment(environment);
    p.setWorkingDirectory(tls.path());
    p.start(program, arguments);
    require(p.waitForFinished(30000) && p.exitCode() == 0,
            "throwaway certificate generation failed");
  }
  /// ca creates a throwaway CA; trusted ones are trusted in this process only.
  void ca(const QString &name, bool trusted) {
    openssl({"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout",
             name + ".key", "-out", name + ".pem", "-days", "2", "-subj",
             "/CN=Nextcloud-MyGo switching test " + name});
    if (!trusted)
      return;
    QFile file(tls.filePath(name + ".pem"));
    require(file.open(QIODevice::ReadOnly), "throwaway CA unreadable");
    auto configuration = QSslConfiguration::defaultConfiguration();
    auto roots = configuration.caCertificates();
    roots.append(QSslCertificate::fromData(file.readAll(), QSsl::Pem));
    configuration.setCaCertificates(roots);
    QSslConfiguration::setDefaultConfiguration(configuration);
  }
  /// leaf issues a server certificate naming dnsName from the named CA,
  /// valid now or expired.
  Identity leaf(const QString &dnsName, const QString &authority = "trusted",
                bool expired = false) {
    const auto name = "leaf" + QString::number(++serial);
    openssl({"req", "-newkey", "rsa:2048", "-nodes", "-keyout", name + ".key",
             "-out", name + ".csr", "-subj", "/CN=" + dnsName, "-addext",
             "subjectAltName=DNS:" + dnsName});
    QStringList sign{"x509", "-req", "-in", name + ".csr", "-CA",
                     authority + ".pem", "-CAkey", authority + ".key",
                     "-set_serial", QString::number(1000 + serial), "-out",
                     name + ".pem", "-copy_extensions", "copyall"};
    sign += expired ? QStringList{"-not_before", "20200101000000Z",
                                  "-not_after", "20200102000000Z"}
                    : QStringList{"-days", "1"};
    openssl(sign);
    QFile cert(tls.filePath(name + ".pem")), key(tls.filePath(name + ".key"));
    require(cert.open(QIODevice::ReadOnly) && key.open(QIODevice::ReadOnly),
            "throwaway certificate unreadable");
    Identity identity{QSslCertificate(cert.readAll(), QSsl::Pem),
                      QSslKey(key.readAll(), QSsl::Rsa, QSsl::Pem)};
    require(!identity.certificate.isNull() && !identity.key.isNull(),
            "throwaway certificate invalid");
    return identity;
  }
  /// proxy starts one TLS-terminating proxy for host and routes host to it.
  TlsProxy &proxy(const QString &host, Identity identity,
                  const QString &upstream,
                  const std::function<void(const QString &, quint16)> &route) {
    const auto port =
        quint16(QUrl(qEnvironmentVariable("NEXTCLOUD_TEST_CLEANUP_URL")).port());
    auto &p = proxies[host];
    p = std::make_unique<TlsProxy>(std::move(identity), upstream, port);
    route(host, p->serverPort());
    return *p;
  }
};

namespace {
const QStringList cases{"auto-switch", "auto-fallback", "tls-trust",
                        "tls-auth",    "tls-circuit",   "redirect",
                        "subpath",     "revoke-switch", "new-folder",
                        "root-change", "late-epoch"};
/// hosts are the two approved origin names of the disposable server.
QString primaryHost() {
  return QUrl(qEnvironmentVariable("NEXTCLOUD_TEST_URL")).host();
}
QString alternateHost() {
  return QUrl(qEnvironmentVariable("NEXTCLOUD_TEST_ALTERNATE_URL")).host();
}
/// origin is an https URL of a fixture name on the server's port.
QString origin(const QString &host, const QString &path = "/") {
  return "https://" + host + ':' +
         QString::number(
             QUrl(qEnvironmentVariable("NEXTCLOUD_TEST_URL")).port()) +
         path;
}

/// autoSwitch: a better address appears while a large upload is held
/// unfinished (N11), with interface flapping first (N08); the dwell is aged,
/// the score margin, stability and freshness are the daemon's own (N09).
void autoSwitch(Run &r) {
  const auto &k = r.k;
  r.policy("auto");
  k.calls.wait(k.ready, "both addresses did not become healthy");
  const auto m = r.map("one");
  const auto before = k.facts();
  const auto original = r.bound();
  require(original.first == k.primary, "the run did not start on the primary");
  // A 12 MiB upload held at 4 MiB on the primary.
  const auto content = random(12 << 20);
  const auto sent = r.counter(m.folder, "uploadBytes");
  NetworkTunnel::setStallAfter(4 << 20);
  r.write(m, "large.bin", content);
  k.calls.wait(
      [&] {
        return r.counter(m.folder, "activeUploads") > 0 &&
               r.counter(m.folder, "uploadBytes") >= sent + (2 << 20);
      },
      "the large upload was not held unfinished");
  // The home address has served long enough: only the transfer stands in the
  // way of an optimization switch now.
  k.age(130000);
  // N08: 20 interface changes 400 ms apart; evidence is rebuilt, never moved.
  for (int i = 0; i < 20; ++i) {
    k.changed();
    hold(400);
    require(r.bound() == original, "interface flapping moved the account");
  }
  // N11: the better address waits for the healthy transfer, past the 30 s
  // drain window, without the upload being cancelled.
  r.steady(
      32000,
      [&] {
        return r.bound() == original &&
               r.counter(m.folder, "activeUploads") > 0;
      },
      "a better address cancelled a healthy transfer or switched before it "
      "finished");
  NetworkTunnel::setStallAfter(0);
  r.uploaded(m, "large.bin", content, "the held upload did not complete");
  k.calls.wait(
      [&] {
        const auto b = r.bound();
        return b.first == k.target && b.second == original.second + 1 &&
               k.calls.stable(k.account);
      },
      "the better address was not used after the transfer finished");
  const auto network = r.network();
  require(network.value("switchReason") == "faster",
          "the automatic switch did not record its reason");
  const auto after = k.facts();
  require(after.folders == before.folders &&
              after.credential == before.credential,
          "the switch changed a journal, root identity or credential");
  k.calls.wait([&] { return r.state(m) == "upToDate"; },
               "the mapping did not reconcile on the better address");
  std::puts("PASS N08 20 interface changes in 8 s moved nothing; N11 a better "
            "home address waited 32 s past the drain window for a held 12 MiB "
            "upload without cancelling it, then switched with reason "
            "faster (preference) and one new epoch; N09 score margin, "
            "stability and "
            "freshness from the daemon's own probes");
}

/// autoFallback: the better address passes verification and fails the first
/// business request after the commit (N14).
void autoFallback(Run &r) {
  const auto &k = r.k;
  r.policy("auto");
  k.calls.wait(k.ready, "both addresses did not become healthy");
  const auto m = r.map("one");
  const auto before = k.facts();
  const auto original = r.bound();
  bool broken = false;
  qint64 committed = 0;
  k.onTargetRequest([&](const QString &path) {
    const auto b = k.calls.binding(k.account);
    if (!broken && path.contains("/remote.php/dav/") && b.first == k.target) {
      broken = true;
      committed = b.second;
      k.failAlternate(true);
    }
  });
  k.age(130000);
  k.calls.wait([&] { return broken; },
               "no business request reached the better address after commit");
  k.onTargetRequest({});
  k.calls.wait(
      [&] {
        const auto b = r.bound();
        return b.first == k.primary && b.second > committed &&
               k.calls.stable(k.account);
      },
      "the account did not return to the freshly verified previous address");
  const auto after = k.facts();
  require(after.folders == before.folders &&
              after.credential == before.credential,
          "the fallback changed a journal, root identity or credential");
  const auto content = random(4096);
  r.write(m, "after.txt", content);
  r.uploaded(m, "after.txt", content, "the previous address did not sync");
  k.failAlternate(false);
  std::puts("PASS N14 verified address failing its first business request "
            "after the commit: the previous address was verified again and "
            "used with a further epoch, journals and credential unchanged");
}

/// tlsTrust: an unregistered reachable address never gets credentials (N03);
/// expired, wrong-host and unknown-CA certificates fail registration and
/// quarantine an approved address (N04).
void tlsTrust(Run &r, SwitchingFixture &f) {
  const auto &k = r.k;
  r.policy("auto");
  k.calls.wait(k.ready, "both addresses did not become healthy");
  // N04 at registration.
  for (const auto *host : {"expired.syncd.test", "wronghost.syncd.test",
                           "unknownca.syncd.test"}) {
    const auto receipt =
        r.settle("endpoints.stage", {{"accountId", k.account},
                                     {"name", "Bad certificate"},
                                     {"baseUrl", origin(host)}});
    require(receipt.value("state") == "failed",
            "an address with a bad certificate was staged");
    require(f.proxies.at(host)->credentialed == 0,
            "credentials reached an address with a bad certificate");
  }
  // N04 on the approved alternate, one bad certificate after another.
  auto &alternate = *f.proxies.at(alternateHost());
  const auto good = f.leaf(alternateHost());
  const std::pair<const char *, Identity> faults[] = {
      {"expired", f.leaf(alternateHost(), "trusted", true)},
      {"wrong host", f.leaf("other.syncd.test")},
      {"unknown CA", f.leaf(alternateHost(), "untrusted")}};
  bool first = true;
  for (const auto &[name, identity] : faults) {
    alternate.setIdentity(identity);
    k.changed();
    k.calls.wait(
        [&] {
          return blockedFor(r.item("endpoints", k.target), "TLS_ERROR") &&
                 k.failed(k.target);
        },
        "a bad certificate on the approved address was not quarantined");
    if (first) {
      k.failPrimary(true);
      k.changed();
      r.steady(
          6000, [&] { return r.bound().first == k.primary; },
          "the account moved to a quarantined address");
      k.failPrimary(false);
      first = false;
    }
    alternate.setIdentity(good);
    require(r.settle("endpoints.verify", {{"endpointId", k.target}})
                    .value("state") == "succeeded",
            "the restored certificate did not pass an explicit review");
    k.calls.wait(
        [&] { return !r.item("endpoints", k.target).value("blocked").toBool(); },
        "the reviewed address stayed quarantined");
    std::printf("PASS N04 %s certificate quarantined, reviewed back\n", name);
  }
  // N03: reachable and unregistered.
  auto &rogue = *f.proxies.at("rogue.syncd.test");
  const auto staged =
      r.settle("endpoints.stage", {{"accountId", k.account},
                                   {"name", "Unregistered"},
                                   {"baseUrl", origin("rogue.syncd.test")}});
  require(staged.value("state") == "succeeded" && rogue.requests > 0,
          "the unregistered address was not reachable");
  const auto seen = rogue.requests;
  k.failPrimary(true);
  k.failAlternate(true);
  k.changed();
  k.calls.wait([&] { return k.failed(k.primary); },
               "the primary failure was not observed");
  r.steady(
      6000, [&] { return r.bound().first == k.primary; },
      "the account moved to an unregistered address");
  require(rogue.credentialed == 0 && rogue.requests == seen,
          "the unregistered address got a request or credentials");
  k.failPrimary(false);
  k.failAlternate(false);
  k.changed();
  std::puts("PASS N03 an unregistered reachable address got its public check "
            "only, no credentials and no further request while both approved "
            "addresses were down; N04 expired, wrong-host and unknown-CA "
            "addresses refused at registration");
}

/// tlsAuth: only the alternate answers 401 (N16); the primary answers 429
/// with Retry-After (N18).
void tlsAuth(Run &r, SwitchingFixture &f) {
  const auto &k = r.k;
  r.policy("auto");
  k.calls.wait(k.ready, "both addresses did not become healthy");
  const auto m = r.map("one");
  const auto before = k.facts();
  auto &primary = *f.proxies.at(primaryHost());
  auto &alternate = *f.proxies.at(alternateHost());
  // N16.
  alternate.setMode(TlsProxy::Mode::Status, 401,
                    {{"WWW-Authenticate", "Basic realm=\"Nextcloud\""}});
  k.changed();
  k.calls.wait(
      [&] { return blockedFor(r.item("endpoints", k.target), "AUTH_REQUIRED"); },
      "a 401 from one address was not recorded on that address");
  require(r.item("accounts", k.account).value("auth") == "signedIn" &&
              k.facts().credential == before.credential,
          "a 401 from one address signed the account out");
  const auto content = random(4096);
  r.write(m, "while-401.txt", content);
  r.uploaded(m, "while-401.txt", content,
             "the healthy address stopped syncing during the other's 401");
  require(!readServerJson(k.base, "ocs/v2.php/cloud/user?format=json",
                          qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
                          k.credential)
               .isEmpty(),
          "the stored credential was removed or changed");
  k.failPrimary(true);
  k.changed();
  r.steady(
      6000, [&] { return r.bound().first == k.primary; },
      "the account moved to the address that answered 401");
  k.failPrimary(false);
  alternate.setMode(TlsProxy::Mode::Forward);
  require(r.settle("endpoints.verify", {{"endpointId", k.target}})
                  .value("state") == "succeeded",
          "the recovered address did not pass an explicit review");
  k.changed();
  k.calls.wait(k.ready, "both addresses did not recover after the 401");
  std::puts("PASS N16 401 from one address only: that address blocked for "
            "authentication, the account signed in, the credential kept and "
            "used, no switch to it");
  // N18.
  const auto before429 = primary.requests;
  primary.setMode(TlsProxy::Mode::Status, 429, {{"Retry-After", "20"}});
  const auto throttled = random(4096);
  r.write(m, "after-429.txt", throttled);
  k.calls.wait([&] { return primary.requests > before429; },
               "the 429 answer was not received");
  hold(1000);
  const auto alternateSeen = alternate.requests, primarySeen = primary.requests;
  r.steady(
      12000,
      [&] {
        return r.bound().first == k.primary &&
               alternate.requests == alternateSeen;
      },
      "the account evaded a 429 through the other address");
  require(primary.requests <= primarySeen + 1,
          "requests continued during the server's Retry-After");
  primary.setMode(TlsProxy::Mode::Forward);
  r.uploaded(m, "after-429.txt", throttled,
             "the upload did not resume after Retry-After");
  require(r.bound().first == k.primary,
          "the account switched after the throttle");
  std::puts("PASS N18 429 with Retry-After 20 on the address in use: no "
            "request to either address during it, no switch, the upload "
            "resumed on the same address afterwards");
}

/// tlsCircuit: three independent connection failures open the circuit (N10);
/// a storm of network notifications admits one half-open probe at a time,
/// at least 10 s apart (N22).
void tlsCircuit(Run &r, SwitchingFixture &f) {
  const auto &k = r.k;
  r.policy("auto");
  k.calls.wait(k.ready, "both addresses did not become healthy");
  auto &alternate = *f.proxies.at(alternateHost());
  alternate.setMode(TlsProxy::Mode::Refuse);
  const auto from = alternate.attempts.size();
  k.calls.wait([&] { return k.failed(k.target); },
               "three connection failures did not open the circuit");
  // Attempts within 5 s of each other are one failure (one request's
  // retries); the circuit needs three independent ones.
  QList<qint64> independent;
  for (auto i = from; i < alternate.attempts.size(); ++i)
    if (independent.isEmpty() ||
        alternate.attempts[i] - independent.last() >= 4500)
      independent.append(alternate.attempts[i]);
  require(independent.size() >= 3 &&
              independent[2] - independent[0] >= 9500,
          "the circuit opened before three independent failures");
  std::printf("PASS N10 circuit opened after %lld connection attempts, %lld "
              "independent over %lld ms\n",
              qint64(alternate.attempts.size() - from),
              qint64(independent.size()),
              qint64(independent.last() - independent.first()));
  // N22: 30 notifications in 3 s, then 15 s of observation.
  const auto stormFrom = alternate.attempts.size();
  QElapsedTimer storm;
  storm.start();
  for (int i = 0; i < 30; ++i) {
    k.changed();
    hold(100);
  }
  hold(15000);
  // One probe's connection retries arrive within a second: a probe is a
  // burst of attempts.
  QList<qint64> probes;
  for (const auto at : alternate.attempts.mid(stormFrom))
    if (probes.isEmpty() || at - probes.last() >= 4500)
      probes.append(at);
  require(probes.size() <= 2, "a notification storm multiplied half-open probes");
  for (qsizetype i = 1; i < probes.size(); ++i)
    require(probes[i] - probes[i - 1] >= 10000,
            "half-open probes closer than 10 s");
  alternate.setMode(TlsProxy::Mode::Forward);
  // N15: every address down for 45 s, with a network notification every 15 s.
  auto &primary = *f.proxies.at(primaryHost());
  const auto bound = r.bound();
  primary.setMode(TlsProxy::Mode::Refuse);
  alternate.setMode(TlsProxy::Mode::Refuse);
  const auto primaryFrom = primary.attempts.size(),
             alternateFrom = alternate.attempts.size();
  for (int i = 0; i < 3; ++i) {
    k.changed();
    r.steady(
        15000, [&] { return r.bound() == bound; },
        "the account moved while every address was down");
  }
  const auto bursts = [](const QList<qint64> &attempts) {
    QList<qint64> result;
    for (const auto at : attempts)
      if (result.isEmpty() || at - result.last() >= 4500)
        result.append(at);
    return qint64(result.size());
  };
  const auto down = bursts(primary.attempts.mid(primaryFrom)),
             other = bursts(alternate.attempts.mid(alternateFrom));
  require(k.failed(k.primary) && k.failed(k.target),
          "both down addresses were not classified as failed");
  require(down <= 6 && other <= 4,
          "probing was not bounded while every address was down");
  primary.setMode(TlsProxy::Mode::Forward);
  alternate.setMode(TlsProxy::Mode::Forward);
  std::printf("PASS N15 both addresses down for 45 s with three network "
              "notifications: no switch, %lld probes of the address in use and "
              "%lld of the other\n",
              down, other);
  std::printf("PASS N22 30 network notifications in 3 s led to %lld half-open "
              "probes in 18 s, none closer than 10 s\n",
              qint64(probes.size()));
}

/// lateEpoch: the address in use answers 6 s late while a switch is asked for
/// during a sync (N13): no DAV request of the old epoch is still unanswered
/// when the new epoch is committed, and replies arriving after it change
/// nothing on the new address.
void lateEpoch(Run &r, SwitchingFixture &f) {
  const auto &k = r.k;
  k.calls.wait(k.ready, "both addresses did not become healthy");
  const auto m = r.map("one");
  auto &primary = *f.proxies.at(primaryHost());
  primary.delay = 6000;
  r.write(m, "slow.txt", random(2048));
  k.calls.wait([&] { return primary.davInFlight > 0; },
               "no DAV request was held on the address in use");
  const auto original = r.bound();
  const auto accepted = k.calls.call(
      "endpoints.requestSwitch",
      {{"accountId", k.account}, {"endpointId", k.target}});
  bool early = false;
  k.calls.wait(
      [&] {
        const auto b = r.bound();
        if (b.first == k.target && primary.davInFlight > 0)
          early = true;
        return b.first == k.target;
      },
      "the switch did not commit");
  require(!early, "an old-epoch DAV request was unanswered at the commit");
  require(k.calls.settled(accepted).value("state") == "succeeded" &&
              r.bound().second > original.second,
          "the switch did not succeed with a new epoch");
  const auto committed = r.bound();
  // Late answers of the old address (probes are not drained) arrive now.
  r.steady(
      9000,
      [&] {
        const auto e = r.item("endpoints", k.target);
        return r.bound() == committed && !e.value("blocked").toBool() &&
               r.state(m) != "offline";
      },
      "a late old-epoch answer moved or blocked the new address");
  const auto content = random(4096);
  r.write(m, "after.txt", content);
  r.uploaded(m, "after.txt", content, "the new address did not sync");
  primary.delay = 0;
  require(r.bound() == committed, "the binding changed after the late answers");
  std::puts("PASS N13 answers 6 s late on the old address: the switch committed "
            "only after every old-epoch DAV request was answered, late probe "
            "answers left the new address, binding and progress unchanged");
}

/// redirect: the address in use redirects engine requests to an unknown
/// domain, then to plain HTTP (N05).
void redirect(Run &r, SwitchingFixture &f) {
  const auto &k = r.k;
  k.calls.wait(k.ready, "both addresses did not become healthy");
  const auto m = r.map("one");
  require(r.settle("endpoints.requestSwitch",
                   {{"accountId", k.account}, {"endpointId", k.target}})
                  .value("state") == "succeeded",
          "the manual switch to the alternate failed");
  k.calls.wait([&] { return r.state(m) == "upToDate"; },
               "the mapping did not reconcile on the alternate");
  auto &alternate = *f.proxies.at(alternateHost());
  auto &evil = *f.proxies.at("evil.syncd.test");
  for (const auto &target :
       {origin("evil.syncd.test", "/remote.php/dav/files/" + r.uid + '/'),
        QString("http://" + alternateHost() + ':' +
                QString::number(QUrl(qEnvironmentVariable(
                                         "NEXTCLOUD_TEST_URL"))
                                    .port()) +
                "/remote.php/dav/files/" + r.uid + '/')}) {
    // Each variant starts on the alternate, reviewed and in use.
    alternate.setMode(TlsProxy::Mode::Forward);
    if (r.item("endpoints", k.target).value("blocked").toBool())
      require(r.settle("endpoints.verify", {{"endpointId", k.target}})
                      .value("state") == "succeeded",
              "the alternate did not pass review between redirects");
    if (r.bound().first != k.target)
      require(r.settle("endpoints.requestSwitch",
                       {{"accountId", k.account}, {"endpointId", k.target}})
                      .value("state") == "succeeded",
              "the switch back to the alternate failed");
    alternate.setMode(TlsProxy::Mode::Status, 302,
                      {{"Location", target.toLatin1()}});
    const auto seen = alternate.requests;
    // A sync and a network notification: engine and probe requests both
    // meet the redirect.
    r.write(m, "redirected.txt", random(1024));
    k.changed();
    k.calls.wait([&] { return alternate.requests > seen; },
                 "no request met the redirect");
    hold(8000);
    require(evil.connections == 0 && evil.requests == 0,
            "a redirect to an unknown domain was followed");
    require(alternate.plain == 0, "a redirect to plain HTTP was followed");
    const auto endpoint = r.item("endpoints", k.target);
    std::printf("PASS N05 redirect to %s: %llu requests met it, none followed; "
                "address %s %s\n",
                target.startsWith("http:") ? "plain HTTP" : "an unknown domain",
                alternate.requests - seen,
                endpoint.value("blocked").toBool() ? "blocked" : "not blocked",
                qPrintable(endpoint.value("blockedReason").toString()));
  }
  // The fixed address is quarantined: the account waits for it (PLAN §8.5)
  // until the user reviews it.
  alternate.setMode(TlsProxy::Mode::Forward);
  require(r.bound().first == k.target,
          "a fixed address that redirected was silently replaced");
  require(r.settle("endpoints.verify", {{"endpointId", k.target}})
                  .value("state") == "succeeded",
          "the alternate did not pass review after the redirects");
  const auto content = random(2048);
  r.write(m, "redirected.txt", content);
  r.uploaded(m, "redirected.txt", content,
             "the mapping did not sync once the redirects ended");
  std::puts("PASS N05 engine and probe requests redirected to an unknown "
            "domain and to plain HTTP: nothing followed, no request and no "
            "credential left the approved origin");
}

/// subpath: a second legitimate entry under another installation path
/// (N06): DAV paths, Destination and hrefs map, or registration fails.
void subpath(Run &r) {
  const auto &k = r.k;
  k.calls.wait(k.ready, "both addresses did not become healthy");
  const auto m = r.map("one");
  const auto first = random(1024);
  r.write(m, "a.txt", first);
  r.uploaded(m, "a.txt", first, "the first file did not upload");
  const auto staged =
      r.settle("endpoints.stage",
               {{"accountId", k.account},
                {"name", "Other path"},
                {"baseUrl", origin("sub.syncd.test", "/nc/")}});
  require(staged.value("state") == "succeeded",
          "the subpath entry's public check failed");
  const auto approved = r.settle(
      "endpoints.approveOrigin",
      {{"stagingId", staged.value("result").toObject().value("id")}});
  if (approved.value("state") != "succeeded") {
    std::printf("PASS N06 the entry under another installation path was "
                "refused at registration (%s); nothing approved\n",
                qPrintable(Run::reason(approved)));
    return;
  }
  const auto sub =
      approved.value("result").toObject().value("endpointId").toString();
  require(r.settle("endpoints.requestSwitch",
                   {{"accountId", k.account}, {"endpointId", sub}})
                  .value("state") == "succeeded",
          "the switch to the subpath entry failed");
  k.calls.wait([&] { return r.state(m) == "upToDate"; },
               "the mapping did not reconcile through the subpath entry");
  const auto second = random(2048);
  r.write(m, "b.txt", second);
  r.uploaded(m, "b.txt", second, "an upload through the subpath was lost");
  require(QFile::rename(m.local + "/a.txt", m.local + "/c.txt"),
          "local rename failed");
  k.calls.call("folders.syncNow", {{"folderId", m.folder}});
  k.calls.wait(
      [&] { return r.remote("one/c.txt") == first && r.remote("one/a.txt").isEmpty(); },
      "a rename through the subpath was not a MOVE to the right Destination");
  const auto third = random(1536);
  require(r.admin("PUT", "one/d.txt", third) == 201,
          "the server-side file could not be created");
  k.calls.call("folders.syncNow", {{"folderId", m.folder}});
  k.calls.wait(
      [&] {
        QFile file(m.local + "/d.txt");
        return file.open(QIODevice::ReadOnly) && file.readAll() == third;
      },
      "a server-side file did not arrive through the subpath hrefs");
  std::puts("PASS N06 an entry under another installation path was verified "
            "and used: upload, rename (MOVE Destination) and download "
            "(PROPFIND hrefs) all mapped to the same server paths");
}

/// revokeSwitch: the server revokes the app password while a switch waits
/// for a held upload (N17).
void revokeSwitch(Run &r) {
  const auto &k = r.k;
  const auto [user, password] = ownedUser(r.root, r.cleanup);
  QByteArray body;
  require(davRequest(r.cleanup, "GET",
                     "ocs/v2.php/core/getapppassword?format=json", user,
                     password, {}, &body) == 200,
          "the owned user's app password was not issued");
  const auto app = QJsonDocument::fromJson(body)
                       .object()
                       .value("ocs")
                       .toObject()
                       .value("data")
                       .toObject()
                       .value("apppassword")
                       .toString();
  require(!app.isEmpty(), "the owned user's app password is empty");
  require(k.calls.login(user, app).value("state") == "succeeded",
          "the owned user could not sign in with its app password");
  QString account;
  for (const auto &entry : r.snapshot().value("accounts").toArray())
    if (entry.toObject().value("user") == user)
      account = entry.toObject().value("id").toString();
  require(!account.isEmpty(), "the owned account is absent");
  auto receipt = r.settle("endpoints.stage", {{"accountId", account},
                                              {"name", "Owned alternate"},
                                              {"baseUrl", k.alternate.toString()}});
  receipt = r.settle(
      "endpoints.approveOrigin",
      {{"stagingId", receipt.value("result").toObject().value("id")}});
  const auto target =
      receipt.value("result").toObject().value("endpointId").toString();
  require(!target.isEmpty(), "the owned account's alternate was not approved");
  const auto local = r.root + "/sw-revoke";
  require(davRequest(r.cleanup, "MKCOL", home(user) + "revoke", user, app) ==
                  201 &&
              QDir().mkpath(local),
          "the owned account's folder could not be created");
  const auto token = k.calls
                         .call("folders.inspect", {{"accountId", account},
                                                   {"remotePath", "/revoke"},
                                                   {"localPath", local}})
                         .value("result")
                         .toObject()
                         .value("inspectToken")
                         .toString();
  require(r.settle("folders.add", {{"inspectToken", token}}).value("state") ==
              "succeeded",
          "the owned account's folder could not be added");
  QString folder;
  for (const auto &entry : r.snapshot().value("folders").toArray())
    if (entry.toObject().value("accountId") == account)
      folder = entry.toObject().value("id").toString();
  k.calls.wait(
      [&] { return r.item("folders", folder).value("state") == "upToDate"; },
      "the owned account's folder did not reconcile");
  const auto original = r.bound(account);
  const auto sent = r.counter(folder, "uploadBytes");
  NetworkTunnel::setStallAfter(2 << 20);
  {
    QFile file(local + "/held.bin");
    require(file.open(QIODevice::WriteOnly) &&
                file.write(random(8 << 20)) == 8 << 20,
            "the held file could not be written");
  }
  k.calls.call("folders.syncNow", {{"folderId", folder}});
  k.calls.wait(
      [&] {
        return r.counter(folder, "activeUploads") > 0 &&
               r.counter(folder, "uploadBytes") >= sent + (1 << 20);
      },
      "the owned account's upload was not held");
  const auto requested = k.calls.call(
      "endpoints.requestSwitch", {{"accountId", account}, {"endpointId", target}});
  hold(1000);
  require(davRequest(r.cleanup, "DELETE", "ocs/v2.php/core/apppassword", user,
                     app) == 200,
          "the server did not revoke the app password");
  NetworkTunnel::setStallAfter(0);
  receipt = k.calls.settled(requested);
  require(receipt.value("state") == "failed",
          "the switch succeeded with a revoked password");
  k.calls.wait(
      [&] { return r.item("accounts", account).value("auth") == "revoked"; },
      "the revoked password did not ask for a new sign-in");
  const auto failedAt = r.bound(account);
  r.steady(
      15000,
      [&] {
        return r.bound(account) == failedAt &&
               r.item("accounts", account).value("auth") == "revoked";
      },
      "the account kept trying other addresses after every address refused it");
  require(failedAt.first == original.first,
          "a refused switch changed the account's address");
  std::printf("PASS N17 app password revoked by the server during a switch "
              "(%s): the account asks for sign-in, binding unchanged, no "
              "further address tried for 15 s\n",
              qPrintable(Run::reason(receipt)));
  r.settle("accounts.remove", {{"accountId", account}});
  // The server may still finish the account's last requests.
  k.calls.wait(
      [&] {
        try {
          removeOwnedUser(r.root, r.cleanup, user);
          return true;
        } catch (const std::exception &) {
          return false;
        }
      },
      "the owned user could not be removed");
}

/// newFolder: a folder added while the alternate cannot verify its root
/// (N19).
void newFolder(Run &r) {
  const auto &k = r.k;
  r.policy("auto");
  k.calls.wait(k.ready, "both addresses did not become healthy");
  r.map("one");
  k.refuseAlternate('/' + r.collection + "/two");
  const auto two = r.map("two");
  const auto original = r.bound();
  k.age(130000);
  r.steady(
      10000, [&] { return r.bound() == original; },
      "an address not verified for the new folder took the account");
  // Refused at once, or accepted and failed in verification.
  const auto reply = k.calls.call(
      "endpoints.requestSwitch",
      {{"accountId", k.account}, {"endpointId", k.target}});
  const auto refused = reply.contains("error") ? reply : k.calls.settled(reply);
  require((reply.contains("error") || refused.value("state") == "failed") &&
              r.bound().first == original.first,
          "a manual switch to the unverified address succeeded");
  k.refuseAlternate({});
  // The refused switch first returns the account to its address; then the
  // daemon checks the alternate again on its own schedule.
  k.calls.wait(
      [&] { return k.calls.stable(k.account) && k.fresh(k.target); },
      "the alternate did not verify the new folder once it could");
  require(r.settle("endpoints.requestSwitch",
                   {{"accountId", k.account}, {"endpointId", k.target}})
                  .value("state") == "succeeded",
          "the address did not take the account once it verified the new "
          "folder");
  k.calls.wait([&] { return r.state(two) == "upToDate"; },
               "the new folder did not reconcile on the alternate");
  std::printf("PASS N19 an address that could not verify a new folder's root "
              "took no automatic switch in 10 s with the dwell aged and "
              "refused a manual one (%s) until it verified it\n",
              qPrintable(Run::reason(refused)));
}

/// rootChange: the server's remote root is replaced by another one with the
/// same path (a restored backup), N20.
void rootChange(Run &r) {
  const auto &k = r.k;
  k.calls.wait(k.ready, "both addresses did not become healthy");
  const auto m = r.map("one");
  const auto kept = random(1024);
  r.write(m, "kept.txt", kept);
  r.uploaded(m, "kept.txt", kept, "the first file did not upload");
  require(r.admin("DELETE", "one") == 204 && r.admin("MKCOL", "one") == 201 &&
              r.admin("PUT", "one/restored.txt", random(512)) == 201,
          "the remote root could not be replaced");
  const auto original = r.bound();
  const auto local = random(700);
  r.write(m, "local.txt", local);
  k.calls.wait(
      [&] {
        // Stopped for a reason of its own, not a network loss.
        require(r.remote("one/local.txt").isEmpty(),
                "a local change was written into the replaced root");
        const auto s = r.state(m);
        const auto why =
            r.item("folders", m.folder).value("reasonCode").toString();
        return s != "syncing" && s != "upToDate" && !why.isEmpty() &&
               why != "NETWORK_UNAVAILABLE";
      },
      "the replaced remote root was synced as a normal change");
  const auto detected = r.state(m);
  const auto reason =
      r.item("folders", m.folder).value("reasonCode").toString();

  k.failPrimary(true);
  k.changed();
  r.steady(
      15000,
      [&] {
        return r.remote("one/local.txt").isEmpty() &&
               r.remote("one/kept.txt").isEmpty() &&
               QFile::exists(m.local + "/kept.txt");
      },
      "a write reached the replaced root or a local file was removed");
  k.failPrimary(false);
  std::printf("PASS N20 a replaced remote root stopped writes (%s %s); a "
              "network change did not resume them, binding %s\n",
              qPrintable(detected), qPrintable(reason),
              r.bound().first == original.first ? "kept" : "moved");
}
} // namespace

bool switchingScenario(const QString &scenario, QString *kind) {
  const auto name = scenario.section('=', 1);
  if (scenario.section('=', 0, 0) != "switching" || !cases.contains(name))
    return false;
  if (kind && (name == "auto-switch" || name == "auto-fallback" ||
               name == "new-folder"))
    *kind = "home";
  return true;
}

std::shared_ptr<SwitchingFixture>
switchingFixture(const QString &scenario, const QString &root,
                 const std::function<void(const QString &, quint16)> &route) {
  auto f = std::make_shared<SwitchingFixture>();
  f->root = root;
  f->name = scenario.section('=', 1);
  const auto &n = f->name;
  if (n != "tls-trust" && n != "tls-auth" && n != "tls-circuit" &&
      n != "redirect" && n != "subpath" && n != "late-epoch")
    return f;
  f->tls = QDir(root + "/tls");
  require(f->tls.mkpath("."), "throwaway certificate directory unavailable");
  f->ca("trusted", true);
  const auto alt = alternateHost();
  if (n == "tls-trust") {
    f->ca("untrusted", false);
    f->proxy(alt, f->leaf(alt), alt, route);
    f->proxy("rogue.syncd.test", f->leaf("rogue.syncd.test"), alt, route);
    f->proxy("expired.syncd.test",
             f->leaf("expired.syncd.test", "trusted", true), alt, route);
    f->proxy("wronghost.syncd.test", f->leaf("other.syncd.test"), alt, route);
    f->proxy("unknownca.syncd.test", f->leaf("unknownca.syncd.test", "untrusted"),
             alt, route);
  } else if (n == "tls-auth") {
    f->proxy(primaryHost(), f->leaf(primaryHost()), primaryHost(), route);
    f->proxy(alt, f->leaf(alt), alt, route);
  } else if (n == "tls-circuit" || n == "late-epoch") {
    f->proxy(primaryHost(), f->leaf(primaryHost()), primaryHost(), route);
    f->proxy(alt, f->leaf(alt), alt, route);
  } else if (n == "redirect") {
    f->proxy(alt, f->leaf(alt), alt, route);
    f->proxy("evil.syncd.test", f->leaf("evil.syncd.test"), alt, route);
  } else {
    f->proxy("sub.syncd.test", f->leaf("sub.syncd.test"), alt, route)
        .setStrip("/nc");
  }
  return f;
}

void switchingAcceptance(const QString &scenario, SwitchingFixture *fixture,
                         const SwitchingControls &controls) {
  Run r{controls, fixture->root,
        canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_CLEANUP_URL"))};
  r.reserve();
  const auto &n = fixture->name;
  Q_UNUSED(scenario);
  if (n == "auto-switch")
    autoSwitch(r);
  else if (n == "auto-fallback")
    autoFallback(r);
  else if (n == "tls-trust")
    tlsTrust(r, *fixture);
  else if (n == "tls-auth")
    tlsAuth(r, *fixture);
  else if (n == "late-epoch")
    lateEpoch(r, *fixture);
  else if (n == "tls-circuit")
    tlsCircuit(r, *fixture);
  else if (n == "redirect")
    redirect(r, *fixture);
  else if (n == "subpath")
    subpath(r);
  else if (n == "revoke-switch")
    revokeSwitch(r);
  else if (n == "new-folder")
    newFolder(r);
  else
    rootChange(r);
}
