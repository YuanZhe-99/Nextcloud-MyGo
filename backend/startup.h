// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QSqlDatabase>
#include <QString>

/// Startup owns the profile lock and records actual prior unclean exits before
/// engine admission (PLAN §14.1–14.2/R02; INV-01/10/14).
/// Notes: the caller supplies cross-process monotonic milliseconds, never UTC.
/// Destruction releases the lock but deliberately does not declare a clean
/// exit.
class Startup {
public:
  /// Decision separates diagnostic admission from bounded crash backoff.
  struct Decision {
    bool diagnostic = false;
    bool priorUnclean = false;
    bool automaticRestartSuppressed = false;
    int crashes = 0;
    qint64 delayMs = 0;
  };
  /// Startup acquires the existing daemon.lock before reading/writing the
  /// ledger. Side effects: atomically marks this run unclean until finishClean
  /// succeeds. Throws if another process owns the profile; damaged ledgers are
  /// preserved.
  Startup(const QString &directory, qint64 monotonicMs);
  /// decision returns fail-closed startup admission evidence, not a capability.
  const Decision &decision() const { return decision_; }
  /// engineAllowed permits admission only after the monotonic backoff expires.
  /// Notes: diagnostic mode cannot age into automatic engine admission.
  bool engineAllowed(qint64 monotonicMs) const;
  /// recordStopIntent persists the user's stopped/running intent before ack.
  /// Notes: false requires an explicit user start/resume, never crash recovery.
  /// A stopped intent survives clean and unclean exits until explicitly
  /// cleared.
  bool recordStopIntent(bool stopped = true);
  /// finishClean records shutdown only after all engines, replies and databases
  /// have closed; failure leaves the next startup conservative.
  bool finishClean();
  /// diagnosticStop appends a DB-less shutdown receipt after durable stop
  /// intent. Returns: a truthful terminal receipt; acceptedNow is false for
  /// prior-run retries. Notes: one atomic file retains at most 32 receipts
  /// without eviction.
  QJsonObject diagnosticStop(const QJsonObject &params, bool &acceptedNow);
  /// diagnosticOperation reads retained history without replaying shutdown.
  QJsonObject diagnosticOperation(const QString &operation) const;
  /// importDiagnosticStop transactionally imports every retained receipt before
  /// deleting the file; repeated import is idempotent and never replays
  /// shutdown.
  void importDiagnosticStop(QSqlDatabase db);

private:
  /// save atomically replaces a valid ledger without a direct-write fallback.
  bool save(bool running);
  /// stopRecords validates size, depth, types and unique IDs without repairing
  /// data.
  QJsonArray stopRecords() const;
  QLockFile lock_;
  QString path_;
  QString stopPath_, run_;
  QJsonArray crashes_;
  Decision decision_;
  qint64 started_ = 0;
  bool writable_ = false, stopped_ = false, finished_ = false;
};

/// backupControlBeforeMigration verifies an existing Control DB read-only and
/// publishes a consistent, independently checked SQLite backup before
/// migration. Returns: the backup path, or empty for a new/current-schema
/// database. Side effects: creates only a new backup; never repairs/deletes
/// source or journals. Notes: the profile lock must already be held, with no
/// engine or other DB writer. Throws a sanitized error on corruption, newer
/// schemas or backup failure.
QString backupControlBeforeMigration(const QString &path, int targetSchema);
