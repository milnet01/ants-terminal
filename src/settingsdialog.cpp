#include "settingsdialog.h"
#include "config.h"
#include "configbackup.h"
#include "configpaths.h"
#include "globalshortcutsportal.h"
#include "secureio.h"
#include "themes.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QColorDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollArea>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QMessageBox>

SettingsDialog::SettingsDialog(Config *config, QWidget *parent)
    : QDialog(parent), m_config(config) {
    setWindowTitle("Settings");
    setMinimumSize(700, 550);
    resize(800, 600);

    auto *mainLayout = new QVBoxLayout(this);

    m_tabs = new QTabWidget(this);

    auto *generalTab = new QWidget();
    setupGeneralTab(generalTab);
    m_tabs->addTab(generalTab, "General");

    auto *appearanceTab = new QWidget();
    setupAppearanceTab(appearanceTab);
    m_tabs->addTab(appearanceTab, "Appearance");

    auto *terminalTab = new QWidget();
    setupTerminalTab(terminalTab);
    m_tabs->addTab(terminalTab, "Terminal");

    auto *aiTab = new QWidget();
    setupAiTab(aiTab);
    m_tabs->addTab(aiTab, "AI Assistant");

    auto *highlightsTab = new QWidget();
    setupHighlightsTab(highlightsTab);
    m_tabs->addTab(highlightsTab, "Highlights");

    auto *triggersTab = new QWidget();
    setupTriggersTab(triggersTab);
    m_tabs->addTab(triggersTab, "Triggers");

    auto *keybindingsTab = new QWidget();
    setupKeybindingsTab(keybindingsTab);
    m_tabs->addTab(keybindingsTab, "Keybindings");

    auto *profilesTab = new QWidget();
    setupProfilesTab(profilesTab);
    m_tabs->addTab(profilesTab, "Profiles");

    auto *pluginsTab = new QWidget();
    setupPluginsTab(pluginsTab);
    m_tabs->addTab(pluginsTab, "Plugins");

    mainLayout->addWidget(m_tabs, 1);

    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, [this]() {
        applySettings();
        accept();
    });
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this]() {
        applySettings();
    });
    mainLayout->addWidget(buttonBox);

    loadSettings();
}

void SettingsDialog::setupGeneralTab(QWidget *tab) {
    auto *layout = new QFormLayout(tab);

    m_shellCombo = new QComboBox(tab);
    m_shellCombo->addItems({"Default (login shell)", "bash", "zsh", "fish", "Custom..."});
    m_shellCustom = new QLineEdit(tab);
    m_shellCustom->setPlaceholderText("/path/to/shell");
    m_shellCustom->setVisible(false);
    connect(m_shellCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        m_shellCustom->setVisible(text == "Custom...");
    });
    layout->addRow("Shell:", m_shellCombo);
    layout->addRow("Custom Shell:", m_shellCustom);

    m_tabTitleFormat = new QComboBox(tab);
    m_tabTitleFormat->addItem("Shell Title", "title");
    m_tabTitleFormat->addItem("Working Directory", "cwd");
    m_tabTitleFormat->addItem("Running Process", "process");
    m_tabTitleFormat->addItem("CWD - Process", "cwd-process");
    layout->addRow("Tab Title:", m_tabTitleFormat);

    m_sessionPersistence = new QCheckBox("Save and restore sessions on exit/start", tab);
    layout->addRow(m_sessionPersistence);

    m_sessionLogging = new QCheckBox("Log session output to files", tab);
    layout->addRow(m_sessionLogging);

    m_autoCopy = new QCheckBox("Auto-copy text on selection", tab);
    layout->addRow(m_autoCopy);

    m_confirmMultilinePaste = new QCheckBox(
        "Confirm multi-line / dangerous pastes (sudo, curl | sh, control chars)", tab);
    layout->addRow(m_confirmMultilinePaste);

    m_editorCmd = new QLineEdit(tab);
    m_editorCmd->setPlaceholderText("code, vim, nano, etc.");
    layout->addRow("Editor Command:", m_editorCmd);

    m_imagePasteDir = new QLineEdit(tab);
    m_imagePasteDir->setPlaceholderText("~/Pictures/ClaudePaste");
    layout->addRow("Image Paste Dir:", m_imagePasteDir);

    // Status-bar notification display time. User spec 2026-04-18:
    // "should have a timeout that can be adjusted in the settings with
    // a default of 5 seconds." Applies only to the transient message
    // slot (the ephemeral "Config loaded", "Theme: Dark" kind of toast).
    // The git branch chip, Claude status, and permission prompt all
    // have independent lifecycles and are not affected by this value.
    m_notificationTimeout = new QSpinBox(tab);
    m_notificationTimeout->setRange(1, 60);
    m_notificationTimeout->setSuffix(" s");
    m_notificationTimeout->setToolTip(
        "How long transient status-bar toasts remain visible.\n"
        "Pinned messages (Claude permission prompts) are unaffected.");
    layout->addRow("Notification Timeout:", m_notificationTimeout);

    // Claude Code hook installer (0.6.33, user-requested 2026-04-18).
    // Writes ~/.config/ants-terminal/hooks/claude-forward.sh and merges
    // hook entries into ~/.claude/settings.json so that tool-use /
    // permission / session events reach the ants-terminal status bar in
    // real time instead of via the ~50ms transcript-file debounce path.
    // See README §"Claude Code integration" for the full scheme.
    m_installClaudeHooksBtn = new QPushButton("Install Claude Code status-bar hooks", tab);
    m_claudeHooksStatus = new QLabel(tab);
    m_claudeHooksStatus->setWordWrap(true);
    connect(m_installClaudeHooksBtn, &QPushButton::clicked,
            this, &SettingsDialog::installClaudeHooks);
    layout->addRow("Claude Code:", m_installClaudeHooksBtn);
    layout->addRow(QString(), m_claudeHooksStatus);
    refreshClaudeHooksStatus();

    // Claude Code `UserPromptSubmit` git-context hook (user ask 2026-04-24).
    // Independent of the status-bar hooks above — this one runs in the
    // opposite direction: Claude pulls git state from the script on every
    // user prompt, so the model gets `git status` context without spending
    // tokens on a Bash tool call. Global across every project because the
    // hook lives in ~/.claude/settings.json (user-scope). Installed
    // separately so users can opt into one and not the other.
    // See tests/features/claude_git_context_hook/spec.md.
    m_installClaudeGitContextBtn = new QPushButton("Install git-context hook", tab);
    m_installClaudeGitContextBtn->setToolTip(
        "Adds a UserPromptSubmit hook to ~/.claude/settings.json that "
        "injects a <git-context> block into every Claude Code prompt. "
        "Claude sees the current branch, staged/unstaged/untracked counts, "
        "and upstream sync state without spending tokens on `git status`.");
    m_claudeGitContextStatus = new QLabel(tab);
    m_claudeGitContextStatus->setWordWrap(true);
    connect(m_installClaudeGitContextBtn, &QPushButton::clicked,
            this, &SettingsDialog::installClaudeGitContextHook);
    layout->addRow(QString(), m_installClaudeGitContextBtn);
    layout->addRow(QString(), m_claudeGitContextStatus);
    refreshClaudeGitContextStatus();

    // Per-tab Claude activity glyph. When enabled, each tab whose shell
    // has a Claude Code child process draws a small state-dependent dot
    // on the tab chrome — muted grey for idle/thinking, blue for most
    // tool-use, green for Bash, cyan for plan mode, violet for
    // compacting, and bright orange (with a white outline) for pending
    // permission prompts. Off → the tracker isn't constructed and the
    // paint pass is skipped entirely. See
    // tests/features/claude_tab_status_indicator/spec.md.
    m_claudeTabStatusIndicator = new QCheckBox(
        "Show per-tab Claude activity dot (idle/tool/bash/awaiting-input/…)", tab);
    m_claudeTabStatusIndicator->setToolTip(
        "Renders a small coloured dot on each tab whose shell is running "
        "Claude Code. Orange with a white outline means Claude is waiting "
        "on you to answer a permission prompt in that tab.");
    layout->addRow(m_claudeTabStatusIndicator);

    // Restore Defaults — resets ONLY the General-tab controls to
    // their schema defaults. Doesn't touch m_config until the user
    // clicks Apply or OK; Cancel rolls everything back as usual.
    auto *generalDefaultsBtn = new QPushButton("Restore Defaults (General tab)", tab);
    generalDefaultsBtn->setObjectName(QStringLiteral("restoreDefaultsGeneral"));
    connect(generalDefaultsBtn, &QPushButton::clicked, this, [this]() {
        m_shellCombo->setCurrentIndex(0);  // Default (login shell)
        m_shellCustom->clear();
        m_tabTitleFormat->setCurrentIndex(
            m_tabTitleFormat->findData(QStringLiteral("title")));
        m_sessionPersistence->setChecked(true);
        m_sessionLogging->setChecked(false);
        m_autoCopy->setChecked(true);
        m_confirmMultilinePaste->setChecked(true);
        m_editorCmd->clear();
        m_imagePasteDir->clear();
        m_notificationTimeout->setValue(5);
        m_claudeTabStatusIndicator->setChecked(true);
    });
    layout->addRow(QString(), generalDefaultsBtn);
}

void SettingsDialog::setupAppearanceTab(QWidget *tab) {
    auto *layout = new QFormLayout(tab);

    m_fontFamily = new QFontComboBox(tab);
    m_fontFamily->setFontFilters(QFontComboBox::MonospacedFonts);
    layout->addRow("Font:", m_fontFamily);

    m_fontSize = new QSpinBox(tab);
    m_fontSize->setRange(4, 48);
    layout->addRow("Font Size:", m_fontSize);

    m_themeCombo = new QComboBox(tab);
    for (const QString &name : Themes::names())
        m_themeCombo->addItem(name);
    layout->addRow("Theme:", m_themeCombo);

    m_opacitySlider = new QSlider(Qt::Horizontal, tab);
    m_opacitySlider->setRange(10, 100);
    m_opacityLabel = new QLabel("100%", tab);
    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int val) {
        m_opacityLabel->setText(QString("%1%").arg(val));
    });
    auto *opacityLayout = new QHBoxLayout();
    opacityLayout->addWidget(m_opacitySlider, 1);
    opacityLayout->addWidget(m_opacityLabel);
    layout->addRow("Window Opacity:", opacityLayout);

    m_backgroundBlur = new QCheckBox("Enable background blur (KDE/KWin)", tab);
    layout->addRow(m_backgroundBlur);

    m_paddingSpinner = new QSpinBox(tab);
    m_paddingSpinner->setRange(0, 32);
    m_paddingSpinner->setSuffix(" px");
    layout->addRow("Terminal Padding:", m_paddingSpinner);

    // Badge text (watermark in terminal background)
    m_badgeEdit = new QLineEdit(tab);
    m_badgeEdit->setPlaceholderText("e.g. hostname, project name...");
    layout->addRow("Badge Text:", m_badgeEdit);

    // Dark/light auto-switching
    layout->addRow(new QLabel(""));  // spacer
    m_autoColorScheme = new QCheckBox("Auto-switch theme with system dark/light mode", tab);
    layout->addRow(m_autoColorScheme);

    m_darkThemeCombo = new QComboBox(tab);
    m_lightThemeCombo = new QComboBox(tab);
    for (const QString &name : Themes::names()) {
        m_darkThemeCombo->addItem(name);
        m_lightThemeCombo->addItem(name);
    }
    layout->addRow("Dark Theme:", m_darkThemeCombo);
    layout->addRow("Light Theme:", m_lightThemeCombo);

    // Dependency-UI gating: dark/light combos only matter when
    // auto-switching is on. Disabled controls are visibly greyed but
    // keep their current value, so toggling auto-switch back on
    // restores the user's prior selection rather than resetting it.
    auto syncAutoColor = [this]() {
        const bool on = m_autoColorScheme->isChecked();
        m_darkThemeCombo->setEnabled(on);
        m_lightThemeCombo->setEnabled(on);
    };
    connect(m_autoColorScheme, &QCheckBox::toggled, this, syncAutoColor);
    syncAutoColor();

    auto *appearanceDefaultsBtn = new QPushButton(
        "Restore Defaults (Appearance tab)", tab);
    appearanceDefaultsBtn->setObjectName(QStringLiteral("restoreDefaultsAppearance"));
    connect(appearanceDefaultsBtn, &QPushButton::clicked, this, [this]() {
        m_fontFamily->setCurrentFont(QFont());
        m_fontSize->setValue(11);
        m_themeCombo->setCurrentText(QStringLiteral("Dark"));
        m_opacitySlider->setValue(100);
        m_backgroundBlur->setChecked(false);
        m_paddingSpinner->setValue(4);
        m_badgeEdit->clear();
        m_autoColorScheme->setChecked(false);
        m_darkThemeCombo->setCurrentText(QStringLiteral("Dark"));
        m_lightThemeCombo->setCurrentText(QStringLiteral("Light"));
    });
    layout->addRow(QString(), appearanceDefaultsBtn);
}

void SettingsDialog::setupTerminalTab(QWidget *tab) {
    auto *layout = new QFormLayout(tab);

    m_scrollbackLines = new QSpinBox(tab);
    m_scrollbackLines->setRange(1000, 1000000);
    m_scrollbackLines->setSingleStep(10000);
    layout->addRow("Scrollback Lines:", m_scrollbackLines);

    m_showCommandMarks = new QCheckBox("Show command markers in scrollbar gutter", tab);
    m_showCommandMarks->setToolTip(
        "Draw a tick next to the scrollbar for each OSC 133 prompt region. "
        "Color indicates last exit status: green (success), red (failure), "
        "gray (in-progress). No-op when shell integration isn't installed.");
    layout->addRow(m_showCommandMarks);

    auto *quakeGroup = new QGroupBox("Dropdown/Quake Mode", tab);
    auto *quakeLayout = new QFormLayout(quakeGroup);
    m_quakeMode = new QCheckBox("Enable dropdown mode", quakeGroup);
    quakeLayout->addRow(m_quakeMode);
    m_quakeHotkey = new QLineEdit(quakeGroup);
    m_quakeHotkey->setPlaceholderText("F12");
    quakeLayout->addRow("Global Hotkey:", m_quakeHotkey);

    // Portal-binding status hint (0.6.41). Reports whether the global
    // hotkey can escape focus via the Freedesktop Portal GlobalShortcuts
    // D-Bus API (KDE Plasma 6 / Hyprland / wlroots) or falls back to the
    // in-app QShortcut (GNOME as of 2026-04, or any session where
    // xdg-desktop-portal isn't registered). Static check at dialog
    // construction — accurate because portal availability is a per-
    // session property that doesn't change while the app is running.
    auto *portalStatus = new QLabel(quakeGroup);
    portalStatus->setWordWrap(true);
    if (GlobalShortcutsPortal::isAvailable()) {
        portalStatus->setText(QStringLiteral(
            "<span style='color:#4aa84a;'>&#x2713; Portal binding active</span> "
            "&mdash; hotkey works when Ants is unfocused."));
    } else {
        portalStatus->setText(QStringLiteral(
            "<span style='color:#c77a1a;'>&#9888; Portal unavailable</span> "
            "&mdash; hotkey works only while Ants is focused "
            "(install <tt>xdg-desktop-portal-kde</tt> / "
            "<tt>-hyprland</tt> / <tt>-wlr</tt> for out-of-focus support)."));
    }
    quakeLayout->addRow(portalStatus);

    layout->addRow(quakeGroup);

    // Dependency-UI gating: hotkey field is meaningless when Quake
    // mode is off. Portal-status label stays visible — its
    // diagnostic value (binding ok / portal absent) is independent
    // of whether the user has enabled Quake yet.
    auto syncQuake = [this, portalStatus]() {
        const bool on = m_quakeMode->isChecked();
        m_quakeHotkey->setEnabled(on);
        portalStatus->setEnabled(on);
    };
    connect(m_quakeMode, &QCheckBox::toggled, this, syncQuake);
    syncQuake();

    auto *terminalDefaultsBtn = new QPushButton(
        "Restore Defaults (Terminal tab)", tab);
    terminalDefaultsBtn->setObjectName(QStringLiteral("restoreDefaultsTerminal"));
    connect(terminalDefaultsBtn, &QPushButton::clicked, this, [this]() {
        m_scrollbackLines->setValue(50000);
        m_showCommandMarks->setChecked(true);
        m_quakeMode->setChecked(false);
        m_quakeHotkey->setText(QStringLiteral("F12"));
    });
    layout->addRow(QString(), terminalDefaultsBtn);
}

void SettingsDialog::setupAiTab(QWidget *tab) {
    auto *layout = new QFormLayout(tab);

    m_aiEnabled = new QCheckBox("Enable AI features", tab);
    layout->addRow(m_aiEnabled);

    m_aiEndpoint = new QLineEdit(tab);
    m_aiEndpoint->setPlaceholderText("http://localhost:11434/v1/chat/completions");
    layout->addRow("API Endpoint:", m_aiEndpoint);

    m_aiApiKey = new QLineEdit(tab);
    m_aiApiKey->setEchoMode(QLineEdit::Password);
    m_aiApiKey->setPlaceholderText("API key (optional for local models)");
    layout->addRow("API Key:", m_aiApiKey);

    m_aiModel = new QLineEdit(tab);
    m_aiModel->setPlaceholderText("llama3, gpt-4, etc.");
    layout->addRow("Model:", m_aiModel);

    m_aiContextLines = new QSpinBox(tab);
    m_aiContextLines->setRange(10, 500);
    layout->addRow("Context Lines:", m_aiContextLines);

    // Dependency-UI gating: every AI field below the master
    // checkbox is unreachable when AI is off. Disabling them
    // surfaces that intent visually instead of letting the user
    // type an endpoint or paste a key into a feature-disabled
    // dialog.
    auto syncAi = [this]() {
        const bool on = m_aiEnabled->isChecked();
        m_aiEndpoint->setEnabled(on);
        m_aiApiKey->setEnabled(on);
        m_aiModel->setEnabled(on);
        m_aiContextLines->setEnabled(on);
    };
    connect(m_aiEnabled, &QCheckBox::toggled, this, syncAi);
    syncAi();

    auto *aiDefaultsBtn = new QPushButton("Restore Defaults (AI tab)", tab);
    aiDefaultsBtn->setObjectName(QStringLiteral("restoreDefaultsAi"));
    connect(aiDefaultsBtn, &QPushButton::clicked, this, [this]() {
        m_aiEnabled->setChecked(false);
        m_aiEndpoint->clear();
        m_aiApiKey->clear();
        m_aiModel->setText(QStringLiteral("llama3"));
        m_aiContextLines->setValue(50);
    });
    layout->addRow(QString(), aiDefaultsBtn);
}

void SettingsDialog::setupHighlightsTab(QWidget *tab) {
    auto *layout = new QVBoxLayout(tab);

    auto *desc = new QLabel("Define regex patterns to always highlight in terminal output.", tab);
    desc->setWordWrap(true);
    layout->addWidget(desc);

    m_highlightTable = new QTableWidget(0, 4, tab);
    m_highlightTable->setHorizontalHeaderLabels({"Pattern", "FG Color", "BG Color", "Enabled"});
    m_highlightTable->horizontalHeader()->setStretchLastSection(true);
    m_highlightTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_highlightTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(m_highlightTable, 1);

    auto *btnLayout = new QHBoxLayout();
    auto *addBtn = new QPushButton("Add Rule", tab);
    connect(addBtn, &QPushButton::clicked, this, [this]() { addHighlightRow(); });
    btnLayout->addWidget(addBtn);

    auto *removeBtn = new QPushButton("Remove Selected", tab);
    connect(removeBtn, &QPushButton::clicked, this, [this]() {
        int row = m_highlightTable->currentRow();
        if (row >= 0) m_highlightTable->removeRow(row);
    });
    btnLayout->addWidget(removeBtn);
    btnLayout->addStretch();
    layout->addLayout(btnLayout);
}

void SettingsDialog::setupTriggersTab(QWidget *tab) {
    auto *layout = new QVBoxLayout(tab);

    auto *desc = new QLabel(
        "Triggers run actions when a regex pattern matches terminal output.\n"
        "<b>Dispatch:</b> notify (desktop), sound (bell), command (shell cmd), "
        "bell (visual+audible), inject (write to PTY), run_script (plugin event).\n"
        "<b>Grid mutation:</b> highlight_line (Action Value = <code>#fg</code> or "
        "<code>#fg/#bg</code>) recolors the whole line; highlight_text same value "
        "format but only the matched substring; make_hyperlink (Action Value = "
        "URL template with <code>$0..$9</code> backrefs) turns the match into a "
        "clickable OSC 8-equivalent link.", tab);
    desc->setWordWrap(true);
    desc->setTextFormat(Qt::RichText);
    layout->addWidget(desc);

    m_triggerTable = new QTableWidget(0, 4, tab);
    m_triggerTable->setHorizontalHeaderLabels({"Pattern", "Action Type", "Action Value", "Enabled"});
    m_triggerTable->horizontalHeader()->setStretchLastSection(true);
    m_triggerTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_triggerTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(m_triggerTable, 1);

    auto *btnLayout = new QHBoxLayout();
    auto *addBtn = new QPushButton("Add Trigger", tab);
    connect(addBtn, &QPushButton::clicked, this, [this]() { addTriggerRow(); });
    btnLayout->addWidget(addBtn);

    auto *removeBtn = new QPushButton("Remove Selected", tab);
    connect(removeBtn, &QPushButton::clicked, this, [this]() {
        int row = m_triggerTable->currentRow();
        if (row >= 0) m_triggerTable->removeRow(row);
    });
    btnLayout->addWidget(removeBtn);
    btnLayout->addStretch();
    layout->addLayout(btnLayout);
}

void SettingsDialog::setupKeybindingsTab(QWidget *tab) {
    auto *layout = new QVBoxLayout(tab);

    m_keybindingTable = new QTableWidget(0, 2, tab);
    m_keybindingTable->setHorizontalHeaderLabels({"Action", "Shortcut"});
    m_keybindingTable->horizontalHeader()->setStretchLastSection(true);
    m_keybindingTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_keybindingTable, 1);

    auto *resetBtn = new QPushButton("Reset to Defaults", tab);
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        // Reload defaults
        static const QVector<QPair<QString, QString>> defaults = {
            {"new_tab", "Ctrl+Shift+T"}, {"close_tab", "Ctrl+Shift+W"},
            {"new_window", "Ctrl+Shift+N"}, {"ssh_manager", "Ctrl+Shift+S"},
            {"exit", "Ctrl+Shift+Q"}, {"clear_line", "Ctrl+Shift+U"},
            {"command_palette", "Ctrl+Shift+P"}, {"split_horizontal", "Ctrl+Shift+E"},
            {"split_vertical", "Ctrl+Shift+O"}, {"close_pane", "Ctrl+Shift+X"},
            {"ai_assistant", "Ctrl+Shift+A"}, {"claude_allowlist", "Ctrl+Shift+L"},
            {"claude_projects", "Ctrl+Shift+J"}, {"record_session", "Ctrl+Shift+R"},
            {"toggle_bookmark", "Ctrl+Shift+B"}, {"next_bookmark", "Ctrl+Shift+Down"},
            {"prev_bookmark", "Ctrl+Shift+Up"}, {"url_quick_select", "Ctrl+Shift+G"},
            {"scratchpad", "Ctrl+Shift+Return"}, {"snippets", "Ctrl+Shift+;"},
            {"toggle_fold", "Ctrl+Shift+."},
        };
        m_keybindingTable->setRowCount(0);
        for (auto &[action, key] : defaults) {
            int row = m_keybindingTable->rowCount();
            m_keybindingTable->insertRow(row);
            auto *actionItem = new QTableWidgetItem(action);
            actionItem->setFlags(actionItem->flags() & ~Qt::ItemIsEditable);
            m_keybindingTable->setItem(row, 0, actionItem);
            auto *keyEdit = new QKeySequenceEdit(QKeySequence(key));
            m_keybindingTable->setCellWidget(row, 1, keyEdit);
        }
    });
    layout->addWidget(resetBtn);
}

void SettingsDialog::setupProfilesTab(QWidget *tab) {
    auto *layout = new QVBoxLayout(tab);

    auto *desc = new QLabel(
        "Profiles save collections of settings (theme, font, opacity, etc.) "
        "that you can quickly switch between.", tab);
    desc->setWordWrap(true);
    layout->addWidget(desc);

    auto *selectLayout = new QHBoxLayout();
    m_profileCombo = new QComboBox(tab);
    m_profileCombo->setMinimumWidth(200);
    selectLayout->addWidget(new QLabel("Profile:", tab));
    selectLayout->addWidget(m_profileCombo, 1);
    m_profileLoad = new QPushButton("Load", tab);
    selectLayout->addWidget(m_profileLoad);
    layout->addLayout(selectLayout);

    layout->addSpacing(10);

    auto *nameLayout = new QHBoxLayout();
    nameLayout->addWidget(new QLabel("Name:", tab));
    m_profileName = new QLineEdit(tab);
    m_profileName->setPlaceholderText("Profile name");
    nameLayout->addWidget(m_profileName, 1);
    layout->addLayout(nameLayout);

    auto *btnLayout = new QHBoxLayout();
    m_profileSave = new QPushButton("Save Current as Profile", tab);
    btnLayout->addWidget(m_profileSave);
    m_profileDelete = new QPushButton("Delete Profile", tab);
    btnLayout->addWidget(m_profileDelete);
    btnLayout->addStretch();
    layout->addLayout(btnLayout);

    layout->addStretch();

    // Save/Delete/Load mutate the pending state, never m_config
    // directly. applySettings() commits the pending state on OK /
    // Apply; reject() leaves m_config untouched so Cancel rolls
    // back the user's profile edits cleanly.
    connect(m_profileSave, &QPushButton::clicked, this, [this]() {
        QString name = m_profileName->text().trimmed();
        if (name.isEmpty()) return;

        QJsonObject p;
        p["theme"] = m_themeCombo->currentText();
        p["font_family"] = m_fontFamily->currentFont().family();
        p["font_size"] = m_fontSize->value();
        p["opacity"] = m_opacitySlider->value() / 100.0;
        p["scrollback_lines"] = m_scrollbackLines->value();
        m_pendingProfiles[name] = p;

        if (m_profileCombo->findText(name) < 0)
            m_profileCombo->addItem(name);
    });

    connect(m_profileDelete, &QPushButton::clicked, this, [this]() {
        QString name = m_profileCombo->currentText();
        if (name.isEmpty()) return;
        m_pendingProfiles.remove(name);
        if (m_pendingActiveProfile == name)
            m_pendingActiveProfile.clear();
        m_profileCombo->removeItem(m_profileCombo->currentIndex());
    });

    connect(m_profileLoad, &QPushButton::clicked, this, [this]() {
        QString name = m_profileCombo->currentText();
        if (name.isEmpty()) return;
        QJsonObject p = m_pendingProfiles.value(name).toObject();
        if (p.isEmpty()) return;

        if (p.contains("theme"))
            m_themeCombo->setCurrentText(p["theme"].toString());
        if (p.contains("font_family"))
            m_fontFamily->setCurrentFont(QFont(p["font_family"].toString()));
        if (p.contains("font_size"))
            m_fontSize->setValue(p["font_size"].toInt());
        if (p.contains("opacity"))
            m_opacitySlider->setValue(static_cast<int>(p["opacity"].toDouble() * 100));
        if (p.contains("scrollback_lines"))
            m_scrollbackLines->setValue(p["scrollback_lines"].toInt());

        m_pendingActiveProfile = name;
    });
}

// -----------------------------------------------------------------------------
// Plugins tab — manifest v2 capability audit UI (0.6.11)
// -----------------------------------------------------------------------------
// Settings → Plugins lists every discovered plugin and, for each, the full
// set of permissions it declared in manifest.json. Each permission is rendered
// as a checkbox whose initial state reflects what's currently granted
// (config.plugin_grants[name]). Unchecking + Apply revokes that capability at
// next plugin reload — same semantics as the first-load permission prompt in
// MainWindow. The tab is always present; when no plugins are loaded (either
// Lua is not compiled in or the user has no plugins installed), we show an
// explanatory label and hide the scroll list.
void SettingsDialog::setupPluginsTab(QWidget *tab) {
    auto *layout = new QVBoxLayout(tab);

    auto *header = new QLabel(
        "Plugin capability audit. Each plugin lists the permissions it "
        "declared in its <code>manifest.json</code>. Uncheck any capability "
        "to revoke it; revocations take effect at the next plugin reload.",
        tab);
    header->setWordWrap(true);
    header->setTextFormat(Qt::RichText);
    layout->addWidget(header);

    m_pluginsEmptyLabel = new QLabel(
        "<i>No plugins discovered. See PLUGINS.md for the authoring guide "
        "and set <code>plugin_dir</code> in config.json to a directory "
        "containing plugin folders.</i>",
        tab);
    m_pluginsEmptyLabel->setWordWrap(true);
    m_pluginsEmptyLabel->setTextFormat(Qt::RichText);
    layout->addWidget(m_pluginsEmptyLabel);

    auto *scroll = new QScrollArea(tab);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_pluginsContainer = new QWidget(scroll);
    auto *containerLayout = new QVBoxLayout(m_pluginsContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->addStretch(1);
    scroll->setWidget(m_pluginsContainer);
    layout->addWidget(scroll, 1);
}

void SettingsDialog::setPlugins(const QList<PluginDisplay> &plugins) {
    m_plugins = plugins;
    populatePluginsTab();
}

void SettingsDialog::populatePluginsTab() {
    if (!m_pluginsContainer) return;

    // Clear existing group boxes + checkbox map. Takes ownership via
    // deleteLater so destroyed widgets don't stale-reference the map.
    m_pluginPermissionChecks.clear();
    auto *layout = qobject_cast<QVBoxLayout *>(m_pluginsContainer->layout());
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget()) w->deleteLater();
        delete item;
    }

    if (m_plugins.isEmpty()) {
        m_pluginsEmptyLabel->setVisible(true);
        layout->addStretch(1);
        return;
    }
    m_pluginsEmptyLabel->setVisible(false);

    // Permission → friendly description. Kept in sync with the first-load
    // permission prompt in mainwindow.cpp so users see the same wording.
    auto describe = [](const QString &p) -> QString {
        if (p == "clipboard.write") return "Write to the system clipboard.";
        if (p == "settings")        return "Store key/value settings under the plugin's name.";
        if (p == "net")             return "Reserved for future use (network access).";
        return QString();
    };

    for (const auto &info : m_plugins) {
        auto *group = new QGroupBox(
            QString("%1 (v%2)%3")
                .arg(info.name, info.version,
                     info.author.isEmpty() ? QString() : QString(" — ") + info.author),
            m_pluginsContainer);
        auto *groupLayout = new QVBoxLayout(group);

        if (!info.description.isEmpty()) {
            auto *desc = new QLabel(info.description, group);
            desc->setWordWrap(true);
            desc->setStyleSheet("color: palette(mid);");
            groupLayout->addWidget(desc);
        }

        if (info.permissions.isEmpty()) {
            auto *none = new QLabel(
                "<i>This plugin declared no permissions.</i>", group);
            none->setTextFormat(Qt::RichText);
            groupLayout->addWidget(none);
        } else {
            QStringList granted = m_config->pluginGrants(info.name);
            for (const QString &perm : info.permissions) {
                auto *cb = new QCheckBox(perm, group);
                cb->setChecked(granted.contains(perm));
                QString tip = describe(perm);
                if (!tip.isEmpty()) cb->setToolTip(tip);
                groupLayout->addWidget(cb);
                m_pluginPermissionChecks[info.name][perm] = cb;
            }
        }

        layout->addWidget(group);
    }
    layout->addStretch(1);
}

void SettingsDialog::addHighlightRow(const QString &pattern, const QString &fg,
                                      const QString &bg, bool enabled) {
    int row = m_highlightTable->rowCount();
    m_highlightTable->insertRow(row);

    m_highlightTable->setItem(row, 0, new QTableWidgetItem(pattern));

    // FG color button
    auto *fgBtn = new QPushButton(fg.isEmpty() ? "Default" : fg);
    if (!fg.isEmpty())
        fgBtn->setStyleSheet(QString("background-color: %1; color: white;").arg(fg));
    connect(fgBtn, &QPushButton::clicked, this, [fgBtn]() {
        QColor c = QColorDialog::getColor(QColor(fgBtn->text()), nullptr, "Foreground Color");
        if (c.isValid()) {
            fgBtn->setText(c.name());
            fgBtn->setStyleSheet(QString("background-color: %1; color: white;").arg(c.name()));
        }
    });
    m_highlightTable->setCellWidget(row, 1, fgBtn);

    // BG color button
    auto *bgBtn = new QPushButton(bg.isEmpty() ? "None" : bg);
    if (!bg.isEmpty())
        bgBtn->setStyleSheet(QString("background-color: %1; color: white;").arg(bg));
    connect(bgBtn, &QPushButton::clicked, this, [bgBtn]() {
        QColor c = QColorDialog::getColor(QColor(bgBtn->text()), nullptr, "Background Color");
        if (c.isValid()) {
            bgBtn->setText(c.name());
            bgBtn->setStyleSheet(QString("background-color: %1; color: white;").arg(c.name()));
        }
    });
    m_highlightTable->setCellWidget(row, 2, bgBtn);

    auto *check = new QCheckBox();
    check->setChecked(enabled);
    m_highlightTable->setCellWidget(row, 3, check);
}

void SettingsDialog::addTriggerRow(const QString &pattern, const QString &actionType,
                                    const QString &actionValue, bool enabled) {
    int row = m_triggerTable->rowCount();
    m_triggerTable->insertRow(row);

    m_triggerTable->setItem(row, 0, new QTableWidgetItem(pattern));

    auto *typeCombo = new QComboBox();
    // Dispatch types (fire a signal, side-effect outside the grid):
    //   notify, sound, command, bell, inject, run_script
    // Grid-mutation types (recolor cells / add hyperlink spans in place):
    //   highlight_line, highlight_text, make_hyperlink
    // The two groups dispatch via different code paths (checkTriggers vs
    // onGridLineCompleted) but share the same rule schema so the UI can
    // list them in one combo.
    typeCombo->addItems({
        "notify", "sound", "command", "bell", "inject", "run_script",
        "highlight_line", "highlight_text", "make_hyperlink"
    });
    typeCombo->setCurrentText(actionType);
    m_triggerTable->setCellWidget(row, 1, typeCombo);

    m_triggerTable->setItem(row, 2, new QTableWidgetItem(actionValue));

    auto *check = new QCheckBox();
    check->setChecked(enabled);
    m_triggerTable->setCellWidget(row, 3, check);
}

void SettingsDialog::loadSettings() {
    // General
    QString shell = m_config->shellCommand();
    if (shell.isEmpty()) {
        m_shellCombo->setCurrentIndex(0);
    } else if (shell == "bash" || shell == "zsh" || shell == "fish") {
        m_shellCombo->setCurrentText(shell);
    } else {
        m_shellCombo->setCurrentText("Custom...");
        m_shellCustom->setText(shell);
        m_shellCustom->setVisible(true);
    }
    m_sessionPersistence->setChecked(m_config->sessionPersistence());
    m_sessionLogging->setChecked(m_config->sessionLogging());
    m_autoCopy->setChecked(m_config->autoCopyOnSelect());
    m_confirmMultilinePaste->setChecked(m_config->confirmMultilinePaste());
    m_editorCmd->setText(m_config->editorCommand());
    m_imagePasteDir->setText(m_config->imagePasteDir());
    if (m_notificationTimeout)
        m_notificationTimeout->setValue(m_config->notificationTimeoutMs() / 1000);
    if (m_claudeTabStatusIndicator)
        m_claudeTabStatusIndicator->setChecked(m_config->claudeTabStatusIndicator());

    int fmtIdx = m_tabTitleFormat->findData(m_config->tabTitleFormat());
    if (fmtIdx >= 0) m_tabTitleFormat->setCurrentIndex(fmtIdx);

    // Appearance
    QString family = m_config->fontFamily();
    if (!family.isEmpty()) m_fontFamily->setCurrentFont(QFont(family));
    m_fontSize->setValue(m_config->fontSize());
    m_themeCombo->setCurrentText(m_config->theme());
    m_opacitySlider->setValue(static_cast<int>(m_config->opacity() * 100));
    m_backgroundBlur->setChecked(m_config->backgroundBlur());
    m_paddingSpinner->setValue(m_config->terminalPadding());
    m_badgeEdit->setText(m_config->badgeText());
    m_autoColorScheme->setChecked(m_config->autoColorScheme());
    m_darkThemeCombo->setCurrentText(m_config->darkTheme());
    m_lightThemeCombo->setCurrentText(m_config->lightTheme());

    // Terminal
    m_scrollbackLines->setValue(m_config->scrollbackLines());
    m_showCommandMarks->setChecked(m_config->showCommandMarks());
    m_quakeMode->setChecked(m_config->quakeMode());
    m_quakeHotkey->setText(m_config->quakeHotkey());

    // AI
    m_aiEnabled->setChecked(m_config->aiEnabled());
    m_aiEndpoint->setText(m_config->aiEndpoint());
    m_aiApiKey->setText(m_config->aiApiKey());
    m_aiModel->setText(m_config->aiModel());
    m_aiContextLines->setValue(m_config->aiContextLines());

    // Highlights
    QJsonArray highlights = m_config->highlightRules();
    for (const QJsonValue &v : highlights) {
        QJsonObject obj = v.toObject();
        addHighlightRow(obj["pattern"].toString(), obj["fg"].toString(),
                        obj["bg"].toString(), obj["enabled"].toBool(true));
    }

    // Triggers
    QJsonArray triggers = m_config->triggerRules();
    for (const QJsonValue &v : triggers) {
        QJsonObject obj = v.toObject();
        addTriggerRow(obj["pattern"].toString(), obj["action_type"].toString("notify"),
                      obj["action_value"].toString(), obj["enabled"].toBool(true));
    }

    // Keybindings
    static const QVector<QPair<QString, QString>> defaults = {
        {"new_tab", "Ctrl+Shift+T"}, {"close_tab", "Ctrl+Shift+W"},
        {"new_window", "Ctrl+Shift+N"}, {"ssh_manager", "Ctrl+Shift+S"},
        {"exit", "Ctrl+Shift+Q"}, {"clear_line", "Ctrl+Shift+U"},
        {"command_palette", "Ctrl+Shift+P"}, {"split_horizontal", "Ctrl+Shift+E"},
        {"split_vertical", "Ctrl+Shift+O"}, {"close_pane", "Ctrl+Shift+X"},
        {"ai_assistant", "Ctrl+Shift+A"}, {"claude_allowlist", "Ctrl+Shift+L"},
        {"claude_projects", "Ctrl+Shift+J"}, {"record_session", "Ctrl+Shift+R"},
        {"toggle_bookmark", "Ctrl+Shift+B"}, {"next_bookmark", "Ctrl+Shift+Down"},
        {"prev_bookmark", "Ctrl+Shift+Up"}, {"url_quick_select", "Ctrl+Shift+G"},
        {"scratchpad", "Ctrl+Shift+Return"}, {"snippets", "Ctrl+Shift+;"},
        {"toggle_fold", "Ctrl+Shift+."},
    };
    for (auto &[action, defaultKey] : defaults) {
        int row = m_keybindingTable->rowCount();
        m_keybindingTable->insertRow(row);
        auto *actionItem = new QTableWidgetItem(action);
        actionItem->setFlags(actionItem->flags() & ~Qt::ItemIsEditable);
        m_keybindingTable->setItem(row, 0, actionItem);
        QString key = m_config->keybinding(action, defaultKey);
        auto *keyEdit = new QKeySequenceEdit(QKeySequence(key));
        m_keybindingTable->setCellWidget(row, 1, keyEdit);
    }

    // Profiles — load into pending state. Save/Delete/Load buttons
    // mutate m_pendingProfiles; applySettings() commits, reject()
    // discards. m_profileCombo is a derived view of m_pendingProfiles
    // (not m_config->profiles()) so an in-dialog "Save" + "Cancel"
    // sequence visibly removes the never-committed profile from the
    // list rather than silently leaving it in m_config.
    m_pendingProfiles = m_config->profiles();
    m_pendingActiveProfile = m_config->activeProfile();
    m_profileCombo->clear();
    for (auto it = m_pendingProfiles.begin(); it != m_pendingProfiles.end(); ++it)
        m_profileCombo->addItem(it.key());
    if (!m_pendingActiveProfile.isEmpty()) {
        const int idx = m_profileCombo->findText(m_pendingActiveProfile);
        if (idx >= 0) m_profileCombo->setCurrentIndex(idx);
    }
}

void SettingsDialog::applySettings() {
    // General
    QString shell;
    if (m_shellCombo->currentIndex() == 0) {
        shell = "";
    } else if (m_shellCombo->currentText() == "Custom...") {
        shell = m_shellCustom->text().trimmed();
    } else {
        shell = m_shellCombo->currentText();
    }
    m_config->setShellCommand(shell);
    m_config->setSessionPersistence(m_sessionPersistence->isChecked());
    m_config->setSessionLogging(m_sessionLogging->isChecked());
    m_config->setAutoCopyOnSelect(m_autoCopy->isChecked());
    m_config->setConfirmMultilinePaste(m_confirmMultilinePaste->isChecked());
    m_config->setEditorCommand(m_editorCmd->text().trimmed());
    m_config->setImagePasteDir(m_imagePasteDir->text().trimmed());
    m_config->setTabTitleFormat(m_tabTitleFormat->currentData().toString());
    if (m_notificationTimeout)
        m_config->setNotificationTimeoutMs(m_notificationTimeout->value() * 1000);
    if (m_claudeTabStatusIndicator)
        m_config->setClaudeTabStatusIndicator(m_claudeTabStatusIndicator->isChecked());

    // Appearance
    m_config->setFontFamily(m_fontFamily->currentFont().family());
    m_config->setFontSize(m_fontSize->value());
    m_config->setTheme(m_themeCombo->currentText());
    m_config->setOpacity(m_opacitySlider->value() / 100.0);
    m_config->setBackgroundBlur(m_backgroundBlur->isChecked());
    m_config->setTerminalPadding(m_paddingSpinner->value());
    m_config->setBadgeText(m_badgeEdit->text().trimmed());
    m_config->setAutoColorScheme(m_autoColorScheme->isChecked());
    m_config->setDarkTheme(m_darkThemeCombo->currentText());
    m_config->setLightTheme(m_lightThemeCombo->currentText());

    // Terminal
    m_config->setScrollbackLines(m_scrollbackLines->value());
    m_config->setShowCommandMarks(m_showCommandMarks->isChecked());
    m_config->setQuakeMode(m_quakeMode->isChecked());
    m_config->setQuakeHotkey(m_quakeHotkey->text().trimmed());

    // AI
    m_config->setAiEnabled(m_aiEnabled->isChecked());
    m_config->setAiEndpoint(m_aiEndpoint->text().trimmed());
    m_config->setAiApiKey(m_aiApiKey->text().trimmed());
    m_config->setAiModel(m_aiModel->text().trimmed());
    m_config->setAiContextLines(m_aiContextLines->value());

    // Highlights
    QJsonArray highlights;
    for (int row = 0; row < m_highlightTable->rowCount(); ++row) {
        QJsonObject obj;
        auto *item = m_highlightTable->item(row, 0);
        obj["pattern"] = item ? item->text() : "";
        auto *fgBtn = qobject_cast<QPushButton *>(m_highlightTable->cellWidget(row, 1));
        obj["fg"] = (fgBtn && fgBtn->text() != "Default") ? fgBtn->text() : "";
        auto *bgBtn = qobject_cast<QPushButton *>(m_highlightTable->cellWidget(row, 2));
        obj["bg"] = (bgBtn && bgBtn->text() != "None") ? bgBtn->text() : "";
        auto *check = qobject_cast<QCheckBox *>(m_highlightTable->cellWidget(row, 3));
        obj["enabled"] = check ? check->isChecked() : true;
        highlights.append(obj);
    }
    m_config->setHighlightRules(highlights);

    // Triggers
    QJsonArray triggers;
    for (int row = 0; row < m_triggerTable->rowCount(); ++row) {
        QJsonObject obj;
        auto *item = m_triggerTable->item(row, 0);
        obj["pattern"] = item ? item->text() : "";
        auto *typeCombo = qobject_cast<QComboBox *>(m_triggerTable->cellWidget(row, 1));
        obj["action_type"] = typeCombo ? typeCombo->currentText() : "notify";
        auto *valItem = m_triggerTable->item(row, 2);
        obj["action_value"] = valItem ? valItem->text() : "";
        auto *check = qobject_cast<QCheckBox *>(m_triggerTable->cellWidget(row, 3));
        obj["enabled"] = check ? check->isChecked() : true;
        triggers.append(obj);
    }
    m_config->setTriggerRules(triggers);

    // Keybindings
    for (int row = 0; row < m_keybindingTable->rowCount(); ++row) {
        auto *actionItem = m_keybindingTable->item(row, 0);
        auto *keyEdit = qobject_cast<QKeySequenceEdit *>(m_keybindingTable->cellWidget(row, 1));
        if (actionItem && keyEdit) {
            m_config->setKeybinding(actionItem->text(), keyEdit->keySequence().toString());
        }
    }

    // Profiles — commit the pending state. Save/Delete/Load slots
    // mutated m_pendingProfiles + m_pendingActiveProfile only; this
    // is the single point where they reach m_config. Cancel skips
    // applySettings entirely so the pending edits are discarded with
    // the dialog.
    m_config->setProfiles(m_pendingProfiles);
    m_config->setActiveProfile(m_pendingActiveProfile);

    // Plugins — collect checked permissions per plugin and persist. We write
    // for every plugin we know about (including ones with zero checked boxes
    // so that revocation-to-none is persisted as an empty array rather than
    // leaving the old grant list in place).
    for (auto pit = m_pluginPermissionChecks.constBegin();
         pit != m_pluginPermissionChecks.constEnd(); ++pit) {
        QStringList granted;
        for (auto cit = pit.value().constBegin(); cit != pit.value().constEnd(); ++cit) {
            if (cit.value() && cit.value()->isChecked()) granted << cit.key();
        }
        m_config->setPluginGrants(pit.key(), granted);
    }

    emit settingsChanged();
}

// --- Claude Code hook installer ---
//
// Two-file operation (idempotent — running it twice does no extra work):
//
//   1. Writes a tiny shell script at
//      ~/.config/ants-terminal/hooks/claude-forward.sh
//      The script reads a Claude-Code hook-event JSON from stdin, walks
//      up the process tree to find the nearest `ants-terminal`, and
//      forwards the JSON to that instance's Unix socket at
//      /tmp/ants-claude-hooks-<pid>. If no ants-terminal is found (user
//      ran `claude` outside ants), the script silently exits 0.
//
//   2. Merges five hook entries into the user's existing
//      ~/.claude/settings.json under `"hooks"` — SessionStart,
//      PreToolUse, PostToolUse, Stop, PreCompact — each pointing at the
//      script. UserPromptSubmit and other hooks the user already has are
//      preserved.
//
// The status-bar update path then becomes: Claude emits event → hook
// fires → script forwards → QLocalServer in ants-terminal fires
// processHookEvent → applyClaudeStatusLabel() re-renders within one
// event loop iteration. Replaces the 50 ms-debounced transcript-file
// watcher for users who opt in.
static QString hookScriptPath() {
    return ConfigPaths::antsClaudeForwardScript();
}

static QString claudeSettingsPath() {
    return ConfigPaths::claudeSettingsJson();
}

void SettingsDialog::refreshClaudeHooksStatus() {
    if (!m_claudeHooksStatus) return;
    const bool scriptPresent = QFile::exists(hookScriptPath());
    bool hooksWired = false;
    QFile sf(claudeSettingsPath());
    if (sf.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(sf.readAll());
        sf.close();
        QJsonObject hooks = doc.object().value("hooks").toObject();
        hooksWired = hooks.contains(QStringLiteral("PreToolUse")) &&
                     hooks.contains(QStringLiteral("PostToolUse")) &&
                     hooks.contains(QStringLiteral("Stop"));
    }
    if (scriptPresent && hooksWired) {
        m_claudeHooksStatus->setText(
            QStringLiteral("✓ Hooks installed. Status updates in real time."));
    } else if (scriptPresent) {
        m_claudeHooksStatus->setText(
            QStringLiteral("Helper script present but ~/.claude/settings.json "
                          "is missing some hook entries."));
    } else {
        m_claudeHooksStatus->setText(
            QStringLiteral("Not installed. Status updates fall back to the "
                          "~50 ms transcript-file watcher."));
    }
}

void SettingsDialog::installClaudeHooks() {
    const QString scriptPath = hookScriptPath();
    const QString scriptDir = QFileInfo(scriptPath).absolutePath();
    if (!QDir().mkpath(scriptDir)) {
        QMessageBox::warning(this, "Install hooks",
            QString("Could not create %1").arg(scriptDir));
        return;
    }

    // Helper script. Uses Python3 for the socket send because every
    // Linux desktop that runs Claude Code also has Python3 in the base
    // install; socat would work but isn't universally present. Timeouts
    // prevent a long-running hook from blocking Claude Code's turn.
    const QString script = QStringLiteral(
        "#!/bin/bash\n"
        "# Ants Terminal — Claude Code hook forwarder.\n"
        "# Walks up the process tree to find the parent ants-terminal\n"
        "# and forwards the hook event JSON on stdin to its Unix socket.\n"
        "pid=$PPID\n"
        "for _ in $(seq 1 20); do\n"
        "    comm=$(cat /proc/$pid/comm 2>/dev/null) || break\n"
        "    if [[ \"$comm\" == \"ants-terminal\" ]]; then\n"
        "        sock=\"/tmp/ants-claude-hooks-$pid\"\n"
        "        if [[ -S \"$sock\" ]]; then\n"
        "            timeout 1 python3 -c \"\n"
        "import socket, sys\n"
        "s = socket.socket(socket.AF_UNIX)\n"
        "s.settimeout(1.0)\n"
        "s.connect('$sock')\n"
        "s.sendall(sys.stdin.buffer.read())\n"
        "s.shutdown(socket.SHUT_WR)\n"
        "s.close()\n"
        "\" 2>/dev/null\n"
        "            exit 0\n"
        "        fi\n"
        "    fi\n"
        "    ppid=$(awk '{print $4}' /proc/$pid/stat 2>/dev/null) || break\n"
        "    [[ -z \"$ppid\" || \"$ppid\" -le 1 ]] && break\n"
        "    pid=$ppid\n"
        "done\n"
        "exit 0\n");

    QSaveFile sf(scriptPath);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Install hooks",
            QString("Could not write %1").arg(scriptPath));
        return;
    }
    sf.write(script.toUtf8());
    if (!sf.commit()) {
        QMessageBox::warning(this, "Install hooks",
            QString("Write failed for %1").arg(scriptPath));
        return;
    }
    QFile::setPermissions(scriptPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ExeOwner | QFileDevice::ReadGroup |
        QFileDevice::ReadOther | QFileDevice::ExeGroup |
        QFileDevice::ExeOther);

    // Merge hook entries into ~/.claude/settings.json, preserving any
    // existing hooks the user has configured (UserPromptSubmit, per-
    // project hooks, etc.). If the settings file is missing, we start
    // from an empty object. If it EXISTS but fails to parse, REFUSE to
    // proceed — otherwise we'd write back a file containing only
    // `{"hooks": {...}}`, silently destroying every non-hooks key
    // (model, env, permissions from ClaudeAllowlistDialog, etc.).
    // 0.7.12 /indie-review cross-cutting fix — same silent-data-loss
    // shape Config and ClaudeAllowlist were hardened against; same
    // file as ClaudeAllowlist too.
    const QString settingsPath = claudeSettingsPath();
    QDir().mkpath(QFileInfo(settingsPath).absolutePath());
    QJsonObject root;
    QFile rf(settingsPath);
    if (rf.exists()) {
        if (!rf.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, "Install hooks",
                QString("Could not read %1 — refusing to overwrite. "
                        "Check file permissions and retry.")
                    .arg(settingsPath));
            return;
        }
        const QByteArray raw = rf.readAll();
        rf.close();
        QJsonParseError err{};
        QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (doc.isObject()) {
            root = doc.object();
        } else {
            const QString backup = rotateCorruptFileAside(settingsPath);
            QMessageBox::warning(this, "Install hooks",
                QString("%1 failed to parse (%2). Refusing to overwrite "
                        "and risk clobbering non-hook keys.\n\n"
                        "A backup of the broken file was written to:\n%3\n\n"
                        "Hand-fix the file and retry.")
                    .arg(settingsPath)
                    .arg(err.errorString())
                    .arg(backup.isEmpty() ? "(backup copy FAILED)" : backup));
            return;
        }
    }
    QJsonObject hooks = root.value("hooks").toObject();
    auto makeEntry = [&scriptPath]() {
        QJsonObject hook;
        hook["type"] = "command";
        hook["command"] = scriptPath;
        hook["timeout"] = 2;
        QJsonArray hookArr; hookArr.append(hook);
        QJsonObject wrapper; wrapper["hooks"] = hookArr;
        QJsonArray outer; outer.append(wrapper);
        return outer;
    };
    for (const QString &event : QStringList{"SessionStart", "PreToolUse",
                                             "PostToolUse", "Stop",
                                             "PreCompact"}) {
        // Only overwrite if our script isn't already referenced — keeps
        // user-added custom hooks on the same event intact.
        QJsonArray existing = hooks.value(event).toArray();
        bool ourHookPresent = false;
        for (const QJsonValue &v : existing) {
            QJsonArray inner = v.toObject().value("hooks").toArray();
            for (const QJsonValue &h : inner) {
                if (h.toObject().value("command").toString() == scriptPath) {
                    ourHookPresent = true;
                    break;
                }
            }
            if (ourHookPresent) break;
        }
        if (ourHookPresent) continue;
        existing.append(makeEntry().first().toObject());
        hooks[event] = existing;
    }
    root["hooks"] = hooks;

    // Serialize against concurrent writers (Ants Allowlist, another Ants
    // Install-hooks invocation, jq -i piping by the user). Last-rename-
    // wins on QSaveFile commit() would otherwise drop a sibling writer's
    // permissions block.
    ConfigWriteLock writeLock(settingsPath);
    if (!writeLock.acquired()) {
        QMessageBox::warning(this, "Install hooks",
            QString("Another writer holds the lock on %1 — try again in a moment.")
                .arg(settingsPath));
        return;
    }

    QSaveFile settingsOut(settingsPath);
    if (!settingsOut.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Install hooks",
            QString("Could not write %1").arg(settingsPath));
        return;
    }
    setOwnerOnlyPerms(settingsOut);  // 0600 on temp fd
    settingsOut.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!settingsOut.commit()) {
        QMessageBox::warning(this, "Install hooks",
            QString("Commit failed for %1").arg(settingsPath));
        return;
    }
    setOwnerOnlyPerms(settingsPath);  // belt-and-suspenders post-rename

    refreshClaudeHooksStatus();
    QMessageBox::information(this, "Install hooks",
        QStringLiteral("Installed Ants Terminal status-bar hooks.\n\n"
                      "Script: %1\n"
                      "Settings: %2\n\n"
                      "Real-time Claude status updates will take effect for new Claude Code sessions.")
            .arg(scriptPath, settingsPath));
}

// --- Claude Code UserPromptSubmit git-context hook ---
//
// Writes a small shell script that prints a <git-context> block on
// stdout, and merges one entry into ~/.claude/settings.json's
// hooks.UserPromptSubmit array. The script runs in the session's cwd
// (CLAUDE_PROJECT_DIR if set, else $PWD) and no-ops silently outside
// a git work tree. See tests/features/claude_git_context_hook/spec.md
// for the contract.

static QString gitContextScriptPath() {
    return ConfigPaths::antsClaudeGitContextScript();
}

void SettingsDialog::refreshClaudeGitContextStatus() {
    if (!m_claudeGitContextStatus) return;
    const bool scriptPresent = QFile::exists(gitContextScriptPath());
    bool hookWired = false;
    QFile sf(claudeSettingsPath());
    if (sf.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(sf.readAll());
        sf.close();
        const QJsonArray entries = doc.object().value("hooks").toObject()
                                      .value("UserPromptSubmit").toArray();
        const QString needle = gitContextScriptPath();
        for (const QJsonValue &v : entries) {
            const QJsonArray inner = v.toObject().value("hooks").toArray();
            for (const QJsonValue &h : inner) {
                if (h.toObject().value("command").toString() == needle) {
                    hookWired = true;
                    break;
                }
            }
            if (hookWired) break;
        }
    }
    if (scriptPresent && hookWired) {
        m_claudeGitContextStatus->setText(
            QStringLiteral("✓ Installed globally. Every Claude Code prompt "
                          "carries a <git-context> block."));
    } else if (scriptPresent) {
        m_claudeGitContextStatus->setText(
            QStringLiteral("Helper script present but the UserPromptSubmit "
                          "entry is missing from ~/.claude/settings.json."));
    } else {
        m_claudeGitContextStatus->setText(
            QStringLiteral("Not installed. Claude Code will run `git status` "
                          "via Bash when it needs repo state (~500 tokens "
                          "per turn)."));
    }
}

void SettingsDialog::installClaudeGitContextHook() {
    const QString scriptPath = gitContextScriptPath();
    const QString scriptDir = QFileInfo(scriptPath).absolutePath();
    if (!QDir().mkpath(scriptDir)) {
        QMessageBox::warning(this, "Install git-context hook",
            QString("Could not create %1").arg(scriptDir));
        return;
    }

    // The script runs on every Claude Code UserPromptSubmit. Must be
    // fast (<100 ms typical) and silent-no-op outside a git repo so we
    // don't pollute every prompt in non-repo projects with chatter.
    //
    // Contract: tests/features/claude_git_context_hook/spec.md §3.
    const QString script = QStringLiteral(
        "#!/bin/bash\n"
        "# Ants Terminal — Claude Code UserPromptSubmit git-context hook.\n"
        "# Prints a <git-context> block so Claude sees repo state without\n"
        "# spending tokens on `git status`. Silent no-op outside a repo.\n"
        "set -u\n"
        "cwd=\"${CLAUDE_PROJECT_DIR:-$PWD}\"\n"
        "cd \"$cwd\" 2>/dev/null || exit 0\n"
        "command -v git >/dev/null 2>&1 || exit 0\n"
        "git rev-parse --is-inside-work-tree >/dev/null 2>&1 || exit 0\n"
        "\n"
        "branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null)\n"
        "if [[ \"$branch\" == \"HEAD\" ]]; then\n"
        "    branch=$(git rev-parse --short=7 HEAD 2>/dev/null)\n"
        "fi\n"
        "[[ -z \"$branch\" ]] && exit 0\n"
        "\n"
        "upstream=$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)\n"
        "ahead_behind=\"\"\n"
        "if [[ -n \"$upstream\" ]]; then\n"
        "    counts=$(git rev-list --left-right --count \"@{u}...HEAD\" 2>/dev/null || true)\n"
        "    if [[ -n \"$counts\" ]]; then\n"
        "        behind=$(awk '{print $1}' <<< \"$counts\")\n"
        "        ahead=$(awk '{print $2}' <<< \"$counts\")\n"
        "        ahead_behind=\" (ahead $ahead, behind $behind)\"\n"
        "    fi\n"
        "fi\n"
        "\n"
        "porcelain=$(git status --porcelain 2>/dev/null || true)\n"
        "staged=0; unstaged=0; untracked=0\n"
        "if [[ -n \"$porcelain\" ]]; then\n"
        "    while IFS= read -r line; do\n"
        "        x=\"${line:0:1}\"\n"
        "        y=\"${line:1:1}\"\n"
        "        if [[ \"$x\" == \"?\" && \"$y\" == \"?\" ]]; then\n"
        "            untracked=$((untracked + 1))\n"
        "            continue\n"
        "        fi\n"
        "        case \"$x\" in [MADRC]) staged=$((staged + 1));; esac\n"
        "        case \"$y\" in [MDARC]) unstaged=$((unstaged + 1));; esac\n"
        "    done <<< \"$porcelain\"\n"
        "fi\n"
        "\n"
        "printf '<git-context>\\n'\n"
        "printf 'Branch: %s%s\\n' \"$branch\" \"$ahead_behind\"\n"
        "printf 'Upstream: %s\\n' \"${upstream:-(none)}\"\n"
        "printf 'Staged: %s file(s)\\n' \"$staged\"\n"
        "printf 'Unstaged: %s file(s)\\n' \"$unstaged\"\n"
        "printf 'Untracked: %s file(s)\\n' \"$untracked\"\n"
        "printf '</git-context>\\n'\n");

    QSaveFile sf(scriptPath);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Install git-context hook",
            QString("Could not write %1").arg(scriptPath));
        return;
    }
    sf.write(script.toUtf8());
    if (!sf.commit()) {
        QMessageBox::warning(this, "Install git-context hook",
            QString("Write failed for %1").arg(scriptPath));
        return;
    }
    QFile::setPermissions(scriptPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ExeOwner | QFileDevice::ReadGroup |
        QFileDevice::ReadOther | QFileDevice::ExeGroup |
        QFileDevice::ExeOther);

    // Merge the UserPromptSubmit entry into ~/.claude/settings.json.
    // Same parse-error-refuse pattern as installClaudeHooks() — we
    // never clobber a corrupt settings file.
    const QString settingsPath = claudeSettingsPath();
    QDir().mkpath(QFileInfo(settingsPath).absolutePath());
    QJsonObject root;
    QFile rf(settingsPath);
    if (rf.exists()) {
        if (!rf.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, "Install git-context hook",
                QString("Could not read %1 — refusing to overwrite. "
                        "Check file permissions and retry.")
                    .arg(settingsPath));
            return;
        }
        const QByteArray raw = rf.readAll();
        rf.close();
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (doc.isObject()) {
            root = doc.object();
        } else {
            const QString backup = rotateCorruptFileAside(settingsPath);
            QMessageBox::warning(this, "Install git-context hook",
                QString("%1 failed to parse (%2). Refusing to overwrite "
                        "and risk clobbering non-hook keys.\n\n"
                        "A backup of the broken file was written to:\n%3\n\n"
                        "Hand-fix the file and retry.")
                    .arg(settingsPath)
                    .arg(err.errorString())
                    .arg(backup.isEmpty() ? "(backup copy FAILED)" : backup));
            return;
        }
    }
    QJsonObject hooks = root.value("hooks").toObject();
    QJsonArray existing = hooks.value("UserPromptSubmit").toArray();

    // Only append if our script isn't already referenced — keeps
    // user-added custom UserPromptSubmit hooks (ripgrep cheat-sheet,
    // TODO injector, team-policy reminder, etc.) intact alongside ours.
    bool ourHookPresent = false;
    for (const QJsonValue &v : existing) {
        const QJsonArray inner = v.toObject().value("hooks").toArray();
        for (const QJsonValue &h : inner) {
            if (h.toObject().value("command").toString() == scriptPath) {
                ourHookPresent = true;
                break;
            }
        }
        if (ourHookPresent) break;
    }
    if (!ourHookPresent) {
        QJsonObject hook;
        hook["type"] = "command";
        hook["command"] = scriptPath;
        hook["timeout"] = 2;
        QJsonArray hookArr; hookArr.append(hook);
        QJsonObject wrapper;
        wrapper["matcher"] = "";
        wrapper["hooks"] = hookArr;
        existing.append(wrapper);
        hooks["UserPromptSubmit"] = existing;
        root["hooks"] = hooks;

        ConfigWriteLock writeLock(settingsPath);
        if (!writeLock.acquired()) {
            QMessageBox::warning(this, "Install git-context hook",
                QString("Another writer holds the lock on %1 — try again in a moment.")
                    .arg(settingsPath));
            return;
        }

        QSaveFile settingsOut(settingsPath);
        if (!settingsOut.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, "Install git-context hook",
                QString("Could not write %1").arg(settingsPath));
            return;
        }
        setOwnerOnlyPerms(settingsOut);
        settingsOut.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        if (!settingsOut.commit()) {
            QMessageBox::warning(this, "Install git-context hook",
                QString("Commit failed for %1").arg(settingsPath));
            return;
        }
        setOwnerOnlyPerms(settingsPath);
    }

    refreshClaudeGitContextStatus();
    QMessageBox::information(this, "Install git-context hook",
        QStringLiteral("Installed globally (applies to every project).\n\n"
                      "Script: %1\n"
                      "Settings: %2\n\n"
                      "Effective for new Claude Code sessions. Claude Code "
                      "will see a <git-context> block on every user prompt.")
            .arg(scriptPath, settingsPath));
}
