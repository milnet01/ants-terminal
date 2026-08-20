#pragma once

// ANTS-4547 / ANTS-4550 — one owner of the "wrapped quotation" matching
// rule, shared by the two verbs that need it.
//
// The rule, stated once: **a run of whitespace in the pattern matches a
// run of whitespace and markdown blockquote markers in the text.**
// Everything else matches literally.
//
// Why it exists. Prose in this corpus is hard-wrapped at ~70 columns, so
// a quotation of ordinary sentence length spans a line break. Every
// line-oriented matcher — ripgrep, grep -F, and roadmap_log's own
// single-line amend — then reports a miss on text that is present and
// exact. For a review gate that DISMISSES a finding whose quote cannot
// be located, a miss is indistinguishable from a defect, so the real
// defect ships (ANTS-4547). For op:amend_body the miss forced one
// paragraph to be edited by N single-line calls whose joint result was
// checked by nothing — the hazard ANTS-4097 names (ANTS-4550).
//
// Both callers need the SAME normalisation and the reference
// implementation of it (a sed | tr | grep pipeline carried by every
// review skill) was wrong for months in one half — it normalised the
// file and not the pattern, so a quotation pasted from a blockquote
// still missed. That is the argument for one owner rather than two
// call-site copies: the rule is subtle, two-sided, and re-derived
// wrongly.

#include <QString>
#include <QVector>

namespace WrapMatch {

// A located occurrence, as character offsets into the haystack that was
// searched. `start` is the first character of the match, `length` its
// extent, both in the ORIGINAL text — so a caller can splice without
// re-deriving anything from the normalised form.
struct Span {
    int start  = 0;
    int length = 0;
};

// Every wrapped-tolerant occurrence of `needle` in `haystack`, in order.
// `maxHits > 0` stops the scan once that many are found (a caller that
// only needs "unique or not" passes 2). An empty or whitespace-only
// needle finds nothing — it would otherwise match everywhere.
//
// The needle is matched LITERALLY apart from its whitespace runs: no
// regex metacharacter in it is honoured. A needle line that is nothing
// but blockquote markers (`>`, `>>`) is dropped, so a multi-line
// quotation copied out of a blockquote can be pasted in unedited.
QVector<Span> find(const QString &haystack, const QString &needle,
                   int maxHits = 0);

// The same rule as a regular expression, for handing to an external
// line-agnostic matcher (ripgrep with --multiline). Returns an empty
// string for a needle that finds nothing by the rule above, which the
// caller must treat as a refusal rather than as a match-everything
// pattern. Uses only constructs the Rust regex crate and PCRE2 both
// accept, so the one string drives ripgrep and QRegularExpression alike.
QString toRegex(const QString &needle);

// How a multi-line `newText` is laid out when it is spliced in. The two
// consumers differ and the difference is real: markdown lines carry the
// roadmap's continuation indent, so a bare newline in `newText` would put
// the rest of the body at column 0 and cut the bullet in two (ANTS-3752);
// the store holds the body as an UNINDENTED residual the renderer indents
// on the way out, so prefixing there would double it.
enum class Indent {
    None,             // splice `newText` verbatim (store bodies)
    MatchLineIndent,  // prefix each continuation line with the matched
                      // line's own leading whitespace (markdown bodies)
};

// One occurrence of `oldText` in `text`, replaced by `newText`.
struct Patch {
    int     hits    = 0;      // 0 -> miss, >1 -> ambiguous; both leave
                              // `text` empty so a caller cannot half-apply
    bool    wrapped = false;  // the hit spanned a line break
    int     line    = -1;     // 0-based line the match STARTS on
    QString text;             // the patched text; valid only when hits == 1
};

// Replace the single occurrence of `oldText` in `text`. An EXACT match is
// tried first and the wrapped rule only if that finds nothing, so no
// currently-succeeding call changes behaviour and the fallback can only
// turn a refusal into an edit. Uniqueness is enforced on whichever pass
// matched — a wrapped phrase occurring twice is ambiguous, not a licence
// to clobber the first.
Patch patchOnce(const QString &text, const QString &oldText,
                const QString &newText, Indent indent = Indent::None);

}  // namespace WrapMatch
