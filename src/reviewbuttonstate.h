#pragma once

#include <QByteArray>
#include <QList>

namespace ants {

// Decision for the bottom-bar "Review Changes" button, derived from
// `git status --porcelain=v1 -b` output.
//   dirty → the worktree has content to review (tracked changes OR
//           untracked new files).
//   ahead → the local branch has unpushed commits.
// The button is shown for any git repo and enabled iff (dirty || ahead).
struct ReviewButtonState {
    bool dirty = false;
    bool ahead = false;
};

// ANTS-1874 — untracked entries ('?? path') now count as dirty. The
// earlier carve-out (user report 2026-05-08) skipped them because the
// diff viewer would open with nothing to show; ANTS-1886 made the
// viewer render new files as synthetic addition diffs, so a repo whose
// only change is a brand-new file Claude just wrote is now genuinely
// reviewable — the button must light up for it.
inline ReviewButtonState parseReviewPorcelain(const QByteArray &raw) {
    ReviewButtonState st;
    const QList<QByteArray> lines = raw.split('\n');
    for (const QByteArray &ln : lines) {
        if (ln.isEmpty()) continue;
        if (ln.startsWith("##")) {
            // Branch header. "ahead N" means local has unpushed commits;
            // "behind N" alone is nothing to review (nothing to push).
            if (ln.contains("[ahead ") || ln.contains(", ahead "))
                st.ahead = true;
        } else {
            // Any porcelain entry — tracked (M/A/D/R…) or untracked
            // ('??') — means there is something to review.
            st.dirty = true;
        }
    }
    return st;
}

}  // namespace ants
