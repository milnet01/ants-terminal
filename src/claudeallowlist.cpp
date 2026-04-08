#include "claudeallowlist.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QMessageBox>

ClaudeAllowlistDialog::ClaudeAllowlistDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Claude Code - Edit Allowlist");
    setMinimumSize(550, 400);
    resize(650, 500);

    auto *mainLayout = new QVBoxLayout(this);

    // Info label
    auto *infoLabel = new QLabel(
        "Manage permission rules for Claude Code. "
        "Rules are saved to <code>.claude/settings.local.json</code> in the shell's working directory.",
        this);
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("color: gray; font-size: 11px; margin-bottom: 4px;");
    mainLayout->addWidget(infoLabel);

    // Tabs for Allow / Deny / Ask
    m_tabs = new QTabWidget(this);

    m_allowList = new QListWidget(this);
    m_tabs->addTab(m_allowList, "Allow");

    m_denyList = new QListWidget(this);
    m_tabs->addTab(m_denyList, "Deny");

    m_askList = new QListWidget(this);
    m_tabs->addTab(m_askList, "Ask");

    mainLayout->addWidget(m_tabs, 1);

    // Rule input row
    auto *inputLayout = new QHBoxLayout();

    m_ruleInput = new QLineEdit(this);
    m_ruleInput->setPlaceholderText("e.g. Bash(git *), Read, Edit(src/**)");
    connect(m_ruleInput, &QLineEdit::returnPressed, this, &ClaudeAllowlistDialog::onAddRule);
    inputLayout->addWidget(m_ruleInput, 1);

    // Presets dropdown
    m_presetsCombo = new QComboBox(this);
    m_presetsCombo->addItem("Presets...");
    m_presetsCombo->addItem("Bash(git *)");
    m_presetsCombo->addItem("Bash(make *)");
    m_presetsCombo->addItem("Bash(cmake *)");
    m_presetsCombo->addItem("Bash(npm *)");
    m_presetsCombo->addItem("Bash(python3 *)");
    m_presetsCombo->addItem("Bash(pip *)");
    m_presetsCombo->addItem("Bash(cargo *)");
    m_presetsCombo->addItem("Bash(ls *)");
    m_presetsCombo->addItem("Bash(cat *)");
    m_presetsCombo->addItem("Bash(mkdir *)");
    m_presetsCombo->addItem("Read");
    m_presetsCombo->addItem("Edit");
    m_presetsCombo->addItem("Write");
    m_presetsCombo->addItem("Glob");
    m_presetsCombo->addItem("Grep");
    m_presetsCombo->addItem("WebFetch");
    m_presetsCombo->addItem("WebSearch");
    connect(m_presetsCombo, &QComboBox::activated, this, &ClaudeAllowlistDialog::onPresetSelected);
    inputLayout->addWidget(m_presetsCombo);

    mainLayout->addLayout(inputLayout);

    // Add / Remove buttons
    auto *btnLayout = new QHBoxLayout();

    auto *addBtn = new QPushButton("Add Rule", this);
    connect(addBtn, &QPushButton::clicked, this, &ClaudeAllowlistDialog::onAddRule);
    btnLayout->addWidget(addBtn);

    auto *removeBtn = new QPushButton("Remove Selected", this);
    connect(removeBtn, &QPushButton::clicked, this, &ClaudeAllowlistDialog::onRemoveRule);
    btnLayout->addWidget(removeBtn);

    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    // Dialog buttons
    auto *dialogBtns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel,
        this);
    connect(dialogBtns, &QDialogButtonBox::accepted, this, [this]() {
        saveSettings();
        accept();
    });
    connect(dialogBtns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(dialogBtns->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &ClaudeAllowlistDialog::onApply);
    mainLayout->addWidget(dialogBtns);
}

void ClaudeAllowlistDialog::setSettingsPath(const QString &path) {
    m_settingsPath = path;
    loadSettings();
}

void ClaudeAllowlistDialog::prefillRule(const QString &rule) {
    m_ruleInput->setText(rule);
    m_ruleInput->selectAll();
    m_tabs->setCurrentIndex(0); // Switch to Allow tab
}

void ClaudeAllowlistDialog::loadSettings() {
    m_allowList->clear();
    m_denyList->clear();
    m_askList->clear();

    if (m_settingsPath.isEmpty()) return;

    QFile file(m_settingsPath);
    if (!file.open(QIODevice::ReadOnly)) return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return;

    QJsonObject perms = doc.object().value("permissions").toObject();

    auto loadList = [](QListWidget *list, const QJsonArray &arr) {
        for (const QJsonValue &v : arr) {
            if (v.isString())
                list->addItem(v.toString());
        }
    };

    loadList(m_allowList, perms.value("allow").toArray());
    loadList(m_denyList, perms.value("deny").toArray());
    loadList(m_askList, perms.value("ask").toArray());
}

void ClaudeAllowlistDialog::saveSettings() {
    if (m_settingsPath.isEmpty()) return;

    // Read existing file to preserve non-permission keys
    QJsonObject root;
    {
        QFile file(m_settingsPath);
        if (file.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            if (doc.isObject()) root = doc.object();
        }
    }

    // Build permissions object
    auto listToArray = [](QListWidget *list) {
        QJsonArray arr;
        for (int i = 0; i < list->count(); ++i)
            arr.append(list->item(i)->text());
        return arr;
    };

    QJsonObject perms;
    perms["allow"] = listToArray(m_allowList);
    perms["deny"] = listToArray(m_denyList);
    perms["ask"] = listToArray(m_askList);
    root["permissions"] = perms;

    // Create .claude directory if needed
    QDir().mkpath(QFileInfo(m_settingsPath).absolutePath());

    // Atomic write with 0600 permissions
    QSaveFile file(m_settingsPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

QListWidget *ClaudeAllowlistDialog::currentList() const {
    switch (m_tabs->currentIndex()) {
    case 0: return m_allowList;
    case 1: return m_denyList;
    case 2: return m_askList;
    default: return m_allowList;
    }
}

void ClaudeAllowlistDialog::onAddRule() {
    QString rule = m_ruleInput->text().trimmed();
    if (rule.isEmpty()) return;

    QListWidget *list = currentList();

    // Check for duplicates
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->text() == rule) return;
    }

    list->addItem(rule);
    m_ruleInput->clear();
}

void ClaudeAllowlistDialog::onRemoveRule() {
    QListWidget *list = currentList();
    auto *item = list->currentItem();
    if (item) delete item;
}

void ClaudeAllowlistDialog::onPresetSelected(int index) {
    if (index <= 0) return; // Skip "Presets..." header
    m_ruleInput->setText(m_presetsCombo->currentText());
    m_presetsCombo->setCurrentIndex(0);
}

void ClaudeAllowlistDialog::onApply() {
    saveSettings();
}
