#include "settingsdialog.h"
#include "config.h"
#include "themes.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QColorDialog>
#include <QJsonArray>
#include <QJsonObject>

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

    m_editorCmd = new QLineEdit(tab);
    m_editorCmd->setPlaceholderText("code, vim, nano, etc.");
    layout->addRow("Editor Command:", m_editorCmd);

    m_imagePasteDir = new QLineEdit(tab);
    m_imagePasteDir->setPlaceholderText("~/Pictures/ClaudePaste");
    layout->addRow("Image Paste Dir:", m_imagePasteDir);
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

    m_gpuRendering = new QCheckBox("GPU rendering (glyph atlas + GLSL shaders)", tab);
    layout->addRow(m_gpuRendering);

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
}

void SettingsDialog::setupTerminalTab(QWidget *tab) {
    auto *layout = new QFormLayout(tab);

    m_scrollbackLines = new QSpinBox(tab);
    m_scrollbackLines->setRange(1000, 1000000);
    m_scrollbackLines->setSingleStep(10000);
    layout->addRow("Scrollback Lines:", m_scrollbackLines);

    auto *quakeGroup = new QGroupBox("Dropdown/Quake Mode", tab);
    auto *quakeLayout = new QFormLayout(quakeGroup);
    m_quakeMode = new QCheckBox("Enable dropdown mode", quakeGroup);
    quakeLayout->addRow(m_quakeMode);
    m_quakeHotkey = new QLineEdit(quakeGroup);
    m_quakeHotkey->setPlaceholderText("F12");
    quakeLayout->addRow("Global Hotkey:", m_quakeHotkey);
    layout->addRow(quakeGroup);
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
        "Action types: notify (desktop notification), sound (system bell), command (run shell command).", tab);
    desc->setWordWrap(true);
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

    connect(m_profileSave, &QPushButton::clicked, this, [this]() {
        QString name = m_profileName->text().trimmed();
        if (name.isEmpty()) return;

        QJsonObject profiles = m_config->profiles();
        QJsonObject p;
        p["theme"] = m_themeCombo->currentText();
        p["font_family"] = m_fontFamily->currentFont().family();
        p["font_size"] = m_fontSize->value();
        p["opacity"] = m_opacitySlider->value() / 100.0;
        p["scrollback_lines"] = m_scrollbackLines->value();
        p["gpu_rendering"] = m_gpuRendering->isChecked();
        profiles[name] = p;
        m_config->setProfiles(profiles);

        // Update combo
        if (m_profileCombo->findText(name) < 0)
            m_profileCombo->addItem(name);
    });

    connect(m_profileDelete, &QPushButton::clicked, this, [this]() {
        QString name = m_profileCombo->currentText();
        if (name.isEmpty()) return;
        QJsonObject profiles = m_config->profiles();
        profiles.remove(name);
        m_config->setProfiles(profiles);
        m_profileCombo->removeItem(m_profileCombo->currentIndex());
    });

    connect(m_profileLoad, &QPushButton::clicked, this, [this]() {
        QString name = m_profileCombo->currentText();
        if (name.isEmpty()) return;
        QJsonObject profiles = m_config->profiles();
        QJsonObject p = profiles.value(name).toObject();
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
        if (p.contains("gpu_rendering"))
            m_gpuRendering->setChecked(p["gpu_rendering"].toBool());

        m_config->setActiveProfile(name);
    });
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
    typeCombo->addItems({"notify", "sound", "command"});
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
    m_editorCmd->setText(m_config->editorCommand());
    m_imagePasteDir->setText(m_config->imagePasteDir());

    int fmtIdx = m_tabTitleFormat->findData(m_config->tabTitleFormat());
    if (fmtIdx >= 0) m_tabTitleFormat->setCurrentIndex(fmtIdx);

    // Appearance
    QString family = m_config->fontFamily();
    if (!family.isEmpty()) m_fontFamily->setCurrentFont(QFont(family));
    m_fontSize->setValue(m_config->fontSize());
    m_themeCombo->setCurrentText(m_config->theme());
    m_opacitySlider->setValue(static_cast<int>(m_config->opacity() * 100));
    m_backgroundBlur->setChecked(m_config->backgroundBlur());
    m_gpuRendering->setChecked(m_config->gpuRendering());
    m_paddingSpinner->setValue(m_config->terminalPadding());
    m_badgeEdit->setText(m_config->badgeText());
    m_autoColorScheme->setChecked(m_config->autoColorScheme());
    m_darkThemeCombo->setCurrentText(m_config->darkTheme());
    m_lightThemeCombo->setCurrentText(m_config->lightTheme());

    // Terminal
    m_scrollbackLines->setValue(m_config->scrollbackLines());
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

    // Profiles
    QJsonObject profiles = m_config->profiles();
    for (auto it = profiles.begin(); it != profiles.end(); ++it)
        m_profileCombo->addItem(it.key());
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
    m_config->setEditorCommand(m_editorCmd->text().trimmed());
    m_config->setImagePasteDir(m_imagePasteDir->text().trimmed());
    m_config->setTabTitleFormat(m_tabTitleFormat->currentData().toString());

    // Appearance
    m_config->setFontFamily(m_fontFamily->currentFont().family());
    m_config->setFontSize(m_fontSize->value());
    m_config->setTheme(m_themeCombo->currentText());
    m_config->setOpacity(m_opacitySlider->value() / 100.0);
    m_config->setBackgroundBlur(m_backgroundBlur->isChecked());
    m_config->setGpuRendering(m_gpuRendering->isChecked());
    m_config->setTerminalPadding(m_paddingSpinner->value());
    m_config->setBadgeText(m_badgeEdit->text().trimmed());
    m_config->setAutoColorScheme(m_autoColorScheme->isChecked());
    m_config->setDarkTheme(m_darkThemeCombo->currentText());
    m_config->setLightTheme(m_lightThemeCombo->currentText());

    // Terminal
    m_config->setScrollbackLines(m_scrollbackLines->value());
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

    emit settingsChanged();
}
