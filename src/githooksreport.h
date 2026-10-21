#pragma once

// git_state op:"hooks" (ANTS-5505) — is this repository's push gate wired
// up? Reports facts, never a verdict: where core.hooksPath points and who set
// it, whether pre-push is the machine-wide hook, hands off to it or is the
// project's own, the ants.gate.* keys that hook reads, the gate script it
// will run, whether a workflow calls that script, and whether HEAD's tree has
// a gate-passed record. Verdicts stay with ~/.claude's tools/ci-gate, whose
// rules change more often than this binary is rebuilt. The machine-wide hook
// owns its discovery list; this asks it (`pre-push --gate-candidates`) rather
// than holding a second copy. Contract: tests/features/git_hooks_report/spec.md.

#include <QJsonObject>
#include <QString>

namespace GitHooksReport {

// ${ANTS_GLOBAL_HOOKS:-$HOME/.claude/githooks}, as the hand-off shims and
// ci-gate spell it.
QString defaultMachineHooksDir();

// The op:"hooks" envelope for the repository at rootCanonical, or a refusal
// {ok:false, code, error}. machineHooksDir holds the machine-wide pre-push.
QJsonObject run(const QString &rootCanonical, const QString &machineHooksDir);

} // namespace GitHooksReport
