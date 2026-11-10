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
