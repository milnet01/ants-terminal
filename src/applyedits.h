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
                                   // | "range_mismatch" | "range_out_of_bounds"
    // ANTS-4418 — the near miss behind a `not_found`, when there is exactly
    // one. `not_found` alone is equally consistent with "the text is gone",
    // "you have the wrong file" and "you are one space out", and this verb is
    // the one where a miss is most often whitespace-only — so it was the verb
    // saying least while its siblings (read_region section-mode,
    // roadmap_log bullet locators) already return `candidates`.
    //
    // -1 / empty when there is no unique near miss, which is the normal case;
    // a caller reads the line number and retries with a line range.
    int     nearMissLine = -1;     // 1-based line in `contents`
    QString nearMissText;          // that line, verbatim, as the file has it
    QString nearMissKind;          // why it differed, e.g. "whitespace"
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

// ANTS-3711 — replace the inclusive 1-based line range [startLine, endLine]
// with `newStr`, as an alternative to naming the text via `oldStr`. The shape
// where apply_edits saves the most tokens is the one it could not express: a
// 76-line deletion meant re-emitting all 76 lines, which is both the bytes the
// caller already read and exactly where one invisible whitespace or em-dash
// mismatch turns into a `not_found`. `read_region` already hands back 1-based
// line numbers, so a caller reaches this point holding the coordinates.
//
// `expectFirst`/`expectLast` are the verbatim text of the range's first and
// last lines and are NOT optional — see the wrapper. A line range carries no
// intrinsic uniqueness guard the way `oldStr` does, so without them a number
// gone stale (from an earlier edit in the same call, or another writer) would
// silently replace the wrong lines. That is precisely the Bash line-splice
// failure this verb exists to avoid, so a range edit refuses `range_mismatch`
// instead. Out-of-file coordinates skip `range_out_of_bounds`.
//
// An empty `newStr` DELETES the range rather than leaving a blank line. Split
// and rejoin are both on '\n', so a trailing newline round-trips (INV-8).
EditOutcome applyRangeToContent(const QString &contents,
                                int startLine, int endLine,
                                const QString &expectFirst,
                                const QString &expectLast,
                                const QString &newStr);

}  // namespace ApplyEdits
