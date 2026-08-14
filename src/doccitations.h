// ANTS-3653 — the citation SCAN layer: what does this document cite?
//
// A pure function — document text in, citation tokens out. No filesystem, no
// path resolution, no status taxonomy, no response caps; those are ANTS-3636's
// read path, which consumes this. Qt6::Core-only, in ants_core_lib beside
// MarkdownScan, whose fenceMask + codeSpans (ANTS-3649) it consumes.
//
// The grammar and the reasoning behind every rule live in
// docs/specs/ANTS-3653.md; the test surface is tests/features/doc_citations_scan/.
// Matching is two-stage — a permissive RECOGNISE pass finds token boundaries,
// then a VALIDATE pass decides citation or bad_locus. That split is not
// decoration: a single strict production cannot report what it fails to match,
// so an over-long line number would vanish instead of landing in `unparsed`.

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace DocCitations {

// `only` — the filter over citations[]. Stale keeps every non-ok citation, and
// (ANTS-3654) an ok one whose anchor no longer matches — the drift the verb
// exists to surface. An ok citation with no anchor was never judged, so it is
// `unchecked` rather than stale, and Stale drops it.
enum class Only { All, Stale };

// Test-only counters. INV-6 asserts "zero filesystem calls" and INV-20 "one
// open for five citations"; neither is observable from the returned JSON, so
// without this they could only be asserted by inspection — which is how a spec
// ends up with an invariant nothing actually checks. Nullptr in production; the
// engine's only cost is a null test per operation.
struct Probe {
    int stats = 0;   // QFileInfo / isInsideProject probes of a candidate
    int opens = 0;   // target files actually read (cache misses)
};

// Every bound this layer sets, in one place. Fields rather than literals so a
// fixture can drive a boundary from the outside — INV-40 needs a digit run
// exactly one too long, INV-11 a target one byte over the cap, and neither is
// expressible against a hard-coded number. The five request-driven fields are
// marked; the rest guard the engine and have no request argument.
struct Options {
    // Basename → project-relative paths, built by the CALLER (ladder step 2).
    // Passing it in keeps the engine Qt6::Core-pure and unit-testable without a
    // CodebaseIndex on disk; empty → steps 0/1/3/4/5 only.
    QHash<QString, QStringList> basenameIndex;
    // True when the index walk hit its file cap. The engine cannot learn this
    // from the map itself — a capped index still looks large — so it is passed
    // in; it drives `basename_index_truncated`.
    bool basenameIndexTruncated = false;

    Only only          = Only::All;   // request arg
    int  offset        = 0;           // request arg, handler-clamped to max(0, offset)
    int  maxRangeLines = 3;           // request arg, handler-clamped [1, 20]
    int  maxDocLines   = 20000;       // request arg, handler-clamped [1000, 50000]
    int  maxBytes      = 128 * 1024;  // request arg, handler-clamped [64 KiB, 4 MiB]

    int maxLocusDigits = 7;    // scan stage 2: longer → bad_locus, before converting
    int maxRawChars    = 120;  // bounds an `unparsed` entry's `raw`
    int maxAnchorGap   = 40;   // ANTS-3654 § 2: columns between the anchor span's
                               // closing delimiter and the citation span's opening
                               // one, beyond which the identifier is not an anchor

    int    maxCitations   = 400;
    int    maxUnparsed    = 200;
    int    maxCandidates  = 8;
    int    maxTextBytes   = 2000;               // UTF-8 BYTES per emitted line
    int    maxTargetReads = 1000;               // absolute read budget
    int    maxCacheFiles  = 16;                 // cache slot count
    qint64 maxDocBytes    = 8 * 1024 * 1024;    // handler-enforced, the DOC's own size
    qint64 maxTargetBytes = 8 * 1024 * 1024;    // on-disk bytes of one target
    qint64 maxCacheBytes  = 24 * 1024 * 1024;   // IN-MEMORY bytes, not on-disk

    // ANTS-4386 — opt-in QUOTATION check. Nothing in the toolchain verified a
    // fragment a document attributes to another document, and it is the
    // highest-yield mechanical class in a cross-referencing corpus: a quote
    // rots exactly when the quoted document is edited, which is when nobody
    // re-reads the quoting one.
    //
    // The hand-rolled substitute is worse than nothing. A quotation in a
    // hard-wrapped document does not survive a line-oriented search, so a grep
    // reported "not found" for a phrase that was present and a reviewer nearly
    // "fixed" a passage that was already correct — the collateral-generating
    // move the review skill exists to prevent.
    bool quotes        = false;
    // Below this many characters a double-quoted span is ordinary prose, not a
    // quotation. Keeps the check off every quoted word in the corpus.
    int  quoteMinChars = 30;
    int  maxQuotes     = 200;

    Probe *probe = nullptr;    // test-only, see above
};

// ANTS-4386 — one harvested quotation and its verdict.
struct Quote {
    int     docLine = 0;      // 1-based line the quoted span starts on
    QString text;             // the quoted text, delimiters stripped
    QString target;           // the attributed document, when one was inferred
    // "ok"        — found in the attributed document
    // "not_found" — the target was read and does not contain it
    // "ambiguous" — the attribution named a basename with several matches
    // "no_target" — no document was attributed in the same sentence
    QString status;
    QStringList candidates;   // ambiguous only
};

// One recognised, validated citation.
struct Citation {
    int     docLine = 0;        // 1-based line the token's FIRST character sits
                                //   on — which matters for the rare span that
                                //   crosses a newline: the citation takes its
                                //   own line, not the span's
    int     docCol  = 0;        // 0-based column of that character (sort key)
    QString raw;                // the matched token WITHOUT its delimiters:
                                //   `` `:45` `` yields ":45"
    QString path;               // empty ⟺ continuation
    bool    continuation = false;  // a bare `:N` filling a whole code span; the
                                   //   antecedent it inherits is ANTS-3636's
                                   //   sticky rule, not this layer's
    int     startLine = 0;      // first locus
    int     endLine   = 0;      // == startLine when the token names no range
    bool    approximate = false;  // `~` on a KEPT locus
    bool    partial     = false;  // a trailing locus or `+` was dropped
};

// A token that recognised but failed validation.
struct Unparsed {
    int     docLine = 0;
    int     docCol  = 0;
    QString raw;                 // clipped to Options::maxRawChars
    bool    rawClipped = false;
    QString reason;              // "bad_locus"
};

struct ScanResult {
    QVector<Citation> citations;   // document order: docLine, then docCol
    QVector<Unparsed> unparsed;    // likewise
    // 1-based line of an unclosed fence opener, or -1. Reported rather than
    // absorbed: inherited silently, a single stray ``` blanks every citation
    // below it and returns a clean-looking all-clear over a document the caller
    // barely read. Comes from MarkdownScan::fenceMask's opener overload and is
    // never inferred from the mask (see that header for why it cannot be).
    int unterminatedFence = -1;
    // ANTS-3659 — 1-based line of an unclosed `<!-- doc-examples: begin -->`,
    // or -1. Relative to the SCANNED PREFIX: with max_doc_lines binding, a
    // region whose `end` is past the cut reports here, and
    // scanned_lines < doc_lines (ANTS-3636 INV-30) is what says so.
    int unterminatedExamples = -1;
    // ANTS-3659 — how many tokens the example mask dropped, across BOTH arrays.
    // Whole-doc: the emission caps trim what is returned, never what was
    // suppressed. Without it a too-wide region is indistinguishable from a
    // document that cites nothing.
    int examplesSuppressed = 0;
};

ScanResult scan(const QStringList &lines, const Options &opts = {});

// ANTS-3636 — the read path. Reads `docAbsPath` ONCE, resolves every citation
// `scan` found under `rootCanonical`, reads each target, and returns the
// § 2.1 envelope including `ok` and `path` (so INV-16 can compare whole
// returns) — or the lone {ok:false, code:"read_failed"} refusal the handler
// cannot raise without decoding the doc itself. Writes nothing, holds no
// cross-call state: paging re-resolves the whole doc on every page.
//
// `path` comes back project-relative to `rootCanonical`; the handler overwrites
// it with the caller's own string, which is what the request echo promises.
QJsonObject check(const QString &rootCanonical, const QString &docAbsPath,
                  const Options &opts = {});

}  // namespace DocCitations
