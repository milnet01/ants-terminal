#pragma once

#include "claudeintegration.h"

#include <QObject>
#include <QTimer>
#include <QFileSystemWatcher>
#include <QHash>
#include <sys/types.h>

// Tracks Claude Code state for N shells concurrently.
//
// The singleton ClaudeIntegration describes *one* shell at a time (the
// active tab's) and re-targets on tab switch — so its m_state always
// reflects whichever tab is focused. That's enough for the bottom
// status bar, but a user running Claude in three tabs gets no glyph on
// the inactive two. This class fills the gap: one entry per shell,
// each with its own poll counter and transcript watcher, emitting
// shellStateChanged(shellPid) whenever a shell's Claude state
// transitions.
//
// Key is shell PID (not tab index) because tab indices shift under
// reorder / close while a shell PID is stable for the shell's lifetime.
// Callers map shellPid → tabIndex on demand.
//
// Pool resource profile: one QFileSystemWatcher path per active Claude
// session. Inotify cost is ~1 KB kernel memory per path; a user would
// need hundreds of tabs before this mattered.
class ClaudeTabTracker : public QObject {
    Q_OBJECT

public:
    struct ShellState {
        ClaudeState state = ClaudeState::NotRunning;
        QString tool;              // non-empty only when state == ToolUse
        bool planMode = false;
        // Overrides the transcript-derived state with AwaitingInput when
        // true. Set by markShellAwaitingInput on PermissionRequest hook,
        // cleared when the user resolves the prompt. Underlying state
        // preserved so reverting keeps context.
        bool awaitingInput = false;
        // /audit skill in flight, derived from the most recent user
        // message in the transcript tail. Spans many tool-use turns —
        // surfaced both on the per-tab dot (Auditing glyph) and on the
        // bottom status-bar label.
        bool auditing = false;
    };

    explicit ClaudeTabTracker(QObject *parent = nullptr);
    ~ClaudeTabTracker() override;

    // Start tracking a shell. shellPid <= 0 is a no-op. Idempotent — a
    // second call with the same pid refreshes state without creating a
    // duplicate entry.
    void trackShell(pid_t shellPid);
    void untrackShell(pid_t shellPid);

    // Flag a shell as awaiting user input (permission prompt active).
    // Overlaid on top of whatever state the transcript parser derives.
    void markShellAwaitingInput(pid_t shellPid, bool awaiting);

    // Read per-shell state. Default-constructed (NotRunning) for
    // untracked PIDs.
    ShellState shellState(pid_t shellPid) const;

    // Reverse-lookup: which shell PID owns the Claude session with this
    // session_id? Session id comes from the JSONL transcript filename
    // (the basename minus the .jsonl extension) — Claude Code writes
    // `~/.claude/projects/<project>/<session-uuid>.jsonl`. Returns 0 if
    // no tracked shell owns that session. Used by the PermissionRequest
    // hook handler to flag the right tab.
    pid_t shellForSessionId(const QString &sessionId) const;

    // Number of currently tracked shells (for tests + introspection).
    int trackedCount() const { return m_shells.size(); }

    // Test-seam: force a transcript re-parse for a shell. In production
    // the watcher + poll path drives parses; tests inject a synthetic
    // transcript via the second argument to bypass proc-walking. If
    // transcriptPath is empty the tracker re-parses whatever path was
    // already stored (production path — e.g. after an atomic rewrite
    // where the watcher needs a nudge).
    void forceRefreshForTest(pid_t shellPid, const QString &transcriptPath = {});

signals:
    // Emitted whenever a tracked shell's state changes (transcript-
    // derived state, plan-mode latch, tool name, or awaitingInput).
    // Callers look up shellState(pid) for the new value.
    void shellStateChanged(pid_t shellPid);

private slots:
    void pollAllShells();

private:
    struct ShellEntry {
        pid_t shellPid = 0;
        pid_t claudePid = 0;
        QString transcriptPath;
        ShellState state;
    };

    void detectClaudeChild(ShellEntry &entry);
    void reparseTranscript(pid_t shellPid);
    void releaseWatch(const QString &path);
    void maybeEmit(pid_t shellPid, const ShellState &before);

    QHash<pid_t, ShellEntry> m_shells;
    QTimer m_pollTimer;
    QFileSystemWatcher m_watcher;
    // Reverse map: transcript path → owning shell pid, so the single
    // shared QFileSystemWatcher can route fileChanged signals to the
    // right entry without a linear scan.
    QHash<QString, pid_t> m_pathToShell;
};
