// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QString>

/// resumeContextError inspects a closed journal without changing it
/// (INV-08/09). Inputs: the caller owns the root lease and has drained its old
/// engine/writers; endpointChanged is false only for the identical approved
/// installation. Cross-origin admission allows only relative ETag-qualified
/// downloads and checksum-qualified single PUT uncertainty; polls/chunks stay
/// blocked. Returns: empty when transfer state is admissible to upstream
/// recovery; otherwise a safe JOURNAL_ERROR or RESUME_CONTEXT_UNSUPPORTED
/// reason, never database content.
QString resumeContextError(const QString &journalPath,
                           bool endpointChanged = true);
