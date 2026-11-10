// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QSqlDatabase>
#include <QStringList>
#include <QUrl>

/// SwitchState stores control-thread switch intents separately from committed
/// bindings. Notes: mutations run inside the caller's Control transaction so
/// receipts/outbox and binding changes commit together. It never touches an
/// engine journal (INV-06/08/12).
class SwitchState {
public:
  /// Binding identifies the committed, explicitly approved origin and monotonic
  /// epoch.
  struct Binding {
    QString endpoint;
    QUrl server;
    qint64 epoch = 0;
  };
  /// SwitchState uses the Control connection on its owning thread only.
  explicit SwitchState(QSqlDatabase db) : db_(db) {}
  /// createSchema creates durable binding/intent tables inside the migration
  /// transaction.
  static bool createSchema(QSqlDatabase db);
  /// binding resolves only a ready account's canonical or explicitly approved
  /// endpoint.
  Binding binding(const QString &account) const;
  /// begin reserves a never-reused epoch and freezes the complete folder
  /// identity set. Inputs: expectedEpoch is the currently committed epoch;
  /// blocked intents may be replaced.
  qint64 begin(const QString &account, const QString &target,
               const QString &operation, qint64 expectedEpoch);
  /// commit publishes a prepared target after the caller proves drainage and
  /// fresh identity. Notes: rechecks durable consent/membership and retains the
  /// reconciliation gate.
  void commit(const QString &account, qint64 epoch);
  /// reconciled consumes one successful new-epoch folder result;
  /// stale/duplicate results do nothing.
  bool reconciled(const QString &account, qint64 epoch, const QString &folder);
  /// gated reports a pending or blocked intent, independently of every user
  /// pause.
  bool gated(const QString &account) const;
  /// target resolves current explicit credential consent for a nominated
  /// endpoint.
  QUrl target(const QString &account, const QString &endpoint) const;
  /// block preserves binding and reconciliation evidence after a failed live
  /// switch.
  void block(const QString &account, qint64 epoch);
  /// defer abandons only an uncommitted preparation after healthy-transfer
  /// timeout.
  void defer(const QString &account, qint64 epoch);
  /// interrupt blocks unfinished intents after restart and returns their
  /// operation IDs. Notes: committed binding/epoch and frozen recovery
  /// membership are preserved.
  QStringList interrupt();

private:
  QSqlDatabase db_;
};
