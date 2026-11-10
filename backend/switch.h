#pragma once
#include <QSet>
#include <QString>

/// SwitchCoordinator requires account-wide, epoch-bound quiescence before
/// publishing a new endpoint. Notes: this guard is control-thread-only; it does
/// not manufacture engine quiescence (INV-05/06).
class SwitchCoordinator {
public:
  /// Target binds an endpoint to explicit origin permission and verified
  /// namespace identity.
  struct Target {
    QString id;
    bool approved = false;
    bool identityVerified = false;
    bool tlsValid = false;
    bool blocked = true;
  };
  /// Proof records all resources whose lifetime must end for one folder's
  /// immutable epoch.
  struct Proof {
    quint64 epoch = 0;
    bool engineStopped = false;
    bool propagatorDestroyed = false;
    bool discoveryDestroyed = false;
    bool requestsDrained = false;
    bool journalFlushed = false;
  };
  /// begin freezes account membership and refuses untrusted or overlapping
  /// switch requests.
  QString begin(const Target &target, quint64 expectedEpoch,
                const QSet<QString> &folders);
  /// observe accepts only complete proofs from the old epoch and known folder
  /// set.
  bool observe(const QString &folder, const Proof &proof);
  /// commit rechecks trust and advances the epoch only after all frozen folders
  /// have proven quiet.
  QString commit(const Target &currentTarget);
  /// reconciled records successful rediscovery for one committed-epoch folder;
  /// stale, duplicate and unknown completions cannot clear recovery work
  /// (INV-08).
  bool reconciled(const QString &folder, quint64 epoch);
  /// recovering reports whether committed folders still need reconciliation.
  bool recovering() const { return !recovering_.isEmpty(); }
  /// cancel abandons a pending switch without changing endpoint, epoch or user
  /// pause.
  void cancel();
  /// epoch returns the currently published account epoch.
  quint64 epoch() const { return epoch_; }
  /// active returns the published endpoint, never a partly activated target.
  QString active() const { return active_; }

private:
  QString active_;
  QString target_;
  QSet<QString> waiting_;
  QSet<QString> members_, recovering_;
  quint64 epoch_ = 0;
};
