// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QDeadlineTimer>
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
  /// Notes: computed once per process (it asks the service manager and loads
  /// the plugin), so the virtualFiles capability, folders.add and conversion
  /// always agree within one SyncD run.
  static bool available();
  /// cloudFilesRunning reports whether the Cloud Files minifilter service
  /// (CldFlt) is running. Notes: Windows Server images may not run it; without
  /// it the Cloud Files API cannot say who owns a folder.
  static bool cloudFilesRunning();
  /// OnDemandRoot registers (or updates) and connects the sync root of a
  /// leased local folder.
  /// Inputs: localPath is the canonical root; remotePath the mapped collection;
  /// account and journal belong to the caller's binding and outlive this root.
  /// Side effects: writes the current user's sync root registration and the
  /// shared Explorer navigation pane entry it names (navigationClsid,
  /// rewritten each time; when a first registration throws, the entry stays
  /// only if another root names it); removes the folder's 0.1.1 entry.
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
  /// holds reports that an entry below the root is a placeholder of this sync
  /// root for exactly one server file, without requesting any content.
  /// Inputs: relativePath is root-relative with '/' separators; fileId as the
  /// journal stores it.
  /// Notes: accepts both identities upstream writes for new placeholders: the
  /// raw file ID (a cycle's) and, from a directory listing
  /// (cfApiFetchPlaceHolders), the file ID with its terminator followed by as
  /// many bytes again, whatever they are: upstream sizes that identity as
  /// UTF-16 but stores UTF-8. False for an ordinary or missing file and for
  /// an empty file ID.
  bool holds(const QString &relativePath, const QByteArray &fileId) const;
  /// Conversion counts what one convertExisting pass did with the journal's
  /// records.
  struct Conversion {
    /// converted became in-sync placeholders; present were placeholders
    /// already (an earlier, interrupted pass); failed could not be converted
    /// (locked, changed underneath); deferred were left when the budget ran
    /// out; skipped are missing, changed since the last sync, excluded or not
    /// a plain file or directory.
    quint64 converted = 0, present = 0, failed = 0, deferred = 0, skipped = 0;
  };
  /// convertExisting turns the content a full-file folder already has into
  /// hydrated, in-sync placeholders, so switching the folder to on-demand
  /// downloads nothing (v0.1.0 folders, schema 12 conversion).
  /// Inputs: journal is the folder's own journal, only read; budget bounds the
  /// control-thread time spent on files.
  /// Returns: what was done; directories are always converted, files only
  /// while the budget lasts.
  /// Side effects: Cloud Files conversion of local entries through upstream's
  /// plugin, each marked in sync with the journal's file ID as its identity;
  /// pin states keep their current value. Never writes the journal, never
  /// deletes, dehydrates or downloads.
  /// Notes: idempotent; a placeholder already there is left as it is. A file
  /// is converted only while its size and modification time still equal its
  /// journal record, so content that was never uploaded is never marked in
  /// sync (and could never be freed up); the first cycle uploads such a file
  /// and makes it a placeholder. A file left unconverted (failed, deferred) is
  /// made one by the first cycle's local discovery without a download: in a
  /// Cloud Files root upstream reads a plain file as metadata-missing and
  /// issues UPDATE_METADATA. Upstream has no such repair for directories,
  /// hence no budget for them. A sparse file is converted like any other: a
  /// placeholder reads as dehydrated only while Cloud Files marks it for
  /// recall on data access, never for being sparse (patch 0021). Throws
  /// JOURNAL_ERROR when the journal cannot be read.
  Conversion convertExisting(OCC::SyncJournalDb &journal,
                             const QDeadlineTimer &budget);
  /// ownershipError checks that a local folder is no sync root, and inside
  /// none, except this folder's own registration.
  /// Returns: empty when the folder may be used; FOLDER_OWNED when a sync root
  /// that is not this folder's covers it; UNSUPPORTED_FOLDER_MODE when Cloud
  /// Files cannot answer (ownershipReason).
  static QString ownershipError(const QString &localPath,
                                const Identity &identity);
  /// conversionError checks, without side effects or network, whether an
  /// existing full-file folder may become this folder's on-demand root.
  /// Returns: empty when it may; UNSUPPORTED_FOLDER_MODE when on-demand is not
  /// available here or upstream refuses the location (a drive root, not NTFS,
  /// a network drive); otherwise as ownershipError.
  static QString conversionError(const QString &localPath,
                                 const Identity &identity);
  /// navigationTarget is the folder the Explorer navigation pane entry of an
  /// on-demand root opens: the folder that holds it (setup puts every folder
  /// of an account below one base folder), or the root itself when it maps
  /// the whole account or its parent is a drive root.
  /// Returns: a cleaned path with native separators.
  static QString navigationTarget(const QString &localPath,
                                  const QString &remotePath);
  /// navigationClsid is the CLSID of that entry, shared by every root with
  /// the same target: a name-based UUID of the target path (case-insensitive),
  /// in registry form ("{…}").
  static QString navigationClsid(const QString &localPath,
                                 const QString &remotePath);
  /// navigationTitle is the entry's name: "Nextcloud MyGo", followed by
  /// " - <target folder name>" unless that name is "Nextcloud".
  static QString navigationTitle(const QString &target);
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
  /// are deleted; the journal and other local content stay. The navigation
  /// pane entry goes with the last registration that names it (checked also
  /// when no registration was left), and so does the folder's 0.1.1 entry.
  /// Returns: empty on success, otherwise a sanitized reason code.
  static QString unregister(const QString &localPath, const Identity &identity);

private:
  QSharedPointer<OCC::Vfs> vfs_;
  /// root_ is the registered root with '/' separators and a trailing '/'.
  QString root_;
  bool closed_ = false;
};
