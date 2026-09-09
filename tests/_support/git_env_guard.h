// ANTS-3841 — neutralise an inherited git environment before any test runs.
//
// Measured 2026-08-06 during ANTS-3833: with GIT_DIR exported, every
// git-fixture test operates on whatever repo that variable names rather than
// its own temp fixture. `git init`, `git commit`, `git checkout` and `git
// branch` all land on the real repository. That session rewrote local `main`
// onto fixture commits, created four stray branches and left thousands of
// staged deletions. origin was untouched and reflog recovered it, but nothing
// in the suite warned.
//
// This is a SAFETY guard, not tidiness: a test run must not be able to destroy
// the developer's history. It sits in the two bundle mains, beside the
// XDG_DATA_HOME (ANTS-3856) and XDG_CONFIG_HOME (ANTS-4898) sandboxes, because
// that is the one place every C++ test bundle passes through. A test wanting a
// specific git environment still sets it inside its own fixture.
//
// Deliberately POSIX rather than qputenv/qunsetenv: both bundle mains include
// it, one of them before QCoreApplication exists, and unsetenv needs no Qt.
#pragma once

#include <cstdlib>

namespace ants_test {

// The variables that redirect git's notion of WHICH repository it is operating
// on. Scoped to those: a variable that only changes git's behaviour inside the
// repo it was already given (GIT_AUTHOR_*, GIT_CONFIG_*) cannot send a write
// somewhere else, and the fixtures set the identity ones themselves.
inline void scrubInheritedGitEnv()
{
    static const char *const kRedirectingVars[] = {
        "GIT_DIR",          // names the repository outright
        "GIT_WORK_TREE",    // names the checkout the index applies to
        "GIT_INDEX_FILE",   // git sets this one for its own hooks, relatively
        "GIT_COMMON_DIR",   // the shared dir a worktree resolves refs through
    };
    for (const char *name : kRedirectingVars)
        ::unsetenv(name);
}

}  // namespace ants_test
