// ANTS-3665 — spec-body parsing, hoisted out of remotecontrol.cpp's anonymous
// namespace so engines in ants_core_lib can link it. ANTS-3662 (`spec_lint`)
// needs this same parse and lives in that library; without the hoist it would
// have to grow a second spec parser beside this one, which is exactly the
// divergence MarkdownScan was hoisted (ANTS-3603) to prevent.
//
// Qt6::Core-only and pure: spec text in, JSON out, no filesystem.

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace SpecParse {

// ANTS-3785 — the lines one `**Field:**` header entry occupies.
//
// A header field's value MAY wrap: docs/standards/specs.md § 3.2 tells authors
// to append progress to Status inline, and the corpus hard-wraps at ~80
// columns, so a WRAPPED Status is the common case rather than the exception.
// ANTS-3789 — the share is cited as a tool, never transcribed: run
// `python3 tools/spec-header-survey.py`. The figure here read "49 of 172" and
// was already wrong when written, because it drifts on every new spec; it
// measured 65 of 190 on 2026-08-15. A number nobody can re-derive is a claim
// with an expiry date nobody can see. Modelling
// the field as a single line is what made the reader truncate it (ANTS-3672)
// and the writer orphan its continuations (ANTS-3785).
struct FieldExtent {
    int     line      = -1;  // 0-based index of the `**Field:**` line; -1 = absent
    int     lineCount = 0;   // 1 + continuation lines
    QString value;           // trailing text + continuations, space-joined
    bool    found() const { return line >= 0; }
};

// First header field named `name` ("Status", "Kind"), searched from the top and
// BOUNDED to the header block: the first `^## ` heading, else end of input.
//
// The bound is load-bearing, not tidiness. Unbounded, a search for an ABSENT
// field runs to the end of the file and matches a `**Status:**` line quoted
// inside a fenced example — after which setStatus rewrites inside a code fence
// and reports success. docs/standards/specs.md § 3.2 contains exactly that
// shape, and spec_log's `path` routing admits any in-repo file.
//
// Extent terminators (spec § 2.1): a blank line, a further `^\*\*Field:\*\*`
// marker, an ATX heading, or the block end. A LIST BULLET is deliberately not
// one — docs/specs/ANTS-1436.md's continuation begins `+ ` and is prose, so
// treating bullets as terminators truncates it. Only a marker at the START of
// a line counts; an inline bold colon-run is value text.
FieldExtent headerField(const QStringList &lines, const QString &name);

// ANTS-3786 — true when `line` closes the header block: an `^## ` heading.
//
// Exported so the bound has ONE expression. headerField applies it internally;
// docsindex.cpp needs the same predicate to know when to stop buffering (it
// streams, and cannot hand headerField a whole file). A private `^##\s` regex
// on that side would be a third copy of the rule this pair exists to share.
//
// H2 only, deliberately: `### ` does NOT close the block, so a `**Status:**`
// beneath a level-3 heading is still a header field. That is headerField's
// pre-existing behaviour, preserved by the extraction rather than chosen here.
bool isHeaderBlockEnd(const QString &line);

// Parse a spec file's body into {title, status, kind, invariants[],
// invariants_count, possible_untabled_invariants}. `body` is the full file
// text. Empty fields are emitted as empty strings; an absent Invariants
// section yields an empty array.
//
// Each invariant is {id, body, test_surface?}. `test_surface` carries the
// invariant's test clause and is **omitted entirely when there is none** —
// absence is the signal a caller needs, since "this invariant has no test
// surface" is the defect ANTS-3662's `invariant_no_test` check reports.
// Both spec forms produce it (docs/standards/specs.md § 6): the third cell of
// a GFM table row, or the `*Test:*` sentence of a bullet.
QJsonObject parseSpecBody(const QString &body);

}  // namespace SpecParse
