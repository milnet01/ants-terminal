// ANTS-2022 — apply_edits MCP tool: pure in-memory edit helper.
// Qt6::Core-only, lives in ants_core_lib so it is unit-testable without
// RemoteControl / MainWindow. The cmdApplyEdits wrapper (remotecontrol.cpp)
// owns ALL filesystem concerns — path validation, the file read, the 4 MiB
// size gate, and the atomic write — and calls applyToContent per edit on
// the in-memory working content. See docs/specs/ANTS-2022.md.

#pragma once

#include <QString>

namespace ApplyEdits {

// Outcome of one old→new edit against in-memory `contents`.
struct EditOutcome {
    bool    applied      = false;  // true iff the edit was applied
    QString newContents;           // valid iff applied
    int     replacements = 0;      // substitutions made (1, or N under replaceAll)
    QString skipReason;            // "" iff applied; else "not_found" | "ambiguous"
};

// Apply one old→new edit to `contents`. Without replaceAll, `oldStr` must
// occur exactly once (unique) or the edit is skipped ("not_found" for 0,
// "ambiguous" for > 1) — the native-Edit uniqueness rule. With replaceAll,
// every occurrence is replaced (and 0 occurrences still skips "not_found",
// never a silent no-op). A whole-content substring replace, so a trailing
// newline is preserved without any split/rejoin (INV-8).
EditOutcome applyToContent(const QString &contents,
                           const QString &oldStr,
                           const QString &newStr,
                           bool replaceAll);

}  // namespace ApplyEdits
