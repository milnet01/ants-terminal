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

namespace VerifyTrust { class Client; }

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
    // ANTS-1337 — optional trust client gating .ants/verify.json.
    // nullptr (default) preserves pre-ANTS-1337 behaviour: bespoke
    // configs are honoured unconditionally. When non-null, runVerify
    // threads the client through to loadGateConfig, which surfaces
    // VerifyReport::verifyUntrusted on a Deny/Headless outcome.
    VerifyTrust::Client *trustClient = nullptr;
};

// Top-level report. `allPassed` is the AND of every gate's
// `ran && passed`. `configSource` is one of the three documented
// strings: ".ants/verify.json" | "auto-detected" | "none" |
// "auto (untrusted-bespoke)".
struct VerifyReport {
    QList<GateResult> gates;
    bool              allPassed = false;
    QString           configSource;
    // ANTS-1337: when the caller passed a trust client and the
    // `.ants/verify.json` SHA wasn't trusted, the engine fell back
    // to auto-detect AND set `verifyUntrusted=true` so the MCP
    // layer can surface it in the response envelope. Empty trust
    // client (nullptr) leaves this false — back-compat.
    bool              verifyUntrusted = false;
};

// Load `.ants/verify.json` if it exists and passes the INV-4
// path-traversal guard; otherwise auto-detect per § 2.5. On total
// failure (malformed config, no detectable build system) returns
// an empty list and sets `*configSource` to "none" or the parse-
// error sentinel — callers check the list size + configSource.
//
// ANTS-1337 — optional `trustClient`. When non-null, a found
// `.ants/verify.json` is gated through the client's
// `outcomeForConfig`. If the SHA isn't trusted, the engine falls
// back to auto-detect AND sets `*verifyUntrusted=true` (when
// non-null) so the caller can surface it in the response envelope.
// When `trustClient` is null, the bespoke config is honoured
// unconditionally — preserves pre-ANTS-1337 behaviour for callers
// that haven't yet wired the trust client through.
//
// `*configSource` is filled when non-null.
QList<GateConfig> loadGateConfig(const QString &projectPath,
                                 QString *configSource,
                                 VerifyTrust::Client *trustClient = nullptr,
                                 bool *verifyUntrusted = nullptr);

// Run gates sequentially per INV-6 (build → tests → lint, never
// concurrent). Per-gate timeout = max(10, opts.timeoutSec /
// max(1, configured-gate-count)).
VerifyReport runVerify(const QString &projectPath,
                       const VerifyOptions &opts);

// ANTS-3373 — orphaned-source lint. Given `addedSourcePaths` (the
// repo-relative source files the caller found newly added in the
// working-tree diff), return those whose basename appears in NO
// CMakeLists.txt / *.cmake under `projectPath`. A returned path is a
// file that compiles in isolation but is silently never built — the
// author forgot to add it to a target's source list. Pure basename
// grep (a literal `contains`); build/vendor dirs are pruned via
// ProjectSettings::isNoiseDir so a generated build-tree CMakeLists
// never masks a real orphan. Git parsing stays caller-side, so this
// helper is git-free and unit-testable. Order and duplicates of the
// input are preserved in the output.
QStringList findUnreferencedSources(const QString &projectPath,
                                    const QStringList &addedSourcePaths);

}  // namespace VerifyEngine
