// ANTS-1504 — changed-file resolution for `audit_run` narrowing scopes
// (since-last-run / files / branch-diff / since-tag). Split pure/impure so
// the parsing, language filtering, and tool classification are unit-testable
// without spawning git (mirrors ReadLog::filter + the AuditEngine
// pure-function pattern). Only `resolveChangedFiles` shells out.
//
// See docs/specs/ANTS-1504.md.

#pragma once

#include <QString>
#include <QStringList>

namespace AuditScope {

// True for tools that narrow by source file. False for the repo-global
// scanners (gitleaks secret-scan, trivy dependency-scan): a secret or a
// vulnerable dependency is not tied to the files you just edited, so they
// are skipped under a narrowing scope (INV-4).
bool isFileScopedTool(const QString &tool);

// Return the subset of `files` whose extension matches `tool`'s language
// set (INV-3): cppcheck/clazy/clang-tidy → C/C++, ruff/bandit/mypy → .py,
// shellcheck → .sh/.bash. semgrep self-selects by rule language, so it
// receives every changed file. A tool with no known language set also
// receives every file (safe default). Not meaningful for the repo-global
// tools — callers gate on isFileScopedTool first.
QStringList filterForTool(const QStringList &files, const QString &tool);

// Parse `git diff --name-only` output ∪ (when includeWorkingTree) the
// porcelain `git status` output into a deduplicated, project-relative file
// list (INV-1). Deleted entries are dropped — nothing to scan. Untracked
// (`??`) entries are kept — a new source file is a change to re-check.
QStringList parseChangedFiles(const QString &diffNameOnly,
                              const QString &statusPorcelain,
                              bool includeWorkingTree);

// Outcome of resolving a scope to a changed-file set.
struct Resolution {
    bool        narrowed = false;   // a narrowing scope was requested
    QStringList files;              // changed project-relative files (narrowed)
    QString     anchorCommit;       // commit diffed against (since-last-run)
    QString     demotedReason;      // non-empty → demote to a full scan
    bool        noChanges = false;  // narrowed, but zero changed files
};

// Impure: run the git commands for `scope` and return the changed-file set.
// `priorCommit` is the manifest's last_run.commit, used only by
// since-last-run (empty / "nogit" → demotes with the matching reason).
//
//   "auto" / ""        → narrowed=false (full scan, unchanged behaviour)
//   "since-last-run"   → diff priorCommit..HEAD ∪ working tree
//   "files"            → diff <merge-base>..HEAD ∪ working tree
//   "branch-diff"      → diff main..HEAD (no working tree)
//   "since-tag:<t>"    → diff <t>..HEAD ∪ working tree
//
// Demotion (demotedReason set, files empty): no_prior_run / no_prior_commit
// / prior_commit_unreachable (since-last-run), no_merge_base (files),
// not_git (any). A demoted run reverts to a full scan (INV-6).
Resolution resolveChangedFiles(const QString &canonProject,
                               const QString &scope,
                               const QString &priorCommit);

}  // namespace AuditScope
