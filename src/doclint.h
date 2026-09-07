#pragma once
// ANTS-3663 — doc_lint: every deterministic doc check in one call.
//
// Composes the five deterministic document checkers into ONE findings list so a
// review pre-pass costs one call rather than five, each of which re-walks the
// same tree. Qt6::Core only, in ants_core_lib beside docfinding.h, so it is
// unit-testable without RemoteControl / MainWindow.
//
// THE SHARED READ IS THE POINT, AND IT IS PARTIAL BY CONSTRUCTION. The three
// NATIVE checkers (doc_dedup, doc_symbols, spec_lint) take document TEXT, so one
// read serves all three — that is INV-1 and the whole saving. The two ADAPTED
// engines re-read: DocIntegrity::check consumes a relative-path list and
// DocCitations::check one document path, and spec § 2.2 freezes both signatures
// rather than change a shipped, tested engine. So the honest per-document budget
// with every check on is three opens, not one; spec § 2.1 states the whole-run
// figure and attaches no assertion to the six that are unobservable from here.
//
// THIS ENGINE NEVER ENUMERATES. The verb layer calls
// RemoteControl::docIntegrityEnumerate and hands the result in, as all three
// shipped siblings do — two enumerations that must agree forever are two that
// will eventually disagree (ANTS-3661 § 2.4). The CAPS are the engine's, because
// eliding a document is an observable outcome a test must be able to drive.
//
// See docs/specs/ANTS-3663.md.

#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "doccitations.h"
#include "docdedup.h"
#include "docfinding.h"
#include "docintegrity.h"
#include "docsymbols.h"
#include "speclint.h"

namespace DocLint {

// The five checker names. This list is BOTH the `checks[]` vocabulary and the
// `verb` string every finding carries — one vocabulary, not two (spec § 2.1),
// so a caller filtering by a name it read off a finding cannot miss.
const QStringList &checkNames();

// INV-1's instrument: documents opened by the shared read. It counts what this
// engine owns and nothing else — the two frozen adapters open their own files
// and expose no counter, which is why spec § 2.1 states nine as a budget and
// asserts three.
struct Probe {
    int opens = 0;
};

struct Options {
    QString rootCanonical;

    // Eligibility for spec_lint (spec § 2.1): the project-relative directory
    // under which a document is a spec. A README has no invariants, and without
    // this predicate a default-path run floods every standard and journal with
    // missing_section and loop_row_no_outcome — which is what the primary caller
    // gets on its first run.
    QString specsDirRel = QStringLiteral("docs/specs");

    // Empty selects all five. An unknown name is the VERB's refusal to make
    // (bad_args), not this engine's: it has no error channel and silently
    // running four checks for a caller who asked for a fifth is the failure.
    QSet<QString> checks;

    // Per-engine knobs, forwarded verbatim. The four injected inputs spec § 2.5
    // enumerates live on these: symbols.excludedNames / symbols.rootCanonical,
    // spec.requiredSections, dedup.excludedPathGlobs. A composer that drops them
    // does not fail — it produces the verb-name false-positive flood ANTS-3661's
    // own loops removed, and a sectionsChecked:false that reads as a missing
    // standard rather than a missing pass-through.
    DocIntegrity::Options walk;       // maxDocBytes / maxDocsPerRun
    DocCitations::Options citations;
    DocDedup::Options     dedup;
    DocSymbols::Options   symbols;
    SpecLint::Options     spec;

    // ANTS-3669 — the only part of this family that writes to a file. `dryRun`
    // computes the identical result down the SAME path and writes nothing: a
    // preview assembled separately is a preview that drifts (INV-12). It is
    // meaningless without `fix`, and the VERB refuses that pair rather than
    // this engine, which has no error channel (INV-16).
    bool fix    = false;
    bool dryRun = false;

    Probe *probe = nullptr;  // test-only

    // INV-21's instrument, and test-only like Probe. Invoked once after the
    // walk has finished and before the fix path opens anything — the exact
    // window in which a user's save makes the findings describe a document that
    // no longer exists. Nothing inside one call can produce that window, so
    // spec § 6 requires the harness to expose a seam for it.
    std::function<void()> afterWalkHook;
};

// A document the run did not check. No `verb`: a document either got read or it
// did not, and the asymmetry with CheckError is deliberate (spec § 2.4).
struct Skip {
    QString file;
    QString reason;  // read_failed | doc_cap | too_large
};

// A checker's output for one document is not the whole story. Carries a `verb`
// because the same document can fail one checker and satisfy the others.
struct CheckError {
    QString verb;
    QString file;
    // check_failed means the checker produced NOTHING for this document. The
    // other three mean it succeeded incompletely and its findings are a floor.
    // A caller treating every entry as a failure under-reports; one treating
    // them all as advisory misses a checker that never ran.
    QString reason;  // check_failed | read_budget_exhausted
                     // | basename_index_truncated | citations_truncated
};

// A repair that did not reach disk, and why. Deliberately NOT a CheckError:
// that names a checker which produced nothing, this names a finding the fixer
// declined or could not land. Same `file`, one phase later, and no `verb` —
// only doc_integrity produces anything fixable today, so a verb column would
// carry one value and read as though it discriminated.
struct FixError {
    QString file;
    // stale       — the re-derivation no longer produces a walk-time gap, so
    //               the document moved under us; nothing is written for it.
    // read_failed — the file vanished or changed permissions between walk and
    //               write. Same name skipped[] uses: the same event, later.
    // no_template — a TOC region with no existing H2 entry whose form the
    //               inserted one could copy. Guessing the convention is how a
    //               fixer starts writing markdown the author did not choose.
    // write_failed— open, short write, or commit() failed. QSaveFile leaves the
    //               original byte-identical.
    QString reason;
};

// Envelope-level signals the per-check envelopes would have carried. Each needs
// a stated way to combine across documents, because the walk is per document and
// this is per checker (spec § 2.1's value-class table).
struct Stats {
    // doc_citations — counters, summed.
    int unparsedTotal      = 0;
    int examplesSuppressed = 0;

    // doc_symbols — counters summed, flag OR-ed.
    int  symbolsTotal      = 0;
    int  symbolsResolved   = 0;
    int  symbolsUnresolved = 0;
    int  symbolsNotChecked = 0;
    bool symbolsTruncated  = false;

    // spec_lint — flag OR-ed, line_count MERGED by project-relative path. A
    // composer that treats line_count as a scalar overwrites it once per
    // document and returns the last one, which looks entirely plausible.
    bool              sectionsChecked   = false;
    QMap<QString,int> lineCount;
    bool              specLintTruncated = false;

    // doc_dedup — RUN-SCOPED, taken verbatim and never combined. This engine
    // ingests every document into one index and produces these once per run;
    // summing them multiplies each by the document count.
    int  passagesTotal    = 0;
    int  passagesCompared = 0;
    bool dedupTruncated   = false;
};

struct Result {
    // Total order: file, line, verb, kind, message, emissionIndex (INV-7). The
    // first five are all properties of a finding, so two findings can agree on
    // every one of them and a five-key comparator is therefore not total.
    QList<DocFinding::Finding> findings;

    QStringList checkedDocs;
    // Ran clean != did not run. A checker appears here when it was selected AND
    // eligible for at least one enumerated document.
    QStringList checksRun;

    QList<Skip>       skipped;
    QList<CheckError> checkErrors;

    // doc_dedup's structured payload, hoisted: DocFinding::Finding has neither a
    // second-location field nor a grouping one. Hoisting only pairs hands the
    // caller the ungrouped output clustering was added to prevent.
    QVector<DocDedup::Pair>    pairs;
    QVector<DocDedup::Cluster> clusters;

    // ANTS-3669. `fixed` is per FINDING and `filesWritten` per FILE, and they
    // are not the same number: three TOC gaps in one document are three
    // elements and one write. Each element carries its WALK-TIME location, so
    // the array still correlates with findings[] where the re-read shifted
    // lines. A finding whose write failed is absent — appending on intent
    // rather than on success reports a repair that never happened (INV-14).
    // Neither is paged by max_findings (INV-15).
    QList<DocFinding::Finding> fixed;
    int                        filesWritten = 0;
    QList<FixError>            fixErrors;

    Stats stats;

    // A cap elided a DOCUMENT — distinct from the response-level flag the verb
    // sets when max_findings elided findings. It is easy to list a skipped file
    // and still report a response that reads as a complete account of the tree.
    bool truncated = false;
};

Result run(const QStringList &relDocs, const Options &opts);

}  // namespace DocLint
