#include "claudestatuswidgets.h"

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>

// Skeleton — Task 2 of docs/plans/ANTS-1146.md. Public surface
// declared and constructible; bodies migrate from mainwindow.cpp
// in Task 3. INV-1 + INV-2a/2b pass against this file as soon
// as the header signatures match the spec; INV-3 onwards still
// fails (no migrated user-visible strings yet).

ClaudeStatusBarController::ClaudeStatusBarController(QStatusBar *statusBar,
                                                     QObject *parent)
    : QObject(parent), m_statusBar(statusBar) {}

void ClaudeStatusBarController::attach(ClaudeIntegration *integration,
                                       ClaudeTabTracker *tracker,
                                       ColoredTabBar *coloredTabBar,
                                       QTabWidget *tabWidget) {
    m_integration = integration;
    m_tracker = tracker;
    m_coloredTabBar = coloredTabBar;
    m_tabWidget = tabWidget;
}

void ClaudeStatusBarController::setPromptActive(bool active) {
    m_promptActive = active;
    apply();
}

void ClaudeStatusBarController::setPlanMode(bool active) {
    m_planMode = active;
    apply();
}

void ClaudeStatusBarController::setAuditing(bool active) {
    m_auditing = active;
    apply();
}

void ClaudeStatusBarController::setError(const QString & /*text*/,
                                         const QString & /*tooltip*/,
                                         int /*autoHideMs*/) {}

void ClaudeStatusBarController::clearError() {}

void ClaudeStatusBarController::resetForTabSwitch() {
    m_lastState = ClaudeState::NotRunning;
    m_lastDetail.clear();
    m_planMode = false;
    m_auditing = false;
    apply();
}

void ClaudeStatusBarController::applyTheme(const QString &themeName) {
    m_currentThemeName = themeName;
}

void ClaudeStatusBarController::refreshBgTasksButton() {}

void ClaudeStatusBarController::setCurrentTerminalProvider(
        std::function<TerminalWidget *()> p) {
    m_currentTerminalProvider = std::move(p);
}

void ClaudeStatusBarController::setFocusedTerminalProvider(
        std::function<TerminalWidget *()> p) {
    m_focusedTerminalProvider = std::move(p);
}

void ClaudeStatusBarController::setTerminalAtTabProvider(
        std::function<TerminalWidget *(int)> p) {
    m_terminalAtTabProvider = std::move(p);
}

void ClaudeStatusBarController::setTabIndicatorEnabledProvider(
        std::function<bool()> p) {
    m_tabIndicatorEnabledProvider = std::move(p);
}

QPushButton *ClaudeStatusBarController::reviewButton() const {
    return m_reviewBtn;
}

QPushButton *ClaudeStatusBarController::bgTasksButton() const {
    return m_bgTasksBtn;
}

ClaudeBgTaskTracker *ClaudeStatusBarController::bgTasksTracker() const {
    return m_bgTasks;
}

void ClaudeStatusBarController::apply() {}
