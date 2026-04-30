#pragma once

// ants-helper: stateless local-subagent CLI for Claude Code (ANTS-1116 v1).
//
// Ships alongside the GUI binary, gated by `-DANTS_ENABLE_HELPER_CLI=ON`
// (default OFF). v1 surface is a single subcommand that wraps
// `packaging/check-version-drift.sh` and emits structured JSON.
// Token-saving framing: replaces the
//
//   bash packaging/check-version-drift.sh; echo $?
//
// pattern with a one-call `{"ok":true,"data":{"clean":true|false,
// "violations":[...]}}` shape Claude can parse without a separate
// "let me read the stdout" round-trip.
//
// Per `docs/specs/ANTS-1116.md`: this is the "shape proves out the
// binary works" v1. v2 will add `audit-run` once ANTS-1119 (audit-engine
// extraction) lands. Other subcommands (id-allocate, test-runner,
// audit-fold-in, …) are deferred until ANTS-1120 measurement validates
// they earn their token-saving claim.
//
// Response shape (matches ANTS-1117's unified envelope):
//   { "ok": true,  "data": { ... } }
//   { "ok": false, "error": "...", "code": "..." }
//
// Exit codes:
//   0 — success (handler ran + result was "clean")
//   1 — handler error / runtime failure
//   2 — usage error (unknown subcommand, malformed JSON input)
//   3 — drift detected (handler ran fine; result is "not clean")
//
// Platform: Linux-only (forks `bash`, expects POSIX `/bin/bash` or
// PATH-resolvable equivalent). Cross-platform exposure is non-goals
// for v1 — Claude Code's primary target is Linux developer machines.
// macOS works in practice (bash 3.2+ on PATH); Windows would need a
// driftCheck rewrite that doesn't shell out.
//
// TOCTOU on the script-presence check: `QFileInfo::exists` then
// `QProcess::start` is a check-then-use window where a malicious
// rename in between could redirect bash to a different file. Bounded
// by the existing UID-scoped trust model — the helper runs in the
// user's own session, not setuid; an attacker with that UID has
// already won. Surface noted here, not addressed.

#include <QJsonObject>
#include <QString>

namespace AntsHelper {

// drift-check: invoke `bash packaging/check-version-drift.sh` from
// `repoRoot`. Returns the unified envelope. Sets `*exitCodeOut` to
// the exit code the dispatcher should use:
//   0 if clean, 3 if drift, 1 if handler error.
QJsonObject driftCheck(const QJsonObject &request,
                       const QString &repoRoot,
                       int *exitCodeOut = nullptr);

// Format a JSON object as a single-line UTF-8 string suitable for
// stdout. Helper for the dispatcher and tests.
QString jsonToCompactString(const QJsonObject &obj);

}  // namespace AntsHelper
