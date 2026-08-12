#pragma once

// ANTS-4108 — spec_conformance engine: run the patterns a spec prescribes
// against the expectation table beside them, instead of asking a reviewer to
// read them. See docs/specs/ANTS-4108-spec-conformance-verb.md.
//
// Deliberately executes PATTERNS ONLY, never fenced code (spec § 2.2): a
// pattern applied to a string cannot reach the filesystem or the network
// whatever it contains. The fixture-execution half is ANTS-4127.
//
// No subprocess, no interpreter (spec INV-8) — this header and its TU are the
// scope of that claim; the shared TUs the verb is wired into carry such calls
// for dozens of unrelated verbs.

#include <QJsonObject>
#include <QString>

namespace SpecConformance {

struct Options {
    // spec § 2.3 — range [1, 1000]; out of range REFUSES the call rather than
    // clamping, so a partial run can never read as a complete one.
    int maxCases = 200;
};

// Byte caps (spec § 2.8). Defence against an accident — a runaway pattern an
// author wrote by mistake — not against a hostile spec: the input is your own
// repository, the same trust boundary as building it.
inline constexpr int kMaxPatternBytes = 512;
inline constexpr int kMaxInputBytes   = 512;
inline constexpr int kMaxCasesCeiling = 1000;

// Returns the spec § 2.3 envelope:
//   {ok, path, cases_run, findings[], candidates[], refusals[],
//    observations[], truncated, etag}
// or a refusal envelope {ok:false, error, code} for a malformed request.
QJsonObject run(const QString &absPath, const Options &opt = {});

}  // namespace SpecConformance
