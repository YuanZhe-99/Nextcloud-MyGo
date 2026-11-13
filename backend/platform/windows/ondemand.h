// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QSharedPointer>
#include <QString>

namespace OCC {
class Account;
class SyncJournalDb;
class Vfs;
using AccountPtr = QSharedPointer<Account>;
} // namespace OCC

/// OnDemandRoot connects one on-demand folder to the Windows Cloud Files API
/// through upstream's CFAPI plugin for one immutable binding epoch (PLAN §2.3).
/// Notes: lives on the control thread; Cloud Files callbacks reach it only
/// through upstream's queued calls. The sync root is registered as
/// "Nextcloud-MyGo!<user SID>!<account ID>!<folder ID>", never under the
/// official client's provider name and never naming the server address, so an
/// endpoint switch keeps the registration (INV-02). A hydration is a request on
/// this epoch's account: the owner closes the root, waits until it is no longer
/// busy and only then destroys it before rebinding (INV-05).
class OnDemandRoot {
public:
  /// Identity names the registration; all parts are stable Control IDs.
  struct Identity {
    QString account, folder, name;
  };
  /// provider is the Cloud Files provider name of this product.
  static constexpr auto provider = "Nextcloud-MyGo";
  /// available reports whether on-demand folders can work here: upstream's
  /// CFAPI plugin loads in this process and the Cloud Files filter runs.
  static bool available();
  /// cloudFilesRunning reports whether the Cloud Files minifilter service
  /// (CldFlt) is running. Notes: Windows Server images may not run it; without
  /// it the Cloud Files API cannot say who owns a folder.
  static bool cloudFilesRunning();
  /// OnDemandRoot registers (or updates) and connects the sync root of a
  /// leased local folder.
  /// Inputs: localPath is the canonical root; remotePath the mapped collection;
  /// account and journal belong to the caller's binding and outlive this root.
  /// Side effects: writes the current user's sync root registration.
  /// Notes: throws FOLDER_OWNED when the folder belongs to another provider or
  /// another folder, UNSUPPORTED_FOLDER_MODE when Cloud Files is unavailable.
  OnDemandRoot(const Identity &identity, const QString &localPath,
               const QString &remotePath, const OCC::AccountPtr &account,
               OCC::SyncJournalDb *journal);
  /// ~OnDemandRoot disconnects the sync root. Notes: the owner must have closed
  /// it and waited until it is not busy; the registration stays.
  ~OnDemandRoot();
  OnDemandRoot(const OnDemandRoot &) = delete;
  OnDemandRoot &operator=(const OnDemandRoot &) = delete;
  /// vfs is the connected upstream plugin for this folder's sync options.
  QSharedPointer<OCC::Vfs> vfs() const { return vfs_; }
  /// close makes hydration and placeholder requests that arrive from now on
  /// fail; it cannot be undone, a new root replaces a closed one.
  void close();
  /// closed reports whether close was called.
  bool closed() const { return closed_; }
  /// busy reports a running hydration or Cloud Files request of this epoch.
  bool busy() const;
  /// ownershipError checks that a local folder is no sync root, and inside
  /// none, except this folder's own registration.
  /// Returns: empty when the folder may be used; FOLDER_OWNED when a sync root
  /// that is not this folder's covers it; UNSUPPORTED_FOLDER_MODE when Cloud
  /// Files cannot answer (ownershipReason).
  static QString ownershipError(const QString &localPath,
                                const Identity &identity);
  /// ownershipReason classifies a CfGetSyncRootInfoByPath answer.
  /// Inputs: queried is its HRESULT; running is cloudFilesRunning(); provider
  /// and ownRecord describe the root found when it succeeded.
  /// Returns: as ownershipError.
  /// Notes: "not under a sync root" is the only free answer. A failure that
  /// means Cloud Files is unavailable (a known code, or any failure while the
  /// filter is not running) is not evidence of an owner and refuses as
  /// unsupported; every other failure still refuses as owned (fail closed).
  static QString ownershipReason(long queried, bool running,
                                 const QString &provider, bool ownRecord);
  /// unregister ends this folder's registration after its root was destroyed.
  /// Side effects: files whose content is on disk become ordinary files again;
  /// online-only placeholders, which nothing could open without the provider,
  /// are deleted; the journal and other local content stay.
  /// Returns: empty on success, otherwise a sanitized reason code.
  static QString unregister(const QString &localPath, const Identity &identity);

private:
  QSharedPointer<OCC::Vfs> vfs_;
  bool closed_ = false;
};
