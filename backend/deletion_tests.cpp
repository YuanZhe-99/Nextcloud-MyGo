// SPDX-License-Identifier: GPL-3.0-only
#include "deletion.h"
#include "namespace.h"
#include <cstdio>
#include <stdexcept>

namespace {
/// require keeps deletion thresholds checked in release builds.
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace
/// main verifies per-side thresholds, descendant expansion, deduplication and
/// ambiguous paths (INV-09).
int main() {
  try {
    QStringList baseline;
    for (int i = 0; i < 50; ++i)
      baseline.append("dir/" + QString::number(i));
    QList<DeleteAction> actions;
    for (int i = 0; i < 9; ++i)
      actions.append({baseline[i], false, false});
    require(!assessDeletion(baseline, actions, false).blocked,
            "nine removals should not trip ratio");
    actions.append({baseline[9], false, false});
    require(assessDeletion(baseline, actions, false).blocked,
            "20 percent boundary missed");
    auto directory = assessDeletion(
        baseline, {{"dir", true, true}, {"dir/1", true, false}}, false);
    require(directory.local == 51 && directory.remote == 0 && directory.blocked,
            "descendants or deduplication incorrect");
    actions.clear();
    for (int i = 0; i < 100; ++i)
      actions.append({QString::number(i), true, false});
    require(assessDeletion({}, actions, false).blocked,
            "absolute threshold depends on denominator");
    require(assessDeletion({}, {}, true).blocked, "upstream guard ignored");
    require(!assessDeletion({}, {}, false).blocked,
            "empty initial baseline divided by zero");
    for (const auto &path :
         {"relative", "/../escape", "/a/./b", "/a//b", "/a\\b"}) {
      bool rejected = false;
      try {
        remotePath(path);
      } catch (...) {
        rejected = true;
      }
      require(rejected, "ambiguous remote path accepted");
    }
    std::puts("PASS deletion thresholds, descendants, deduplication and "
              "namespace path boundaries");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
