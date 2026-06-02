#include "claudestatuswidgets.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
// ANTS-1893 — ::kill(pid, 0) ESRCH dead-pid probe in onUndoSwitchClicked.
#include <sys/types.h>
#include <signal.h>
#include <cerrno>

#include "claudeallowlist.h"
#include "claudebgtasks.h"
#include "claudestateresolver.h"   // ANTS-1873
#include "config.h"                // ANTS-1735 §2.7 — claudeAutoModel()
#include "modelautoswitch.h"       // ANTS-1735 §2.3 — decide() gate
#include "modelrecommender.h"
#include "modelswitchledger.h"     // ANTS-1735 §2.5 — append + statsEnvelope
#include "modelnearmissledger.h"   // ANTS-1894 — near-miss telemetry sibling ledger
#include "claudeintegration.h"
#include "claudetabtracker.h"
#include "claudetasklist.h"
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

    // ANTS-1053 — Background tasks button. One ClaudeBgTaskTracker is
    // created per shell PID (via trackBgShell) so background tabs keep
    // their task state live across tab switches. The button itself is a
    // single widget; refreshBgTasksButton reads from the focused tab's
    // per-PID tracker entry.
    m_bgTasksBtn = new QPushButton(tr("Background Tasks"), m_statusBar);
    m_bgTasksBtn->setAccessibleName(tr("Background tasks"));
    m_bgTasksBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_bgTasksBtn->hide();
    m_statusBar->addPermanentWidget(m_bgTasksBtn);
    connect(m_bgTasksBtn, &QPushButton::clicked,
            this, &ClaudeStatusBarController::bgTasksClicked);

    // ANTS-1158 — task-list chip. Sibling to bg-tasks; same size
    // contract. Hidden until the focused tab's transcript reports
    // ≥ 1 plan row. Label shape "<unfinished>/<total>", e.g. 3/5.
    m_tasks = new ClaudeTaskListTracker(this);
    // Empty initial label — the hide() below + ANTS-1246 hide predicate
    // (total <= 0 || done >= total) mean the chip is never shown before
    // the first refreshTasksButton tick. Constructing with "☰ 0/0" was
    // internally inconsistent with that hide contract.
    m_tasksBtn = new QPushButton(QString(), m_statusBar);
    m_tasksBtn->setObjectName(QStringLiteral("claudeTasksBtn"));
    m_tasksBtn->setAccessibleName(tr("Task list"));
    m_tasksBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_tasksBtn->hide();
    m_statusBar->addPermanentWidget(m_tasksBtn);
    connect(m_tasksBtn, &QPushButton::clicked,
            this, &ClaudeStatusBarController::tasksClicked);
    // ANTS-1219-INV-5: tracker → controller feedback connect. A
    // parser-driven content change (file appended on disk →
    // QFileSystemWatcher::fileChanged → rescan → tasksChanged) re-runs
    // refreshTasksButton on the same thread (Qt::AutoConnection,
    // direct delivery) so the chip text follows tracker state without
    // an event-loop gap.
    connect(m_tasks, &ClaudeTaskListTracker::tasksChanged,
            this, &ClaudeStatusBarController::refreshTasksButton);

    // ANTS-1226 — model recommender chip.
    m_modelBtn = new QPushButton(QString(), m_statusBar);
    m_modelBtn->setObjectName(QStringLiteral("claudeModelBtn"));
    m_modelBtn->setAccessibleName(tr("Model recommendation"));
    m_modelBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_modelBtn->hide();
    m_statusBar->addPermanentWidget(m_modelBtn);
    connect(m_modelBtn, &QPushButton::clicked, this, [this]() {
        if (!m_modelBtn) return;
        // Read the tier from a stored property, NOT by reverse-parsing the
        // visible (translatable / RTL-reorderable) label — that round-trip
        // could write a garbage /model argument to the user's shell.
        // indie-review-2026-05-21.
        const QString tier = m_modelBtn->property("modelTier").toString();
        if (tier.isEmpty()) return;
        auto *focused = m_focusedTerminalProvider
            ? m_focusedTerminalProvider() : nullptr;
        if (!focused) return;
        // ANTS-1915 — if Claude is mid-generation on this tab, a `/model`
        // sent now would sit unsubmitted in the composer until the user
        // presses Escape (interrupting the turn). Defer the switch instead:
        // record the tier + owning shell PID and fire it when the shell next
        // goes Idle (maybeFireDeferredChipSwitch on shellStateChanged). This
        // gives the user a "switch without interrupting" path through Ants's
        // own UI — the typed-`/model`-in-composer case stays blocked on
        // Claude Code exposing composer state (tracked on the roadmap).
        const pid_t pid = focused->shellPid();
        const ClaudeState st = (m_tracker && pid > 0)
            ? m_tracker->shellState(pid).state : ClaudeState::NotRunning;
        const bool generating = (st == ClaudeState::Thinking ||
                                 st == ClaudeState::ToolUse ||
                                 st == ClaudeState::Compacting);
        if (generating) {
            m_deferredChipTier     = tier;
            m_deferredChipShellPid = pid;
            m_modelBtn->setToolTip(
                tr("Switch to %1 queued — applies when Claude finishes this turn")
                    .arg(tier));
            focused->setFocus();
            return;
        }
        // ANTS-1912 — submit with `\r`, not `\n`. The Enter key produces
        // CR (0x0D) on a PTY (terminalwidget.cpp:1930); Claude Code's TUI
        // treats `\n` as a literal newline character in the composer and
        // does NOT submit. Sending `\n` left `/model <tier>` sitting in
        // the prompt waiting for the user to press Enter manually —
        // defeats the entire auto-switch loop and also self-vetoes the
        // next tick via the composer_not_empty gate (ANTS-1908).
        focused->sendToPty(
            (QStringLiteral("/model ") + tier + QStringLiteral("\r")).toUtf8());
        // ANTS-1918/1924 — ESC + ENTER handshake + continuation prompt.
        performModelSwitchHandshake(focused);
        // ANTS-1840 — the user just acted on the recommendation, so hide the
        // chip immediately rather than letting it linger until the next
        // assistant turn re-scores the transcript. We deliberately leave the
        // mtime cache (m_modelChipPath / m_modelChipMtimeMs) untouched: the
        // short-circuit in refreshModelChip then keeps the chip hidden until
        // the transcript actually changes, at which point the new turn's
        // message.model is re-scored and the chip re-shows only if still
        // warranted (INV-4).
        m_modelBtn->hide();
        // ANTS-1849 — clicking the button stole keyboard focus from the
        // terminal; return it so the user can keep typing without a manual
        // click back into the terminal.
        focused->setFocus();
    });

    // ANTS-1888 — passive model + thinking-level chip. Placed
    // immediately after the recommender chip so when INV-14 hides
    // the recommender, this chip becomes the visible widget in the
    // same slot. Always read-only; clicks are no-op in v1 (the
    // tooltip documents the source for keyboard-only users).
    m_modelStateBtn = new QPushButton(QString(), m_statusBar);
    m_modelStateBtn->setObjectName(QStringLiteral("claudeModelStateBtn"));
    m_modelStateBtn->setAccessibleName(tr("Current Claude model and thinking level"));
    m_modelStateBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_modelStateBtn->setFlat(true);   // visual cue: passive readout, not actionable
    m_modelStateBtn->setFocusPolicy(Qt::NoFocus);
    m_modelStateBtn->hide();
    m_statusBar->addPermanentWidget(m_modelStateBtn);

    // ANTS-1893 — Undo button. Sits between the model chip and the
    // error label so the visual order is
    // [recommender] [model+thinking] [Undo: back to <Tier>] [error].
    // Hidden by default; emitSwitchSurfacing shows it for ~10 s after
    // a live auto-switch fires.
    m_undoSwitchBtn = new QPushButton(QString(), m_statusBar);
    m_undoSwitchBtn->setObjectName(QStringLiteral("claudeUndoSwitchBtn"));
    m_undoSwitchBtn->setAccessibleName(tr("Undo last auto-model switch"));
    m_undoSwitchBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_undoSwitchBtn->hide();
    m_statusBar->addPermanentWidget(m_undoSwitchBtn);
    connect(m_undoSwitchBtn, &QPushButton::clicked, this,
            &ClaudeStatusBarController::onUndoSwitchClicked);

    // Error indicator label — surfaces the exit-code from the
    // commandFailed terminal signal, auto-hides after the timeout the
    // caller passes to setError().
    m_errorLabel = new QLabel(m_statusBar);
    m_errorLabel->setAccessibleName(tr("Last command error"));
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
            // ANTS-1873 — shared resolver. INV-5 widens the Bash check to
            // case-insensitive (the pre-fix lambda was case-sensitive
            // `s.tool == "Bash"`); unification with the status bar's
            // toLower() lookup at apply().
            const claudestate::Resolved r =
                claudestate::fromShell(m_tracker->shellState(pid));
            switch (claudestate::display(r)) {
                case claudestate::Display::Hidden:
                    ind.glyph = ClaudeTabIndicator::Glyph::None; break;
                case claudestate::Display::AwaitingInput:
                    ind.glyph = ClaudeTabIndicator::Glyph::AwaitingInput; break;
                case claudestate::Display::Planning:
                    ind.glyph = ClaudeTabIndicator::Glyph::Planning; break;
                case claudestate::Display::Auditing:
                    ind.glyph = ClaudeTabIndicator::Glyph::Auditing; break;
                case claudestate::Display::Idle:
                    ind.glyph = ClaudeTabIndicator::Glyph::Idle; break;
                case claudestate::Display::Thinking:
                    ind.glyph = ClaudeTabIndicator::Glyph::Thinking; break;
                case claudestate::Display::ToolUseBash:
                    ind.glyph = ClaudeTabIndicator::Glyph::Bash; break;
                case claudestate::Display::ToolUseGeneric:
                    ind.glyph = ClaudeTabIndicator::Glyph::ToolUse; break;
                case claudestate::Display::Compacting:
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

    // ANTS-1873 — repaint the status-bar label whenever the focused
    // tab's tracker entry changes. Distinct from the tab-bar repaint
    // connect above (which updates per-tab dot + tooltips); this one
    // drives the bottom Claude: <state> label so a tracker write (e.g.
    // markShellAwaitingInput, watcher-driven reparseTranscript) shows
    // up on the bar without waiting for an integration signal.
    if (m_tracker) {
        connect(m_tracker, &ClaudeTabTracker::shellStateChanged,
                this, [this](pid_t shellPid) {
            auto *focused = m_focusedTerminalProvider
                ? m_focusedTerminalProvider() : nullptr;
            if (focused && focused->shellPid() == shellPid) apply();
        });
    }

    // ANTS-1915 — fire any deferred manual chip-switch when its owning shell
    // transitions to Idle (turn complete). Event-driven so it works regardless
    // of whether the autonomous switcher is enabled.
    if (m_tracker) {
        connect(m_tracker, &ClaudeTabTracker::shellStateChanged,
                this, [this](pid_t shellPid) {
            maybeFireDeferredChipSwitch(shellPid);
        });
    }

    if (!m_integration) return;

    // ANTS-1873 — integration signals are kept as apply() nudges; their
    // payloads are now ignored (the source of truth is the focused tab's
    // tracker entry, read inside apply()).
    connect(m_integration, &ClaudeIntegration::stateChanged,
            this, [this](ClaudeState, const QString &) { apply(); });

    connect(m_integration, &ClaudeIntegration::planModeChanged,
            this, [this](bool) { apply(); });

    connect(m_integration, &ClaudeIntegration::auditingChanged,
            this, [this](bool) { apply(); });

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
        // ANTS-1835 — route the prompt to its owning tab BEFORE painting the
        // bottom-bar message + Allow/Deny buttons. The hook server is a
        // single UDS shared across every Claude in every tab, and the
        // PermissionRequest hook is deliberately ungated at the integration
        // layer (claude_status_bar_per_tab I3) — the slot must do the
        // routing. Resolve the owning shell from the hook's session_id (the
        // SAME routing the glyph used). When the session isn't tracked yet
        // (first prompt before the poll notices the new Claude child) fall
        // back to the focused tab, matching the pre-1835 contract that the
        // bottom bar owns the active tab's prompt.
        const pid_t owningPid = m_tracker
            ? m_tracker->shellForSessionId(m_integration->lastHookSessionId())
            : 0;
        pid_t focusedPid = 0;
        if (auto *term = m_currentTerminalProvider
                             ? m_currentTerminalProvider() : nullptr)
            focusedPid = term->shellPid();
        const pid_t awaitingPid = owningPid > 0 ? owningPid : focusedPid;
        const bool belongsToFocused = owningPid <= 0 || owningPid == focusedPid;

        QString rawRule = tool;
        if (!input.isEmpty()) rawRule += "(" + input + ")";
        // Normalize and generalize to a useful allowlist pattern
        QString rule = ClaudeAllowlistDialog::normalizeRule(rawRule);
        QString gen = ClaudeAllowlistDialog::generalizeRule(rule);
        if (!gen.isEmpty()) rule = gen;

        // Tab-glyph feedback: flag the owning tab's shell as awaiting input
        // so its tab-bar dot turns loud/orange — ALWAYS, even for a
        // background-tab prompt. Only the bottom-bar message/buttons are
        // gated on belongsToFocused (the glyph IS the at-a-glance signal for
        // a prompt on a tab you're not looking at). The rule is retained so
        // a switch back to a backgrounded tab can rebuild its prompt UI
        // (ANTS-1851).
        if (m_tracker && awaitingPid > 0)
            m_tracker->markShellAwaitingInput(awaitingPid, true, rule);

        showPermissionPrompt(awaitingPid, belongsToFocused, rule);
    });

    // ANTS-1890 INV-7 — Restart-safety seed of the override cool-down
    // cache. Runs once at attach() after services are wired but BEFORE
    // the 2 s status timer fires its first tick. Without this, an
    // override 30 s before app restart is forgotten — the very turn
    // that should fire cool-down passes — because
    // fillPendingLedgerOutcomes only walks pending records.
    seedOverrideCacheFromLedger(ModelSwitchLedger::defaultLedgerPath());
}

void ClaudeStatusBarController::showPermissionPrompt(pid_t awaitingPid,
                                                     bool belongsToFocused,
                                                     const QString &rule) {
    // Per-shell dedup (ANTS-1850). Remove only anchors this prompt
    // legitimately supersedes:
    //   * the SAME owning shell's prior anchor (a re-prompt), and
    //   * the scroll-scan path's bare buttons (no awaitingPid property),
    //     which must never stack beside the hook UI.
    // A DIFFERENT shell's pending hook anchor is LEFT ALONE: deleting it
    // would sever the retraction connections that clear ITS tab's glyph,
    // orphaning that dot until restart (the original ANTS-1850 symptom —
    // two prompts in different tabs, one dot stuck lit). Keeping each
    // pending prompt's anchor independent is what lets its own resolution
    // clear its own glyph.
    for (auto *w : m_statusBar->findChildren<QWidget *>(QStringLiteral("claudeAllowBtn"))) {
        const QVariant pidVar = w->property("claudeAwaitingPid");
        const qlonglong p = pidVar.toLongLong();
        const bool sameShell = (p > 0 && p == static_cast<qlonglong>(awaitingPid));
        const bool scrollScanButton = !pidVar.isValid() || p <= 0;
        if (sameShell || scrollScanButton)
            w->deleteLater();
    }

    // btnWidget is the lifecycle anchor for the retraction connections
    // that clear the glyph (wired below). It exists even for a
    // background-tab prompt — kept hidden — so the owning tab's dot is
    // cleared when the prompt resolves.
    // 0.6.29 — same objectName as the scroll-scan path's button so the
    // tab-switch cleanup in refreshStatusBarForActiveTab removes both.
    auto *btnWidget = new QWidget(m_statusBar);
    btnWidget->setObjectName(QStringLiteral("claudeAllowBtn"));
    // Tag the anchor with its owning shell so the per-shell dedup above can
    // tell a re-prompt (same pid → supersede) from a different tab's still-
    // pending prompt (keep). ANTS-1850.
    btnWidget->setProperty("claudeAwaitingPid", static_cast<qlonglong>(awaitingPid));
    // Fixed horizontal sizePolicy — must never be squeezed when the
    // notification slot is wide (user spec 2026-04-18).
    btnWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    auto clearPromptActive = [this, awaitingPid]() {
        // ANTS-1873 — the tracker is the source of truth. Updating it
        // emits shellStateChanged → apply() automatically.
        if (m_tracker && awaitingPid > 0)
            m_tracker->markShellAwaitingInput(awaitingPid, false);
    };

    if (belongsToFocused) {
        emit statusMessageRequested(QString("Claude permission: %1").arg(rule), 0);

        // Enhanced permission action buttons
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

        // ANTS-1873 — the tracker was set true by the caller
        // (permissionRequested slot or MainWindow scroll-scan); apply()
        // is now driven by the tracker's shellStateChanged signal.
        apply();

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
    } else {
        // Background-tab prompt — the owning tab's glyph already flags it;
        // don't paint the message/buttons on the focused tab. btnWidget
        // stays a hidden anchor for the retraction wiring.
        btnWidget->hide();
    }

    // Retraction — clear the buttons + glyph when the prompt resolves.
    // Scope the primary signal (`claudePermissionCleared`) to the OWNING
    // terminal so a prompt resolving in tab A can't clear tab B's still-
    // pending glyph (ANTS-1850; with per-shell anchors now coexisting, the
    // pre-1850 "listen on every terminal" would mis-fire). When the owning
    // shell isn't resolvable (untracked / awaitingPid <= 0) fall back to
    // every terminal — the pre-1835 behaviour.
    // Don't tie retraction to `outputReceived`, which fires on every
    // repaint and would retract while the prompt is still visible.
    TerminalWidget *owningTerm = nullptr;
    if (m_tabWidget && awaitingPid > 0) {
        for (auto *term : m_tabWidget->findChildren<TerminalWidget *>()) {
            if (term->shellPid() == awaitingPid) { owningTerm = term; break; }
        }
    }
    auto wireCleared = [btnWidget, clearPromptActive](TerminalWidget *term) {
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = connect(term, &TerminalWidget::claudePermissionCleared,
                        btnWidget, [btnWidget, conn, clearPromptActive]() {
            QObject::disconnect(*conn);
            btnWidget->deleteLater();
            clearPromptActive();
        });
    };
    if (owningTerm) {
        wireCleared(owningTerm);
    } else if (m_tabWidget) {
        for (auto *term : m_tabWidget->findChildren<TerminalWidget *>())
            wireCleared(term);
    }

    // 0.6.31 — belt-and-suspenders retraction. `claudePermissionCleared`
    // above only fires if the scroll-scanner saw this prompt; a hook-only
    // prompt (unmatched footer format, scrolled past the lookback window,
    // headless session) needs `toolFinished` (permission granted + tool
    // done) / `sessionStopped` (session ended) as proxies so the button
    // doesn't linger. These come from the SINGLETON integration, which
    // always reflects the FOCUSED tab — guard so a background anchor isn't
    // wrongly cleared by the focused tab's tool finishing (ANTS-1850).
    auto focusedMatches = [this, awaitingPid]() -> bool {
        if (awaitingPid <= 0) return true;   // unrouted → behave as before
        auto *term = m_currentTerminalProvider ? m_currentTerminalProvider() : nullptr;
        return term && term->shellPid() == awaitingPid;
    };
    auto finishedConn = std::make_shared<QMetaObject::Connection>();
    *finishedConn = connect(m_integration, &ClaudeIntegration::toolFinished,
                            btnWidget, [btnWidget, finishedConn, clearPromptActive, focusedMatches](const QString &, bool) {
        if (!focusedMatches()) return;       // not this tab's tool — keep waiting
        QObject::disconnect(*finishedConn);
        btnWidget->deleteLater();
        clearPromptActive();
    });
    auto stoppedConn = std::make_shared<QMetaObject::Connection>();
    *stoppedConn = connect(m_integration, &ClaudeIntegration::sessionStopped,
                           btnWidget, [btnWidget, stoppedConn, clearPromptActive, focusedMatches](const QString &) {
        if (!focusedMatches()) return;
        QObject::disconnect(*stoppedConn);
        btnWidget->deleteLater();
        clearPromptActive();
    });
}

void ClaudeStatusBarController::maybeShowPromptForActiveTab(pid_t focusedPid) {
    if (!m_tracker || focusedPid <= 0) return;
    const auto st = m_tracker->shellState(focusedPid);
    if (!st.awaitingInput || st.awaitingRule.isEmpty()) return;
    // The newly-focused tab has a still-pending permission prompt whose
    // bottom-bar buttons were torn down on the previous tab switch
    // (refreshStatusBarForActiveTab Category C). Re-paint them so the user
    // can Allow/Deny from the bar without hunting in the terminal. The
    // prompt now belongs to the focused tab, so belongsToFocused is true.
    // ANTS-1851.
    showPermissionPrompt(focusedPid, /*belongsToFocused=*/true, st.awaitingRule);
}

void ClaudeStatusBarController::clearPromptAnchorsForTabSwitch(pid_t newlyFocusedPid) {
    if (!m_statusBar) return;
    // The hook-server path uses a QWidget container named "claudeAllowBtn"
    // holding Allow/Deny/Add children; the scroll-scan path creates a bare
    // QPushButton with the same objectName. Finding by QWidget covers both.
    for (auto *w : m_statusBar->findChildren<QWidget *>(QStringLiteral("claudeAllowBtn"))) {
        const QVariant pidVar = w->property("claudeAwaitingPid");
        const qlonglong p = pidVar.toLongLong();
        // ANTS-1852 — keep a BACKGROUND tab's still-pending anchor alive
        // (hidden) instead of destroying it. The anchor owns the
        // claudePermissionCleared / toolFinished / sessionStopped retraction
        // connections (scoped to the owning terminal — INV-3) that clear the
        // tab's awaiting-input dot. Pre-1852 the blanket deleteLater severed
        // them on switch-away, so a prompt resolving while you viewed a
        // DIFFERENT tab left the dot stuck lit until the next switch back. A
        // surviving hidden anchor catches its own off-tab resolution and
        // clears the glyph. Still deleteLater'd: the focused tab's own anchor
        // (rebuilt fresh by maybeShowPromptForActiveTab right after the
        // Category-C teardown), scroll-scan bare buttons (no pid property),
        // and any anchor whose shell is no longer awaiting input (resolved /
        // shell gone). newlyFocusedPid <= 0 (teardown) deletes everything.
        const bool pending =
            newlyFocusedPid > 0 && pidVar.isValid() && p > 0 &&
            p != static_cast<qlonglong>(newlyFocusedPid) &&
            m_tracker &&
            m_tracker->shellState(static_cast<pid_t>(p)).awaitingInput;
        if (pending)
            w->hide();
        else
            w->deleteLater();
    }
}

// ANTS-1873 — setPromptActive / setPlanMode / setAuditing are kept as
// thin apply() triggers so existing MainWindow callers still compile.
// The value parameters are ignored: apply() re-derives state from the
// focused tab's tracker entry every call.
void ClaudeStatusBarController::setPromptActive(bool /*active*/) {
    apply();
}

void ClaudeStatusBarController::setPlanMode(bool /*active*/) {
    apply();
}

void ClaudeStatusBarController::setAuditing(bool /*active*/) {
    apply();
}

void ClaudeStatusBarController::setError(const QString &text,
                                         const QString &tooltip,
                                         int autoHideMs) {
    if (!m_errorLabel) return;
    m_errorLabel->setText(text);
    m_errorLabel->setToolTip(tooltip);
    m_errorLabel->show();
    // Cancel any prior auto-hide; without this a second setError() in
    // the first's window would inherit the prior singleShot and hide
    // the new message early.
    if (!m_errorHideTimer) {
        m_errorHideTimer = new QTimer(this);
        m_errorHideTimer->setSingleShot(true);
        connect(m_errorHideTimer, &QTimer::timeout,
                m_errorLabel, &QWidget::hide);
    }
    m_errorHideTimer->stop();
    // autoHideMs <= 0 means "sticky" — leave shown until clearError().
    if (autoHideMs > 0) m_errorHideTimer->start(autoHideMs);
}

void ClaudeStatusBarController::clearError() {
    if (m_errorHideTimer) m_errorHideTimer->stop();
    if (m_errorLabel) m_errorLabel->hide();
}

void ClaudeStatusBarController::resetForTabSwitch() {
    // ANTS-1873 — cached scalars deleted; the trailing apply() below
    // re-derives the new focused tab's display state from its tracker
    // entry. Widget hides + bg-tasks/tasks/model-chip resets remain.
    if (m_reviewBtn)   m_reviewBtn->hide();
    if (m_contextBar)  m_contextBar->hide();
    if (m_bgTasksBtn)  m_bgTasksBtn->hide();
    // ANTS-1053 — per-PID trackers retain their transcript paths across
    // tab switches; only the button visibility is reset here. The next
    // refreshBgTasksButton tick re-derives the count for the new focused tab.
    if (m_tasksBtn)    m_tasksBtn->hide();
    // ANTS-1219-INV-4: tab-switch reset clears the tracker's bound
    // path synchronously so a stale path from the prior tab cannot
    // contribute tasks under the newly-focused tab. The next
    // refreshTasksButton tick re-binds via INV-1.
    if (m_tasks)       m_tasks->setTranscriptPath(QString());
    // ANTS-1814 — the model-recommender chip was the one tracker omitted from
    // the tab-switch reset: it would keep showing the prior tab's "→ Opus"
    // recommendation (with a click that sends `/model …` to the now-focused,
    // wrong tab) until the next 2 s timer tick. Hide it and drop its mtime
    // cache so refreshModelChip re-scores against the newly-focused tab.
    if (m_modelBtn)    m_modelBtn->hide();
    m_modelChipPath.clear();
    m_modelChipMtimeMs = -1;
    // ANTS-1888 — same reset for the model-state chip so it doesn't briefly
    // show the previous tab's tier after a switch.
    if (m_modelStateBtn) m_modelStateBtn->hide();
    m_modelStatePath.clear();
    m_modelStateMtimeMs = -1;
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
    if (!m_bgTasksBtn) return;
    // Resolve the focused shell's PID → look up its per-PID tracker.
    // Per-PID trackers (ANTS-1053) own their own QFileSystemWatcher so
    // background tabs stay up-to-date; we only need to update the
    // focused tab's transcript path on each tick.
    auto *focused = m_focusedTerminalProvider ? m_focusedTerminalProvider() : nullptr;
    const bool focusedTabPresent = (focused != nullptr);
    const pid_t focusedPid = focused ? focused->shellPid() : 0;

    // Lazy-create a tracker for the focused shell in case trackBgShell
    // wasn't called yet (e.g. restored tabs arriving before the signal
    // connection is wired).
    if (focusedPid > 0 && !m_bgTrackers.contains(focusedPid))
        trackBgShell(focusedPid);

    ClaudeBgTaskTracker *tracker = focusedPid > 0
        ? m_bgTrackers.value(focusedPid, nullptr) : nullptr;
    if (!tracker) {
        if (DebugLog::enabled(DebugLog::Claude))
            ANTS_LOG(DebugLog::Claude,
                     "bgtasks/refresh: focused-pid=%d no tracker — early return",
                     static_cast<int>(focusedPid));
        m_bgTasksBtn->hide();
        return;
    }

    // Resolve the transcript path scoped to the active tab's project
    // tree. activeSessionPath walks up the cwd, encodes each ancestor
    // to Claude Code's `<dashed-cwd>` form, and returns the newest
    // `.jsonl` from the deepest matching `~/.claude/projects/<…>/`
    // subdir. Without this scoping, sessions from *other* projects
    // (e.g. another tab's tree) would leak into the bg-tasks surface
    // — which is exactly the user-reported 2026-04-27 bug (INV-11).
    QString cwd;
    if (focused) cwd = focused->shellCwd();
    QString path;
    if (m_integration) path = m_integration->activeSessionPath(cwd);
    const QString prevPath = tracker->transcriptPath();
    tracker->setTranscriptPath(path);
    // 0.7.55 — sweep liveness only on unchanged path, not full rescan.
    // setTranscriptPath() triggers a full rescan only when the path changes.
    // The QFileSystemWatcher drives full rescan on transcript appends.
    if (!path.isEmpty() && path == prevPath) {
        tracker->poll();
        tracker->sweepLiveness();
    }

    const int running = tracker->runningCount();
    const int total = tracker->tasks().size();

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
        // ANTS-1854 — log only on a state transition. The signature
        // excludes the prev-changed flag (a pure per-tick artifact)
        // and keys on the fields a reviewer cares about: tab presence,
        // resolved path, running/total counts, and the visibility
        // branch. Consecutive identical no-op polls are suppressed.
        const QString sig = QStringLiteral("%1|%2|%3|%4|%5")
            .arg(focusedTabPresent ? 1 : 0)
            .arg(pathShort)
            .arg(running)
            .arg(total)
            .arg(QLatin1String(branch));
        if (sig != m_lastBgTasksLogSig) {
            m_lastBgTasksLogSig = sig;
            ANTS_LOG(DebugLog::Claude,
                     "bgtasks/refresh: focused-tab=%s cwd=%s path=%s "
                     "prev-changed=%s running=%d total=%d → %s",
                     focusedTabPresent ? "yes" : "no",
                     cwdShort.toUtf8().constData(),
                     pathShort.toUtf8().constData(),
                     (path == prevPath) ? "no" : "yes",
                     running, total, branch);
        }
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

void ClaudeStatusBarController::refreshTasksButton() {
    if (!m_tasks || !m_tasksBtn) {
        ANTS_LOG(DebugLog::Claude,
                 "tasks/refresh: tracker=%p btn=%p — early return",
                 static_cast<void *>(m_tasks),
                 static_cast<void *>(m_tasksBtn));
        return;
    }

    // Resolve transcript path the same way refreshBgTasksButton does
    // — scope to the focused tab's project tree.
    QString cwd;
    auto *focused = m_focusedTerminalProvider
                        ? m_focusedTerminalProvider() : nullptr;
    if (focused) cwd = focused->shellCwd();
    const bool focusedTabPresent = (focused != nullptr);
    QString path;
    // ANTS-1219-INV-1: refresh wiring — resolver result becomes the
    // tracker's bound source path on every change. Empty path (no
    // live Claude / freshness floor rejected all candidates) is
    // pushed through identically and clears the tracker, satisfying
    // ANTS-1219-INV-3.
    if (m_integration) path = m_integration->activeSessionPath(cwd);
    const QString prevPath = m_tasks->transcriptPath();
    if (path != prevPath)
        m_tasks->setTranscriptPath(path);

    // ANTS-1219-INV-6: polling rescue — QFileSystemWatcher silently
    // drops its watch on atomic rewrite (Claude writes the JSONL via
    // tmpfile+rename), so fileChanged stops firing and the dialog
    // freezes. poll() mtime-checks m_transcriptPath and only rescans
    // on real change. Same shape as ClaudeBgTaskTracker::sweepLiveness
    // on the bg-tasks side.
    //
    // ANTS-1458 — latency instrumentation (phase 1). Sample mtime +
    // task count BEFORE poll() so the debug log can report whether
    // (a) the tick saw an mtime advance, (b) the rescan picked up
    // new tasks, and (c) how long the parse took. Cheap (no extra
    // stat — QFileInfo's lastModified is the same call poll() makes
    // internally; the value reads from kernel cache after the first
    // hit per tick).
    const int beforeTotal = m_tasks->totalCount();
    QFileInfo preFi(path);
    const qint64 preMtimeMs = (path.isEmpty() || !preFi.exists())
        ? -1 : preFi.lastModified().toMSecsSinceEpoch();
    const qint64 preRescanMs = m_tasks->lastRescanMtimeMs();
    QElapsedTimer pollTimer;
    pollTimer.start();
    m_tasks->poll();
    const qint64 pollDurUs = pollTimer.nsecsElapsed() / 1000;

    const int total      = m_tasks->totalCount();
    const int unfinished = m_tasks->unfinishedCount();
    const int inProgress = m_tasks->inProgressCount();
    const int pending    = m_tasks->pendingCount();
    const int done       = m_tasks->completedCount();

    // Diagnostic logging mirrors refreshBgTasksButton's pattern (see
    // line 615+). Gated on ANTS_DEBUG=claude (or runtime toggle).
    // Lets the user capture why the chip hides under realistic
    // conditions — added 2026-05-08 because the 11-task TaskCreate
    // run that day left the chip hidden with no signal to debug.
    if (DebugLog::enabled(DebugLog::Claude)) {
        const QString cwdShort = cwd.isEmpty()
            ? QStringLiteral("(empty)") : cwd;
        const QString pathShort = path.isEmpty()
            ? QStringLiteral("(empty)")
            : path.section('/', -1);
        const char *branch = (total > 0 && done < total)
            ? "SHOW" : "HIDE/empty-or-done";
        // ANTS-1458 — mtime + delta + parse-duration columns.
        const int totalDelta = total - beforeTotal;
        const qint64 mtimeDeltaMs = (preMtimeMs < 0 || preRescanMs <= 0)
            ? -1 : (preMtimeMs - preRescanMs);
        // ANTS-1854 — suppress consecutive no-op poll lines. The
        // signature keys on the transition set (focus, path, all task
        // counts, visibility branch) and deliberately excludes
        // poll-dur-us (pure per-tick timing) and the prev-changed flag,
        // so a quiet 2 s tick that re-derives identical state emits
        // nothing.
        //
        // ANTS-1859 — the transcript mtime is excluded from the key.
        // Claude streams its output into the JSONL continuously, so the
        // mtime advanced on essentially every poll even when the task
        // list was unchanged, re-logging a near-identical line every
        // ~2 s for the whole session. mtime stays in the logged *line*
        // below (the ANTS-1458 latency column) so a genuine state change
        // still carries its mtime context — it is just no longer part of
        // the dedup key.
        const QString sig = QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
            .arg(focusedTabPresent ? 1 : 0)
            .arg(pathShort)
            .arg(total)
            .arg(unfinished)
            .arg(inProgress)
            .arg(pending)
            .arg(QStringLiteral("%1|%2").arg(done).arg(QLatin1String(branch)));
        if (sig != m_lastTasksLogSig) {
            m_lastTasksLogSig = sig;
            ANTS_LOG(DebugLog::Claude,
                     "tasks/refresh: focused-tab=%s cwd=%s path=%s "
                     "prev-changed=%s mtime=%lld rescan-mtime=%lld "
                     "mtime-delta-ms=%lld poll-dur-us=%lld "
                     "delta=%+d total=%d unfinished=%d "
                     "in-progress=%d pending=%d done=%d → %s",
                     focusedTabPresent ? "yes" : "no",
                     cwdShort.toUtf8().constData(),
                     pathShort.toUtf8().constData(),
                     (path == prevPath) ? "no" : "yes",
                     static_cast<long long>(preMtimeMs),
                     static_cast<long long>(preRescanMs),
                     static_cast<long long>(mtimeDeltaMs),
                     static_cast<long long>(pollDurUs),
                     totalDelta,
                     total, unfinished, inProgress, pending, done, branch);
        }
    }

    // ANTS-1219-INV-3 / ANTS-1216 / ANTS-1246: hide branch covers
    // (a) the empty-resolver case (path was "", tracker cleared,
    // total == 0) and (b) the "all done" case (every task in
    // status `completed`). ANTS-1216 user report 2026-05-08: a tab
    // whose tasks are all completed kept the chip visible at
    // "☰ N/N", which read as actionable chrome when there was
    // nothing left to do. ANTS-1246 (2026-05-12) refined the
    // visibility predicate: the chip's purpose is now pure
    // *progress*, visible end-to-end from 0/N to N/N; an all-done
    // state hides because the user already sees the completed
    // list in the dialog. The "is Claude doing something?" signal
    // is carried separately by the Claude: <state> widget.
    if (total <= 0 || done >= total) {
        m_tasksBtn->hide();
        return;
    }
    // ANTS-1218 / ANTS-1246: chip reads `☰ <done>/<total>` —
    // pure completion progress. Numerator was previously
    // `total - unfinished` (= done + in_progress under post-1221
    // semantics), which kept the chip hidden when only in_progress
    // tasks remained (no pending), surprising the user mid-run.
    // ANTS-1246 switches to `done` so the chip is visible during
    // the entire active task list and hides only on completion.
    m_tasksBtn->setText(QStringLiteral("☰ %1/%2").arg(done).arg(total));
    m_tasksBtn->setToolTip(
        tr("Claude Code task list — %1 task%2 (%3 done, %4 running, "
           "%5 outstanding). Click to view.")
            .arg(total)
            .arg(total == 1 ? QString() : QStringLiteral("s"))
            .arg(done)
            .arg(inProgress)
            .arg(pending));
    m_tasksBtn->show();
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
    // Return the focused shell's tracker (the one whose count is displayed).
    auto *focused = m_focusedTerminalProvider ? m_focusedTerminalProvider() : nullptr;
    if (!focused) return nullptr;
    return m_bgTrackers.value(focused->shellPid(), nullptr);
}

void ClaudeStatusBarController::trackBgShell(pid_t pid) {
    if (pid <= 0 || m_bgTrackers.contains(pid)) return;
    auto *tracker = new ClaudeBgTaskTracker(this);
    connect(tracker, &ClaudeBgTaskTracker::tasksChanged,
            this, &ClaudeStatusBarController::refreshBgTasksButton);
    m_bgTrackers.insert(pid, tracker);
}

void ClaudeStatusBarController::untrackBgShell(pid_t pid) {
    ClaudeBgTaskTracker *tracker = m_bgTrackers.take(pid);
    delete tracker;  // QObject: disconnects signals and frees; nullptr-safe
}

QPushButton *ClaudeStatusBarController::tasksButton() const {
    return m_tasksBtn;
}

ClaudeTaskListTracker *ClaudeStatusBarController::tasksTracker() const {
    return m_tasks;
}

void ClaudeStatusBarController::apply() {
    if (!m_statusLabel) return;
    const QString statusStyle = QStringLiteral("padding: 0 8px; font-size: 11px;");

    // ANTS-1873 — read the focused tab's state via the shared resolver,
    // same source the tab dot consumes. The two surfaces cannot diverge
    // because they walk the same precedence ladder over the same data.
    const claudestate::Resolved r = claudestate::forFocused(
        m_tracker,
        m_focusedTerminalProvider ? m_focusedTerminalProvider() : nullptr);
    const claudestate::Display d = claudestate::display(r);

    if (d == claudestate::Display::Hidden) {
        m_statusLabel->hide();
        if (m_contextBar) m_contextBar->hide();
        return;
    }

    QString text;
    ClaudeTabIndicator::Glyph glyph = ClaudeTabIndicator::Glyph::Idle;

    // Status text vocabulary (user spec 2026-04-18):
    //   idle / thinking / prompting / bash / reading a file / planning /
    //   auditing / compacting / etc.
    switch (d) {
        case claudestate::Display::Hidden:
            return;  // handled above
        case claudestate::Display::AwaitingInput:
            text = QStringLiteral("Claude: prompting");
            glyph = ClaudeTabIndicator::Glyph::AwaitingInput;
            break;
        case claudestate::Display::Planning:
            text = QStringLiteral("Claude: planning");
            glyph = ClaudeTabIndicator::Glyph::Planning;
            break;
        case claudestate::Display::Auditing:
            text = QStringLiteral("Claude: auditing");
            glyph = ClaudeTabIndicator::Glyph::Auditing;
            break;
        case claudestate::Display::Idle:
            text = QStringLiteral("Claude: idle");
            glyph = ClaudeTabIndicator::Glyph::Idle;
            break;
        case claudestate::Display::Thinking:
            text = QStringLiteral("Claude: thinking");
            glyph = ClaudeTabIndicator::Glyph::Thinking;
            break;
        case claudestate::Display::ToolUseBash:
            text = QStringLiteral("Claude: bash");
            glyph = ClaudeTabIndicator::Glyph::Bash;
            break;
        case claudestate::Display::ToolUseGeneric: {
            // Map tool name → friendly vocabulary per user spec. Unknown
            // tools fall through to the raw name so MCP / custom tools
            // remain legible.
            const QString lower = r.tool.trimmed().toLower();
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
            } else if (lower.isEmpty()) {
                text = QStringLiteral("Claude: thinking");
            } else {
                text = QStringLiteral("Claude: %1").arg(r.tool.trimmed());
            }
            glyph = ClaudeTabIndicator::Glyph::ToolUse;
            break;
        }
        case claudestate::Display::Compacting:
            text = QStringLiteral("Claude: compacting");
            glyph = ClaudeTabIndicator::Glyph::Compacting;
            break;
    }

    // ANTS-1847 — same contrast adaptation as the tab dot, against the
    // status bar's own background (theme.bgSecondary — the surface the
    // label paints on, identical to the tab bar's), so the text and the
    // active tab's dot resolve to the same colour on every theme.
    const QColor bg = Themes::byName(m_currentThemeName).bgSecondary;
    const QColor color = ClaudeTabIndicator::contrastColor(glyph, bg);
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

// ANTS-1226 — Passive model-tier recommender chip.
// Reads the last 20 assistant turns from the active session's
// transcript, scores complexity, and shows a chip when the
// recommendation differs from the current model in use.
void ClaudeStatusBarController::refreshModelChip()
{
    if (!m_modelBtn) return;

    // ANTS-1735 INV-14 — Shape A chip is fully suppressed when the
    // autonomous switcher is enabled. The user opted in to "Ants picks
    // the model"; showing a clickable → Opus chip during the 90 s dwell
    // reintroduces the manual-decision surface that pushed the user to
    // autonomy in the first place.
    Config cfg;
    if (cfg.claudeAutoModel().value("switch_enabled").toBool()) {
        m_modelBtn->hide();
        return;
    }

    // Resolve transcript path — same pattern as refreshTasksButton.
    QString cwd;
    auto *focused = m_focusedTerminalProvider
        ? m_focusedTerminalProvider() : nullptr;
    if (focused) cwd = focused->shellCwd();
    QString transcriptPath;
    if (m_integration) transcriptPath = m_integration->activeSessionPath(cwd);

    if (transcriptPath.isEmpty()) {
        m_modelBtn->hide();
        return;
    }

    // ANTS-1787 — mtime short-circuit. score() tail-reads ≤512 KB and
    // scores the last 20 turns on every 2 s tick; the sibling task /
    // bg-task trackers already gate on mtime via poll(). Skip the work
    // when the transcript hasn't changed since the last score — the chip
    // already reflects that parse.
    const qint64 mtimeMs =
        QFileInfo(transcriptPath).lastModified().toMSecsSinceEpoch();
    if (transcriptPath == m_modelChipPath && mtimeMs == m_modelChipMtimeMs) {
        return;
    }
    m_modelChipPath    = transcriptPath;
    m_modelChipMtimeMs = mtimeMs;

    const ModelRecommender::Result rec =
        ModelRecommender::score(transcriptPath);

    // INV-4: hide chip when recommendation matches current model tier.
    const ModelRecommender::Tier currentTier =
        ModelRecommender::tierFromModelId(rec.currentModel);
    if (rec.tier == currentTier) {
        m_modelBtn->hide();
        return;
    }

    const QString tierLabel = [&]() -> QString {
        switch (rec.tier) {
        case ModelRecommender::Tier::Haiku: return tr("→ Haiku");
        case ModelRecommender::Tier::Opus:  return tr("→ Opus");
        default:                            return tr("→ Sonnet");
        }
    }();
    // Stable, locale-independent /model argument for the click handler.
    const QString tierArg = [&]() -> QString {
        switch (rec.tier) {
        case ModelRecommender::Tier::Haiku: return QStringLiteral("haiku");
        case ModelRecommender::Tier::Opus:  return QStringLiteral("opus");
        default:                            return QStringLiteral("sonnet");
        }
    }();

    m_modelBtn->setText(tierLabel);
    m_modelBtn->setProperty("modelTier", tierArg);
    m_modelBtn->setToolTip(
        tr("Suggested model: %1\nReason: %2\n"
           "Click to send /model %3 to the focused terminal.")
            .arg(ModelRecommender::tierName(rec.tier))
            .arg(rec.reason.isEmpty()
                 ? tr("default heuristic") : rec.reason)
            .arg(ModelRecommender::tierName(rec.tier)));
    m_modelBtn->show();
}

// ANTS-1888 — Passive readout: current model tier + last-used thinking
// level for the focused tab. Always shows when a Claude session is active
// in the focused tab (the recommender chip suppresses itself when the
// auto-switcher is on; this chip is the always-on counterpart). Hides
// only the thinking half when the level is undetectable — never renders
// the word "Unknown".
void ClaudeStatusBarController::refreshModelStateChip()
{
    if (!m_modelStateBtn) return;

    // Resolve transcript path — same pattern as refreshModelChip.
    QString cwd;
    auto *focused = m_focusedTerminalProvider
        ? m_focusedTerminalProvider() : nullptr;
    if (focused) cwd = focused->shellCwd();
    QString transcriptPath;
    if (m_integration) transcriptPath = m_integration->activeSessionPath(cwd);

    if (transcriptPath.isEmpty()) {
        m_modelStateBtn->hide();
        return;
    }

    // mtime short-circuit — score() and thinkingLevelFromLatestUserTurn
    // both tail-read up to 512 KB on every tick; skip when nothing has
    // changed since the last paint.
    const qint64 mtimeMs =
        QFileInfo(transcriptPath).lastModified().toMSecsSinceEpoch();
    // ANTS-1926 — also invalidate when pending-switch tier changes (set/cleared
    // by refreshAutoModelSwitch independently of transcript mtime).
    if (transcriptPath == m_modelStatePath && mtimeMs == m_modelStateMtimeMs
            && m_autoSwitchPendingTier == m_modelStateLastPendingTier) {
        return;
    }
    m_modelStatePath              = transcriptPath;
    m_modelStateMtimeMs           = mtimeMs;
    m_modelStateLastPendingTier   = m_autoSwitchPendingTier;

    // Reuse the scorer's transcript parse to get the current model id —
    // it's the same ≤512 KB tail-read the auto-switcher tick already pays
    // for in the 2 s status timer.
    const ModelRecommender::Result rec =
        ModelRecommender::score(transcriptPath);
    if (rec.currentModel.isEmpty()) {
        // No assistant turn yet — the session is brand new. Hide rather
        // than guess at "Sonnet" (score()'s default fallback).
        m_modelStateBtn->hide();
        return;
    }
    const ModelRecommender::Tier tier =
        ModelRecommender::tierFromModelId(rec.currentModel);
    const QString tierTitle = [&]() -> QString {
        switch (tier) {
        case ModelRecommender::Tier::Haiku: return tr("Haiku");
        case ModelRecommender::Tier::Opus:  return tr("Opus");
        default:                            return tr("Sonnet");
        }
    }();

    const ModelRecommender::ThinkingLevel level =
        ModelRecommender::thinkingLevelFromLatestUserTurn(transcriptPath);
    const QString thinkLabel = ModelRecommender::thinkingLevelLabel(level);

    // Compose: "Opus · ultrathink" / "Sonnet · standard" / just "Haiku"
    // when the thinking half is empty (Unknown).
    QString text = tierTitle;
    if (!thinkLabel.isEmpty()) {
        text += QStringLiteral(" · ") + thinkLabel;
    }
    // ANTS-1926 — pending-switch annotation. When the auto-switcher has
    // queued a switch (blocked only by composer_not_empty), append a
    // "(→ Haiku)" suffix so the user can see the intent at a glance.
    if (!m_autoSwitchPendingTier.isEmpty()) {
        const QString pendingTitle =
            m_autoSwitchPendingTier.left(1).toUpper() +
            m_autoSwitchPendingTier.mid(1);
        text += QStringLiteral(" (→ ") + pendingTitle + QStringLiteral(")");
    }
    m_modelStateBtn->setText(text);
    const QString pendingTip = m_autoSwitchPendingTier.isEmpty()
        ? QString()
        : tr("\nAuto-switch to %1 is queued — will fire when you send "
             "or clear the composer.")
              .arg(m_autoSwitchPendingTier.left(1).toUpper() +
                   m_autoSwitchPendingTier.mid(1));
    m_modelStateBtn->setToolTip(
        tr("Focused tab is using %1 (from the transcript's most recent "
           "assistant turn).\nThinking level: %2 (parsed from the most "
           "recent user prompt; \"standard\" when no directive is set).")
            .arg(tierTitle)
            .arg(thinkLabel.isEmpty() ? tr("unknown") : thinkLabel)
        + pendingTip);
    m_modelStateBtn->show();
}

// ANTS-1735 §2.3 — autonomous switcher tick. Runs on the 2 s status
// timer, alongside refreshModelChip. Default-off: bails immediately
// when claude.auto_model_switch is false (INV-14). When enabled,
// builds the Gate from the focused tab's tracker entry + scorer +
// keystroke-timing composerEmpty proxy, calls decide(), and on act
// injects `/model <tier>\n` plus appends a ledger record.
void ClaudeStatusBarController::refreshAutoModelSwitch()
{
    Config cfg;
    const QJsonObject autoCfg = cfg.claudeAutoModel();
    const bool enabled = autoCfg.value("switch_enabled").toBool();

    auto *focused = m_focusedTerminalProvider
        ? m_focusedTerminalProvider() : nullptr;
    if (!focused || !m_tracker) return;
    const pid_t pid = focused->shellPid();
    if (pid <= 0) return;

    // INV-2 — focused tab's per-shell state (not the process-global
    // currentState()).
    const ClaudeTabTracker::ShellState s = m_tracker->shellState(pid);

    // ANTS-1735 §8 OQ-3 — first-run nudge. Fire at most once per process
    // when the user has Claude Code running in the focused tab but hasn't
    // yet seen the opt-in prompt. MainWindow persists the dismissal via
    // Config::setClaudeAutoModelNudgeShown so a future session never
    // re-prompts. The latch m_firstRunNudgeEmitted prevents repeated
    // emission within this process before MainWindow's slot persists.
    if (!enabled
        && !m_firstRunNudgeEmitted
        && !cfg.claudeAutoModelNudgeShown()
        && s.state != ClaudeState::NotRunning) {
        m_firstRunNudgeEmitted = true;
        emit firstRunNudgeRequested();
    }

    if (!enabled) return;

    // §2.4 — keystroke-timing composerEmpty proxy.
    const bool composerEmpty =
        focused->lastUserKeystrokeMs() < s.idleSinceMs;
    // ANTS-1908 — soft-veto telemetry. ms since the most-recent
    // keystroke landed in the focused tab; -1 when no keystroke has
    // been recorded yet (the controller leaves the gate at its
    // legacy hard-veto behaviour in that case). The decide() helper
    // honours the configured composerStaleThreshold to yield the veto
    // when the composer carries text that hasn't been touched recently —
    // the dominant blocker pattern observed on long autonomous /loop
    // sessions where the user's last continuation prompt is sitting
    // idle in the composer (ROADMAP ANTS-1908 + model_switch_stats
    // near-miss telemetry; ANTS-1914 makes the threshold configurable).
    const qint64 lastKeystrokeMs = focused->lastUserKeystrokeMs();
    // ANTS-1914 — read the user-configurable composer-stale threshold.
    // Defaults to -1 (use kComposerStaleVetoMs); advanced users can lower
    // it via Config to unblock slash-command queueing (e.g. 30000 ms = 30 s).
    // The config key is "claude.auto_model_composer_stale_ms".
    const qint64 composerStaleThresholdMs =
        static_cast<qint64>(autoCfg.value("composer_stale_ms").toInt(-1));

    // Score the focused transcript.
    QString transcriptPath;
    if (m_integration) {
        transcriptPath = m_integration->activeSessionPath(focused->shellCwd());
    }
    if (transcriptPath.isEmpty()) return;
    const ModelRecommender::Result rec =
        ModelRecommender::score(transcriptPath);

    // Resolve floor from config string. ANTS-1944 — floor + clampedTarget are
    // computed BEFORE `current` so reconcileCurrentTier can compare the actuator
    // record against the exact tier decide() would re-fire.
    const QString floorStr = autoCfg.value("floor").toString(QStringLiteral("haiku"));
    const ModelRecommender::Tier floor =
        (floorStr == QLatin1String("sonnet"))
            ? ModelRecommender::Tier::Sonnet
            : ModelRecommender::Tier::Haiku;
    const ModelRecommender::Tier clampedTarget =
        ModelAutoSwitch::clampToFloor(rec.tier, floor);

    // ANTS-1944 — anchor `current` to the actuator's last-injected tier when the
    // transcript read is stale (idle / context-compression drops the /model
    // events), but ONLY to suppress re-firing the SAME tier — never to override
    // toward a tier the user manually picked. Pure helper; see modelautoswitch.h.
    const ModelRecommender::Tier current = ModelAutoSwitch::reconcileCurrentTier(
        ModelRecommender::tierFromModelId(rec.currentModel),
        rec.currentModelFromCommand,
        rec.currentModelTsMs,
        m_autoSwitchLastTier,
        m_autoSwitchLastMs,
        clampedTarget);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    // INV-5 — stability is counted against the CLAMPED target.
    // ANTS-1925 — reset-hysteresis: a single noisy tick back to the current
    // model does not wipe the stable counter. ANTS-1928 — the tier-lock window
    // additionally holds a near-ready candidate against brief boundary-noise
    // reversions, and accrual is now per-candidate-tier. All of this is folded
    // by the pure advanceStability() helper (table-tested in
    // tests/features/model_auto_switch/). ANTS-1944 — advanceStability now sees
    // the reconciled `current`, so a stale transcript no longer accrues stability
    // toward a repeat of the switch already made.
    m_autoSwitchStability = ModelAutoSwitch::advanceStability(
        m_autoSwitchStability, clampedTarget, current, nowMs);
    const qint64 dwellMs = m_autoSwitchLastMs > 0
                               ? (nowMs - m_autoSwitchLastMs)
                               : kMaxDwellSentinel();

    // Configurable min-dwell — clamped via Config::claudeAutoModel().
    // ANTS-1940 — scale by the regret-driven conservatism multiplier (1.0
    // when trust is established or regret is low; up to 2× while calibrating
    // with high regret, halved to 1.5× in a clearly-mechanical session).
    const qint64 minDwellMsBase =
        static_cast<qint64>(autoCfg.value("min_dwell_sec").toInt(90)) * 1000;
    const double conservMult = conservatismMultiplierFor(
        focused->shellCwd(), nowMs, rec.isMechanical);
    const qint64 minDwellMs = static_cast<qint64>(
        static_cast<double>(minDwellMsBase) * conservMult);

    // ANTS-1890 INV-7 / INV-8 — per-project override cool-down. Look up
    // the cached ms timestamp for the focused tab's project; -1 sentinel
    // means no override on record for THIS project (an override on
    // project A must NOT block an auto-switch on project B). Translate
    // to ms-since-then; the gate's `>= 0 && < kOverrideCooldownMs` rule
    // does the rest in decide().
    const QString projectRoot = focused->shellCwd();
    const qint64 lastOverrideMs =
        m_lastOverrideMsByProject.value(projectRoot, -1);
    const qint64 msSinceOverride =
        (lastOverrideMs > 0) ? (nowMs - lastOverrideMs) : -1;

    ModelAutoSwitch::Gate gate;
    gate.enabled              = true;
    gate.focusedState         = s.state;
    gate.composerEmpty        = composerEmpty;
    gate.current              = current;
    gate.recommended          = rec.tier;
    gate.floor                = floor;
    gate.ticksTargetStable    = m_autoSwitchStability.ticksStable;
    gate.msSinceLastSwitch    = dwellMs;
    // ANTS-1894 INV-3 — configurable min-dwell folded into the gate so
    // dwell_time_insufficient is observable as a near-miss reason. The
    // pre-1894 pre-decide short-circuit at L1415 is removed; decide()
    // now applies max(kMinDwellMs, configuredMinDwellMs) via the new
    // effectiveMinDwellMs helper.
    gate.configuredMinDwellMs = minDwellMs;
    gate.msSinceLastOverride  = msSinceOverride;
    // ANTS-1908 — populate composerStaleMs from the keystroke
    // timestamp. -1 when no keystroke recorded yet (matches the gate
    // sentinel exactly), else the elapsed ms.
    gate.composerStaleMs      = (lastKeystrokeMs > 0)
        ? (nowMs - lastKeystrokeMs)
        : -1;
    // ANTS-1914 — populate the configurable composer-stale threshold
    // (default -1 = use kComposerStaleVetoMs).
    gate.composerStaleThresholdMs = composerStaleThresholdMs;
    // ANTS-1917 — idle end-of-session suppression. idleElapsedMs is how long
    // the focused shell has sat in Idle (re-stamped each turn boundary, so a
    // fresh between-turns gap reads small and a walk-away grows unboundedly).
    // -1 = not idle or no idleSinceMs stamp yet → the gate doesn't fire. The
    // ceiling is configurable via claude.auto_model_idle_ceiling_sec (default
    // kIdleEndOfSessionMs / 1000); a value <= 0 disables the gate entirely by
    // leaving idleElapsedMs at -1.
    const int idleCeilingSec = autoCfg.value("idle_ceiling_sec").toInt(
        static_cast<int>(ModelAutoSwitch::kIdleEndOfSessionMs / 1000));
    if (idleCeilingSec > 0) {
        gate.idleCeilingMs = static_cast<qint64>(idleCeilingSec) * 1000;
        if (s.state == ClaudeState::Idle && s.idleSinceMs > 0)
            gate.idleElapsedMs = nowMs - s.idleSinceMs;
    }

    const ModelAutoSwitch::Decision dec = ModelAutoSwitch::decide(gate);
    if (!dec.act) {
        // ANTS-1894 — emit a near-miss record on signature change (INV-5/6).
        maybeEmitNearMiss(dec, gate, projectRoot, nowMs);
        // ANTS-1919 — pending-switch intent tracking. When the ONLY blocker is
        // composer_not_empty all other guards (dwell, stability, tier delta) are
        // already satisfied. Record the target tier so the status-bar can show a
        // "pending switch" annotation on the model chip and the intent is visible
        // to the user. The natural 2 s tick fires the switch as soon as the
        // composer empties — no extra polling needed.
        const bool onlyComposerBlocks =
            dec.blockedBy.size() == 1 &&
            dec.blockedBy.first() == QStringLiteral("composer_not_empty");
        if (onlyComposerBlocks) {
            // dec.tierArg is empty when act=false; use recommendedTier directly.
            m_autoSwitchPendingTier = ModelRecommender::tierName(dec.recommendedTier);
        } else {
            m_autoSwitchPendingTier.clear();
        }
        return;
    }
    m_autoSwitchPendingTier.clear();   // switch is about to fire — clear intent

    // Act — INV-9 keeps tierArg derived solely from the enum.
    // QStringLiteral matches the model-chip click pattern at :152 — CI's
    // stricter Qt build rejected the `u"..."` form (char16_t[8] vs QString
    // ambiguity at ants_claude_lib build).
    // ANTS-1912 — `\r` (CR) not `\n` — see chip-click site for rationale.
    focused->sendToPty(
        (QStringLiteral("/model ") + dec.tierArg + QStringLiteral("\r"))
            .toUtf8());
    // ANTS-1918/1924 — ESC + ENTER handshake + continuation prompt.
    performModelSwitchHandshake(focused);

    // ANTS-1893 — fire the firing-side surfacing (toast + chip-pulse
    // + Undo button) immediately after sendToPty, BEFORE the ledger
    // append. The helper is mute-toggle-aware (three Config bools);
    // when ALL three are off it's a single Config read + early
    // returns. Reads controller state at call time; does NOT write
    // any m_autoSwitchLastMs / m_autoSwitchTicksStable / m_autoSwitchLastTier
    // fields — those are still owned by the post-helper updates below.
    emitSwitchSurfacing(current, dec.recommendedTier, rec.reason,
                        focused, projectRoot, nowMs);

    // Append ledger record (§2.5). Outcome is `pending:true` — filled
    // by a later tick once turns-on-to-tier accumulates.
    ModelSwitchLedger::Record rec_;
    rec_.ts        = ModelSwitchLedger::nowIso8601();
    rec_.sessionId = m_integration ? m_integration->lastHookSessionId()
                                   : QString();
    rec_.project   = focused->shellCwd();
    rec_.fromTier  = ModelRecommender::tierName(current);
    rec_.toTier    = dec.tierArg;
    rec_.scoreReason = rec.reason;
    rec_.trigger   = QStringLiteral("auto");
    rec_.epoch     = ModelSwitchLedger::kSwitcherEpoch;   // ANTS-1941
    rec_.outcome.pending = true;
    ModelSwitchLedger::appendRecord(
        ModelSwitchLedger::defaultLedgerPath(), rec_);

    m_autoSwitchLastMs         = nowMs;
    // ANTS-1928 — clear the full stability accrual (ticksStable + reset-
    // hysteresis counter + tier-lock candidate) on every actual switch.
    m_autoSwitchStability      = ModelAutoSwitch::StabilityState{};
    m_autoSwitchPendingTier.clear();  // ANTS-1919 — switch fired; clear intent
    m_autoSwitchLastTier       = dec.tierArg;
}

// ANTS-1924 / ANTS-1920 — shared PTY handshake for every model-switch path
// (auto-switch, chip-click, undo). The caller has already injected
// `/model <tier>\r`; this confirms CC's "Switch model?" dialog and then sends
// a continuation prompt so CC resumes its task.
//
// ANTS-1920 replaced the original blind sequence (ESC@250 + \r@400 +
// continuation@1500) with an output-driven confirm. The blind \r@400 fired
// whether or not the dialog had rendered: when CC was slow to draw it, the CR
// landed in the wrong place and left `/model <tier>` stranded in the composer
// (user report 2026-05-29 — model stayed on Sonnet). Now we poll recentOutput
// for the dialog (ModelAutoSwitch::switchConfirmVisible) and press ENTER —
// which confirms the pre-highlighted "Yes" row — only once it is actually
// visible, aborting cleanly (a single ESC, never a blind CR) if it never
// appears. The original pre-confirm ESC is dropped: every handshake path fires
// with an empty composer (the auto-switch gate queues on composer_not_empty;
// chip-click/undo defer while mid-turn), so there was no stale state to clear,
// and a pre-confirm ESC would have dismissed the very dialog we now wait for.
//
// All terminators are \r (CR, 0x0D). \n only inserts a newline in CC's input
// buffer without submitting (ANTS-1912). Continuation prompt text is read once
// here so an in-flight poll uses a stable value; empty prompt = no continuation.
void ClaudeStatusBarController::performModelSwitchHandshake(TerminalWidget *focused) {
    QPointer<TerminalWidget> g(focused);
    // ANTS-1951 — mark the handshake in-flight so maybeAutoConfirmUserModelSwitch
    // stands down (this path owns the confirm for the dialog it raised).
    m_modelHandshakeInFlight = true;
    const QString cont = Config().claudeAutoModelContinuationPrompt();
    // Delay the first poll one interval: `/model\r` was just injected, so the
    // dialog has not rendered yet — an immediate check would always miss.
    QPointer<ClaudeStatusBarController> self(this);
    QTimer::singleShot(kSwitchConfirmPollMs, this, [self, g, cont]() {
        if (self) self->pollModelSwitchConfirm(g, cont, 0);
    });
}

// ANTS-1920 — one poll tick of the output-driven confirm; see header.
void ClaudeStatusBarController::pollModelSwitchConfirm(
        QPointer<TerminalWidget> g, const QString &cont, int attempt) {
    if (!g) {            // tab closed mid-poll — nothing to confirm.
        m_modelHandshakeInFlight = false;   // ANTS-1951 — release the gate.
        return;
    }

    if (ModelAutoSwitch::switchConfirmVisible(
            g->recentOutput(kSwitchConfirmScanLines))) {
        g->sendToPty("\r");   // ENTER — confirms the pre-highlighted "Yes" row.
        m_modelHandshakeInFlight = false;   // ANTS-1951 — handshake resolved.
        if (!cont.isEmpty()) {
            QPointer<TerminalWidget> g2(g);
            QTimer::singleShot(kSwitchContinuationDelayMs, this, [g2, cont]() {
                if (g2) g2->sendToPty((cont + QStringLiteral("\r")).toUtf8());
            });
        }
        return;
    }

    if (attempt + 1 >= kSwitchConfirmMaxPolls) {
        // Dialog never rendered within the budget. Either `/model` switched
        // directly (no confirmation needed) or it stranded in the composer.
        // Send a single ESC to clear any stranded text — NOT a blind CR (the
        // exact failure mode ANTS-1920 fixes) — and skip the continuation.
        g->sendToPty("\x1b");
        m_modelHandshakeInFlight = false;   // ANTS-1951 — budget exhausted.
        return;
    }

    QPointer<ClaudeStatusBarController> self(this);
    QTimer::singleShot(kSwitchConfirmPollMs, this, [self, g, cont, attempt]() {
        if (self) self->pollModelSwitchConfirm(g, cont, attempt + 1);
    });
}

// ANTS-1951 — auto-confirm a user-typed /model "Switch model?" dialog. Runs on
// the 2 s status tick independently of the auto-switch master toggle (a user
// who types /model wants the confirm regardless). Stands down while an
// Ants-initiated handshake owns the dialog, and presses ENTER at most once per
// dialog instance (latch cleared when the dialog clears).
// ANTS-1953 — after confirming, inject the continuation prompt (same as
// performModelSwitchHandshake) so the session resumes rather than halting at
// a blank `>` prompt. Uses the same config key and delay constant.
void ClaudeStatusBarController::maybeAutoConfirmUserModelSwitch() {
    const bool enabled = Config().claudeAutoModelConfirmUserSwitch();
    auto *focused = m_focusedTerminalProvider
        ? m_focusedTerminalProvider() : nullptr;
    if (!enabled || !focused) {
        m_unarmedSwitchConfirmed = false;   // nothing on screen we own
        return;
    }

    const bool visible = ModelAutoSwitch::switchConfirmVisible(
        focused->recentOutput(kSwitchConfirmScanLines));
    if (!ModelAutoSwitch::shouldAutoConfirmUnarmedSwitch(
            visible, enabled, m_modelHandshakeInFlight,
            m_unarmedSwitchConfirmed)) {
        if (!visible) m_unarmedSwitchConfirmed = false;  // dialog gone — re-arm
        return;
    }

    focused->sendToPty("\r");   // ENTER — confirms the pre-highlighted "Yes" row.
    m_unarmedSwitchConfirmed = true;

    // ANTS-1953 — inject continuation prompt after a short settle delay so CC
    // has rendered the post-confirm prompt before we start typing.
    const QString cont = Config().claudeAutoModelContinuationPrompt();
    if (!cont.isEmpty()) {
        QPointer<TerminalWidget> g(focused);
        QTimer::singleShot(kSwitchContinuationDelayMs, this, [g, cont]() {
            if (g) g->sendToPty((cont + QStringLiteral("\r")).toUtf8());
        });
    }
}

// ANTS-1915 — fire a deferred manual chip-switch once its owning shell is Idle.
void ClaudeStatusBarController::maybeFireDeferredChipSwitch(pid_t shellPid) {
    if (m_deferredChipTier.isEmpty() || shellPid != m_deferredChipShellPid)
        return;
    if (!m_tracker ||
            m_tracker->shellState(shellPid).state != ClaudeState::Idle)
        return;   // still generating — keep waiting for the turn to complete.

    // Resolve the terminal that owns this shell by PID, NOT by current focus:
    // the user may have moved to another tab while the turn finished, and the
    // /model + handshake must land on the PTY they clicked the chip for.
    TerminalWidget *term = nullptr;
    if (m_tabWidget && m_terminalAtTabProvider) {
        const int n = m_tabWidget->count();
        for (int i = 0; i < n; ++i) {
            auto *t = m_terminalAtTabProvider(i);
            if (t && t->shellPid() == shellPid) { term = t; break; }
        }
    }
    if (!term) {                 // tab closed before the turn ended — drop it.
        m_deferredChipTier.clear();
        m_deferredChipShellPid = 0;
        return;
    }

    const QString tier = m_deferredChipTier;
    m_deferredChipTier.clear();
    m_deferredChipShellPid = 0;

    term->sendToPty(
        (QStringLiteral("/model ") + tier + QStringLiteral("\r")).toUtf8());
    performModelSwitchHandshake(term);
    // ANTS-1890 / ANTS-1915 — seed the per-project override cool-down so the
    // autonomous switcher does not immediately undo this user-initiated switch
    // (mirrors the undo + chip-click-while-idle paths).
    m_lastOverrideMsByProject[term->shellCwd()] =
        QDateTime::currentMSecsSinceEpoch();
    if (m_modelBtn) m_modelBtn->setToolTip(QString());
}

// Sentinel for "never switched yet" — large enough to clear any
// reasonable min-dwell on the first eligible tick.
qint64 ClaudeStatusBarController::kMaxDwellSentinel() {
    return 1ll << 50;
}

namespace {

// ANTS-1894 — ClaudeState → lowercase snake_case JSON string. NEW helper
// (no equivalent in claudestateresolver — that switch maps to Display::*
// glyph enums, a different domain). 5-case mapping over the enum at
// claudeintegration.h L47-53.
QString claudeStateName(ClaudeState s) {
    switch (s) {
        case ClaudeState::NotRunning: return QStringLiteral("not_running");
        case ClaudeState::Idle:       return QStringLiteral("idle");
        case ClaudeState::Thinking:   return QStringLiteral("thinking");
        case ClaudeState::ToolUse:    return QStringLiteral("tool_use");
        case ClaudeState::Compacting: return QStringLiteral("compacting");
    }
    return QStringLiteral("idle");   // unreachable; satisfies non-void return
}

// ANTS-1893 — Tier-to-capitalised-label for user-facing surfaces.
// Distinct from ModelRecommender::tierName() which returns the
// lowercase JSON / /model-command form. The Result.tier and on-disk
// ledger use the lowercase form; UI strings use this one.
QString tierTitle(ModelRecommender::Tier tier) {
    switch (tier) {
        case ModelRecommender::Tier::Haiku:  return QStringLiteral("Haiku");
        case ModelRecommender::Tier::Opus:   return QStringLiteral("Opus");
        default:                              return QStringLiteral("Sonnet");
    }
}

// ANTS-1893 — Maps the recommender's `reason` field to a short
// user-facing label. The reason field comes in three shapes per
// `modelrecommender.cpp` (cpp:86, 173, 239-244): "default"
// (no-transcript fallback), "commit_intent" (hard override),
// or a space-joined token list from the scoring path. First match
// wins; the precedence is defensive against future reason-string
// reshapings (today commit_intent never appears alongside the
// scoring-path tokens — the override returns early). All labels
// are wrapped in tr() at the call site (Qt i18n discipline).
QString shortenReason(const QString &reason) {
    if (reason.contains(QStringLiteral("commit_intent")))
        return QObject::tr("commit intent");
    if (reason.contains(QStringLiteral("mechanical")))
        return QObject::tr("mechanical");
    if (reason.contains(QStringLiteral("plan_keyword")))
        return QObject::tr("design / plan");
    if (reason.contains(QStringLiteral("many_writes")))
        return QObject::tr("many writes");
    if (reason.contains(QStringLiteral("long_prompts")))
        return QObject::tr("long prompts");
    if (reason.contains(QStringLiteral("tool_diversity")))
        return QObject::tr("varied tools");
    return QObject::tr("recommender");
}

}  // namespace

// ANTS-1940 — conservatism multiplier, lazily refreshed every 60 s.
// Reads the firing ledger (with a 30-day window) to get the current regret
// picture. Result cached per project root; the ledger read only happens on
// the first tick after the TTL expires, not on every 2 s tick.
double ClaudeStatusBarController::conservatismMultiplierFor(
        const QString &projectRoot,
        qint64         nowMs,
        bool           isMechanical)
{
    // Check TTL.
    if (m_conservatismStampMsByProject.contains(projectRoot)) {
        const qint64 stamp = m_conservatismStampMsByProject.value(projectRoot);
        if (nowMs - stamp < kConservatismTtlMs) {
            // Cache still fresh; re-derive based on current isMechanical.
            const double cached = m_conservatismMultByProject.value(projectRoot, 1.0);
            // Re-apply isMechanical influence to cached multiplier if > 1.
            if (cached > 1.0 && isMechanical) {
                const double penalty = ModelAutoSwitch::kCalibrationDwellMult - 1.0;
                return 1.0 + penalty * 0.5;
            }
            return cached;
        }
    }

    // TTL expired (or first call) — re-read the ledger.
    ModelSwitchLedger::StatsConfig sc;
    sc.scope     = QStringLiteral("project");
    sc.windowDays = ModelSwitchLedger::kDefaultStatsWindowDays;   // ANTS-1942 — shared default
    sc.minEpoch  = ModelSwitchLedger::kSwitcherEpoch;   // ANTS-1941 — current-epoch only
    const QJsonObject env = ModelSwitchLedger::statsForScope(
        ModelSwitchLedger::defaultLedgerPath(), projectRoot, sc);

    const int measured = env.value(QStringLiteral("measured_downgrades")).toInt(0);
    const int floor    = env.value(QStringLiteral("headline_floor")).toInt(10);
    const int regret   = static_cast<int>(
        env.value(QStringLiteral("regret_rate")).toDouble(0.0) + 0.5);

    const double mult = ModelAutoSwitch::conservatismDwellMultiplier(
        measured, regret, floor, isMechanical);

    m_conservatismMultByProject[projectRoot]    = mult;
    m_conservatismStampMsByProject[projectRoot] = nowMs;
    return mult;
}

// ANTS-1894 — sole producer of near-miss records. Emits on signature change
// (INV-5) gated by a 5 s per-project floor (INV-6). dec.blockedBy must be
// non-empty (caller-enforced: this is called only on the !dec.act branch).
void ClaudeStatusBarController::maybeEmitNearMiss(
        const ModelAutoSwitch::Decision &dec,
        const ModelAutoSwitch::Gate     &gate,
        const QString                   &projectRoot,
        qint64                           nowMs,
        const QString                   &ledgerPathOverride)
{
    if (dec.blockedBy.isEmpty()) return;   // defensive — act must be false
    if (projectRoot.isEmpty())   return;   // can't key the throttle
    // ANTS-1946 INV-14 — target_equals_current means clamped recommendation
    // already equals current tier: no switch was ever possible. This is the
    // healthy idle default, not a blocked switch. Suppress without updating
    // throttle state so a later real near-miss can still fire unimpeded.
    if (dec.blockedBy.contains(QStringLiteral("target_equals_current"))) return;

    // Sort tokens for a stable, order-independent signature comparison.
    // The emitted record preserves the original (evaluation-order) sequence.
    QStringList sig = dec.blockedBy;
    std::sort(sig.begin(), sig.end());

    const QStringList lastSig = m_nearMissLastSigByProject.value(projectRoot);
    if (sig == lastSig) return;                          // INV-5 — same shape

    // INV-6 — 5 s floor measured against the last emit for THIS project.
    // When no prior emit exists (key absent), the first transition always
    // lands — there's nothing to throttle against.
    if (m_nearMissLastEmitMsByProject.contains(projectRoot)) {
        const qint64 lastMs = m_nearMissLastEmitMsByProject.value(projectRoot);
        if (nowMs - lastMs < kNearMissEmitFloorMs) return;
    }

    ModelNearMissLedger::Record r;
    r.ts                  = ModelSwitchLedger::nowIso8601();
    // m_integration is a ClaudeIntegration* member declared in this header.
    r.sessionId           = m_integration ? m_integration->lastHookSessionId() : QString();
    r.project             = projectRoot;
    r.currentTier         = ModelRecommender::tierName(dec.currentTier);
    r.recommendedTier     = ModelRecommender::tierName(dec.recommendedTier);
    r.blockedBy           = dec.blockedBy;       // original (evaluation) order
    r.composerEmpty       = gate.composerEmpty;
    r.focusedState        = claudeStateName(gate.focusedState);
    r.ticksTargetStable   = gate.ticksTargetStable;
    r.dwellMs             = gate.msSinceLastSwitch;
    r.msSinceLastOverride = gate.msSinceLastOverride;
    r.epoch               = ModelSwitchLedger::kSwitcherEpoch;  // ANTS-1954

    const QString ledgerPath = ledgerPathOverride.isEmpty()
        ? ModelNearMissLedger::defaultLedgerPath()
        : ledgerPathOverride;
    ModelNearMissLedger::appendRecord(ledgerPath, r);

    m_nearMissLastSigByProject[projectRoot]    = sig;
    m_nearMissLastEmitMsByProject[projectRoot] = nowMs;
}

// ANTS-1893 — fire the firing-side surfacing (toast + chip-pulse +
// Undo button) immediately after a live auto-switch fires. Reads
// controller state at call time but does NOT write
// refreshAutoModelSwitch-owned fields (m_autoSwitchLastMs etc.) —
// the firing-side caller still owns those after the helper returns.
void ClaudeStatusBarController::emitSwitchSurfacing(
        ModelRecommender::Tier fromTier,
        ModelRecommender::Tier toTier,
        const QString &scoreReason,
        TerminalWidget *focused,
        const QString &projectRoot,
        qint64 /*nowMs*/)
{
    if (!focused) return;
    Config cfg;

    // (1) Toast — reuse the existing statusMessageRequested signal
    // that MainWindow routes into m_statusMessage (mainwindow.cpp
    // L725 construct, L728 addWidget). 6 s timeout is short enough
    // to clear before the next user turn but long enough to catch
    // with peripheral vision.
    if (cfg.claudeAutoModelToastEnabled()) {
        const QString body = tr("Ants switched: %1 → %2 (%3)")
            .arg(tierTitle(fromTier))
            .arg(tierTitle(toTier))
            .arg(shortenReason(scoreReason));
        emit statusMessageRequested(body, kSwitchToastTimeoutMs);
    }

    // (2) Chip-pulse — set the dynamic `pulse` property on
    // m_modelStateBtn and force a style repolish so QSS picks up
    // the [pulse="true"] attribute selector (Qt requirement). The
    // single-shot timer clears the property + repolishes after
    // kModelChipPulseMs (600 ms). A second pulse inside the window
    // restarts the timer; the property stays true throughout.
    if (cfg.claudeAutoModelChipPulseEnabled() && m_modelStateBtn) {
        m_modelStateBtn->setProperty("pulse", true);
        if (m_modelStateBtn->style()) {
            m_modelStateBtn->style()->unpolish(m_modelStateBtn);
            m_modelStateBtn->style()->polish(m_modelStateBtn);
        }
        if (!m_modelStatePulseTimer) {
            m_modelStatePulseTimer = new QTimer(this);
            m_modelStatePulseTimer->setSingleShot(true);
            connect(m_modelStatePulseTimer, &QTimer::timeout, this, [this]() {
                if (!m_modelStateBtn) return;
                m_modelStateBtn->setProperty("pulse", false);
                if (m_modelStateBtn->style()) {
                    m_modelStateBtn->style()->unpolish(m_modelStateBtn);
                    m_modelStateBtn->style()->polish(m_modelStateBtn);
                }
            });
        }
        m_modelStatePulseTimer->start(kModelChipPulseMs);
    }

    // (3) Undo button — show for kUndoVisibleMs (10 s). Pending
    // fields capture the project + shellPid at switch time so the
    // click handler can refuse the undo if the user switched tabs.
    // A second switch fired inside the window overwrites the pending
    // fields + restarts the timer (most-recent-undo only; see INV-9).
    if (cfg.claudeAutoModelUndoEnabled() && m_undoSwitchBtn) {
        m_undoSwitchBtn->setText(
            tr("Undo: back to %1").arg(tierTitle(fromTier)));
        m_undoSwitchPendingFromTier     = ModelRecommender::tierName(fromTier);
        m_undoSwitchPendingFromTierEnum = fromTier;
        m_undoSwitchPendingProject      = projectRoot;
        m_undoSwitchPendingShellPid     = focused->shellPid();
        m_undoSwitchBtn->show();
        if (!m_undoSwitchHideTimer) {
            m_undoSwitchHideTimer = new QTimer(this);
            m_undoSwitchHideTimer->setSingleShot(true);
            connect(m_undoSwitchHideTimer, &QTimer::timeout, this, [this]() {
                if (m_undoSwitchBtn) m_undoSwitchBtn->hide();
                m_undoSwitchPendingFromTier.clear();
                m_undoSwitchPendingProject.clear();
                m_undoSwitchPendingShellPid = 0;
                m_undoSwitchPendingFromTierEnum =
                    ModelRecommender::Tier::Sonnet;
            });
        }
        m_undoSwitchHideTimer->start(kUndoVisibleMs);
    }
}

// ANTS-1893 — Undo click handler. Refuses on cwd or shellPid
// mismatch (per-tab guard); a dead shellPid gets a distinct
// "session ended" toast so the user knows the lossy click was due
// to their shell exiting, not a tab switch.
void ClaudeStatusBarController::onUndoSwitchClicked()
{
    if (m_undoSwitchPendingFromTier.isEmpty() ||
        m_undoSwitchPendingProject.isEmpty() ||
        m_undoSwitchPendingShellPid <= 0) return;

    auto *focused = m_focusedTerminalProvider
        ? m_focusedTerminalProvider() : nullptr;
    if (!focused) return;

    // Project mismatch first — distinct toast.
    if (focused->shellCwd() != m_undoSwitchPendingProject) {
        emit statusMessageRequested(
            tr("Undo unavailable — switch was on a different project"),
            3000);
        return;
    }
    // PID mismatch: distinguish dead PID (session ended) from
    // alive-but-different (another tab in the same project).
    if (focused->shellPid() != m_undoSwitchPendingShellPid) {
        if (::kill(m_undoSwitchPendingShellPid, 0) == -1 &&
                errno == ESRCH) {
            emit statusMessageRequested(
                tr("Undo unavailable — the original session has ended"),
                3000);
        } else {
            emit statusMessageRequested(
                tr("Undo unavailable — switch was on a different session"),
                3000);
        }
        return;
    }

    // Capture before clearing — the "Switched back to <X>" toast
    // below reads the prior tier; clearing first would leak a
    // default-Sonnet read into the toast.
    const QString priorTierName    = m_undoSwitchPendingFromTier;
    const QString priorProject     = m_undoSwitchPendingProject;
    const ModelRecommender::Tier priorTier = m_undoSwitchPendingFromTierEnum;

    // ANTS-1912 — `\r` (CR) not `\n` — see chip-click site for rationale.
    focused->sendToPty(
        (QStringLiteral("/model ") + priorTierName +
         QStringLiteral("\r")).toUtf8());
    // ANTS-1918/1924 — ESC + ENTER handshake + continuation prompt.
    performModelSwitchHandshake(focused);

    // Seed the in-memory cool-down so the gate's 10-min lock-out
    // (ANTS-1890 / kOverrideCooldownMs) trips on the next tick.
    m_lastOverrideMsByProject[priorProject] =
        QDateTime::currentMSecsSinceEpoch();

    m_undoSwitchBtn->hide();
    m_undoSwitchPendingFromTier.clear();
    m_undoSwitchPendingProject.clear();
    m_undoSwitchPendingShellPid     = 0;
    m_undoSwitchPendingFromTierEnum = ModelRecommender::Tier::Sonnet;
    if (m_undoSwitchHideTimer) m_undoSwitchHideTimer->stop();

    emit statusMessageRequested(
        tr("Switched back to %1").arg(tierTitle(priorTier)),
        3000);

    focused->setFocus();   // ANTS-1849 — keep typing focus in the terminal
}

namespace {

// Parse a JSONL transcript tail into the projection the §2.5 outcome
// fill-in needs (assistant turns + user turns with their content). Matches
// the 512 KB tail-read pattern in ModelRecommender::score so behaviour is
// consistent. Returns turns in chronological order, oldest first.
QList<ModelSwitchLedger::TranscriptTurn>
parseTranscriptTurnsForLedger(const QString &path)
{
    QList<ModelSwitchLedger::TranscriptTurn> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    constexpr qint64 kMaxTailBytes = 512LL * 1024LL;
    const bool didTailSeek = (f.size() > kMaxTailBytes);
    if (didTailSeek) f.seek(f.size() - kMaxTailBytes);

    QStringList lines;
    while (!f.atEnd()) {
        const QString ln = QString::fromUtf8(f.readLine()).trimmed();
        if (!ln.isEmpty()) lines.append(ln);
    }
    f.close();
    if (didTailSeek && !lines.isEmpty()) lines.removeFirst();   // truncated head

    out.reserve(lines.size());
    for (const QString &ln : lines) {
        const QJsonObject obj = QJsonDocument::fromJson(ln.toUtf8()).object();
        const QString type = obj.value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("assistant") && type != QStringLiteral("user"))
            continue;

        ModelSwitchLedger::TranscriptTurn t;
        const QString ts = obj.value(QStringLiteral("timestamp")).toString();
        t.tsMs = ModelSwitchLedger::parseIso8601Ms(ts);

        const QJsonObject msg = obj.value(QStringLiteral("message")).toObject();
        if (type == QStringLiteral("assistant")) {
            t.isAssistant = true;
            t.modelId     = msg.value(QStringLiteral("model")).toString();
        } else {
            t.isUser = true;
            // user.message.content can be a string OR an array of content
            // blocks (text/image/tool_result). Join all text into userText.
            const QJsonValue cv = msg.value(QStringLiteral("content"));
            if (cv.isString()) {
                t.userText = cv.toString();
            } else if (cv.isArray()) {
                QStringList parts;
                for (const QJsonValue &blk : cv.toArray()) {
                    const QJsonObject b = blk.toObject();
                    if (b.value(QStringLiteral("type")).toString()
                            == QStringLiteral("text")) {
                        parts.append(b.value(QStringLiteral("text")).toString());
                    }
                }
                t.userText = parts.join(QLatin1Char(' '));
            }
        }
        out.append(t);
    }
    return out;
}

}  // namespace

// ANTS-1735 §2.5 — outcome fill-in tick. Runs on the 2 s status timer,
// internally throttled to once per kPendingFillIntervalMs (30 s). Reads
// the ledger, fills any pending records whose post-switch transcript has
// settled, and writes back only when something changed. Cheap when the
// ledger is empty or has no pending records.
void ClaudeStatusBarController::fillPendingLedgerOutcomes()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastPendingFillMs > 0 &&
        nowMs - m_lastPendingFillMs < kPendingFillIntervalMs) {
        return;
    }
    m_lastPendingFillMs = nowMs;
    // ANTS-1891 — forward `nowMs` so the path overload (and the pure
    // `computeOutcome` it calls per pending record) can evaluate the
    // new quiet-window settlement clause.
    fillPendingLedgerOutcomes(ModelSwitchLedger::defaultLedgerPath(), nowMs);
}

// ANTS-1890 — Test seam. Behavioural overload reachable from
// behavioural tests; bypasses the 30 s throttle (tests run sub-second).
// The production caller above pays the throttle and then delegates here.
// ANTS-1891 — `nowMs` is the clock seam for `computeOutcome`'s
// quiet-window settlement (defaults to 0 = pre-1891 behaviour).
void ClaudeStatusBarController::fillPendingLedgerOutcomes(
    const QString &ledgerPath, qint64 nowMs)
{
    QList<ModelSwitchLedger::Record> recs =
        ModelSwitchLedger::readRecords(ledgerPath);
    if (recs.isEmpty()) return;

    // Quick exit when nothing is pending — saves the per-record transcript
    // lookups in the common steady-state case.
    bool hasPending = false;
    for (const ModelSwitchLedger::Record &r : recs) {
        if (r.outcome.pending) { hasPending = true; break; }
    }
    if (!hasPending) return;

    bool anyChanged = false;
    for (int i = 0; i < recs.size(); ++i) {
        ModelSwitchLedger::Record &r = recs[i];
        if (!r.outcome.pending) continue;

        const QString transcriptPath =
            ClaudeIntegration::sessionPathForCwd(r.project);
        if (transcriptPath.isEmpty()) continue;

        const QList<ModelSwitchLedger::TranscriptTurn> turns =
            parseTranscriptTurnsForLedger(transcriptPath);
        const qint64 switchMs = ModelSwitchLedger::parseIso8601Ms(r.ts);

        // Subsequent auto-switches for the same project — used by
        // detectUserOverride (author correlation) and to settle the dwell
        // when the controller has already moved on.
        QList<ModelSwitchLedger::AutoSwitch> nextAutos;
        for (int j = i + 1; j < recs.size(); ++j) {
            if (recs[j].project != r.project) continue;
            ModelSwitchLedger::AutoSwitch a;
            a.toTier = recs[j].toTier;
            a.tsMs   = ModelSwitchLedger::parseIso8601Ms(recs[j].ts);
            if (a.tsMs > 0) nextAutos.append(a);
        }

        // INV-12 under-route — pass the live recommender snapshot once at
        // least one post-switch assistant turn has happened. Earlier than
        // that, the scorer has nothing new to bite on.
        QStringList recTiers;
        int postSwitchAsstCount = 0;
        for (const ModelSwitchLedger::TranscriptTurn &t : turns) {
            if (t.isAssistant && t.tsMs > switchMs) ++postSwitchAsstCount;
        }
        if (postSwitchAsstCount >= 1) {
            const ModelRecommender::Result rr =
                ModelRecommender::score(transcriptPath);
            recTiers << ModelRecommender::tierName(rr.tier);
        }

        const ModelSwitchLedger::OutcomeFillResult fill =
            ModelSwitchLedger::computeOutcome(
                r.fromTier, r.toTier, switchMs,
                turns, nextAutos, recTiers, r.outcome,
                nowMs);   // ANTS-1891 — clock seam for quiet-window
        if (fill.changed) {
            r.outcome = fill.outcome;
            anyChanged = true;
        }
        // ANTS-1890 INV-7 — once a record settles with userOverrideWithin5,
        // remember its timestamp per project so the gate cool-down can read
        // it without re-scanning the ledger every 2 s tick. We use the
        // computed outcome here (not r.outcome) — fill.outcome is the
        // post-compute value; r.outcome may be the unchanged input.
        if (fill.outcome.userOverrideWithin5 && !fill.outcome.pending) {
            const qint64 tsMs = ModelSwitchLedger::parseIso8601Ms(r.ts);
            qint64 &slot = m_lastOverrideMsByProject[r.project];
            if (tsMs > slot) slot = tsMs;
        }
    }

    if (anyChanged)
        ModelSwitchLedger::writeRecords(ledgerPath, recs);
}

// ANTS-1890 INV-7 — bootstrap seed. Scans the ledger once at controller
// construction and populates m_lastOverrideMsByProject from any settled
// `userOverrideWithin5` records already on disk. Without this, an
// override 30 s before app restart would be "forgotten" (the very turn
// that should fire cool-down passes) because fillPendingLedgerOutcomes
// only walks PENDING records, and a 30-s-old override is usually
// already settled.
void ClaudeStatusBarController::seedOverrideCacheFromLedger(
    const QString &ledgerPath)
{
    const QList<ModelSwitchLedger::Record> recs =
        ModelSwitchLedger::readRecords(ledgerPath);
    for (const ModelSwitchLedger::Record &r : recs) {
        if (!r.outcome.userOverrideWithin5 || r.outcome.pending) continue;
        const qint64 tsMs = ModelSwitchLedger::parseIso8601Ms(r.ts);
        qint64 &slot = m_lastOverrideMsByProject[r.project];
        if (tsMs > slot) slot = tsMs;
    }
}

qint64 ClaudeStatusBarController::lastOverrideMsForProject(
    const QString &project) const
{
    return m_lastOverrideMsByProject.value(project, -1);
}
