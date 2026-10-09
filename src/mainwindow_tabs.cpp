// ANTS-1677 mainwindow piece 3/8 — tabs, panes and remote tab control
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "guithread.h"       // ANTS-4682 — ants::onGuiThread in inline verbs
#include "themes.h"             // ANTS-1325: include directly where Themes:: is called
#include "coloredtabbar.h"
#include "terminalwidget.h"
#include "dialogchrome.h"
#include "remotecontrol.h"
#include "resolvedroot.h"      // ANTS-1401 — terminalForCaller helper
#include "rootprovider.h"      // ANTS-4932 § 2.4
#include "claudeallowlist.h"
#include "claudeintegration.h"
#include "claudestatuswidgets.h"
#include "claudetabtracker.h"
#include <QJsonObject>
#include <QApplication>
#include <QMessageBox>
#include <QPointer>
#include <QVBoxLayout>
#include <QSplitter>
#include <QUuid>
#include <QTimer>
#include <QJsonArray>
#include <QHBoxLayout>
#include <QPainter>
#include <algorithm>
#ifdef ANTS_LUA_PLUGINS
#include "pluginmanager.h"
#include "luaengine.h"
#endif

using namespace mainwindowdetail;

TerminalWidget *MainWindow::createTerminal() {
    auto *terminal = new TerminalWidget();
    applyConfigToTerminal(terminal);

    if (!m_currentTheme.isEmpty()) {
        const Theme &theme = Themes::byName(m_currentTheme);
        terminal->applyThemeColors(theme.textPrimary, theme.bgPrimary, theme.cursor,
                                    theme.accent, theme.border);
    }

    return terminal;
}

void MainWindow::applyConfigToTerminal(TerminalWidget *terminal) {
    terminal->setFontSize(m_config.fontSize());
    terminal->setMaxScrollback(m_config.scrollbackLines());
    terminal->setSessionLogging(m_config.sessionLogging());
    terminal->setAutoCopyOnSelect(m_config.autoCopyOnSelect());
    terminal->setConfirmMultilinePaste(m_config.confirmMultilinePaste());
    terminal->setEditorCommand(m_config.editorCommand());
    terminal->setImagePasteDir(m_config.imagePasteDir());
    terminal->setWindowOpacityLevel(m_config.opacity());
    terminal->setVisualBell(m_config.visualBell());
    terminal->setPadding(m_config.terminalPadding());
    terminal->setShowCommandMarks(m_config.showCommandMarks());
    QString family = m_config.fontFamily();
    if (!family.isEmpty()) terminal->setFontFamily(family);
    // Per-style fonts
    QString boldFamily = m_config.boldFontFamily();
    if (!boldFamily.isEmpty()) terminal->setBoldFontFamily(boldFamily);
    QString italicFamily = m_config.italicFontFamily();
    if (!italicFamily.isEmpty()) terminal->setItalicFontFamily(italicFamily);
    QString biFamily = m_config.boldItalicFontFamily();
    if (!biFamily.isEmpty()) terminal->setBoldItalicFontFamily(biFamily);
    // Background image
    QString bgImg = m_config.backgroundImage();
    if (!bgImg.isEmpty()) terminal->setBackgroundImage(bgImg);
    // Badge text
    QString badge = m_config.badgeText();
    if (!badge.isEmpty()) terminal->setBadgeText(badge);
}

QList<TerminalWidget *> MainWindow::liveTerminals() const {
    // ANTS-1182: O(N) over a small contiguous list rather than the
    // full QObject child tree. Returned snapshot is owned by the
    // caller so iteration is stable even if a terminal is destroyed
    // mid-loop. The pointers themselves remain owned by Qt's parent
    // chain.
    //
    // ANTS-1324: compact null entries (Qt auto-nulled them when the
    // wrapped TerminalWidget was destroyed) lazily here, rather than
    // eagerly via a destroyed() slot. The eager path fired UBSan
    // because the slot runs during ~QWidget() with vptr=QWidget, and
    // QPointer<TerminalWidget>::data() would static_cast that to
    // TerminalWidget*. m_allTerminals is `mutable` for this reason.
    m_allTerminals.removeIf(
        [](const QPointer<TerminalWidget> &p) { return p.isNull(); });
    QList<TerminalWidget *> live;
    live.reserve(m_allTerminals.size());
    for (const QPointer<TerminalWidget> &p : m_allTerminals) {
        live.append(p.data());
    }
    return live;
}

void MainWindow::connectTerminal(TerminalWidget *terminal) {
    // ANTS-1182: register with the flat all-terminals list so
    // iterate-all sites don't each walk the QObject child tree.
    // ANTS-1324: removal is lazy (see liveTerminals()) — no eager
    // destroyed() handler, because that handler would call
    // QPointer<TerminalWidget>::data() while ~QWidget() is on the
    // stack and trip UBSan -fsanitize=vptr.
    m_allTerminals.append(QPointer<TerminalWidget>(terminal));

    connect(terminal, &TerminalWidget::titleChanged, this, [this, terminal](const QString &title) {
        // Find which tab this terminal is in
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            QWidget *tabRoot = m_tabWidget->widget(i);
            if (tabRoot->isAncestorOf(terminal) || tabRoot == terminal) {
                // Skip if rc_protocol set-title pinned this tab —
                // the user/script chose a label and the shell's OSC
                // 0/2 must not stomp it.
                if (m_tabTitlePins.contains(tabRoot)) break;
                QString tabTitle = title.isEmpty() ? "Shell" : title;
                if (tabTitle.length() > 30)
                    tabTitle = tabTitle.left(27) + "...";
                m_tabWidget->setTabText(i, tabTitle);
                break;
            }
        }
        if (terminal == focusedTerminal()) {
            onTitleChanged(title);
        }
    });

    connect(terminal, &TerminalWidget::shellExited, this, [this, terminal](int /*code*/) {
        // Find parent splitter
        QSplitter *splitter = findParentSplitter(terminal);
        if (splitter) {
            // The exiting pane's own tab, found before it is unparented: a
            // shell exiting in a background tab left that tab's empty
            // splitter behind while the current tab was cleaned instead.
            QWidget *ownTab = nullptr;
            for (int i = 0; i < m_tabWidget->count(); ++i) {
                QWidget *w = m_tabWidget->widget(i);
                if (w->isAncestorOf(terminal)) { ownTab = w; break; }
            }
            terminal->setParent(nullptr);
            terminal->deleteLater();
            cleanupEmptySplitters(ownTab);
        } else {
            // It's the only terminal in the tab
            int idx = m_tabWidget->indexOf(terminal);
            if (idx >= 0) closeTab(idx);
        }
    });

    // Broadcast callback. ANTS-5223 — only the other panes of the tab being
    // typed in; every tab once received it, so answers typed into one Claude
    // session reached all of them. ANTS-5224 — each pane encodes the key for
    // its own modes rather than receiving the source's bytes.
    terminal->setBroadcastCallback([this](TerminalWidget *source, const QKeyEvent *event) {
        if (!m_broadcastMode) return;
        QWidget *page = tabPageOf(m_tabWidget, source);
        if (!page) return;
        const QList<TerminalWidget *> panes = page->findChildren<TerminalWidget *>();
        for (TerminalWidget *t : panes) {
            if (t == source) continue;
            const QByteArray bytes = t->encodeKey(event);
            if (!bytes.isEmpty()) t->sendToPty(bytes);
        }
    });

    // Trigger signals
    connect(terminal, &TerminalWidget::triggerFired, this, &MainWindow::onTriggerFired);

#ifdef ANTS_LUA_PLUGINS
    // 0.6.9 — forward shell-integration + iTerm2 hooks out as plugin events.
    // command_finished payload: "exit_code=N&duration_ms=N" (URL-form so
    // plugins can parse with a simple split — no escaping needed).
    connect(terminal, &TerminalWidget::commandFinished, this,
            [this](int exitCode, qint64 durationMs) {
        if (m_pluginManager) {
            m_pluginManager->fireEvent(PluginEvent::CommandFinished,
                QString("exit_code=%1&duration_ms=%2").arg(exitCode).arg(durationMs));
        }
    });
    // user_var_changed payload: "NAME=value" (raw — names are already
    // identifier-shaped per the OSC 1337 SetUserVar spec).
    connect(terminal, &TerminalWidget::userVarChanged, this,
            [this](const QString &name, const QString &value) {
        if (m_pluginManager) {
            m_pluginManager->fireEvent(PluginEvent::UserVarChanged,
                                        name + QStringLiteral("=") + value);
        }
    });
    // 0.7.0 — surface OSC 133 forgery attempts in the status bar so the
    // user sees an in-terminal process trying to spoof prompt markers.
    // Throttled grid-side (5 s) so a tight forgery loop can't spam the bar.
    connect(terminal, &TerminalWidget::osc133ForgeryDetected, this,
            [this](int count) {
        showStatusMessage(
            QStringLiteral("⚠ OSC 133 forgery detected (count: %1) — an in-terminal "
                           "process tried to spoof a shell-integration marker").arg(count),
            5000);
    });
    // run_script trigger action: route the matched substring to plugins as a
    // PaletteAction event with the action id as payload. Plugins listening
    // for the matching id can dispatch their own logic.
    connect(terminal, &TerminalWidget::triggerRunScript, this,
            [this](const QString &actionId, const QString &matched) {
        if (m_pluginManager) {
            // Broadcast — any plugin can react; payload "actionId\tmatched"
            // gives the plugin both the dispatch key and the captured text.
            m_pluginManager->fireEvent(PluginEvent::PaletteAction,
                                        actionId + QStringLiteral("\t") + matched);
        }
    });
#endif

    // OSC 9;4 progress reporting — show a small colored dot as the tab icon.
    // ConEmu / Microsoft Terminal convention.
    connect(terminal, &TerminalWidget::progressChanged, this,
            [this, terminal](int state, int /*percent*/) {
        // Find which tab this terminal is in
        int tabIdx = -1;
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            QWidget *w = m_tabWidget->widget(i);
            if (w == terminal || w->isAncestorOf(terminal)) { tabIdx = i; break; }
        }
        if (tabIdx < 0) return;
        if (state < 0 || state > 4) return;

        // ANTS-5079 — the icon depends only on the state, so skip repainting
        // it on every percent. Recorded on the tab page, not the pane, so a
        // pane in a split tab still redraws it after another pane cleared it.
        QWidget *page = m_tabWidget->widget(tabIdx);
        const QVariant drawn = page->property("antsProgressIconState");
        if (drawn.isValid() && drawn.toInt() == state) return;
        page->setProperty("antsProgressIconState", state);

        if (state == 0) {
            m_tabWidget->setTabIcon(tabIdx, QIcon());
            return;
        }
        QColor dot;
        switch (state) {
            case 1: dot = QColor(0x89, 0xB4, 0xFA); break; // Normal — blue
            case 2: dot = QColor(0xF3, 0x8B, 0xA8); break; // Error — red
            case 3: dot = QColor(0xB4, 0xBE, 0xFE); break; // Indeterminate — lavender
            default: dot = QColor(0xF9, 0xE2, 0xAF); break; // Warning — yellow
        }
        QPixmap pm(12, 12);
        pm.fill(Qt::transparent);
        QPainter pp(&pm);
        pp.setRenderHint(QPainter::Antialiasing);
        pp.setBrush(dot);
        pp.setPen(Qt::NoPen);
        pp.drawEllipse(1, 1, 10, 10);
        pp.end();
        m_tabWidget->setTabIcon(tabIdx, QIcon(pm));
    });

    // Desktop notifications (OSC 9/777)
    connect(terminal, &TerminalWidget::desktopNotification, this,
            [this](const QString &title, const QString &body) {
        // Only show notification if window is not focused (avoid distracting the user)
        if (!isActiveWindow())
            showDesktopNotification(title, body);
    });

    // ANTS-5151 — logging or recording refused for want of owner-only perms.
    // Shown in the window, not as a desktop notification: the user is looking
    // at it when they switch capture on.
    connect(terminal, &TerminalWidget::captureFailed, this,
            [this](const QString &message) { showStatusMessage(message, 8000); });

    // Apply highlight and trigger rules from config
    terminal->setHighlightRules(m_config.highlightRules());
    terminal->setTriggerRules(m_config.triggerRules());

    // Error detection — show failed command in status bar
    connect(terminal, &TerminalWidget::commandFailed, this, [this](int exitCode, const QString &output) {
        if (m_claudeStatusBarController)
            m_claudeStatusBarController->setError(QString("Exit %1").arg(exitCode),
                                                  output.left(500),
                                                  10000);
    });

    // Claude Code permission detection → status bar notification
    connect(terminal, &TerminalWidget::claudePermissionDetected, this, [this, terminal](const QString &rawRule) {
        // Only show for the currently active tab
        if (terminal != focusedTerminal() && terminal != currentTerminal()) return;

        // Normalize and generalize the detected rule
        QString rule = ClaudeAllowlistDialog::normalizeRule(rawRule);
        QString gen = ClaudeAllowlistDialog::generalizeRule(rule);
        if (!gen.isEmpty()) rule = gen;

        // Remove any existing allowlist button to prevent accumulation.
        // Must use QWidget* not QPushButton* — the hook-path permissionRequested
        // handler creates a QWidget container (line ~2537) with objectName
        // "claudeAllowBtn"; a QPushButton-typed findChildren would miss it and
        // leave both buttons stacked when a scroll-scan detection fires while
        // a hook-path container is already visible. Mirrors the
        // onTabChanged (line ~1716) and hook-path dedup (line ~2533) lookups
        // which both already use QWidget*.
        auto existing = statusBar()->findChildren<QWidget *>(QStringLiteral("claudeAllowBtn"));
        for (auto *btn : existing) btn->deleteLater();

        showStatusMessage(
            QString("Claude Code permission: %1 — ").arg(rule), 0);
        auto *addBtn = new QPushButton("Add to allowlist", statusBar());
        addBtn->setObjectName("claudeAllowBtn");
        // Fixed horizontal sizePolicy — the button must never be
        // squeezed when the notification slot is full of text. Same
        // layout principle as the branch chip / Claude status label
        // introduced on 2026-04-18.
        addBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        statusBar()->addPermanentWidget(addBtn);

        // Scroll-scan permission detection always belongs to the terminal
        // whose scrollback was scanned — `terminal` here is a direct
        // pointer, so we capture its shell PID and flag that tab's
        // tracker entry as awaiting input. No session_id routing needed
        // (unlike the hook path); the terminal pointer IS the route.
        pid_t scrollScanAwaitingPid =
            (terminal && m_claudeTabTracker) ? terminal->shellPid() : pid_t(0);
        if (scrollScanAwaitingPid > 0)
            m_claudeTabTracker->markShellAwaitingInput(scrollScanAwaitingPid, true);

        auto clearPromptActive = [this, scrollScanAwaitingPid]() {
            if (m_claudeStatusBarController)
                m_claudeStatusBarController->setPromptActive(false);
            if (m_claudeTabTracker && scrollScanAwaitingPid > 0)
                m_claudeTabTracker->markShellAwaitingInput(scrollScanAwaitingPid, false);
        };

        connect(addBtn, &QPushButton::clicked, this, [this, rule, addBtn, clearPromptActive]() {
            openClaudeAllowlistDialog(rule);
            addBtn->deleteLater();
            clearStatusMessage();
            clearPromptActive();
        });

        // 0.6.27 — mark prompt active so the Claude status label switches
        // to "Claude: prompting". Useful when the user is scrolled up in
        // the terminal history and can't see the prompt directly.
        if (m_claudeStatusBarController)
            m_claudeStatusBarController->setPromptActive(true);

        // Primary retraction: terminal scrollback scanner notices the
        // footer is gone. Now debounced against transient TUI repaints
        // (see terminalwidget.cpp:checkForClaudePermissionPrompt).
        // ANTS-1174: Qt::SingleShotConnection auto-disconnects on
        // first emission so we no longer need a heap-allocated
        // shared_ptr<Connection> just to capture-and-call-disconnect
        // from inside the lambda.
        connect(terminal, &TerminalWidget::claudePermissionCleared, addBtn,
                [addBtn, clearPromptActive, this]() {
            addBtn->deleteLater();
            clearStatusMessage();
            clearPromptActive();
        }, Qt::SingleShotConnection);

        // 0.6.33 — belt-and-suspenders retraction parity with the hook
        // path (see line ~2676). If the terminal scanner never notices
        // the prompt clearing (unmatched footer format on a future
        // Claude Code release; prompt scrolled off the 12-line lookback
        // before the debounce settled), toolFinished / sessionStopped
        // give us a resolve signal so the button doesn't linger. Same
        // reasoning as the hook path: errs on the side of closing the
        // button too early rather than leaving a stale "Add to
        // allowlist" stranded on the bar after the user already
        // approved.
        if (m_claudeIntegration) {
            // ANTS-1174: same SingleShotConnection treatment.
            connect(m_claudeIntegration, &ClaudeIntegration::toolFinished,
                    addBtn, [addBtn, clearPromptActive, this](const QString &, bool) {
                addBtn->deleteLater();
                clearStatusMessage();
                clearPromptActive();
            }, Qt::SingleShotConnection);
            connect(m_claudeIntegration, &ClaudeIntegration::sessionStopped,
                    addBtn, [addBtn, clearPromptActive, this](const QString &) {
                addBtn->deleteLater();
                clearStatusMessage();
                clearPromptActive();
            }, Qt::SingleShotConnection);
        }
    });

    // ANTS-1858 — AskUserQuestion / selection-prompt detection. Unlike
    // the permission path above there is no rule and no allow/deny
    // button: Claude is blocked on the user's choice, so we light the
    // owning tab's "awaiting input" dot + the "Claude: prompting" label
    // and nothing else. Routed by the emitting terminal pointer (no
    // session-id needed), mirroring the scroll-scan permission branch.
    connect(terminal, &TerminalWidget::claudeQuestionDetected, this,
            [this, terminal]() {
        if (terminal != focusedTerminal() && terminal != currentTerminal())
            return;
        const pid_t pid =
            (terminal && m_claudeTabTracker) ? terminal->shellPid() : pid_t(0);
        if (pid > 0)
            m_claudeTabTracker->markShellAwaitingInput(pid, true);
        if (m_claudeStatusBarController)
            m_claudeStatusBarController->setPromptActive(true);
    });
    connect(terminal, &TerminalWidget::claudeQuestionCleared, this,
            [this, terminal]() {
        const pid_t pid =
            (terminal && m_claudeTabTracker) ? terminal->shellPid() : pid_t(0);
        if (pid > 0)
            m_claudeTabTracker->markShellAwaitingInput(pid, false);
        if (m_claudeStatusBarController)
            m_claudeStatusBarController->setPromptActive(false);
    });

    // ANTS-1858 follow-up — reliable hook-driven clear for the question
    // dot, mirroring the permission path's belt (see ~line 2302). The
    // footer-gone debounce in checkForClaudePermissionPrompt can't
    // complete while Claude streams output (the trailing-edge detect
    // timer rarely fires N=3 times), so the dot would stay orange after
    // the user answered. An AskUserQuestion is a mid-turn tool call:
    // PostToolUse (→ toolFinished) fires the instant it is answered and
    // Stop (→ sessionStopped) at end-of-turn — neither fires while the
    // question is still on screen, so this never clears prematurely.
    // clearClaudeQuestionPrompt no-ops unless a question is active and
    // resets the sticky flag so the next question re-lights.
    if (m_claudeIntegration) {
        connect(m_claudeIntegration, &ClaudeIntegration::toolFinished,
                terminal, [terminal](const QString &, bool) {
            terminal->clearClaudeQuestionPrompt();
        });
        connect(m_claudeIntegration, &ClaudeIntegration::sessionStopped,
                terminal, [terminal](const QString &) {
            terminal->clearClaudeQuestionPrompt();
        });
    }
}

void MainWindow::newTab() {
    // Inherit CWD from the currently focused terminal
    QString inheritCwd;
    if (auto *prev = focusedTerminal())
        inheritCwd = prev->shellCwd();
    else if (auto *fallback = currentTerminal())
        inheritCwd = fallback->shellCwd();

    auto *terminal = createTerminal();
    connectTerminal(terminal);

    QString tabId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    int idx = m_tabWidget->addTab(terminal, "Shell");
    m_tabWidget->setCurrentIndex(idx);
    m_tabSessionIds[terminal] = tabId;

    if (!terminal->startShell(inheritCwd, m_config.shellCommand())) {
        showStatusMessage("Failed to start shell!");
    }

    terminal->setFocus();

    // Track shell process for Claude Code integration
    if (m_claudeIntegration)
        m_claudeIntegration->setShellPid(terminal->shellPid());
    trackTerminalShell(terminal);

    // Hide tab bar when only one tab
    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);

    // ANTS-1159 — persist the new tab in the on-disk order
    // immediately so a crash in the next 30 s window doesn't
    // lose it. saveTabOrderOnly's own guards short-circuit
    // when sessionPersistence is off or during the 5 s uptime
    // floor (the constructor's restoreSessions path lands here
    // for every restored tab and we don't want those replays
    // overwriting the on-disk order with a partially-built
    // list).
    saveTabOrderOnly();
}

void MainWindow::onSshConnect(const QString &sshCommand, bool inNewTab) {
    if (inNewTab) {
        newTab();
    }
    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();
    if (t) {
        // Small delay to let shell start
        // ANTS-5079 — guarded like newTabForRemote's settle timer: the tab
        // can close inside the 200 ms.
        QPointer<TerminalWidget> guard(t);
        QTimer::singleShot(200, this, [guard, sshCommand]() {
            if (guard) guard->writeCommand(sshCommand);
        });
    }
}

// ANTS-1735 §8 OQ-3 — one-shot opt-in nudge. The controller decides
// when to fire it; this slot just shows the prompt, persists the
// shown-flag (regardless of answer), and flips the switch on Yes.
void MainWindow::showClaudeAutoModelNudge() {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Let Ants pick the Claude model?"));
    box.setText(tr("Ants can swap Claude Code between fast/cheap and "
                   "big/slow models for you automatically."));
    box.setInformativeText(
        tr("It only ever switches between turns and before you start "
           "typing, so it never interrupts. You can change this any time "
           "in Settings → General → \"Let Ants pick the Claude model "
           "for me\"."));
    auto *enableBtn = box.addButton(tr("Enable"), QMessageBox::AcceptRole);
    box.addButton(tr("Not now"), QMessageBox::RejectRole);
    box.setDefaultButton(enableBtn);
    box.exec();

    Config cfg;
    cfg.setClaudeAutoModelNudgeShown(true);
    if (box.clickedButton() == enableBtn) {
        cfg.setClaudeAutoModelSwitch(true);
    }
}

void MainWindow::splitCurrentPane(Qt::Orientation orientation) {
    TerminalWidget *current = focusedTerminal();
    if (!current) current = currentTerminal();
    if (!current) return;

    // Create new terminal
    auto *newTerm = createTerminal();
    connectTerminal(newTerm);

    QWidget *parent = current->parentWidget();
    QSplitter *parentSplitter = qobject_cast<QSplitter *>(parent);

    if (parentSplitter) {
        // Already in a splitter — add new pane alongside current
        int idx = parentSplitter->indexOf(current);
        if (parentSplitter->orientation() == orientation) {
            // Same orientation — just insert next to it
            parentSplitter->insertWidget(idx + 1, newTerm);
        } else {
            // Different orientation — need to nest a new splitter
            auto *newSplitter = new QSplitter(orientation);
            parentSplitter->insertWidget(idx, newSplitter);
            current->setParent(nullptr);
            newSplitter->addWidget(current);
            newSplitter->addWidget(newTerm);
        }
    } else {
        // Current terminal is the direct tab widget content
        int tabIdx = m_tabWidget->indexOf(current);
        if (tabIdx < 0) return;

        auto *splitter = new QSplitter(orientation);

        // Transfer session ID from the terminal to the splitter
        QString sessionId = m_tabSessionIds.value(current);
        if (!sessionId.isEmpty()) {
            m_tabSessionIds.remove(current);
            m_tabSessionIds[splitter] = sessionId;
        }

        current->setParent(nullptr);
        splitter->addWidget(current);
        splitter->addWidget(newTerm);

        m_tabWidget->removeTab(tabIdx);
        m_tabWidget->insertTab(tabIdx, splitter, "Shell");
        m_tabWidget->setCurrentIndex(tabIdx);
    }

    if (!newTerm->startShell(QString(), m_config.shellCommand())) {
        showStatusMessage("Failed to start shell!");
    }
    // ANTS-5079 — a split pane's shell is tracked like a tab's, so Claude in
    // it lights the tab dot and its tasks are counted.
    trackTerminalShell(newTerm);
    newTerm->setFocus();
}

void MainWindow::splitHorizontal() {
    splitCurrentPane(Qt::Vertical); // Vertical splitter = horizontal split (panes stacked)
}

void MainWindow::splitVertical() {
    splitCurrentPane(Qt::Horizontal); // Horizontal splitter = vertical split (panes side by side)
}

void MainWindow::trackTerminalShell(TerminalWidget *terminal) {
    const pid_t pid = terminal ? terminal->shellPid() : 0;
    if (pid <= 0) return;
    if (m_claudeTabTracker) m_claudeTabTracker->trackShell(pid);
    if (m_claudeStatusBarController) m_claudeStatusBarController->trackBgShell(pid);
}

void MainWindow::releaseTerminalShell(TerminalWidget *terminal) {
    const pid_t pid = terminal ? terminal->shellPid() : 0;
    if (pid <= 0) return;
    if (m_claudeTabTracker) m_claudeTabTracker->untrackShell(pid);
    if (m_claudeStatusBarController) m_claudeStatusBarController->untrackBgShell(pid);
    // ANTS-1131 — also prune the ClaudeIntegration plan-mode cache for the
    // PID, so Linux PID reuse cannot hand a new shell a stale plan-mode flag.
    if (m_claudeIntegration) m_claudeIntegration->forgetShell(pid);
}

void MainWindow::closeFocusedPane() {
    TerminalWidget *focused = focusedTerminal();
    if (!focused) return;

    QSplitter *parent = findParentSplitter(focused);
    if (!parent) {
        // Only terminal in the tab -- close tab
        closeCurrentTab();
        return;
    }

    releaseTerminalShell(focused);  // ANTS-5079
    focused->setParent(nullptr);
    focused->deleteLater();
    cleanupEmptySplitters(m_tabWidget->currentWidget());

    // Focus the next available terminal
    if (auto *t = focusedTerminal()) {
        t->setFocus();
    } else if (auto *t2 = currentTerminal()) {
        t2->setFocus();
    }
}

QSplitter *MainWindow::findParentSplitter(QWidget *w) const {
    if (!w) return nullptr;
    return qobject_cast<QSplitter *>(w->parentWidget());
}

void MainWindow::cleanupEmptySplitters(QWidget *tabRoot) {
    if (!tabRoot) return;

    // Recursively clean up splitters with 0 or 1 children
    auto *splitter = qobject_cast<QSplitter *>(tabRoot);
    if (!splitter) return;

    // First, recurse into children
    for (int i = splitter->count() - 1; i >= 0; --i) {
        auto *childSplitter = qobject_cast<QSplitter *>(splitter->widget(i));
        if (childSplitter) cleanupEmptySplitters(childSplitter);
    }

    if (splitter->count() == 0) {
        // Empty splitter — close the tab
        int idx = m_tabWidget->indexOf(splitter);
        if (idx >= 0) closeTab(idx);
    } else if (splitter->count() == 1) {
        // Only one child left — promote it
        QWidget *child = splitter->widget(0);
        QSplitter *parentSplitter = qobject_cast<QSplitter *>(splitter->parentWidget());

        if (parentSplitter) {
            int idx = parentSplitter->indexOf(splitter);
            child->setParent(nullptr);
            parentSplitter->insertWidget(idx, child);
            splitter->setParent(nullptr);
            splitter->deleteLater();
        } else {
            // This splitter is the tab root
            int tabIdx = m_tabWidget->indexOf(splitter);
            if (tabIdx >= 0) {
                // Transfer session ID from splitter back to the surviving child
                QString sessionId = m_tabSessionIds.value(splitter);
                if (!sessionId.isEmpty()) {
                    m_tabSessionIds.remove(splitter);
                    m_tabSessionIds[child] = sessionId;
                }

                child->setParent(nullptr);
                splitter->setParent(nullptr);
                m_tabWidget->removeTab(tabIdx);
                m_tabWidget->insertTab(tabIdx, child, "Shell");
                m_tabWidget->setCurrentIndex(tabIdx);
                splitter->deleteLater();
            }
        }
    }
}

TerminalWidget *MainWindow::focusedTerminal() const {
    // ANTS-1911 — scope focus tracking to the CURRENTLY-SELECTED tab's
    // subtree. Pre-1911 the function walked QApplication::focusWidget()
    // first and only fell back to currentTerminal() when the walk
    // returned nullptr — but `QApplication::focusWidget()` is the
    // *global* focus across all windows, so a sibling tab whose
    // terminal still held keyboard focus (Qt does not always move
    // focus on a mouse-driven tab switch) could be returned even when
    // the user's *visually-current* tab is different. The status bar's
    // Claude chip + state label, the model chips, and a handful of
    // other callers ride this resolver — and a wrong-tab read leaks
    // the other tab's Claude state into the focused tab's chrome
    // (user report 2026-05-28 screenshots, ROADMAP ANTS-1911).
    //
    // The fix: get the current tab's root widget, then only accept a
    // QApplication::focusWidget() that lives inside that subtree.
    // Within-tab split-pane focus still resolves to the focused pane;
    // an unrelated tab's focus is rejected so the chrome stays
    // anchored to what the user sees.
    QWidget *currentTabRoot = m_tabWidget
        ? m_tabWidget->currentWidget() : nullptr;
    if (!currentTabRoot) return nullptr;
    QWidget *focused = QApplication::focusWidget();
    if (focused) {
        // ancestorOf accepts the widget itself as well.
        for (QWidget *w = focused; w; w = w->parentWidget()) {
            if (w == currentTabRoot) {
                // Focus is inside the current tab — walk up from
                // `focused` to find the enclosing TerminalWidget (so
                // a child line-edit or inner subwidget resolves to
                // its terminal).
                for (QWidget *p = focused; p; p = p->parentWidget()) {
                    if (auto *t = qobject_cast<TerminalWidget *>(p)) {
                        return t;
                    }
                }
                break;  // current-tab subtree but no terminal up the chain
            }
        }
    }
    // Fallback (no in-tab focus, or focus is in a foreign tab/window):
    // resolve to the visually-current tab's terminal so the chrome
    // tracks the user's view, not Qt's stale focus.
    return activeTerminalInTab(currentTabRoot);
}

// For a given tab root (a TerminalWidget or a QSplitter of panes), return the
// "active" terminal — the descendant that currently holds focus if any, else
// the first one in the subtree. findChild() alone returns an arbitrary first
// child, which gives the wrong pane in split layouts.
// ANTS-5223 — the page of `tabs` holding `w`: `w` itself or the ancestor the
// tab widget lists. Null when `w` is in no tab.
QWidget *mainwindowdetail::tabPageOf(const QTabWidget *tabs, QWidget *w) {
    if (!tabs) return nullptr;
    for (; w; w = w->parentWidget()) {
        if (tabs->indexOf(w) >= 0) return w;
    }
    return nullptr;
}

TerminalWidget *mainwindowdetail::activeTerminalInTab(QWidget *root) {
    if (!root) return nullptr;
    if (auto *t = qobject_cast<TerminalWidget *>(root)) return t;
    // Prefer a descendant that currently has focus
    const QList<TerminalWidget *> terms = root->findChildren<TerminalWidget *>();
    for (TerminalWidget *t : terms) {
        if (t && t->hasFocus()) return t;
    }
    return terms.isEmpty() ? nullptr : terms.first();
}

void MainWindow::closeTab(int index) {
    if (m_tabWidget->count() <= 1) {
        close();
        return;
    }

    QWidget *w = m_tabWidget->widget(index);
    if (!w) return;

    // Confirm-on-close (ANTS-1102): if the tab's shell has any non-shell
    // descendant running (vim, top, claude, tail -f, ...), ask before
    // tearing down. The dialog is async (Wayland-correct non-modal
    // pattern); confirmation calls performTabClose(idx) on Close-anyway.
    TerminalWidget *term = activeTerminalInTab(w);
    if (m_config.confirmCloseWithProcesses() && term && term->shellPid() > 0) {
        const QString descendant = firstNonShellDescendant(term->shellPid());
        if (!descendant.isEmpty()) {
            showCloseTabConfirmDialog(w, descendant);
            return;
        }
    }

    performTabClose(index);
}

void MainWindow::performTabClose(int index) {
    QWidget *w = m_tabWidget->widget(index);
    if (!w) return;

    // Save info for undo-close-tab — prefer the focused pane for split layouts
    TerminalWidget *term = activeTerminalInTab(w);
    if (term) {
        ClosedTabInfo info;
        info.cwd = term->shellCwd();
        info.title = m_tabWidget->tabText(index);
        m_closedTabs.prepend(info);
        if (m_closedTabs.size() > 10) m_closedTabs.removeLast();
    }

    // Drop any persisted tab-colour entry before we forget the UUID —
    // otherwise the config would accumulate orphan entries for closed
    // tabs that no future tab will ever re-use (UUIDs are unique).
    {
        QString tabId = m_tabSessionIds.value(w);
        if (!tabId.isEmpty()) {
            QJsonObject groups = m_config.tabGroups();
            if (groups.contains(tabId)) {
                groups.remove(tabId);
                m_config.setTabGroups(groups);
            }
        }
    }

    // Release every pane's Claude tracker entries BEFORE removeTab — once
    // the widget is detached we can't recover the shell PIDs. ANTS-5079: a
    // split tab holds several terminals, and only the active one used to be
    // released, leaving the rest in all three trackers.
    if (auto *single = qobject_cast<TerminalWidget *>(w))
        releaseTerminalShell(single);
    for (auto *pane : w->findChildren<TerminalWidget *>(QString()))
        releaseTerminalShell(pane);

    m_tabSessionIds.remove(w);
    m_tabTitlePins.remove(w);  // free pin alongside session id
    m_tabWidget->removeTab(index);
    w->deleteLater();

    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);

    if (auto *t = currentTerminal()) {
        t->setFocus();
    }

    // ANTS-1159 — persist the post-close order so a crash within
    // the next 30 s timer window doesn't resurrect the closed tab.
    saveTabOrderOnly();
}

void MainWindow::showCloseTabConfirmDialog(QWidget *tabWidget,
                                           const QString &processName) {
    // Wayland-correct non-modal QDialog pattern (mirrors the About
    // dialog at MainWindow ctor — see commit 6bea531 / 0.7.50 for the
    // QTBUG-79126 rationale). Plain QPushButtons; no setModal.
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Close tab?"));
    dlg->setObjectName(QStringLiteral("confirmCloseTabDialog"));
    // ANTS-5123 — dialogs.md D1–D4: theme chrome, resizable, size persisted,
    // re-centred, as the window-close dialog has.
    auto chrome = DialogChrome::install(dlg, QString(),
                                        /*resizable=*/true,
                                        QStringLiteral("CloseTabConfirmDialog"));
    auto *layout = new QVBoxLayout(chrome.contentArea);

    auto *label = new QLabel(
        tr("This tab is running <b>%1</b>.<br>Close anyway? Long-running "
           "processes will be terminated.")
            .arg(processName.toHtmlEscaped()),
        dlg);
    label->setObjectName(QStringLiteral("confirmCloseTabBody"));
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setAccessibleName(tr("Confirm tab close"));
    label->setAccessibleDescription(label->text());

    auto *dontAsk = new QCheckBox(
        tr("Don't ask again (close tabs silently in future)"), dlg);
    dontAsk->setObjectName(QStringLiteral("confirmCloseDontAsk"));

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *cancelBtn = new QPushButton(tr("Cancel"), dlg);
    cancelBtn->setObjectName(QStringLiteral("confirmCloseCancelBtn"));
    cancelBtn->setDefault(true);
    cancelBtn->setAutoDefault(true);
    auto *closeBtn = new QPushButton(tr("Close anyway"), dlg);
    closeBtn->setObjectName(QStringLiteral("confirmCloseProceedBtn"));
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(closeBtn);

    layout->addWidget(label);
    layout->addWidget(dontAsk);
    layout->addLayout(btnRow);

    // Track the actual widget; the index can shift if other tabs close
    // while this dialog is open.
    QPointer<QWidget> widgetRef(tabWidget);

    connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::close);
    connect(closeBtn, &QPushButton::clicked, this,
        [this, dlg, dontAsk, widgetRef]() {
            if (dontAsk->isChecked())
                m_config.setConfirmCloseWithProcesses(false);
            dlg->close();
            if (!widgetRef) return;
            const int idx = m_tabWidget->indexOf(widgetRef);
            if (idx >= 0) performTabClose(idx);
        });

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

// ANTS-5120 — the window-close counterpart of showCloseTabConfirmDialog:
// the same non-modal pattern and the same "don't ask again" setting.
void MainWindow::showCloseWindowConfirmDialog(const QString &processName) {
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Close window?"));
    dlg->setObjectName(QStringLiteral("confirmCloseWindowDialog"));
    // dialogs.md D1–D4: theme chrome, resizable, size persisted, re-centred.
    auto chrome = DialogChrome::install(dlg, QString(),
                                        /*resizable=*/true,
                                        QStringLiteral("CloseWindowConfirmDialog"));
    auto *layout = new QVBoxLayout(chrome.contentArea);

    auto *label = new QLabel(
        tr("This window is running <b>%1</b>.<br>Close anyway? Every program "
           "in its tabs will be terminated.")
            .arg(processName.toHtmlEscaped()),
        dlg);
    label->setObjectName(QStringLiteral("confirmCloseWindowBody"));
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setAccessibleName(tr("Confirm window close"));
    label->setAccessibleDescription(label->text());

    auto *dontAsk = new QCheckBox(
        tr("Don't ask again (close silently in future)"), dlg);
    dontAsk->setObjectName(QStringLiteral("confirmCloseWindowDontAsk"));

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *cancelBtn = new QPushButton(tr("Cancel"), dlg);
    cancelBtn->setObjectName(QStringLiteral("confirmCloseWindowCancelBtn"));
    cancelBtn->setDefault(true);
    cancelBtn->setAutoDefault(true);
    auto *closeBtn = new QPushButton(tr("Close anyway"), dlg);
    closeBtn->setObjectName(QStringLiteral("confirmCloseWindowProceedBtn"));
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(closeBtn);

    layout->addWidget(label);
    layout->addWidget(dontAsk);
    layout->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::close);
    // ANTS-5560 — a cancelled close also cancels a pending Restart now.
    connect(cancelBtn, &QPushButton::clicked, this, [] { g_relaunchOnQuit = false; });
    connect(closeBtn, &QPushButton::clicked, this, [this, dlg, dontAsk]() {
        if (dontAsk->isChecked())
            m_config.setConfirmCloseWithProcesses(false);
        dlg->close();
        m_closeConfirmed = true;
        close();
    });

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::closeCurrentTab() {
    closeTab(m_tabWidget->currentIndex());
}

void MainWindow::onTabChanged(int index) {
    // All per-tab status-bar state — branch chip, process name,
    // notification slot, Claude state, Review Changes button, Add-to-
    // allowlist button — funnels through a single refresh point so
    // nothing bleeds from the previous tab. See
    // refreshStatusBarForActiveTab() for the lifecycle contract.
    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();
    if (t) {
        t->setFocus();
        onTitleChanged(t->shellTitle());
    }
    refreshStatusBarForActiveTab();

#ifdef ANTS_LUA_PLUGINS
    // 0.6.9 — fire `pane_focused` so plugins can swap context (per-pane
    // status, badge, ssh-connection-aware behavior). Today this fires on
    // tab switches; once split-pane focus tracking lands the same event
    // covers within-tab pane changes without further plugin churn.
    if (m_pluginManager) {
        QString tabTitle = (index >= 0 && index < m_tabWidget->count())
                           ? m_tabWidget->tabText(index) : QString();
        m_pluginManager->fireEvent(PluginEvent::PaneFocused, tabTitle);
    }
#endif
}

TerminalWidget *MainWindow::currentTerminal() const {
    // Prefer the focused pane so split layouts dispatch commands correctly;
    // falls back to the first pane in the tab subtree.
    return activeTerminalInTab(m_tabWidget->currentWidget());
}

TerminalWidget *MainWindow::terminalAtTab(int index) const {
    if (index < 0 || index >= m_tabWidget->count()) return nullptr;
    return activeTerminalInTab(m_tabWidget->widget(index));
}

// ANTS-1392 — caller_cwd-anchored terminal lookup. Walks every tab
// (including split-pane subtrees via activeTerminalInTab) for a
// terminal whose canonical shellCwd matches the canonical callerCwd.
// First match wins.
//
// ANTS-1396 — contract split. Three cases:
//   1. caller_cwd is empty  → fall back to focusedTerminal()
//      (preserves the pre-ANTS-1392 contract for tools invoked
//      without the arg).
//   2. caller_cwd given, matches a tab → return that tab.
//   3. caller_cwd given, NO matching tab → return nullptr (do NOT
//      fall back to focused). A caller that names a specific cwd
//      is asking for *that* project's data; silently substituting
//      whatever happens to be focused leaks cross-project data
//      (the originating report was `get_git_status` returning the
//      Ants Terminal repo while the caller's cwd was a different
//      project with no Ants tab open).
//
// Callers already null-check the return (verified at the four
// terminalForCaller call sites in this file:
// `if (auto *t = terminalForCaller(...))` or `if (!t) return {};`).
// ANTS-4932 § 2.4 — the terminal's root provider. Every read is of tab
// state, so each one marshals to the GUI thread: the verbs that call it run
// on the dispatch worker. A refused marshal (shutdown) answers "nothing".
namespace {
class MainWindowRootProvider final : public ants::RootProvider {
public:
    explicit MainWindowRootProvider(const MainWindow *w) : m_w(w) {}
    QString fallbackRoot() const override {
        const MainWindow *w = m_w;
        const auto cwd = ants::onGuiThread([w]() -> QString {
            auto *t = w->focusedTerminal();
            return t ? t->shellCwd() : QString();
        });
        return cwd ? *cwd : QString();
    }
    QString fallbackRoadmapPath() const override {
        const MainWindow *w = m_w;
        const auto p = ants::onGuiThread(
            [w]() { return w->roadmapPathForRemote(); });
        return p ? *p : QString();
    }
    std::optional<int> fallbackTab() const override {
        const MainWindow *w = m_w;
        const auto i = ants::onGuiThread(
            [w]() { return w->currentTabIndexForRemote(); });
        if (i && *i >= 0) return *i;
        return std::nullopt;
    }
    ants::ResolvedRoot::Source fallbackSource() const override {
        return ants::ResolvedRoot::Source::EmptyFallback;
    }
    std::optional<int> tabForCwd(const QString &canonical) const override {
        // ANTS-2132 — snapshot every tab's cwd in ONE marshal, then
        // canonicalise here: QFileInfo is thread-safe, the widget reads are
        // not. Index order is kept, so INV-5's lowest-index tie-break holds.
        const MainWindow *w = m_w;
        const auto snap = ants::onGuiThread([w]() {
            QList<QPair<int, QString>> v;
            for (int i = 0; i < w->tabCount(); ++i) {
                if (auto *t = w->terminalAtTab(i))
                    v.append(QPair<int, QString>(i, t->shellCwd()));
            }
            return v;
        });
        if (!snap) return std::nullopt;
        for (const auto &entry : *snap) {
            if (entry.second.isEmpty()) continue;
            const QString c = QFileInfo(entry.second).canonicalFilePath();
            if (!c.isEmpty() && c == canonical) return entry.first;
        }
        return std::nullopt;
    }
private:
    const MainWindow *m_w;
};
}  // namespace

std::unique_ptr<ants::RootProvider> MainWindow::makeRootProvider(
        const MainWindow *w) {
    return std::make_unique<MainWindowRootProvider>(w);
}

TerminalWidget *MainWindow::terminalForCaller(const QString &callerCwd) const {
    // ANTS-1401 — single source of truth. The four-case decision tree
    // ANTS-1396 introduced now lives in `ants::resolveCallerCwdRoot`
    // (declared in resolvedroot.h, defined alongside the
    // `resolveRootCanonical` overloads in remotecontrol.cpp). This
    // function maps the tagged variant back to a TerminalWidget *.
    const ants::ResolvedRoot rr =
        ants::resolveCallerCwdRoot(m_rootProvider.get(), callerCwd);
    switch (rr.source) {
        case ants::ResolvedRoot::Source::EmptyFallback:
        case ants::ResolvedRoot::Source::ServerCwd:
            // Case 1 — legacy back-compat. Accessor may return nullptr
            // if no tab is focused; preserve that shape unchanged.
            return focusedTerminal();
        case ants::ResolvedRoot::Source::ExplicitMatch:
            // Case 2: caller_cwd canonicalises and matches an open tab.
            return rr.tabIndex ? terminalAtTab(*rr.tabIndex) : nullptr;
        case ants::ResolvedRoot::Source::NoMatch:
        case ants::ResolvedRoot::Source::Unresolvable:
            // Case 3: explicit caller_cwd, no match → nullptr.
            //         Unresolvable path also degrades to nullptr.
            return nullptr;
    }
    return nullptr;  // -Wreturn-type
}

int MainWindow::tabCount() const {
    return m_tabWidget->count();
}

int MainWindow::currentTabIndexForRemote() const {
    return m_tabWidget->currentIndex();
}

bool MainWindow::setTabTitleForRemote(int index, const QString &title) {
    if (index < 0 || index >= m_tabWidget->count()) return false;
    QWidget *w = m_tabWidget->widget(index);
    if (title.isEmpty()) {
        // Clear the pin and refresh immediately. Two cases:
        //   - tabTitleFormat != "title" → updateTabTitles() does the
        //     work for us based on cwd / process.
        //   - tabTitleFormat == "title" → updateTabTitles bails;
        //     we have to restore the most recent shell-provided title
        //     manually, otherwise the pinned label sits there until
        //     the *next* OSC 0/2 fires (which may be never on a
        //     quiet prompt). Pull it from the active terminal's
        //     `shellTitle()` cache (the same value the titleChanged
        //     signal would have set).
        m_tabTitlePins.remove(w);
        updateTabTitles();
        if (m_config.tabTitleFormat() == "title") {
            if (auto *term = activeTerminalInTab(w)) {
                QString shellTitle = term->shellTitle();
                if (shellTitle.isEmpty()) shellTitle = "Shell";
                if (shellTitle.length() > 30)
                    shellTitle = shellTitle.left(27) + "...";
                m_tabWidget->setTabText(index, shellTitle);
            }
        }
    } else {
        // Pin the label. The titleChanged handler and updateTabTitles
        // both check the pin map before calling setTabText, so the
        // value sticks across both the per-shell signal and the 2 s
        // refresh tick. Truncated to the same 30-char ceiling the
        // signal handler uses to avoid the tab strip ballooning.
        m_tabTitlePins[w] = title;
        QString display = title.length() > 30 ? title.left(27) + "..." : title;
        m_tabWidget->setTabText(index, display);
    }
    return true;
}

bool MainWindow::selectTabForRemote(int index) {
    if (index < 0 || index >= m_tabWidget->count()) return false;
    m_tabWidget->setCurrentIndex(index);
    // Refocus the new tab's terminal so follow-up send-text calls
    // without an explicit tab field land on this pane. Without the
    // explicit setFocus the keyboard focus can stay on whatever
    // widget (menubar, search bar, dialog button) owned it at
    // switch-time.
    if (auto *term = activeTerminalInTab(m_tabWidget->widget(index))) {
        term->setFocus();
    }
    return true;
}

int MainWindow::newTabForRemote(const QString &cwd, const QString &command,
                                bool *shellStarted) {
    // Mirror of the newTab() slot but with explicit cwd/command
    // plumbing so rc_protocol `new-tab` doesn't need to round-trip
    // through signals. Returns the index of the created tab so the
    // caller can target it in follow-up commands.
    QString effectiveCwd = cwd;
    if (effectiveCwd.isEmpty()) {
        if (auto *prev = focusedTerminal())
            effectiveCwd = prev->shellCwd();
        else if (auto *fallback = currentTerminal())
            effectiveCwd = fallback->shellCwd();
    }

    auto *terminal = createTerminal();
    connectTerminal(terminal);

    QString tabId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    int idx = m_tabWidget->addTab(terminal, "Shell");
    m_tabWidget->setCurrentIndex(idx);
    m_tabSessionIds[terminal] = tabId;

    // The tab stays when the shell fails, as the menu's newTab() leaves it;
    // the caller is told, and no command is queued into a dead tab.
    const bool started = terminal->startShell(effectiveCwd, m_config.shellCommand());
    if (shellStarted) *shellStarted = started;
    if (!started) showStatusMessage("Failed to start shell!");
    terminal->setFocus();

    if (m_claudeIntegration)
        m_claudeIntegration->setShellPid(terminal->shellPid());
    trackTerminalShell(terminal);

    // Hide tab bar when only one tab (same logic as newTab slot).
    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);

    if (started && !command.isEmpty()) {
        // 200 ms settle before writing — same timing the SSH-manager
        // wiring uses (onSshConnect) because the shell child needs a
        // moment to finish its init before it can accept input reliably.
        // Use sendToPty (raw bytes) rather than writeCommand: the
        // caller owns the trailing newline, matching send-text
        // semantics. `launch` is the rc command that auto-appends
        // newlines for the convenience case; `new-tab` stays
        // byte-faithful so a script can write partial lines or
        // include control sequences.
        QPointer<TerminalWidget> guard(terminal);
        QByteArray cmdBytes = command.toUtf8();
        QTimer::singleShot(200, this, [guard, cmdBytes]() {
            if (guard) guard->sendToPty(cmdBytes);
        });
    }
    return idx;
}

QJsonArray MainWindow::tabListForRemote() const {
    // One JSON object per tab. `active: true` on exactly the tab that
    // `currentTerminal()` is inside, so a remote-control client can
    // tell which pane receives input by default. `cwd` reads the
    // focused terminal's shell cwd (via OSC 7 or /proc fallback); may
    // be empty when the shell hasn't sent OSC 7 yet and /proc is
    // unavailable (e.g. stale PID after fork).
    QJsonArray tabs;
    const int n = m_tabWidget->count();
    const int current = m_tabWidget->currentIndex();
    for (int i = 0; i < n; ++i) {
        QJsonObject t;
        t["index"] = i;
        t["title"] = m_tabWidget->tabText(i);
        t["active"] = (i == current);
        QString cwd;
        if (auto *term = activeTerminalInTab(m_tabWidget->widget(i))) {
            cwd = term->shellCwd();
        }
        t["cwd"] = cwd;
        tabs.append(t);
    }
    return tabs;
}

QJsonArray MainWindow::tabsAsJson() const {
    // Richer per-tab snapshot for the `tab-list` IPC verb (ANTS-1117).
    // Adds `shell_pid`, `claude_running`, and `color` on top of the
    // narrower `tabListForRemote` shape — keeps the existing `ls`
    // verb's contract unchanged for backward compat.
    QJsonArray tabs;
    const int n = m_tabWidget->count();
    for (int i = 0; i < n; ++i) {
        QJsonObject t;
        t["index"] = i;
        t["title"] = m_tabWidget->tabText(i);
        QString cwd;
        pid_t shellPid = 0;
        if (auto *term = activeTerminalInTab(m_tabWidget->widget(i))) {
            cwd = term->shellCwd();
            shellPid = term->shellPid();
        }
        t["cwd"] = cwd;
        t["shell_pid"] = qint64(shellPid);
        bool claudeRunning = false;
        if (m_claudeTabTracker && shellPid > 0) {
            // ANTS-1865 — surface the per-tab Claude glyph state so dot /
            // prompt-state behaviour is programmatically verifiable instead
            // of needing the user to eyeball the tab strip. `claude_state`
            // is the transcript-derived base state; `awaiting_input` /
            // `plan_mode` / `auditing` are the overlays that (together with
            // the base) determine the resolved dot, so a caller can compute
            // the expected glyph without the resolver.
            const auto ss = m_claudeTabTracker->shellState(shellPid);
            claudeRunning = (ss.state != ClaudeState::NotRunning);
            // Local snake_case mapping (no default → a new enum value trips
            // -Wswitch here, mirroring claudeStateName in claudestatuswidgets).
            QString stateName;
            switch (ss.state) {
                case ClaudeState::NotRunning: stateName = QStringLiteral("not_running"); break;
                case ClaudeState::Idle:       stateName = QStringLiteral("idle");        break;
                case ClaudeState::Thinking:   stateName = QStringLiteral("thinking");    break;
                case ClaudeState::ToolUse:    stateName = QStringLiteral("tool_use");    break;
                case ClaudeState::Compacting: stateName = QStringLiteral("compacting");  break;
            }
            if (stateName.isEmpty()) stateName = QStringLiteral("idle");
            t["claude_state"] = stateName;
            t["awaiting_input"] = ss.awaitingInput;
            // Lean envelope: emit the boolean/string overlays only when set.
            if (ss.planMode) t["plan_mode"] = true;
            if (ss.auditing) t["auditing"] = true;
            if (ss.state == ClaudeState::ToolUse && !ss.tool.isEmpty())
                t["tool"] = ss.tool;
        }
        t["claude_running"] = claudeRunning;
        QString color;
        if (m_coloredTabBar) {
            const QColor c = m_coloredTabBar->tabColor(i);
            if (c.isValid()) color = c.name();
        }
        t["color"] = color;
        tabs.append(t);
    }
    return tabs;
}

const QString &MainWindow::roadmapPathForRemote() const {
    return m_roadmapPath;
}
