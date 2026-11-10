// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QString>

/// resumeContextError inspects a closed journal without changing it
/// (INV-08/09). Inputs: the caller owns the root lease and has drained its old
/// engine/writers; endpointChanged is false only for the identical approved
/// installation. Cross-origin admission allows only relative ETag-qualified
/// downloads, checksum-qualified single PUT uncertainty and checksum-qualified
/// chunking-v2 sessions; poll URLs and legacy v1 chunk indexes stay blocked.
/// Notes: the target is the same verified namespace, so upstream re-lists the
/// session there before sending and adopts an already committed MOVE by
/// checksum (patch 0016 regressions). Returns: empty when transfer state is
/// admissible to upstream recovery; otherwise a safe JOURNAL_ERROR or
/// RESUME_CONTEXT_UNSUPPORTED reason, never database content.
QString resumeContextError(const QString &journalPath,
                           bool endpointChanged = true);
