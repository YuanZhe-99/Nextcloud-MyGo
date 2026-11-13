// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QByteArray>
#include <QList>
#include <QStringList>

/// DeleteAction is a genuine local or remote removal; rename and selective
/// exclusion are not removals.
struct DeleteAction {
  QString path;
  bool local;
  bool directory;
};
/// DeleteAssessment summarizes both sides against the last valid journal
/// baseline (PLAN §11).
struct DeleteAssessment {
  quint64 local = 0, remote = 0, baseline = 0;
  bool blocked = false;
};
/// assessDeletion expands directory descendants and counts each baseline object
/// once per side.
DeleteAssessment assessDeletion(QStringList baseline,
                                const QList<DeleteAction> &actions,
                                bool upstreamGuard);
/// emptiesMount reports whether the local removals take away a whole external
/// storage: a mount point itself, or every baseline entry below one.
/// Inputs: mounts are the baseline directories the server marked as mount
/// points (permission `M`).
/// Notes: the server lists a storage it cannot reach as missing or as an
/// empty folder, just as if it had been emptied; any such removal, however
/// small, holds the cycle (S14: offline is not empty). Removing part of a
/// storage's content stays an ordinary deletion.
bool emptiesMount(QStringList baseline, const QStringList &mounts,
                  const QList<DeleteAction> &actions);
