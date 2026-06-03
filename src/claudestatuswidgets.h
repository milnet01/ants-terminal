#pragma once

// ClaudeStatusBarController — owns every Claude-specific status-bar
// widget + the per-session render state. Extracted from
// mainwindow.cpp by ANTS-1146 (Bundle G Tier 3, sibling to
// ANTS-1145's DiffViewerDialog carve-out). See
// docs/specs/ANTS-1146.md for the full design rationale, the
// 12-row external touch-site rewrite map, and the INV-9 per-test
// re-pointing table.
//
// Top-level class (no namespace) — matches the codebase convention
// used by ClaudeIntegration / ClaudeTabTracker / ClaudeAllowlistDialog
// / DiffViewerDialog so connect-PMF call sites in mainwindow.cpp
// stay unqualified.

#include <QObject>
#include <QHash>
#include <QPointer>   // ANTS-1920 — pollModelSwitchConfirm parameter type
#include <QString>
#include <functional>

#include "claudeintegration.h"   // ClaudeState — member-typed
#include "modelrecommender.h"   // ANTS-1893 — ModelRecommender::Tier member-typed
                                //   (m_undoSwitchPendingFromTierEnum + emitSwitchSurfacing
                                //   parameter types).

class QStatusBar;
class QPushButton;
class QLabel;
class QProgressBar;
class QTabWidget;
class QTimer;
class ClaudeTabTracker;
class ColoredTabBar;
class ClaudeBgTaskTracker;
class ClaudeTaskListTracker;
class TerminalWidget;

// ANTS-1928 — modelautoswitch.h is now included directly (was forward-decl'd
// for the maybeEmitNearMiss/emitSwitchSurfacing by-ref params). The original
// avoidance — "would pull modelrecommender.h into every consumer" — is moot:
// modelrecommender.h is already included above (line 22) and is the only
// header modelautoswitch.h adds beyond claudeintegration.h (also already in).
// The StabilityState member (ANTS-1928) is held by value and needs the
// complete type, which a forward declaration cannot provide.
#include "modelautoswitch.h"

class ClaudeStatusBarController : public QObject {
    Q_OBJECT
public:
    explicit ClaudeStatusBarController(QStatusBar *statusBar,
                                       QObject *parent);

    // Wiring (called once from MainWindow ctor — services are
    // owned by MainWindow, the controller observes them).
    void attach(ClaudeIntegration *integration, ClaudeTabTracker *tracker, ColoredTabBar *coloredTabBar, QTabWidget *tabWidget);

    // State pokes (MANUAL — from MainWindow's terminal-event
    // handlers; auto state from ClaudeIntegration::stateChanged /
    // planModeChanged / auditingChanged is consumed by the
    // controller's own connect blocks set up in attach()).
    void setPromptActive(bool active);
    void setPlanMode(bool active);
    void setAuditing(bool active);

    // Error-label surface (used by MainWindow's commandFailed
    // handler to surface "Exit N" with output tooltip).
    void setError(const QString &text, const QString &tooltip, int autoHideMs);
    void clearError();

    // Tab-switch reset, called from onTabChanged. Bundles the
    // five-line clear-and-hide block at mainwindow.cpp:4604-4612
    // that today directly mutates m_claudeLastState,
    // m_claudeLastDetail, m_claudePlanMode, m_claudeAuditing,
    // hides three widgets, and resets the bg-tasks transcript path.
    // Single entry point keeps the reset atomic.
    void resetForTabSwitch();

    // Theme application — REPLACES MainWindow::updateClaudeThemeColors.
    // Stores the theme name on the controller so the contextUpdated
    // handler can read it on every percentage tick.
    void applyTheme(const QString &themeName);

    // External refresh entry-points called from MainWindow's
    // tab/timer plumbing (status timer pulse, onTabChanged,
    // showBgTasksDialog dismissal). Body reads focusedTerminal()
    // + m_claudeIntegration (both already supplied) + internal
    // tracker state.
    void refreshBgTasksButton();

    // ANTS-1158 — task-list chip refresh. Called from the same
    // status-bar tick + tab-switch sites as refreshBgTasksButton.
    // Re-targets m_tasks at the focused tab's transcript path,
    // hides m_tasksBtn when the parsed list is empty, otherwise
    // shows the "<unfinished>/<total>" label.
    void refreshTasksButton();
    void refreshModelChip();   // ANTS-1226

    // ANTS-1888 — Passive per-tab readout of the focused tab's current
    // Claude model + last-used thinking level. Pairs with the recommender
    // chip's INV-14 suppression (when the auto-switcher is on the
    // recommender hides; this chip stays visible so the user still knows
    // what model is in use). Hides entirely when no transcript is found
    // for the focused tab; hides only the thinking half when the level is
    // undetectable. Reuses ModelRecommender::tierFromModelId +
    // thinkingLevelFromLatestUserTurn.
    void refreshModelStateChip();

    // ANTS-1735 §2.3 — autonomous switcher tick. Reads the focused
    // tab's tracker entry, builds a ModelAutoSwitch::Gate, calls
    // decide(), and on act: injects `/model <tier>\n` into the focused
    // terminal + appends a ledger record. Default-off via
    // Config::claudeAutoModel().switch_enabled (INV-14).
    void refreshAutoModelSwitch();

    // ANTS-1951 — auto-confirm a "Switch model?" dialog the user raised by
    // typing /model directly (no Ants-initiated handshake is polling for it).
    // Same 2 s tick as refreshAutoModelSwitch but independent of the auto-switch
    // master toggle — a user who types /model wants the prompt confirmed
    // regardless. Sends ENTER, then a continuation prompt only when auto mode
    // is on + a turn is active (ANTS-1969, via sendUnarmedConfirm).
    // Gated by ModelAutoSwitch::shouldAutoConfirmUnarmedSwitch + the
    // claude.auto_model_confirm_user_switch config key.
    // When the dialog has not yet rendered at tick time, arms a short polling
    // burst (pollUnarmedSwitchConfirm) so it is caught within
    // kSwitchConfirmPollMs rather than waiting a full 2-s tick cycle.
    void maybeAutoConfirmUserModelSwitch();

    // ANTS-1955 — one poll step of the burst started by
    // maybeAutoConfirmUserModelSwitch when the dialog had not rendered yet.
    // Mirrors pollModelSwitchConfirm (auto-switch path) but without setting
    // m_modelHandshakeInFlight (this path does not own the dialog).
    void pollUnarmedSwitchConfirm(int attempt);

    // ANTS-1969 — confirm a user-typed /model dialog: press ENTER, then inject
    // the continuation prompt when shouldContinueAfterUnarmedConfirm holds (auto
    // mode on + an active turn to resume). Shared by both unarmed-confirm sites
    // so the auto-mode + billing-safety gate lives in one place.
    void sendUnarmedConfirm(TerminalWidget *term);

    // ANTS-1735 §2.5 — outcome fill-in tick. Scans the global ledger for
    // pending records, finds the matching transcript per record's project,
    // parses post-switch turns, runs `computeOutcome`, and writes back the
    // ledger if anything changed. Idempotent; cheap when there are no
    // pending records. Wired to the same 2 s status timer, internally
    // throttled to ≤ once per kPendingFillIntervalMs.
    void fillPendingLedgerOutcomes();

    // ANTS-1890 — Path-injecting overload for behavioural tests. Same
    // outcome-fill contract as the no-arg form except (a) the ledger path
    // is supplied directly (defaultLedgerPath() is process-global and
    // unsuitable for tests), and (b) the 30 s throttle is bypassed
    // unconditionally so tests don't have to wait between calls. The
    // throttle state (`m_lastPendingFillMs`) lives only inside the
    // no-arg overload above — this path overload never reads or writes
    // it. ANTS-1891 — `nowMs` is the clock seam for `computeOutcome`'s
    // quiet-window settlement (defaults to 0 = pre-1891 behaviour;
    // production caller passes a real `nowMs`).
    void fillPendingLedgerOutcomes(const QString &ledgerPath,
                                   qint64 nowMs = 0);

    // ANTS-1890 — Bootstrap (restart-safety): seed
    // m_lastOverrideMsByProject from any settled `userOverrideWithin5`
    // records already on disk. Called once from attach() after services
    // are wired but before the 2 s status timer fires its first tick.
    // Public so the behavioural test can inject a fixture path —
    // defaultLedgerPath() is process-global and unsuitable for tests.
    void seedOverrideCacheFromLedger(const QString &ledgerPath);

    // ANTS-1890 — Test seam. Returns the cached ms-timestamp for
    // `project` (the most-recent userOverrideWithin5 record's ts), or
    // -1 if the project key is not in the cache. Pure, const, O(1)
    // (QHash lookup). Lives on the public surface alongside the
    // refresh/fill helpers; behavioural tests in
    // tests/features/model_auto_switch_outcome_fillin/ assert the
    // value after a seed or fill-in call.
    qint64 lastOverrideMsForProject(const QString &project) const;

private:
    // Sentinel returned by msSinceLastSwitch when m_autoSwitchLastMs==0
    // (no switch yet). Large enough to clear any configured min-dwell.
    static qint64 kMaxDwellSentinel();
    // Outcome fill-in tick cadence — runs at most every 30 s. Each call
    // rescans the ledger, so the 2 s tick would be wasteful.
    static constexpr qint64 kPendingFillIntervalMs = 30'000;
    qint64 m_lastPendingFillMs = 0;

    // ANTS-1890 — per-project override cool-down cache. Keyed by the
    // ledger record's `project` field (== focused tab's shellCwd() at
    // switch time). Populated at controller bootstrap from any settled
    // records on disk (seedOverrideCacheFromLedger) and incrementally
    // by fillPendingLedgerOutcomes after each userOverrideWithin5
    // settles. Read by refreshAutoModelSwitch to populate
    // gate.msSinceLastOverride. Single-digit-KB even for a power user
    // with 50 projects (24-48 bytes per entry).
    QHash<QString, qint64> m_lastOverrideMsByProject;

public:

    // Provider injection — Qt-idiomatic; matches the existing
    // ClaudeIntegration::set*Provider pattern.
    void setCurrentTerminalProvider(std::function<TerminalWidget *()>);
    void setFocusedTerminalProvider(std::function<TerminalWidget *()>);
    void setTerminalAtTabProvider(std::function<TerminalWidget *(int)>);
    void setTabIndicatorEnabledProvider(std::function<bool()>);

    // ANTS-1053 — per-shell bg-task tracker lifecycle. Call trackBgShell
    // when a terminal tab opens (same site as ClaudeTabTracker::trackShell)
    // and untrackBgShell when it closes. Each shell gets its own
    // ClaudeBgTaskTracker so background tabs retain their task state
    // across tab switches without forcing a 16 MiB transcript re-parse.
    void trackBgShell(pid_t pid);
    void untrackBgShell(pid_t pid);

    // Accessors (inline non-virtual; legacy MainWindow paths such
    // as refreshReviewButton + showDiffViewer use these rather
    // than re-acquiring direct member references).
    QPushButton *reviewButton() const;
    QPushButton *bgTasksButton() const;
    ClaudeBgTaskTracker *bgTasksTracker() const;
    QPushButton *tasksButton() const;
    ClaudeTaskListTracker *tasksTracker() const;

signals:
    void reviewClicked();
    void bgTasksClicked();
    void tasksClicked();
    void allowlistRequested(const QString &rule);
    void reviewButtonShouldRefresh();
    void statusMessageRequested(const QString &text, int timeoutMs);
    void statusMessageCleared();
    // ANTS-1735 §8 OQ-3 — fired at most once per process when the
    // controller detects Claude Code running in the focused tab while
    // the auto-model switch is still default-off AND the nudge-shown
    // flag is false. MainWindow shows a one-shot opt-in prompt and
    // (regardless of the user's answer) sets the nudge-shown flag so
    // it never fires again.
    void firstRunNudgeRequested();

public:
    // ANTS-1851 — re-paint a still-pending permission prompt's bottom-bar
    // message + Allow/Deny buttons when the user switches TO the tab that
    // owns it. Called from MainWindow::refreshStatusBarForActiveTab AFTER
    // its Category-C anchor teardown. No-op unless the focused shell's
    // tracker entry has awaitingInput && a retained rule.
    void maybeShowPromptForActiveTab(pid_t focusedPid);

    // ANTS-1852 — tear down the bottom-bar permission-prompt anchors on a
    // tab switch, but KEEP a background tab's still-pending anchor alive
    // (hidden) so its owning-terminal retraction wiring survives to clear
    // the tab's awaiting-input dot if the prompt resolves while another tab
    // is focused. Replaces the blanket findChildren->deleteLater the refresh
    // used to do inline. `newlyFocusedPid` is the shell of the tab being
    // switched TO (0 during teardown → delete everything).
    void clearPromptAnchorsForTabSwitch(pid_t newlyFocusedPid);

private slots:
    // ANTS-1893 — Undo handler. Refuses on cwd or shellPid mismatch
    // (per-tab guard); on a dead shellPid distinguishes "session
    // ended" from "different session". On success, injects
    // `/model <fromTier>\n` and seeds m_lastOverrideMsByProject so
    // the gate's 10-min cool-down (ANTS-1890) trips on the next tick.
    void onUndoSwitchClicked();

private:
    void apply();   // private status-label renderer (formerly
                    // MainWindow::applyClaudeStatusLabel)

    // ANTS-1924 / ANTS-1920 — shared model-switch PTY handshake. After a
    // caller injects `/model <tier>\r`, this confirms CC's "Switch model?"
    // dialog and then sends a configurable continuation prompt so CC resumes
    // without user input. ANTS-1920 replaced the original blind timer (ESC@250
    // + \r@400) with an output-driven confirm: it polls recentOutput for the
    // dialog and presses ENTER only once it is visible, aborting (no blind CR)
    // if the prompt never renders within the budget.
    void performModelSwitchHandshake(TerminalWidget *focused);

    // ANTS-1920 — one poll tick of the output-driven confirm. Re-arms itself
    // via singleShot until the dialog is seen (→ send \r + continuation) or the
    // budget (kSwitchConfirmMaxPolls) is exhausted (→ send a single ESC to
    // clear any stranded `/model`, no continuation). `g` may dangle if the tab
    // closes mid-poll; guarded each tick. `cont` is captured once at handshake
    // start so an in-flight poll uses a stable continuation prompt.
    void pollModelSwitchConfirm(QPointer<TerminalWidget> g,
                                const QString &cont, int attempt);

    // ANTS-1915 — fire a deferred manual chip-switch when its owning shell
    // transitions to Idle. No-op unless a deferral is pending for `shellPid`
    // AND that shell is now Idle AND its terminal still exists. Resolves the
    // terminal by shellPid (not focus) so the switch lands on the right PTY
    // even if the user moved to another tab; seeds the ANTS-1890 override
    // cool-down so the auto-switcher does not immediately undo it.
    void maybeFireDeferredChipSwitch(pid_t shellPid);

    // ANTS-1835/1850/1851 — build (or rebuild) the permission-prompt UI for
    // an owning shell: per-shell dedup, lifecycle anchor, Allow/Deny/Add
    // buttons (only when belongsToFocused), and retraction wiring scoped to
    // the owning terminal. Shared by the permissionRequested slot and the
    // tab-switch rebuild path so both stay byte-identical.
    void showPermissionPrompt(pid_t awaitingPid, bool belongsToFocused,
                              const QString &rule);

    QStatusBar          *m_statusBar = nullptr;
    QString              m_currentThemeName;

    // Services — supplied via attach(), owned by MainWindow.
    ClaudeIntegration   *m_integration = nullptr;
    ClaudeTabTracker    *m_tracker = nullptr;
    ColoredTabBar       *m_coloredTabBar = nullptr;
    QTabWidget          *m_tabWidget = nullptr;

    // Widgets — owned by the controller (parented to m_statusBar
    // via addPermanentWidget).
    QLabel              *m_statusLabel = nullptr;
    QProgressBar        *m_contextBar = nullptr;
    QPushButton         *m_reviewBtn = nullptr;
    QLabel              *m_errorLabel = nullptr;
    // Owned auto-hide timer for m_errorLabel. Without an owned timer,
    // a second setError() during the first's window would leave the
    // first QTimer::singleShot in flight, hiding the second message
    // ~early. Cancellable on re-entry.
    QTimer              *m_errorHideTimer = nullptr;
    // ANTS-1053 — one tracker per shell PID; replaces the single m_bgTasks.
    // Each entry owns a QFileSystemWatcher on its own transcript, so
    // background tabs stay up-to-date without re-parsing on tab switch.
    QHash<pid_t, ClaudeBgTaskTracker *> m_bgTrackers;
    QPushButton         *m_bgTasksBtn = nullptr;

    // ANTS-1158 — Claude Code task-list chip (TodoWrite snapshot
    // OR TaskCreate / TaskUpdate replay of the focused tab's
    // session JSONL). Sibling to m_bgTrackers; one tracker per
    // controller, retargeted on tab switch.
    ClaudeTaskListTracker *m_tasks = nullptr;
    QPushButton           *m_tasksBtn = nullptr;
    QPushButton           *m_modelBtn = nullptr;  // ANTS-1226
    // ANTS-1787 — mtime short-circuit for refreshModelChip: skip the
    // ≤512 KB tail-read + scoring on every 2 s tick when the transcript
    // hasn't changed since the last score.
    QString                m_modelChipPath;
    qint64                 m_modelChipMtimeMs = -1;

    // ANTS-1888 — Passive model + thinking-level readout. Sibling of
    // m_modelBtn; placed immediately after it in addPermanentWidget order
    // so when INV-14 hides the recommender chip, this chip occupies the
    // same visual slot.
    QPushButton           *m_modelStateBtn = nullptr;
    QString                m_modelStatePath;
    qint64                 m_modelStateMtimeMs = -1;
    // ANTS-1926 — tracks last-rendered pending tier so the mtime short-circuit
    // can be bypassed when pending state changes independently of the transcript.
    QString                m_modelStateLastPendingTier;

    // ANTS-1735 §2.3 actuator state. Lives on the controller (one set
    // per window — the gate runs on the focused tab's read, so there's
    // no per-tab stab to track here). msSinceLastSwitch derived as
    // (now - m_autoSwitchLastMs); zero = never switched.
    // ANTS-1928 — stability accrual (ticksStable + the ANTS-1925 reset-
    // hysteresis counter + the tier-lock window candidate) is now a single
    // pure-advanced value. advanceStability() folds each tick's clamped
    // recommendation in; gate.ticksTargetStable reads .ticksStable. Reset to
    // a default-constructed value on every actual switch fire.
    ModelAutoSwitch::StabilityState m_autoSwitchStability;
    // ANTS-1919 — pending-switch intent. Set (to the target tier name) when
    // decide() is blocked ONLY by composer_not_empty — all other guards pass.
    // The natural 2 s tick fires the queued switch as soon as the composer
    // empties. Cleared on any actual switch or when the recommendation changes
    // away from the pending tier. Read by the status-bar to show a "pending"
    // annotation on the model chip.
    QString m_autoSwitchPendingTier;
    qint64 m_autoSwitchLastMs = 0;
    QString m_autoSwitchLastTier;          // last tier we injected; ledger +
                                           // ANTS-1944 gate reconciliation anchor
    // ANTS-1915 — deferred manual chip-switch. When the user clicks the model
    // chip while Claude is mid-generation, sending `/model` immediately would
    // sit unsubmitted until the user presses Escape (interrupting the turn).
    // Instead we record the requested tier + owning shell PID here and fire the
    // switch when that shell next transitions to Idle (turn complete). Empty
    // tier = nothing deferred. Cleared on fire, or dropped if the tab closes.
    QString m_deferredChipTier;
    pid_t   m_deferredChipShellPid = 0;
    // ANTS-1735 §8 OQ-3 — per-process latch so the first-run nudge fires
    // at most once even before MainWindow gets a chance to flip the
    // persistent claude.auto_model_nudge_shown flag.
    bool   m_firstRunNudgeEmitted = false;

    // ANTS-1951 — auto-confirm coordination. m_modelHandshakeInFlight is true
    // while performModelSwitchHandshake/pollModelSwitchConfirm is polling for an
    // Ants-initiated dialog, so the user-typed-/model path stands down (no
    // double ENTER). m_unarmedSwitchConfirmed latches a single ENTER per dialog
    // instance; cleared once switchConfirmVisible goes false again.
    // ANTS-1955 — m_unarmedPollActive is true while pollUnarmedSwitchConfirm
    // is running its burst; prevents the 2-s tick from stacking a second burst.
    // ANTS-1959 — track when the focused tab entered ToolUse so decide()
    // can compute toolUseElapsedMs. Reset to 0 when state leaves ToolUse.
    qint64 m_toolUseSinceMs = 0;

    bool   m_modelHandshakeInFlight = false;
    bool   m_unarmedSwitchConfirmed = false;
    bool   m_unarmedPollActive      = false;

    // ANTS-1894 — near-miss telemetry throttle. Keyed by the focused tab's
    // project root (== shellCwd()). m_nearMissLastSigByProject holds the
    // last emitted (post-sort) blocked_by signature per project;
    // m_nearMissLastEmitMsByProject holds the wall-clock ts of the last
    // emit per project. Process-local, not persisted — rebuilt from disk-
    // less state on relaunch. Bounded by the number of distinct
    // shellCwd() values seen this process (~1 KiB for 10 projects).
    QHash<QString, QStringList> m_nearMissLastSigByProject;
    QHash<QString, qint64>      m_nearMissLastEmitMsByProject;
    static constexpr qint64     kNearMissEmitFloorMs = 5'000;   // 5 s per project

    // ANTS-1940 — regret-driven conservatism cache. Reading + parsing the
    // firing ledger every 2 s tick is wasteful, so the computed dwell
    // multiplier is cached per project root with a TTL. Recomputed lazily on
    // the first tick past the TTL. Process-local; same bound as the near-miss
    // hashes above (~1 KiB for 10 projects).
    QHash<QString, double>      m_conservatismMultByProject;
    QHash<QString, qint64>      m_conservatismStampMsByProject;
    static constexpr qint64     kConservatismTtlMs = 60'000;    // 60 s per project

    // ANTS-1893 — switch-event surfacing state. Owned by the controller,
    // populated by emitSwitchSurfacing on each live auto-switch firing.
    // Per-tab guard via shellPid: two splits of the same repo share
    // shellCwd() but have distinct shellPid(); without the PID match an
    // Undo click in tab B would inject /model into tab B's PTY even
    // though the switch fired on tab A. The cached enum avoids a
    // tierFromModelId() round-trip on the bare alias string.
    QPushButton           *m_undoSwitchBtn = nullptr;
    QTimer                *m_undoSwitchHideTimer = nullptr;  // single-shot
    QTimer                *m_modelStatePulseTimer = nullptr; // single-shot
    QString                m_undoSwitchPendingFromTier;      // empty = no pending
    QString                m_undoSwitchPendingProject;       // == shellCwd() at switch
    pid_t                  m_undoSwitchPendingShellPid = 0;  // == shellPid() at switch
    ModelRecommender::Tier m_undoSwitchPendingFromTierEnum =
        ModelRecommender::Tier::Sonnet;                      // cached enum
    static constexpr int   kSwitchToastTimeoutMs = 6'000;    // 6 s
    static constexpr int   kModelChipPulseMs     = 600;      // 600 ms
    static constexpr int   kUndoVisibleMs        = 10'000;   // 10 s
    // ANTS-1920 — output-driven model-switch confirm tuning. Poll the
    // terminal tail every kSwitchConfirmPollMs for the "Switch model?"
    // dialog, up to kSwitchConfirmMaxPolls times (~2 s budget — comfortably
    // past the old blind 400 ms confirm + a slow render). On confirm, the
    // continuation prompt fires kSwitchContinuationDelayMs later so CC has
    // applied the switch first. Scan only the last kSwitchConfirmScanLines
    // grid rows: the dialog is a live bottom-anchored TUI element, so a stale
    // confirm from an earlier switch has scrolled out of this window.
    static constexpr int   kSwitchConfirmPollMs        = 120;     // 120 ms
    static constexpr int   kSwitchConfirmMaxPolls      = 16;      // ~1.9 s
    static constexpr int   kSwitchContinuationDelayMs  = 600;     // 600 ms
    static constexpr int   kSwitchConfirmScanLines     = 12;

public:
    // ANTS-1893 — fire the firing-side surfacing (toast + chip-pulse +
    // Undo button) immediately after a live auto-switch sends `/model`
    // to the PTY. Public to mirror the maybeEmitNearMiss test-seam
    // precedent below — behavioural tests under
    // tests/features/auto_switch_surfacing/ drive it directly. Sole
    // production caller is the `if (dec.act)` branch of
    // refreshAutoModelSwitch (the `mode=="on"` firing path); under
    // ANTS-1895 the dry-run branch will reuse this helper too.
    void emitSwitchSurfacing(ModelRecommender::Tier fromTier,
                             ModelRecommender::Tier toTier,
                             const QString &scoreReason,
                             TerminalWidget *focused,
                             const QString &projectRoot,
                             qint64 nowMs);

    // ANTS-1894 — emit a near-miss record on signature change (INV-5 / INV-6).
    // Called from refreshAutoModelSwitch's `if (!dec.act)` branch with the
    // gate snapshot, decision (must have non-empty blockedBy), project root,
    // and wall-clock nowMs. Public as a test seam — behavioural tests in
    // tests/features/model_near_miss_ledger/ drive it directly to verify
    // the signature-change and throttle-floor rules without standing up a
    // full controller. Sole producer of records in
    // model-switch-nearmiss.jsonl. Path override used only by tests; the
    // production caller (refreshAutoModelSwitch) omits it and the helper
    // writes to ModelNearMissLedger::defaultLedgerPath().
    void maybeEmitNearMiss(const ModelAutoSwitch::Decision &dec,
                           const ModelAutoSwitch::Gate     &gate,
                           const QString                   &projectRoot,
                           qint64                           nowMs,
                           const QString                   &ledgerPathOverride = QString());

    // ANTS-1940 — lazily recompute the per-project conservatism dwell
    // multiplier at most once per kConservatismTtlMs (60 s). Reads the
    // firing ledger; result cached in m_conservatismMultByProject.
    double conservatismMultiplierFor(const QString &projectRoot,
                                     qint64         nowMs,
                                     bool           isMechanical);

    // ANTS-1854 — last emitted diagnostic-line signatures for the two
    // 2 s poll refreshers. The Claude debug lane wrote a bgtasks +
    // tasks line on EVERY tick (≈25 k no-op lines / 5.7 MB across a
    // few sessions), burying the rare `mcp dispatch` lines a reviewer
    // actually needs. Gate each poll line on a state-transition: log
    // only when the signature differs from the last one emitted. Empty
    // sentinel forces the first line through.
    QString                m_lastBgTasksLogSig;
    QString                m_lastTasksLogSig;

    // ANTS-1873 — the five cached scalars (m_lastState, m_lastDetail,
    // m_promptActive, m_planMode, m_auditing) used to mirror the focused
    // tab's state via integration signals + manual MainWindow pokes;
    // they drifted from the tab dot's per-tab tracker entry in both
    // directions. apply() now reads the focused tab's tracker entry
    // directly via claudestate::forFocused, so the cache is deleted and
    // the bar can no longer disagree with the dot by construction.

    // Provider callbacks — set by MainWindow before attach().
    std::function<TerminalWidget *()>      m_currentTerminalProvider;
    std::function<TerminalWidget *()>      m_focusedTerminalProvider;
    std::function<TerminalWidget *(int)>   m_terminalAtTabProvider;
    std::function<bool()>                  m_tabIndicatorEnabledProvider;
};
