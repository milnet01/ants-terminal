#include "claudestatuswidgets.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QString>
#include <QStringLiteral>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>
#include <memory>

#include "claudeallowlist.h"
#include "claudebgtasks.h"
#include "claudeintegration.h"
#include "claudetabtracker.h"
#include "coloredtabbar.h"
#include "debuglog.h"
#include "terminalwidget.h"
#include "themes.h"

// ANTS-1146 — extracted from mainwindow.cpp by Bundle G Tier 3.
// See docs/specs/ANTS-1146.md for the design rationale, the
// 12-row external touch-site rewrite map, and the per-test
// re-pointing contract.

ClaudeStatusBarController::ClaudeStatusBarController(QStatusBar *statusBar,
                                                     QObject *parent)
    : QObject(parent), m_statusBar(statusBar) {
    // Status bar indicator for Claude Code state. Plain QLabel with Fixed
    // horizontal sizePolicy — the vocabulary is a bounded set of short
    // labels ("Claude: idle", "Claude: thinking...", "Claude: bash",
    // "Claude: reading a file", "Claude: planning", "Claude: auditing",
    // "Claude: prompting", "Claude: compacting..."), so the widget can
    // grow to fit its natural width without ever needing to elide. The
    // widget is hidden when the tab's shell has no Claude process.
    m_statusLabel = new QLabel(m_statusBar);
    m_statusLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_statusLabel->setStyleSheet("color: gray; padding: 0 8px; font-size: 11px;");
    // 0.7.54 (2026-04-27 indie-review WCAG) — accessible name for the
    // status-bar Claude session label. The visible text already carries
    // the state (e.g. "Claude: thinking…"), but screen readers benefit
    // from a stable accessibleName that doesn't change with the visible
    // text. The accessibleDescription tracks the live state via apply().
    m_statusLabel->setAccessibleName(tr("Claude Code session status"));
    m_statusLabel->hide();
    m_statusBar->addPermanentWidget(m_statusLabel);

    // Context window pressure indicator (progress bar)
    m_contextBar = new QProgressBar(m_statusBar);
    m_contextBar->setRange(0, 100);
    m_contextBar->setValue(0);
    m_contextBar->setFixedWidth(80);
    m_contextBar->setFixedHeight(14);
    m_contextBar->setFormat("%p%");
    // Styled dynamically by applyTheme()
    m_contextBar->setToolTip("Claude Code context window usage");
    m_contextBar->setAccessibleName(tr("Claude Code context window usage"));
    m_contextBar->hide();
    m_statusBar->addPermanentWidget(m_contextBar);

    // Review Changes button (shown when Claude edits files). Size/height
    // intentionally left at Qt's default so it matches the sibling
    // "Add to allowlist" button (mainwindow.cpp:1234) that inherits the
    // global QPushButton stylesheet. applyTheme() applies the palette
    // force-set for contrast without adding compact-height overrides
    // that would re-introduce the size mismatch.
    m_reviewBtn = new QPushButton("Review Changes", m_statusBar);
    // Fixed horizontal sizePolicy — never squeezed for the benefit of
    // the notification slot. See layout contract at mainwindow.cpp:~320.
    m_reviewBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_reviewBtn->setAccessibleName(tr("Review Claude code changes"));
    m_reviewBtn->hide();
    m_statusBar->addPermanentWidget(m_reviewBtn);
    // Click → reviewClicked signal. MainWindow connects the signal to
    // showDiffViewer; this indirection lets future consumers (command
    // palette, remote-control verb) wire to the same surface.
    connect(m_reviewBtn, &QPushButton::clicked,
            this, &ClaudeStatusBarController::reviewClicked);

    // 0.7.38 — Background tasks button. Sibling to Review Changes; same
    // size/policy contract. Hidden by default; shown only when the
    // per-session tracker reports ≥1 background task in the active
    // Claude Code transcript.
    m_bgTasks = new ClaudeBgTaskTracker(this);
    m_bgTasksBtn = new QPushButton(tr("Background Tasks"), m_statusBar);
    m_bgTasksBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_bgTasksBtn->hide();
    m_statusBar->addPermanentWidget(m_bgTasksBtn);
    connect(m_bgTasksBtn, &QPushButton::clicked,
            this, &ClaudeStatusBarController::bgTasksClicked);
    connect(m_bgTasks, &ClaudeBgTaskTracker::tasksChanged,
            this, &ClaudeStatusBarController::refreshBgTasksButton);

    // Error indicator label — surfaces the exit-code from the
    // commandFailed terminal signal, auto-hides after the timeout the
    // caller passes to setError().
    m_errorLabel = new QLabel(m_statusBar);
    // Styled dynamically by applyTheme()
    m_errorLabel->hide();
    m_statusBar->addPermanentWidget(m_errorLabel);
}

void ClaudeStatusBarController::attach(ClaudeIntegration *integration,
                                       ClaudeTabTracker *tracker,
                                       ColoredTabBar *coloredTabBar,
                                       QTabWidget *tabWidget) {
    m_integration = integration;
    m_tracker = tracker;
    m_coloredTabBar = coloredTabBar;
    m_tabWidget = tabWidget;

    // Per-tab activity tracker. Always available (its polling cost is
    // trivial: one /proc read per tracked shell every 2 s, zero when
    // no shell has Claude). The user-facing toggle
    // claude_tab_status_indicator gates the glyph rendering at the
    // provider level — flipping it off doesn't destroy the tracker, it
    // just makes the provider return Glyph::None until the toggle
    // flips back on. That way live config reloads take effect on the
    // next paint without any construct/destruct dance.
    if (m_coloredTabBar) {
        // Provider: look up the tab's active terminal, read its shell
        // PID, and translate the tracker's per-shell state into a
        // Glyph. The `awaitingInput` flag short-circuits whatever the
        // transcript parser said because a pending prompt is what the
        // user most needs to notice.
        m_coloredTabBar->setClaudeIndicatorProvider([this](int tabIndex) {
            ClaudeTabIndicator ind;
            if (!m_tracker) return ind;
            if (m_tabIndicatorEnabledProvider &&
                !m_tabIndicatorEnabledProvider()) return ind;  // toggle off
            auto *term = m_terminalAtTabProvider
                ? m_terminalAtTabProvider(tabIndex) : nullptr;
            if (!term) return ind;
            const pid_t pid = term->shellPid();
            if (pid <= 0) return ind;
            const ClaudeTabTracker::ShellState s = m_tracker->shellState(pid);
            if (s.awaitingInput) {
                ind.glyph = ClaudeTabIndicator::Glyph::AwaitingInput;
                return ind;
            }
            if (s.planMode && s.state != ClaudeState::NotRunning) {
                ind.glyph = ClaudeTabIndicator::Glyph::Planning;
                return ind;
            }
            if (s.auditing && s.state != ClaudeState::NotRunning) {
                ind.glyph = ClaudeTabIndicator::Glyph::Auditing;
                return ind;
            }
            switch (s.state) {
                case ClaudeState::NotRunning:
                    ind.glyph = ClaudeTabIndicator::Glyph::None; break;
                case ClaudeState::Idle:
                    ind.glyph = ClaudeTabIndicator::Glyph::Idle; break;
                case ClaudeState::Thinking:
                    ind.glyph = ClaudeTabIndicator::Glyph::Thinking; break;
                case ClaudeState::ToolUse:
                    // Bash is the tool with the most user-relevant runtime
                    // (long-running commands, compilations, greps over
                    // large repos) — split it out so the glyph carries
                    // that signal at a glance.
                    ind.glyph = (s.tool == QLatin1String("Bash"))
                        ? ClaudeTabIndicator::Glyph::Bash
                        : ClaudeTabIndicator::Glyph::ToolUse;
                    break;
                case ClaudeState::Compacting:
                    ind.glyph = ClaudeTabIndicator::Glyph::Compacting; break;
            }
            return ind;
        });
    }
    // Repaint the tab bar whenever any shell's state transitions.
    // Cheap — QWidget::update() coalesces to one paint per event
    // loop iteration, and paintEvent only queries the tracker once
    // per tab. Also refresh the hover tooltip for the owning tab so
    // the user can hover-to-disambiguate ("Claude: Bash" vs
    // "Claude: reading a file") without opening the tab.
    if (m_tracker) {
        connect(m_tracker, &ClaudeTabTracker::shellStateChanged,
                this, [this](pid_t shellPid) {
            if (m_coloredTabBar) m_coloredTabBar->update();
            if (!m_tabWidget) return;
            const int n = m_tabWidget->count();
            for (int i = 0; i < n; ++i) {
                auto *term = m_terminalAtTabProvider
                    ? m_terminalAtTabProvider(i) : nullptr;
                if (!term || term->shellPid() != shellPid) continue;
                const auto st = m_tracker->shellState(shellPid);
                QString tip;
                if (st.awaitingInput) {
                    tip = tr("Claude: awaiting input");
                } else if (st.planMode && st.state != ClaudeState::NotRunning) {
                    tip = tr("Claude: planning");
                } else if (st.auditing && st.state != ClaudeState::NotRunning) {
                    tip = tr("Claude: auditing");
                } else switch (st.state) {
                    case ClaudeState::NotRunning: tip.clear(); break;
                    case ClaudeState::Idle:       tip = tr("Claude: idle"); break;
                    case ClaudeState::Thinking:   tip = tr("Claude: thinking…"); break;
                    case ClaudeState::Compacting: tip = tr("Claude: compacting…"); break;
                    case ClaudeState::ToolUse:
                        tip = st.tool.isEmpty()
                            ? tr("Claude: tool use")
                            : tr("Claude: %1").arg(st.tool);
                        break;
                }
                m_tabWidget->setTabToolTip(i, tip);
                break;
            }
        });
    }

    if (!m_integration) return;

    connect(m_integration, &ClaudeIntegration::stateChanged,
            this, [this](ClaudeState state, const QString &detail) {
        m_lastState = state;
        m_lastDetail = detail;
        apply();
    });

    connect(m_integration, &ClaudeIntegration::planModeChanged,
            this, [this](bool active) {
        m_planMode = active;
        apply();
    });

    connect(m_integration, &ClaudeIntegration::auditingChanged,
            this, [this](bool active) {
        m_auditing = active;
        apply();
    });

    connect(m_integration, &ClaudeIntegration::contextUpdated,
            this, [this](int percent) {
        // 0.6.26 — contextUpdated(0) is emitted by ClaudeIntegration::setShellPid
        // on every tab switch as part of the state reset (alongside
        // stateChanged(NotRunning)). Previously we called show() here
        // unconditionally, which re-exposed the bar at 0% immediately after
        // the NotRunning handler had hidden it — so a fresh tab (or a tab
        // where Claude was never started) still painted a "0%" widget in
        // the status bar. Treat 0 as "no session / nothing to show" and
        // hide. The bar only re-appears once Claude emits a real,
        // non-zero context percentage (claudeintegration.cpp:333-334).
        if (percent <= 0) {
            m_contextBar->hide();
            return;
        }
        m_contextBar->setValue(percent);
        m_contextBar->show();
        // Color-code: green < 60%, yellow 60-80%, red > 80%
        const Theme &th = Themes::byName(m_currentThemeName);
        QString chunkColor = th.ansi[2].name();  // green
        if (percent > 80) chunkColor = th.ansi[1].name();  // red
        else if (percent > 60) chunkColor = th.ansi[3].name();  // yellow
        m_contextBar->setStyleSheet(
            QStringLiteral("QProgressBar { border: 1px solid %1; border-radius: 3px; background: %2; font-size: 10px; color: %3; }"
                    "QProgressBar::chunk { background: %4; border-radius: 2px; }")
                .arg(th.border.name(), th.bgSecondary.name(), th.textPrimary.name(), chunkColor));
        if (percent >= 80) {
            m_contextBar->setToolTip(
                QString("Context %1% — consider using /compact").arg(percent));
        }
    });

    connect(m_integration, &ClaudeIntegration::fileChanged,
            this, [this](const QString &path) {
        emit statusMessageRequested(QString("Claude edited: %1").arg(path), 3000);
        // 0.6.22 — refreshReviewButton decides visibility + enabled state:
        //   * not a git repo (or no cwd) → hidden entirely
        //   * git repo with no diff     → visible but disabled
        //   * git repo with diff        → visible and enabled
        // This replaces the old unconditional show() which could leave
        // the button visible in non-git contexts (where clicking it
        // only produced a "No changes detected" toast).
        emit reviewButtonShouldRefresh();
    });

    connect(m_integration, &ClaudeIntegration::permissionRequested,
            this, [this](const QString &tool, const QString &input) {
        QString rawRule = tool;
        if (!input.isEmpty()) rawRule += "(" + input + ")";
        // Normalize and generalize to a useful allowlist pattern
        QString rule = ClaudeAllowlistDialog::normalizeRule(rawRule);
        QString gen = ClaudeAllowlistDialog::generalizeRule(rule);
        if (!gen.isEmpty()) rule = gen;
        emit statusMessageRequested(QString("Claude permission: %1").arg(rule), 0);

        // Dedup: a PermissionRequest hook that arrives while a scroll-scan
        // permission button is already on screen should not stack a second
        // button group beside it. Remove any existing "claudeAllowBtn"
        // widgets (from either path) first — same objectName as the
        // scroll-scan path so the onTabChanged cleanup catches both.
        for (auto *w : m_statusBar->findChildren<QWidget *>(QStringLiteral("claudeAllowBtn")))
            w->deleteLater();

        // Enhanced permission action buttons
        auto *btnWidget = new QWidget(m_statusBar);
        // 0.6.29 — same objectName as the scroll-scan path's button
        // (see mainwindow.cpp commandFailed handler) so the tab-switch
        // cleanup in onTabChanged removes both. Previously this widget
        // had no objectName, so switching tabs mid-prompt left a
        // stranded button group visible on the wrong tab.
        btnWidget->setObjectName(QStringLiteral("claudeAllowBtn"));
        // Fixed horizontal sizePolicy — must never be squeezed when
        // the notification slot is wide. See layout principle at
        // mainwindow.cpp:~320 (user spec 2026-04-18).
        btnWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        auto *btnLayout = new QHBoxLayout(btnWidget);
        btnLayout->setContentsMargins(0, 0, 0, 0);
        btnLayout->setSpacing(4);

        const Theme &th = Themes::byName(m_currentThemeName);
        auto *allowBtn = new QPushButton("Allow", btnWidget);
        allowBtn->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; border-radius: 3px; padding: 1px 8px; font-size: 10px; }")
            .arg(th.ansi[2].name(), th.bgPrimary.name()));
        auto *denyBtn = new QPushButton("Deny", btnWidget);
        denyBtn->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; border-radius: 3px; padding: 1px 8px; font-size: 10px; }")
            .arg(th.ansi[1].name(), th.bgPrimary.name()));
        auto *addBtn = new QPushButton("Add to allowlist", btnWidget);
        addBtn->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; border-radius: 3px; padding: 1px 8px; font-size: 10px; }")
            .arg(th.bgSecondary.name(), th.textPrimary.name()));

        btnLayout->addWidget(allowBtn);
        btnLayout->addWidget(denyBtn);
        btnLayout->addWidget(addBtn);
        m_statusBar->addPermanentWidget(btnWidget);

        // Mark prompt active so the Claude status label switches to
        // "prompting" — matches the scroll-scan path's behavior and
        // gives the user a second at-a-glance indicator beyond the
        // button group itself.
        m_promptActive = true;
        apply();

        // Tab-glyph feedback: flag the owning tab's shell as awaiting
        // input so its tab-bar dot turns loud/orange. The hook server
        // is a single UDS shared across every Claude in every tab, so
        // we must route by session_id (captured by ClaudeIntegration
        // before this slot runs) to find the right shell. Fallback
        // to the active tab when the session isn't tracked (e.g. the
        // first prompt before the tracker's poll has noticed the new
        // Claude child) — the bottom-status-bar already shows
        // "Claude: prompting" for the active tab, so the glyph
        // matches that contract when routing fails.
        pid_t awaitingPid = 0;
        if (m_tracker) {
            const QString sid = m_integration->lastHookSessionId();
            awaitingPid = m_tracker->shellForSessionId(sid);
            if (awaitingPid == 0) {
                if (auto *term = m_currentTerminalProvider
                                     ? m_currentTerminalProvider() : nullptr)
                    awaitingPid = term->shellPid();
            }
            if (awaitingPid > 0)
                m_tracker->markShellAwaitingInput(awaitingPid, true);
        }

        auto clearPromptActive = [this, awaitingPid]() {
            m_promptActive = false;
            apply();
            if (m_tracker && awaitingPid > 0)
                m_tracker->markShellAwaitingInput(awaitingPid, false);
        };

        connect(allowBtn, &QPushButton::clicked, btnWidget, [this, btnWidget, clearPromptActive]() {
            btnWidget->deleteLater();
            emit statusMessageCleared();
            clearPromptActive();
        });
        connect(denyBtn, &QPushButton::clicked, btnWidget, [this, btnWidget, clearPromptActive]() {
            btnWidget->deleteLater();
            emit statusMessageCleared();
            clearPromptActive();
        });
        connect(addBtn, &QPushButton::clicked, this, [this, rule, btnWidget, clearPromptActive]() {
            emit allowlistRequested(rule);
            btnWidget->deleteLater();
            emit statusMessageCleared();
            clearPromptActive();
        });

        // Remove buttons when the prompt disappears from the screen.
        // Same lesson as the grid-scan path above: don't tie retraction to
        // `outputReceived`, which fires on every repaint and would retract
        // the buttons while the prompt is still visible. `claudePermissionCleared`
        // fires only on the transition to "no prompt on screen".
        //
        // Listen on ALL terminals (not just currentTerminal) so a prompt
        // raised via hook on tab A disappears when the user approves/declines
        // in tab A even after briefly visiting tab B. Multiple connects are
        // fine; each disconnects itself via the shared pointer once fired.
        if (m_tabWidget) {
            for (auto *term : m_tabWidget->findChildren<TerminalWidget *>()) {
                auto conn = std::make_shared<QMetaObject::Connection>();
                *conn = connect(term, &TerminalWidget::claudePermissionCleared,
                                btnWidget, [btnWidget, conn, clearPromptActive]() {
                    QObject::disconnect(*conn);
                    btnWidget->deleteLater();
                    clearPromptActive();
                });
            }
        }

        // 0.6.31 — `claudePermissionCleared` above only fires if the terminal
        // scroll-scanner previously emitted `claudePermissionDetected` for
        // this prompt (gated on `m_lastDetectedRule` being non-empty in
        // terminalwidget.cpp:3658). When a PermissionRequest HOOK fires for
        // a prompt the scroll-scanner never saw — unmatched prompt format,
        // prompt already scrolled past the 12-line lookback window, or a
        // headless Claude Code session where the hook is the only signal
        // — `m_lastDetectedRule` stays empty and `claudePermissionCleared`
        // never fires, orphaning the button forever. User-reported symptom:
        // "Claude Code permission: Bash(cd * && cmake * | tail *) —" visible
        // with no live prompt in any terminal.
        //
        // Retract on `toolFinished` (permission was granted and the tool
        // completed), `sessionStopped` (session ended — prompt is moot),
        // and `permissionRequested` (a new prompt implicitly resolves the
        // previous one; the existing `findChildren` dedup at the top of
        // this handler already removes the old btnWidget, which auto-
        // disconnects these connections via the btnWidget context).
        //
        // These are proxy signals — Claude Code has no canonical
        // "PermissionResolved" hook (confirmed in claudeintegration.cpp
        // processHookEvent; PermissionRequest has no inverse). Using
        // toolFinished/sessionStopped errs on the side of closing the
        // button too early (user never clicks it) rather than too late
        // (button lingers indefinitely on a resolved prompt, inviting a
        // misdirected click).
        auto finishedConn = std::make_shared<QMetaObject::Connection>();
        *finishedConn = connect(m_integration, &ClaudeIntegration::toolFinished,
                                btnWidget, [btnWidget, finishedConn, clearPromptActive](const QString &, bool) {
            QObject::disconnect(*finishedConn);
            btnWidget->deleteLater();
            clearPromptActive();
        });
        auto stoppedConn = std::make_shared<QMetaObject::Connection>();
        *stoppedConn = connect(m_integration, &ClaudeIntegration::sessionStopped,
                               btnWidget, [btnWidget, stoppedConn, clearPromptActive](const QString &) {
            QObject::disconnect(*stoppedConn);
            btnWidget->deleteLater();
            clearPromptActive();
        });
    });
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

void ClaudeStatusBarController::setError(const QString &text,
                                         const QString &tooltip,
                                         int autoHideMs) {
    if (!m_errorLabel) return;
    m_errorLabel->setText(text);
    m_errorLabel->setToolTip(tooltip);
    m_errorLabel->show();
    QTimer::singleShot(autoHideMs, m_errorLabel, &QWidget::hide);
}

void ClaudeStatusBarController::clearError() {
    if (m_errorLabel) m_errorLabel->hide();
}

void ClaudeStatusBarController::resetForTabSwitch() {
    m_lastState = ClaudeState::NotRunning;
    m_lastDetail.clear();
    m_planMode = false;
    m_auditing = false;
    if (m_reviewBtn)   m_reviewBtn->hide();
    if (m_contextBar)  m_contextBar->hide();
    if (m_bgTasksBtn)  m_bgTasksBtn->hide();
    if (m_bgTasks)     m_bgTasks->setTranscriptPath(QString());
    apply();
}

void ClaudeStatusBarController::applyTheme(const QString &themeName) {
    m_currentThemeName = themeName;
    const Theme &th = Themes::byName(m_currentThemeName);
    QString statusStyle = QStringLiteral("padding: 0 8px; font-size: 11px;");
    if (m_statusLabel)
        m_statusLabel->setStyleSheet(QStringLiteral("color: %1; %2").arg(th.textSecondary.name(), statusStyle));
    if (m_contextBar)
        m_contextBar->setStyleSheet(
            QStringLiteral("QProgressBar { border: 1px solid %1; border-radius: 3px; background: %2; font-size: 10px; color: %3; }"
                    "QProgressBar::chunk { background: %4; border-radius: 2px; }")
                .arg(th.border.name(), th.bgSecondary.name(), th.textPrimary.name(), th.ansi[2].name()));
    if (m_reviewBtn) {
        // 0.6.26 — side-by-side with the "Add to allowlist" button in the
        // status bar, the custom-styled Review Changes button looked wildly
        // out of place (see screenshot attached to the original report).
        // "Add to allowlist" is created at mainwindow.cpp:1234 with *no*
        // stylesheet, so it inherits the global QPushButton rule from
        // mainwindow.cpp:1625-1630 — that's the target styling. Stop
        // over-styling the enabled state: reset the font to Qt's default,
        // clear any fixedHeight so the size-hint matches its sibling.
        //
        // Disabled state — the button stays visible on clean git repos so
        // the user still sees "Claude edited something" (see
        // refreshReviewButton at mainwindow.cpp:~5149). The visual must
        // clearly read as non-actionable without shouting. Three layered
        // cues: italic text (typographic "this is passive"), dashed border
        // (borrowed from common desktop-toolkit conventions for disabled
        // chip buttons), and textSecondary on bgSecondary (muted palette).
        // Only the :disabled selector is set on the widget, so the global
        // QPushButton enabled/hover/pressed rules still apply for the
        // enabled state — no duplication, no drift.
        //
        // Palette force-set: survives the "dim on Gruvbox" contrast issue
        // on the enabled state regardless of how Qt composites the text
        // rect on a statusbar-parented widget (pre-0.6.26 this was
        // rendered dim even with the stylesheet's color property set —
        // root cause: platform style composited a reduced-alpha overlay).
        m_reviewBtn->setFont(QFont());
        m_reviewBtn->setMinimumHeight(0);
        m_reviewBtn->setMaximumHeight(QWIDGETSIZE_MAX);
        m_reviewBtn->setStyleSheet(QStringLiteral(
            "QPushButton:disabled {"
            "  color: %1;"
            "  background-color: %2;"
            "  border: 1px dashed %3;"
            "  font-style: italic;"
            "}").arg(th.textSecondary.name(),
                     th.bgSecondary.name(),
                     th.border.name()));

        QPalette pal = m_reviewBtn->palette();
        pal.setColor(QPalette::Active,   QPalette::ButtonText, th.textPrimary);
        pal.setColor(QPalette::Inactive, QPalette::ButtonText, th.textPrimary);
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, th.textSecondary);
        pal.setColor(QPalette::Active,   QPalette::WindowText, th.textPrimary);
        pal.setColor(QPalette::Inactive, QPalette::WindowText, th.textPrimary);
        pal.setColor(QPalette::Disabled, QPalette::WindowText, th.textSecondary);
        m_reviewBtn->setPalette(pal);
    }
    if (m_errorLabel)
        m_errorLabel->setStyleSheet(QStringLiteral("color: %1; padding: 0 4px; font-size: 11px;").arg(th.ansi[1].name()));
}

void ClaudeStatusBarController::refreshBgTasksButton() {
    if (!m_bgTasks || !m_bgTasksBtn) {
        ANTS_LOG(DebugLog::Claude,
                 "bgtasks/refresh: tracker=%p btn=%p — early return",
                 static_cast<void *>(m_bgTasks),
                 static_cast<void *>(m_bgTasksBtn));
        return;
    }
    // Resolve the transcript path scoped to the active tab's project
    // tree. activeSessionPath walks up the cwd, encodes each ancestor
    // to Claude Code's `<dashed-cwd>` form, and returns the newest
    // `.jsonl` from the deepest matching `~/.claude/projects/<…>/`
    // subdir. Without this scoping, sessions from *other* projects
    // (e.g. another tab's tree) would leak into the bg-tasks surface
    // — which is exactly the user-reported 2026-04-27 bug.
    QString cwd;
    auto *focused = m_focusedTerminalProvider ? m_focusedTerminalProvider() : nullptr;
    if (focused) cwd = focused->shellCwd();
    const bool focusedTabPresent = (focused != nullptr);
    QString path;
    if (m_integration) path = m_integration->activeSessionPath(cwd);
    const QString prevPath = m_bgTasks->transcriptPath();
    m_bgTasks->setTranscriptPath(path);
    // 0.7.55 (2026-04-27 indie-review) — sweep liveness only, not full
    // rescan. setTranscriptPath() already triggers a full rescan when
    // the path changes (initial bind, tab switch). When the path is
    // unchanged but we still want a fresh staleness check, the cheap
    // sweepLiveness() does N stat() calls — avoiding the 16 MiB
    // transcript walk that the previous rescan() call caused on every
    // 2 s timer tick. The QFileSystemWatcher continues to drive full
    // rescan() on transcript-changed (Claude appended JSONL).
    if (!path.isEmpty() && path == prevPath) {
        m_bgTasks->sweepLiveness();
    }

    const int running = m_bgTasks->runningCount();
    const int total = m_bgTasks->tasks().size();

    // ANTS-1052 diagnostic: log every refresh outcome so the user can
    // capture why the button hides under realistic conditions. Gated
    // on ANTS_DEBUG=claude (or runtime menu toggle). Truncate path to
    // its basename for brevity — full path is in prevPath state.
    if (DebugLog::enabled(DebugLog::Claude)) {
        const QString cwdShort = cwd.isEmpty()
            ? QStringLiteral("(empty)") : cwd;
        const QString pathShort = path.isEmpty()
            ? QStringLiteral("(empty)")
            : path.section('/', -1);
        const char *branch =
            (running > 0) ? "SHOW" :
            path.isEmpty() ? "HIDE/no-path" :
            (total == 0)  ? "HIDE/no-tasks-parsed" :
                            "HIDE/all-finished";
        ANTS_LOG(DebugLog::Claude,
                 "bgtasks/refresh: focused-tab=%s cwd=%s path=%s "
                 "prev-changed=%s running=%d total=%d → %s",
                 focusedTabPresent ? "yes" : "no",
                 cwdShort.toUtf8().constData(),
                 pathShort.toUtf8().constData(),
                 (path == prevPath) ? "no" : "yes",
                 running, total, branch);
    }

    if (running <= 0) {
        // No active background work — keep the chrome quiet.
        m_bgTasksBtn->hide();
        return;
    }
    m_bgTasksBtn->setText(tr("Background Tasks (%1)").arg(running));
    m_bgTasksBtn->setToolTip(
        tr("%1 running · %2 total in this session").arg(running).arg(total));
    m_bgTasksBtn->show();
}

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

void ClaudeStatusBarController::apply() {
    if (!m_statusLabel) return;
    const QString statusStyle = QStringLiteral("padding: 0 8px; font-size: 11px;");

    // NotRunning hides both label and the context progress bar regardless
    // of prompt state — no Claude process = nothing to announce.
    if (m_lastState == ClaudeState::NotRunning) {
        m_statusLabel->hide();
        if (m_contextBar) m_contextBar->hide();
        return;
    }

    QString text;
    ClaudeTabIndicator::Glyph glyph = ClaudeTabIndicator::Glyph::Idle;

    // Status text vocabulary (user spec 2026-04-18):
    //   idle / thinking / prompting / bash / reading a file / planning /
    //   auditing / compacting / etc.
    // Colour comes from the unified Claude state palette
    // (`ClaudeTabIndicator::color`) so the status-bar text matches the
    // per-tab dot one-for-one — see
    // `tests/features/claude_state_dot_palette/spec.md`.
    if (m_promptActive) {
        // Prompt-active overrides the base state — a waiting permission
        // prompt is what the user needs to see.
        text = QStringLiteral("Claude: prompting");
        glyph = ClaudeTabIndicator::Glyph::AwaitingInput;
    } else if (m_planMode) {
        // Plan mode is a user-selected interaction mode (Shift+Tab),
        // orthogonal to the transcript-derived state. While in plan mode
        // the assistant can think/read but cannot edit or run commands —
        // "planning" is the honest label.
        text = QStringLiteral("Claude: planning");
        glyph = ClaudeTabIndicator::Glyph::Planning;
    } else if (m_auditing) {
        // Auditing is detected from a recent user message that invoked
        // the /audit skill in the transcript. Lives beside state because
        // the user can audit during tool use, thinking, or idle — the
        // skill's lifecycle is not the same as any single tool.
        text = QStringLiteral("Claude: auditing");
        glyph = ClaudeTabIndicator::Glyph::Auditing;
    } else {
        switch (m_lastState) {
        case ClaudeState::NotRunning:
            return;  // handled above
        case ClaudeState::Idle:
            text = QStringLiteral("Claude: idle");
            glyph = ClaudeTabIndicator::Glyph::Idle;
            break;
        case ClaudeState::Thinking:
            text = QStringLiteral("Claude: thinking");
            glyph = ClaudeTabIndicator::Glyph::Thinking;
            break;
        case ClaudeState::ToolUse: {
            // Map tool name → friendly vocabulary per user spec. Unknown
            // tools fall through to the raw name so MCP / custom tools
            // remain legible. Comparison is case-insensitive because
            // transcript tool names and hook tool names have historically
            // differed in casing across Claude Code releases.
            const QString t = m_lastDetail.trimmed();
            const QString lower = t.toLower();
            if (lower == QLatin1String("bash")) {
                text = QStringLiteral("Claude: bash");
                glyph = ClaudeTabIndicator::Glyph::Bash;
            } else {
                if (lower == QLatin1String("read")) {
                    text = QStringLiteral("Claude: reading a file");
                } else if (lower == QLatin1String("edit") ||
                           lower == QLatin1String("write") ||
                           lower == QLatin1String("notebookedit")) {
                    text = QStringLiteral("Claude: editing");
                } else if (lower == QLatin1String("grep") ||
                           lower == QLatin1String("glob")) {
                    text = QStringLiteral("Claude: searching");
                } else if (lower == QLatin1String("webfetch") ||
                           lower == QLatin1String("websearch")) {
                    text = QStringLiteral("Claude: browsing");
                } else if (lower == QLatin1String("task") ||
                           lower == QLatin1String("agent")) {
                    text = QStringLiteral("Claude: delegating");
                } else if (t.isEmpty()) {
                    text = QStringLiteral("Claude: thinking");
                } else {
                    text = QStringLiteral("Claude: %1").arg(t);
                }
                glyph = ClaudeTabIndicator::Glyph::ToolUse;
            }
            break;
        }
        case ClaudeState::Compacting:
            text = QStringLiteral("Claude: compacting");
            glyph = ClaudeTabIndicator::Glyph::Compacting;
            break;
        }
    }

    const QColor color = ClaudeTabIndicator::color(glyph);
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color: %1; %2").arg(color.name(), statusStyle));
    // 0.7.54 (2026-04-27 indie-review WCAG) — keep the accessible
    // description in sync with the visible state. Screen readers
    // announce accessibleName + accessibleDescription on focus, so
    // colour-only state encoding is no longer the sole signal.
    m_statusLabel->setAccessibleDescription(text);
    m_statusLabel->show();
}
