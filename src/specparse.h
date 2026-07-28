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

namespace SpecParse {

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
