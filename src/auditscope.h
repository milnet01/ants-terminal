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
    QString     anchorCommit;       // base diffed against (since-last-run, files, since-tag)
    QString     demotedReason;      // non-empty → demote to a full scan
    bool        noChanges = false;  // narrowed, but zero changed files
};

// Impure: the audit code's one git runner (cache, scope and SARIF provenance).
// Runs `git <args>` in `root` with GIT_OPTIONAL_LOCKS=0 (ANTS-4999) and a
// 1.5 s start and finish budget, and returns trimmed stdout. Returns {} on a
// timeout, a crash or a non-zero exit. ANTS-5043 — `ok`, when given, tells a
// failure apart from an empty but successful output.
QString runGit(const QString &root, const QStringList &args,
               bool *ok = nullptr);

// ANTS-2015 — impure: the tracked source tree for a deterministic full-tree
// sweep (`scope:"full"`). `git ls-files src/` respects .gitignore (build/ and
// .audit_cache/ never appear) and is independent of diff state — so it works
// on a clean `main` where `auto`'s clazy positional list is empty (→ scans
// nothing). Restricted to src/ when the project has one (mirroring the GUI's
// `../src/*.cpp` clazy glob), else the whole tracked tree (ANTS-3741);
// dir-default tools (ruff/bandit/semgrep/shellcheck) fall back to their own
// full scan when this list has no file in their language.
QStringList enumerateSourceFiles(const QString &canonProject);

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
// not_git, ref_unresolved, git_failed (any; ANTS-5043). A demoted run
// reverts to a full scan (INV-6).
Resolution resolveChangedFiles(const QString &canonProject,
                               const QString &scope,
                               const QString &priorCommit);

}  // namespace AuditScope
