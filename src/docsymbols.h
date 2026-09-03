// ANTS-3661 — doc_symbols: resolve the identifiers a document asserts
// something about. Qt6::Core only, in ants_core_lib beside markdownscan.h /
// docfinding.h, so it is unit-testable without RemoteControl / MainWindow.
//
// /cold-eyes § 1e calls this "the highest-yield check in the pre-pass": the
// corpus's two worst review findings were a function documented as returning
// something it did not, and a test seam a later reviewer's notes call
// "fictional". Both were written from recall; both die at a symbol lookup.
// Harvesting the question from prose is the new part — answering it is a call
// to SymbolQuery::findDefinition, the same ladder find_definition uses.
//
// REPORT-ONLY, and that is the whole design constraint (spec § 2.3). This
// engine never decides that an unresolved name is a defect: rot and a
// legitimate forward reference (a doc naming a thing it is about to create)
// look identical from here, and the judgement stays with the reader. What the
// lookup buys is the short list — the expensive half a reviewer cannot do
// cheaply. Hence no severity, no defect vocabulary, and never autoFixable.
//
// See docs/specs/ANTS-3661.md.

#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QVector>

#include "docfinding.h"
#include "symbolquery.h"

namespace DocSymbols {

// Three states, not two. `NotChecked` is a needle the run never looked up —
// elided by Options::maxSymbolsPerRun or by the resolve deadline. Reporting one
// as `Unresolved` would assert that a symbol does not exist on the strength of
// never having asked, which is the exact false claim this engine exists to
// avoid and is worse than a missing row because it is indistinguishable from a
// real one. A tri-state rather than a bool plus a flag: a two-boolean encoding
// admits a meaningless fourth combination, and the first consumer to read only
// the bool would silently reintroduce the defect (spec INV-7).
enum class Resolution { Resolved, Unresolved, NotChecked };

// Wire strings (snake_case) — the CamelCase enum never reaches the wire.
QString resolutionStr(Resolution r);

// One candidate OCCURRENCE. A document naming `parse` forty times yields forty
// of these and one tree walk; de-duplication is a resolution cache, never a
// filter on the output.
struct Symbol {
    QString    symbol;       // the code span VERBATIM, e.g. "Foo::bar()"
    int        docLine = 0;  // 1-based
    int        docCol  = 0;  // 0-based, first content column of the span
    Resolution resolution = Resolution::NotChecked;
    QVector<SymbolQuery::DefMatch> definitions;  // only when Resolved
};

struct Options {
    // Verb names, schema argument names and refusal codes — the corpus's
    // largest backtick population. They are registered by STRING and declared
    // nowhere, so every one resolves nowhere and would be reported forever.
    // Injected rather than looked up: ants_core_lib is Qt6::Core-only and the
    // library dependency runs core → claude, so this engine cannot link
    // ClaudeIntegration to read the registry itself. The verb layer fills it
    // (spec § 2.4); a test supplies its own.
    QSet<QString> excludedNames;

    // Distinct needles this call may resolve. The caller decrements a run-wide
    // budget by ScanResult::needlesResolved between documents, which is what
    // keeps the bound run-wide while the engine stays per-document.
    int maxSymbolsPerRun = 500;

    // A single lowercase word shorter than this is emphasis far more often
    // than an identifier. A `::`-bearing, `()`-bearing or mixed-case span is
    // exempt — those shapes are unambiguous.
    int minIdentChars = 5;

    // Wall-clock ceiling on the resolution phase: a cap on needles alone does
    // not bound wall-clock, since each needle's cost is the tree's size. 0
    // disables. Remaining needles become NotChecked. ANTS-3680 — checked
    // between resolve chunks rather than between needles, so the granularity
    // is a chunk; what it still guarantees is that the run stops and says so.
    int resolveDeadlineMs = 10000;

    // Resolution needs the project root: this engine never opens the DOCUMENT
    // under review (its text is handed in, per the family convention in
    // ANTS-3664 § 2.3), but it does walk the SOURCE TREE. That is this
    // engine's stated exception rather than a contradiction of the rule.
    // Empty root → nothing is looked up and every needle is NotChecked.
    QString rootCanonical;
};

struct ScanResult {
    QVector<Symbol>            symbols;   // ascending by docLine, then docCol
    QList<DocFinding::Finding> findings;  // one per UNRESOLVED occurrence

    // Occurrence counts, not distinct needles — the two are not comparable,
    // which is why they are named differently.
    int total      = 0;
    int resolved   = 0;
    int unresolved = 0;
    // ANTS-4359 — the unresolved bucket split by the SHAPE the document wrote,
    // so a reader can start with the class where "no such symbol" is a real
    // defect. A span with `()` is a claim about a FUNCTION; a bare identifier
    // is as likely to be an enum value, a macro, a CMake variable or a JSON
    // key. A count, not a filter: nothing is dropped and nothing is judged.
    int unresolvedCall      = 0;
    int unresolvedQualified = 0;
    int unresolvedBare      = 0;
    int notChecked = 0;

    // ANTS-3692 — NEEDLE-scoped, not occurrence-scoped: true iff some needle
    // went unwalked (budget or deadline). An ambiguous needle that resolves
    // nowhere is dropped from `symbols` rather than reported, so deriving this
    // from the emitted occurrences would call an incomplete run complete.
    bool truncated = false;
    int  needlesResolved = 0;  // needles attempted; the caller's budget debit
};

// `text` is the document's content; `relPath` is only carried onto findings.
ScanResult scan(const QString &text, const QString &relPath,
                const Options &opts = {});

}  // namespace DocSymbols
