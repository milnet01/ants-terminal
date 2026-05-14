// ANTS-1372 — Caller-cwd gate for mutating MCP fold-in verbs.
//
// The MCP socket at $XDG_RUNTIME_DIR/ants-terminal/control.sock is
// shared by every Claude Code session on the machine. Verbs that
// mutate project-scoped state (.roadmap-counter, ROADMAP.md, source
// files, session-memory store, build artifacts) MUST verify that the
// calling session's cwd matches the focused tab's cwd before they
// write anything. Otherwise a Claude in project A can silently mutate
// project B when the user has a tab focused on B.
//
// See docs/specs/ANTS-1372.md.

#pragma once

#include <QJsonObject>
#include <QString>

namespace RcGate {

struct CallerCwdGate {
    bool    ok        = false;
    QString focused;   // canonical project root from focused tab (or "")
    QString caller;    // canonical caller cwd (or "" on missing/bad)
    QString errorCode; // "cwd_missing" / "cwd_bad" / "no_project" / "cwd_mismatch"
    QString error;     // human-readable
};

// Read `caller_cwd` from `req`, canonicalise both it and `focusedRaw`,
// compare. Returns a verdict struct; caller materialises a JSON error
// envelope on `!ok`.
//
// `focusedRaw` is the focused-tab cwd as already-canonicalised by the
// caller (typically `resolveRootCanonical(m_main)` — passed in
// pre-resolved so this helper has zero MainWindow dependency and is
// trivially unit-testable).
//
// On `errorCode == "cwd_mismatch"`, the helper emits one qWarning line
// with the verb name and both canonical paths. Other refusal codes do
// NOT log (per INV-11) — they're caller-side bugs, not cross-project
// incidents.
CallerCwdGate checkCallerCwd(const QString &focusedRaw,
                             const QJsonObject &req,
                             const QString &verb);

// Materialise the standard JSON-RPC error envelope from a gate
// verdict. Key order matches ANTS-1295: {ok, error, code}.
QJsonObject gateErrorEnvelope(const CallerCwdGate &g);

} // namespace RcGate
