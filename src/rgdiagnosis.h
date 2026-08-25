#pragma once

// ANTS-4650 — one owner of the "why did ripgrep not start?" answer, shared by
// the three verbs that spawn it (workspace_search, cited_by,
// co_change_family).
//
// Why it exists. The refusal read "rg failed to start (is ripgrep
// installed?)" — a message that can only ever name ONE cause. That exact
// string has now been produced by two opposite ones: ripgrep genuinely absent
// (CI red on main for several commits, 2026-08-14, recorded in
// tests/features/ci_workflow_deps/spec.md) and ripgrep present and working
// (2026-08-25, `which rg` → /usr/bin/rg in the same session). So in one of
// those two cases the message sent the reader to the wrong repair, every
// time — and the fallback it implies is raw grep, which is exactly the cost
// workspace_search exists to remove.
//
// The evidence is gathered at the spawn site and passed in rather than
// classified there, so the branches are testable without making ripgrep
// vanish — and so this stays out of remotecontrol_internal.h, which
// tests/features/rc_tu_split INV-5 keeps out of the test tree.

#include <QString>

namespace RgDiagnosis {

// Everything known about a spawn that did not start.
struct StartFailure {
    QString exePath;                  // resolved rg; EMPTY means not on PATH
    QString workingDir;
    bool    workingDirExists = true;
    bool    stillStarting    = false; // wait expired, process still Starting
    QString errorString;              // QProcess::errorString()
    QString pathEnv;                  // the PATH actually searched
    int     startTimeoutMs   = 0;
};

// One sentence naming the cause and the repair it implies. Four causes, four
// different repairs: two that read alike are two of which one gets the wrong
// one.
QString explain(const StartFailure &f);

}  // namespace RgDiagnosis
