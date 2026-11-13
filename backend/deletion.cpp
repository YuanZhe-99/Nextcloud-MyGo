// SPDX-License-Identifier: GPL-3.0-only
#include "deletion.h"
#include <QSet>
#include <algorithm>

namespace {
/// removed expands one side's removals over the sorted baseline.
/// Returns: every removed path, directory descendants included.
QSet<QString> removed(const QStringList &baseline,
                      const QList<DeleteAction> &actions, bool local) {
  QSet<QString> side;
  for (const auto &action : actions) {
    if (action.local != local)
      continue;
    side.insert(action.path);
    if (action.directory) {
      const auto prefix = action.path + '/';
      for (auto it = std::lower_bound(baseline.begin(), baseline.end(), prefix);
           it != baseline.end() && it->startsWith(prefix); ++it)
        side.insert(*it);
    }
  }
  return side;
}
} // namespace

DeleteAssessment assessDeletion(QStringList baseline,
                                const QList<DeleteAction> &actions,
                                bool upstreamGuard) {
  std::sort(baseline.begin(), baseline.end());
  baseline.removeDuplicates();
  const auto local = removed(baseline, actions, true),
             remote = removed(baseline, actions, false);
  const auto threshold = [&](quint64 count) {
    return count >= 100 || (count >= 10 && !baseline.isEmpty() &&
                            count * 5 >= quint64(baseline.size()));
  };
  return {quint64(local.size()), quint64(remote.size()),
          quint64(baseline.size()),
          upstreamGuard || threshold(local.size()) || threshold(remote.size())};
}

bool emptiesMount(QStringList baseline, const QStringList &mounts,
                  const QList<DeleteAction> &actions) {
  std::sort(baseline.begin(), baseline.end());
  baseline.removeDuplicates();
  const auto local = removed(baseline, actions, true);
  for (const auto &mount : mounts) {
    if (local.contains(mount))
      return true;
    const auto prefix = mount + '/';
    auto it = std::lower_bound(baseline.begin(), baseline.end(), prefix);
    bool content = false, kept = false;
    for (; it != baseline.end() && it->startsWith(prefix); ++it) {
      content = true;
      kept = kept || !local.contains(*it);
    }
    if (content && !kept)
      return true;
  }
  return false;
}
