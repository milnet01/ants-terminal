// VerifyEngine — pure-function helpers for the `verify_changes` MCP
// tool (ANTS-1289). Qt::Core only, no widgets.
//
// Drives a project's build → tests → lint gates sequentially and
// returns structured pass/fail data. Replaces the
// `verification-before-completion` skill's mechanical 3-step loop.
// See docs/specs/ANTS-1289.md for the full invariant list.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace VerifyEngine {

enum class GateName { Build, Tests, Lint };

// MCP-layer key string for a gate name. Stable identifier used in
// the JSON response object (`gates.build`, `gates.tests`, `gates.lint`)
// and in `.ants/verify.json`'s top-level keys.
QString gateKey(GateName g);

// One gate's configuration. `command` is shell-quoted whole-string;
// the engine runs it via `/bin/sh -c <command>` with `projectPath`
// as cwd. `format` hints to the parser: "ctest" | "lint" | "plain".
struct GateConfig {
    GateName name = GateName::Build;
    QString  command;
    QString  format;
};

// Result of executing one gate. Fields whose "Present when" rule
// in docs/specs/ANTS-1289.md § 2.3 doesn't fire are simply omitted
// from the JSON serialisation by the caller — the engine fills the
// struct unconditionally.
struct GateResult {
    GateName    name = GateName::Build;
    bool        ran  = false;
    bool        passed = false;
    int         exitCode = 0;
    double      durationSec = 0.0;
    QString     skippedReason;
    QString     logTail;
    bool        logTruncated = false;
    int         logTotalLines = 0;
    int         passedCount = -1;   // -1 sentinel: not parsed
    int         totalCount  = -1;
    QStringList failingTests;
};

// Knobs the caller can pass to `runVerify`. All optional.
struct VerifyOptions {
    QList<GateName> only;            // empty = run every configured gate
    int             maxLogLines = 50;
    int             timeoutSec  = 1200;  // INV-2 default (20 min total)
    // Test-only floors. Production callers leave these at the default
    // 10 s — the rationale being a real verify gate (build / tests /
    // lint) below 10 s is almost certainly mis-configured. Tests
    // exercising timeout-enforcement behaviour drop these to 1 to
    // assert kill-on-expiry without sleeping the per-gate floor on
    // every run. Mirrored in `runVerify` (total clamp) and the
    // per-gate divisor.
    int             minPerGateSec      = 10;
    int             minTotalTimeoutSec = 10;
};

// Top-level report. `allPassed` is the AND of every gate's
// `ran && passed`. `configSource` is one of the three documented
// strings: ".ants/verify.json" | "auto-detected" | "none".
struct VerifyReport {
    QList<GateResult> gates;
    bool              allPassed = false;
    QString           configSource;
};

// Load `.ants/verify.json` if it exists and passes the INV-4
// path-traversal guard; otherwise auto-detect per § 2.5. On total
// failure (malformed config, no detectable build system) returns
// an empty list and sets `*configSource` to "none" or the parse-
// error sentinel — callers check the list size + configSource.
//
// `*configSource` is filled when non-null.
QList<GateConfig> loadGateConfig(const QString &projectPath,
                                 QString *configSource);

// Run gates sequentially per INV-6 (build → tests → lint, never
// concurrent). Per-gate timeout = max(10, opts.timeoutSec /
// max(1, configured-gate-count)).
VerifyReport runVerify(const QString &projectPath,
                       const VerifyOptions &opts);

}  // namespace VerifyEngine
