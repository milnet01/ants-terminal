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

#include <QString>
#include <QStringList>
#include <QVector>

namespace DocCitations {

// Grammar caps. Fields rather than literals so a fixture can drive the boundary
// from the outside — INV-40 needs a digit run exactly one too long, which is
// not expressible against a hard-coded number. ANTS-3636 § 2.7 owns the full
// struct (the read path's caps join it there); these are the scan's own.
struct Options {
    int maxLocusDigits = 7;    // stage 2: longer → bad_locus, before converting
    int maxRawChars    = 120;  // bounds an `unparsed` entry's `raw`
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
};

ScanResult scan(const QStringList &lines, const Options &opts = {});

}  // namespace DocCitations
