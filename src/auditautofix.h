// ANTS-1719 — native auto-fix for mechanically-safe, behaviour-neutral
// audit findings. "Fix the fixable; surface the rest." Pure (Qt6::Core)
// so the AuditDialog opt-in action and the feature test share one
// implementation. Every repair is single-line, behaviour-neutral, and
// gated on BOTH a finding signal and the exact source-line shape — a
// finding that is not provably one of the safe-list cases is never
// touched (the caller surfaces it instead).

#pragma once

#include "auditengine.h"  // Finding

#include <QString>
#include <optional>

namespace ants {
namespace autofix {

// One planned repair on a single source line. `removeLine` deletes the
// line; otherwise `fixed` replaces it. `original` is the line as it read
// when planned — applyRepair re-verifies it before writing.
struct Repair {
    QString rule;             // safe-list rule id ("autofix.*")
    QString file;             // absolute path (as resolved by the caller)
    int     line = 0;         // 1-based
    QString original;         // current line text (no EOL)
    QString fixed;            // replacement text (no EOL); empty on removal
    bool    removeLine = false;
};

// Exhaustive behaviour-neutral safe-list. Returns a Repair only when `f`
// is provably one of:
//   - autofix.unused_include — a valid Repair rule, but planRepair never
//     emits it: cppcheck `unusedInclude` is too false-positive-prone on
//     Qt's transitive / moc-generated includes to delete safely
//     (ANTS-2006), so it is declined here and left for manual review.
//   - autofix.dead_q_unused  — a finding naming Q_UNUSED on a standalone
//     `Q_UNUSED(...);` line -> remove the line.
//   - autofix.stale_todo     — a `// ... TODO/FIXME ... remove after
//     <X.Y.Z>` comment with X.Y.Z <= currentVersion -> remove the line.
//   - autofix.comment_space  — a finding mentioning "comment" on a
//     standalone `//word` line -> reformat to `// word`.
// Anything else (including future-version TODOs, trailing code comments,
// or a line whose shape doesn't match) returns nullopt. `absPath` must be
// the caller-resolved absolute file path; `fileContents` is its full text.
std::optional<Repair> planRepair(const Finding &f,
                                 const QString &absPath,
                                 const QString &fileContents,
                                 const QString &currentVersion);

// Apply a planned repair atomically (QSaveFile). Re-reads the file and
// refuses (returns false) if the target line no longer equals
// `r.original` — a plan stale against on-disk content is never applied.
bool applyRepair(const Repair &r);

// Append one repair record to <cacheDir>/autofix-YYYY-MM-DD.jsonl
// ({file,line,rule,original,fixed,timestamp}). 0600 + a header comment on
// first write; append-only thereafter. Returns true on a committed write.
bool logRepair(const QString &cacheDir, const Repair &r);

// "a.b.c" <= "x.y.z" by numeric component. Unparseable input -> false
// (conservative: don't treat a malformed marker as removable).
bool versionLE(const QString &a, const QString &b);

}  // namespace autofix
}  // namespace ants
