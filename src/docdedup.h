// ANTS-3660 — doc_dedup: find the same passage written twice. Qt6::Core only,
// in ants_core_lib beside markdownscan.h / docfinding.h, so it is unit-testable
// without RemoteControl / MainWindow.
//
// /cold-eyes § 1e's one dimension with no mechanical check at all, and the
// expensive one: the skill's Phase 4 says a run that will not converge usually
// has a duplication problem, and that reconciling two copies is the WRONG fix
// because they diverge again on the next edit. So every loop rediscovers them
// one at a time, at cold-reader prices.
//
// THIS ENGINE IS ANTS-3664 § 2.3's STATED EXCEPTION to the per-document
// signature. Its siblings score one document alone; a pair needs two, so
// scoring cannot happen until the walk is done. Hence add()/finish() rather
// than check(): ANTS-3663 hands each document's text to all five checkers from
// a single read, and a doc_dedup that took a *path* would re-open every file in
// the tree and defeat the composition it exists for.
//
// SCORING AND CANDIDATE GATHERING ARE TWO STEPS, and conflating them is what
// made the spec's INV-1 and INV-7 contradict each other for three review loops
// (spec § 2.2):
//
//   * Candidate gathering decides which pairs are LOOKED AT. `maxPostings`
//     prunes it — a shingle in more paragraphs than that carries no signal.
//   * Scoring decides which of those are REPORTED. It is the exact Jaccard over
//     each passage's FULL shingle set; a pruned shingle is still counted in
//     both the intersection and the union.
//
// A prune that also removed shingles from the arithmetic would make a reported
// similarity depend on how common a shingle happened to be corpus-wide, so two
// identical paragraphs could score below 1.0.
//
// REPORT-ONLY. Which copy is canonical is a question about document
// architecture, and a verb deleting the wrong one destroys the authoritative
// copy — so nothing here is ever autoFixable, and ANTS-3669 refuses to fix a
// doc_dedup finding by name.
//
// See docs/specs/ANTS-3660.md.

#pragma once

#include "docfinding.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

namespace DocDedup {

struct Options {
    int    shingleSize   = 3;      // words per shingle
    double minSimilarity = 0.40;   // spec § 1.1 measured: catches both real
                                   // pairs (0.583 and 0.450) with punctuation
                                   // stripped; ~0.17 paraphrase is out of reach
                                   // at any value a lexical measure can use
    int    minWords      = 15;     // below this a pair is coincidence; the real
                                   // finds are 19-31 words, so it must stay low
    int    maxPassages   = 20000;  // paragraphs COMPARED, not enumerated
    int    maxPostings   = 400;    // stop-shingle cut for candidate gathering
                                   // only. Measured over docs/: NOTHING in this
                                   // corpus reaches it (longest posting list is
                                   // 176 of 12,995 passages), so it is
                                   // insurance against a doc set that stamps
                                   // one banner into thousands of files, not a
                                   // live optimisation here.

    // Generated / templated artifacts (spec § 2.4). Globs, matched against the
    // whole project-relative path. Duplication between two dated audit reports,
    // or between two plan files carrying the same stamped banner, is what those
    // files ARE — measured, they supply more than half of every exact-duplicate
    // pair in this corpus. Injected rather than hardcoded because the engine is
    // corpus-agnostic and ANTS-3663 § 2.4 forwards this from the composer.
    QStringList excludedPathGlobs;
};

struct Passage {
    QString file;
    int     line = 0;   // 1-based, the paragraph's first line
};

struct Pair {
    Passage a;          // canonically first in (file, line) order — INV-4
    Passage b;
    double  similarity = 0.0;
};

// Connected components over the reported pairs. N documents sharing one stanza
// produce N(N-1)/2 pairs but are ONE thing to fix; without this a single
// repeated boilerplate line reads as forty findings. `size` is deliberately not
// a member — it is passages.size(), and a second copy of a fact is what this
// engine reports on.
struct Cluster {
    QVector<Passage> passages;
    double           maxSimilarity = 0.0;
};

struct Result {
    QList<DocFinding::Finding> findings;
    QVector<Pair>              pairs;
    QVector<Cluster>           clusters;

    int passagesTotal    = 0;  // every comparable paragraph the walk saw
    int passagesCompared = 0;  // how many were indexed (<= maxPassages)

    // INV-7's probe: candidate pairs actually scored. Reported so a test can
    // show the prune bit, since its effect is invisible in the findings.
    int candidatePairsConsidered = 0;

    // The index, reported rather than described. Spec § 4 makes a specific
    // claim about all three — that nothing in this corpus reaches maxPostings,
    // so the prune is insurance rather than a live optimisation — and an
    // earlier draft asserted the opposite for two review loops because nobody
    // could check it. These are what the calibration harness prints.
    int distinctShingles  = 0;
    int totalPostings     = 0;
    int maxPostingLength  = 0;

    bool truncated = false;    // passagesTotal exceeded maxPassages
};

// Accumulate documents, then score once. Members rather than free functions
// because the accumulator between them IS state: free functions with these
// signatures would need a file-scope corpus and stop being callable twice in
// one process.
//
// `opts` is taken per add() to match the family's calling convention; the
// caller fills it once and passes the same value to every call, and finish()
// uses the last one it saw.
class Accumulator {
public:
    void   add(const QString &text, const QString &relPath, const Options &opts = {});
    Result finish();

private:
    struct Entry {
        QString      file;
        int          line = 0;
        QVector<int> shingles;   // interned ids, sorted ascending
    };

    QHash<QString, int> m_vocab;
    QVector<Entry>      m_passages;
    Options             m_opts;
    int                 m_total = 0;
};

// --- exposed for the conformance tests, and because the verb layer needs the
// --- exclusion predicate to skip a file rather than read it and drop it.

// Paragraphs, per spec § 2.1: a maximal run of lines that are none of blank, a
// heading, a list marker, a GFM table row, or fence-masked; a masked fence line
// acts as a blank; and A LIST ITEM IS ONE PASSAGE — its marker line joined with
// its indented continuations. That last rule is not a preference: measured, the
// alternative scores this corpus's clearest duplication at 0.024 instead of
// 0.545, because every `- **INV-N** — …` is then compared without its subject.
// Returns (1-based first line, joined text).
QVector<QPair<int, QString>> segment(const QStringList &lines);

// Strip inline-code delimiters and emphasis; replace every non-word non-space
// character with a space; fold whitespace; lowercase. Order matters — folding
// before stripping leaves the delimiters adjacent to words and changes the
// tokens. Punctuation stripping is pinned, not optional: it moves one measured
// pair 47% and the other 7%, and the larger is bigger than the gap between any
// two thresholds this engine might use.
QString normalise(const QString &text);

// A paragraph whose entire content is one markdown inline link or one bare URL,
// optionally preceded by see/See, optionally carrying a list marker, and
// optionally followed by a single '.' or ','. Excluded, because a pointer is
// the shape /cold-eyes recommends FOR an already-deduplicated fact — reporting
// pointers would flag the fix as the defect. A sentence that merely contains a
// link is prose and is compared.
bool isPointerLine(const QString &raw);

// True when `relPath` matches any of `opts.excludedPathGlobs`.
bool isExcludedPath(const QString &relPath, const Options &opts);

}  // namespace DocDedup
