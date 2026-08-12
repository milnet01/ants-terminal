// ANTS-3662 — spec_lint: the greppable half of the spec-format contract.
// Qt6::Core only, in ants_core_lib beside markdownscan.h / docfinding.h /
// specparse.h, so it is unit-testable without RemoteControl / MainWindow.
//
// /cold-eyes § 1e runs these checks by hand on every single review pass:
// required sections present, every INV-N carrying a test surface, gaps in the
// doc's own id sequence, a loop-log row with no outcome, and a test clause that
// is a command stating nothing it should return. All five are deterministic,
// and § 1e's own notes record what hand-rolling costs — the "what-checks-this
// table integrity" class was caught in three consecutive loops before it became
// a check, and a stated count of un-caught rules "has been wrong every time a
// human wrote it".
//
// TWO CHECKS SHIP GATED, and that is deliberate rather than unfinished:
//
//   * `missing_section` runs only when the project's format standard carries a
//     machine-readable `<!-- required-sections -->` block. No standard in this
//     project has one yet, so the shipping default is skipped
//     (`sectionsChecked == false`). A check against a guessed list manufactures
//     findings against every conforming spec in the corpus, which is worse than
//     not running — spec § 2.1.
//   * § 1e's loop-log BALANCE check (enumerated findings vs the row's severity
//     counts) is not implemented at all. It needs a document carrying both a
//     counts row and a per-loop enumeration; measured over `docs/` on
//     2026-07-28, 10 documents have the counts table and 22 have the
//     enumerating headings, and the two sets are DISJOINT. ANTS-3682 carries
//     the prerequisite — spec § 5.
//
// ANTS-4127 adds a sixth check to the same walk: a `*Test:*` clause naming a
// `tests/features/<name>` directory is RESOLVED against disk instead of read,
// yielding `test_surface_absent` / `test_surface_unresolved` / `_unwired`. It
// executes nothing — resolving a citation runs nothing — and it opens nothing
// either: both filesystem facts are injected (see Options below).
//
// See docs/specs/ANTS-3662.md and docs/specs/ANTS-4127-test-surface-resolution.md.

#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "docfinding.h"

namespace SpecLint {

struct Options {
    // The required-section list, read once per run from the format standard's
    // `<!-- required-sections -->` block. This engine is Qt6::Core-only and
    // never opens the document under review (its text is handed in, per the
    // family convention in ANTS-3664 § 2.3), so it cannot find or read that
    // file either: the verb layer supplies the list, the same injection shape
    // as `doc_symbols`' `excludedNames`. Empty means the block was absent and
    // the check is SKIPPED — not that nothing is required.
    QStringList requiredSections;

    // ANTS-4110 — the INV numbers OTHER specs in the same corpus own. A project
    // may number invariants once across the corpus rather than restarting per
    // document, and the shared spec-format standard permits it: ids are stable
    // handles cited from CHANGELOG and sibling specs, and nothing says they
    // restart. Without this set every id living in a neighbouring spec reads as
    // a gap — 26 findings on one three-spec corpus, all false, in a bucket
    // /cold-eyes records as pre-verified. Injected the same way
    // `requiredSections` is, and for the same reason: this engine never opens a
    // second file. Empty means per-document numbering is assumed, which is the
    // pre-ANTS-4110 behaviour exactly.
    QSet<int> siblingInvNumbers;

    // ANTS-4127 — the two filesystem facts behind test-surface resolution: the
    // `tests/features/<name>` directories that EXIST, and the subset of those
    // holding a `test_*.cpp` named in `CMakeLists.txt` (so it compiles into a
    // bundle and actually runs). Both hold the BARE `<name>`, never the
    // `tests/features/` prefix; the prefix is re-attached onto the finding's
    // `surface` field, which is the only place the full path means anything.
    //
    // Injected for the reason stated twice above: this engine is Qt6::Core-only
    // and opens nothing, so it can neither stat a directory nor read
    // `CMakeLists.txt`. The verb layer gathers both ONCE per run and shares them
    // across every document in the walk (spec § 2.8).
    //
    // EACH SET GATES ITS OWN CHECK, AND EMPTY MEANS SKIP — the contract
    // `requiredSections` already uses, for the same reason: a check against an
    // empty set condemns everything it reads. An empty `existingTestDirs` skips
    // both checks; a non-empty `existingTestDirs` with an empty `wiredTestDirs`
    // runs the existence check and skips the wiring one, so one failed read of
    // `CMakeLists.txt` cannot present as a corpus-wide defect (spec § 2.6).
    //
    // `wiredTestDirs` is a SUBSET of `existingTestDirs` by construction — the
    // gatherer filters the directories it just scanned — so a name in the second
    // and not the first is not a state it can produce. This engine does not
    // validate that and defines no behaviour for its breach.
    QSet<QString> existingTestDirs;
    QSet<QString> wiredTestDirs;

    // A directory-scoped checker with no cap is one bad corpus away from an
    // unbounded response. Per call; the verb decrements a run-wide budget.
    int maxFindings = 500;
};

struct Result {
    QList<DocFinding::Finding> findings;

    // Distinguishes "every required section is there" from "nobody checked".
    // Those are the two states this verb ships between, so the flag is always
    // reported and never inferred from an empty findings list.
    bool sectionsChecked = false;

    // ANTS-4110 — id gaps NOT reported because a sibling spec owns the number.
    // Reported rather than left to be inferred, for the same reason
    // `sectionsChecked` is: a check that quietly declines to fire is what the
    // defect this field exists for was made of.
    int idGapsSuppressed = 0;

    // ANTS-4127 — DISTINCT test surfaces this document cited that were found on
    // disk. Distinct is scoped PER DOCUMENT: a directory two invariants of one
    // spec both name counts once, and a walk's total is the sum over documents,
    // so the same directory cited by three specs contributes three. Without that
    // scope two conforming engines report different totals for one corpus.
    //
    // It is the denominator without which two zero-finding runs cannot be told
    // apart — but zero is also what a skipped run reports, which is why the flag
    // below exists and is not inferred from this.
    int  surfacesResolved = 0;

    // ANTS-4127 — false EXACTLY when `existingTestDirs` was empty, the one state
    // in which neither check ran. True once the existence check ran, INCLUDING
    // the case where the wiring check skipped for an empty `wiredTestDirs`: it
    // reports whether resolution happened at all, not whether every check fired.
    // Always reported and never inferred from a zero count, for the same reason
    // `sectionsChecked` is never inferred from an empty findings list.
    bool surfacesChecked = false;

    int  lineCount = 0;   // reported, NEVER emitted as a finding (INV-6)
    bool truncated = false;
};

// `text` is the document's content; `relPath` is only carried onto findings.
Result check(const QString &text, const QString &relPath,
             const Options &opts = {});

// Extract a format standard's `<!-- required-sections -->` block into the
// heading list Options::requiredSections wants — the fenced block immediately
// following the marker, one literal `## N. Name` heading per line. Empty when
// the marker or its fence is absent, which is the skip signal.
//
// Here rather than in the verb layer because it is the other half of this
// engine's contract: the block's format and the list's meaning are one thing,
// and splitting them across a library boundary is how the two drift.
QStringList parseRequiredSections(const QString &standardText);

// ANTS-4110 — the INV numbers a document ANCHORS (bullet or table form), for the
// verb layer's corpus scan. Deliberately cheaper and looser than `check`'s own
// scan: it does not locate the Invariants section, because its only consumer is
// an ownership question ("does any other spec declare this number?") and an
// example id quoted in prose can only make two documents look like they share a
// number — which turns the sibling suppression OFF. Failing toward the old
// behaviour is the right direction for a check that suppresses findings.
QSet<int> invariantNumbers(const QString &text);

}  // namespace SpecLint
