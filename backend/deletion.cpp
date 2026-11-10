// SPDX-License-Identifier: GPL-3.0-only
#include "deletion.h"
#include <QSet>
#include <algorithm>

DeleteAssessment assessDeletion(QStringList baseline,
                                const QList<DeleteAction> &actions,
                                bool upstreamGuard) {
  std::sort(baseline.begin(), baseline.end());
  baseline.removeDuplicates();
  QSet<QString> local, remote;
  for (const auto &action : actions) {
    auto &side = action.local ? local : remote;
    side.insert(action.path);
    if (action.directory) {
      const auto prefix = action.path + '/';
      for (auto it = std::lower_bound(baseline.begin(), baseline.end(), prefix);
           it != baseline.end() && it->startsWith(prefix); ++it)
        side.insert(*it);
    }
  }
  const auto threshold = [&](quint64 count) {
    return count >= 100 || (count >= 10 && !baseline.isEmpty() &&
                            count * 5 >= quint64(baseline.size()));
  };
  return {quint64(local.size()), quint64(remote.size()),
          quint64(baseline.size()),
          upstreamGuard || threshold(local.size()) || threshold(remote.size())};
}
