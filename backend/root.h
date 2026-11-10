// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QString>
#include <memory>

/// RootLease holds an ordinary local NTFS directory and its ancestors against
/// replacement and excludes equal/nested SyncD writers across processes and
/// profiles of one Windows user (INV-01). Notes: future official-client
/// registration is not detected.
class RootLease {
public:
  /// RootLease rejects reparse ancestors, nonlocal/non-NTFS paths and existing
  /// upstream journals before publishing an identity-based hierarchical lease.
  explicit RootLease(const QString &path);
  /// ~RootLease releases only its own process handles; it never removes a
  /// journal or user directory.
  ~RootLease();
  RootLease(const RootLease &) = delete;
  RootLease &operator=(const RootLease &) = delete;
  /// path returns the canonical, case-preserving local directory used by the
  /// engine.
  QString path() const { return path_; }
  /// identity returns volume/file ID, independent of drive letter and endpoint
  /// URL.
  QString identity() const { return identity_; }
  /// verify rejects removal, replacement or reparse conversion before another
  /// cycle starts.
  bool verify() const;
  /// validateTree rejects unreadable directories, reparse objects and cloud
  /// placeholders before discovery/propagation.
  bool validateTree() const;

private:
  /// Handles owns the process-scoped kernel objects; process death releases all
  /// ownership without persistent records or PID-based recovery.
  struct Handles;
  QString path_, identity_;
  std::unique_ptr<Handles> handles_;
};
