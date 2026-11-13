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
/// main verifies per-side thresholds, descendant expansion, deduplication, a
/// vanished external storage (S14) and ambiguous paths (INV-09).
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
    // S14: a vanished external storage (listed as missing or empty) holds the
    // cycle however few entries it had; part of one, or a plain folder, does
    // not.
    const QStringList mounted{"ext", "ext/a", "ext/sub", "ext/sub/b", "keep",
                              "plain", "plain/c"};
    const struct {
      const char *name;
      QList<DeleteAction> actions;
      bool held;
    } mountCases[] = {
        {"mount point removed", {{"ext", true, true}}, true},
        {"mount listed empty",
         {{"ext/a", true, false}, {"ext/sub", true, true}},
         true},
        {"part of a mount", {{"ext/a", true, false}}, false},
        {"removed on the server side", {{"ext", false, true}}, false},
        {"plain folder emptied", {{"plain/c", true, false}}, false},
        {"nothing removed", {}, false},
    };
    for (const auto &test : mountCases)
      require(emptiesMount(mounted, {"ext"}, test.actions) == test.held,
              test.name);
    require(!emptiesMount({"ext"}, {"ext"}, {{"keep", true, false}}),
            "an empty mount with nothing removed was held");
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
    std::puts("PASS deletion thresholds, descendants, deduplication, vanished "
              "external storage and namespace path boundaries");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
