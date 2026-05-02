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
#include <QString>
#include <functional>

#include "claudeintegration.h"   // ClaudeState — member-typed

class QStatusBar;
class QPushButton;
class QLabel;
class QProgressBar;
class QTabWidget;
class ClaudeTabTracker;
class ColoredTabBar;
class ClaudeBgTaskTracker;
class ClaudeTaskListTracker;
class TerminalWidget;

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

    // Provider injection — Qt-idiomatic; matches the existing
    // ClaudeIntegration::set*Provider pattern.
    void setCurrentTerminalProvider(std::function<TerminalWidget *()>);
    void setFocusedTerminalProvider(std::function<TerminalWidget *()>);
    void setTerminalAtTabProvider(std::function<TerminalWidget *(int)>);
    void setTabIndicatorEnabledProvider(std::function<bool()>);

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

private:
    void apply();   // private status-label renderer (formerly
                    // MainWindow::applyClaudeStatusLabel)

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
    ClaudeBgTaskTracker *m_bgTasks = nullptr;
    QPushButton         *m_bgTasksBtn = nullptr;

    // ANTS-1158 — Claude Code task-list chip (TodoWrite snapshot
    // OR TaskCreate / TaskUpdate replay of the focused tab's
    // session JSONL). Sibling to m_bgTasks; one tracker per
    // controller, retargeted on tab switch.
    ClaudeTaskListTracker *m_tasks = nullptr;
    QPushButton           *m_tasksBtn = nullptr;

    // Render state — formerly the m_claude{LastState, LastDetail,
    // PromptActive, PlanMode, Auditing} fivesome on MainWindow.
    ClaudeState m_lastState = ClaudeState::NotRunning;
    QString     m_lastDetail;
    bool        m_promptActive = false;
    bool        m_planMode = false;
    bool        m_auditing = false;

    // Provider callbacks — set by MainWindow before attach().
    std::function<TerminalWidget *()>      m_currentTerminalProvider;
    std::function<TerminalWidget *()>      m_focusedTerminalProvider;
    std::function<TerminalWidget *(int)>   m_terminalAtTabProvider;
    std::function<bool()>                  m_tabIndicatorEnabledProvider;
};
