#pragma once

#include <QDialog>
#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QTabWidget>
#include <QPushButton>
#include <QDialogButtonBox>

// Dialog for editing Claude Code's .claude/settings.local.json permission rules
class ClaudeAllowlistDialog : public QDialog {
    Q_OBJECT

public:
    explicit ClaudeAllowlistDialog(QWidget *parent = nullptr);

    void setSettingsPath(const QString &path);
    void prefillRule(const QString &rule);

private slots:
    void onAddRule();
    void onRemoveRule();
    void onPresetSelected(int index);
    void onApply();

private:
    void loadSettings();
    void saveSettings();
    QListWidget *currentList() const;

    QTabWidget *m_tabs = nullptr;
    QListWidget *m_allowList = nullptr;
    QListWidget *m_denyList = nullptr;
    QListWidget *m_askList = nullptr;
    QLineEdit *m_ruleInput = nullptr;
    QComboBox *m_presetsCombo = nullptr;

    QString m_settingsPath;
};
