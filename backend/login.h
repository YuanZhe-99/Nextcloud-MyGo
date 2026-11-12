#pragma once
#include <QDateTime>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <optional>

/// canonicalServer validates and normalizes an explicitly selected HTTPS
/// installation URL (INV-03).
QUrl canonicalServer(const QString &input);
/// withinServer checks an exact origin and installation-path boundary before
/// attaching any secret.
bool withinServer(const QUrl &base, const QUrl &target);
/// writeCredential stores one application password in the current user's native
/// credential vault.
void writeCredential(const QString &reference, const QString &user,
                     const QString &password);
/// readCredential retrieves only the named account's secret, with no plaintext
/// fallback.
QString readCredential(const QString &reference);
/// findCredential distinguishes an absent owned vault slot from an inaccessible
/// credential store.
std::optional<QString> findCredential(const QString &reference);
/// removeCredential deletes an owned reference, treating an already absent item
/// as success.
void removeCredential(const QString &reference);

/// LoginFlow owns one bounded, cancelable Login Flow v2 and read-only namespace
/// verification. Notes: all methods and callbacks execute on the Qt control
/// thread; tokens never cross IPC. A session is valid for 20 monotonic minutes
/// and a server delay never extends it; only retiring an acquired password
/// ("discarding") may outlive it, because cleanup is not acceptance.
class LoginFlow : public QObject {
  /// LoginTestAccess substitutes the transport and monotonic clock in the
  /// isolated contract executable; production has no such switch.
  friend struct LoginTestAccess;

public:
  /// LoginFlow binds all requests to the user-approved installation and a
  /// private network context.
  LoginFlow(const QUrl &base, const QString &session, const QString &account);
  /// ~LoginFlow disconnects in-flight replies before their manager and callback
  /// state are destroyed.
  ~LoginFlow() override;
  /// start begins the unauthenticated Login Flow v2 request.
  void start();
  /// verifyCredentials persists an acquired password before verifying the
  /// authenticated namespace. Inputs: only credentials returned by Login Flow
  /// v2, or explicit in-process acceptance fixtures.
  void verifyCredentials(const QString &user, const QString &password);
  /// cancel invalidates the session and aborts its in-flight request without
  /// accepting late replies.
  void cancel();
  /// accept completes a root-verified flow only after its owner has installed
  /// the credential safely.
  /// Notes: installed transfers secret ownership before publication; a
  /// publication failure cannot revoke it.
  void accept(bool installed = false);
  /// reject discards only the newly acquired credential and never an existing
  /// account password.
  void reject(const QString &reason);
  /// status returns the public session state, never its token or credential.
  QString status() const { return state_; }
  /// reason returns a stable failure code suitable for localized UI handling.
  QString reason() const { return reason_; }
  QString id, accountId, loginUrl, loginName, uid, displayName, rootId;
  QDateTime expires;
  /// saveCredential must durably reserve the account before writing its secret
  /// and return false on failure.
  std::function<bool(const QString &, const QString &)> saveCredential;
  /// changed commits externally visible transitions; false prevents a
  /// successful login being reported.
  std::function<bool(LoginFlow &)> changed;
  /// verified defers final acceptance to account-wide identity and runtime
  /// checks when supplied.
  std::function<void(LoginFlow &)> verified;
  /// throttled durably records a confirmed HTTP 429 before any later request.
  /// Inputs: MetadataError delay semantics (0 unspecified, positive bounded,
  /// -1 no automatic retry). Returns: false when nothing was committed; the
  /// flow then sends no further request.
  std::function<bool(qint64)> throttled;
  /// held durably records (true) or clears (false) that the acquired
  /// password's DELETE is only delayed by the server and none is in flight, so
  /// a restarted daemon may send it once. Returns: false when nothing was
  /// committed. Notes: never called for a borrowed credential; a failed clear
  /// sends no DELETE, because a later boot could then send a second one.
  std::function<bool(bool)> held;

private:
  /// request sends a bounded, nonredirecting request within the approved server
  /// boundary. Notes: the single admission point of the server-delay gate;
  /// 403/423/429 are classified here and never reach done.
  void request(const QUrl &url, const QByteArray &method,
               const QByteArray &body, const QByteArray &authorization,
               std::function<void(int, const QByteArray &)> done);
  /// constrained applies a confirmed 403/423/429. Side effects: closes the
  /// gate before persisting a throttle; keeps a still-owned password and its
  /// reservation while a bounded delay allows a later revocation.
  void constrained(const QString &reason, qint64 retryAfterMs);
  /// tick enforces monotonic expiry and resumes polling or revocation only
  /// after the gate opens.
  void tick();
  /// revoke asks the server once to retire the acquired password. Notes: an
  /// unanswered DELETE is uncertain and is never replayed.
  void revoke();
  /// hold records a server-delayed revocation once, if the owner keeps one.
  void hold() { heldRecorded_ = heldRecorded_ || (held && held(true)); }
  /// now reads the injected or owned monotonic millisecond clock.
  qint64 now() const { return clock_ ? clock_() : elapsed_.elapsed(); }
  /// gated reports a server delay that still forbids any request.
  bool gated() const {
    return gateUntil_ < 0 || (gateUntil_ > 0 && now() < gateUntil_);
  }
  /// poll obtains an application password without treating pending HTTP 404 as
  /// a failed login.
  void poll();
  /// verifyRoot requires a successful DAV collection identity for the
  /// authenticated UID.
  void verifyRoot();
  /// publish commits a transition and fails closed if persistent state cannot
  /// be updated.
  void publish();
  /// fail invalidates pending work and publishes a sanitized reason.
  void fail(const QString &reason);
  QUrl base_, pollUrl_;
  QString state_ = "starting", reason_;
  QString discardReason_;
  bool ownedPassword_ = false;
  bool heldRecorded_ = false;
  bool rootVerified_ = false;
  QByteArray token_, authorization_;
  QNetworkAccessManager network_;
  QPointer<QNetworkReply> reply_;
  QTimer timer_;
  QElapsedTimer elapsed_;
  std::function<qint64()> clock_;
  std::function<QNetworkReply *(const QNetworkRequest &, const QByteArray &,
                                const QByteArray &)>
      send_;
  // Monotonic milliseconds. gateUntil_: 0 open, -1 no automatic retry.
  qint64 deadline_ = 0, gateUntil_ = 0;
};
