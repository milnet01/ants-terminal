#pragma once

#include <QDialog>
#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QTabWidget>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QLabel>

// Dialog for editing Claude Code's .claude/settings.local.json permission rules
class ClaudeAllowlistDialog : public QDialog {
    Q_OBJECT

public:
    explicit ClaudeAllowlistDialog(QWidget *parent = nullptr);

    void setSettingsPath(const QString &path);
    void prefillRule(const QString &rule);
    void saveSettings();

    // Rule normalization (public so mainwindow can pre-process rules)
    static QString normalizeRule(const QString &raw);
    static QString generalizeRule(const QString &rule);

private slots:
    void onAddRule();
    void onRemoveRule();
    void onPresetSelected(int index);
    void onApply();

private:
    void loadSettings();
    QListWidget *currentList() const;

    // Rule validation and correction
    static bool ruleSubsumes(const QString &broad, const QString &narrow);
    bool isDuplicate(QListWidget *list, const QString &rule) const;
    QString findSubsumingRule(QListWidget *list, const QString &rule) const;
    void showValidationHint(const QString &msg, bool isError = false);

    // Add rule with companion "cd * && cmd *" variant for Bash rules
    void addRuleWithCompanion(QListWidget *list, const QString &rule);

    QTabWidget *m_tabs = nullptr;
    QListWidget *m_allowList = nullptr;
    QListWidget *m_denyList = nullptr;
    QListWidget *m_askList = nullptr;
    QLineEdit *m_ruleInput = nullptr;
    QComboBox *m_presetsCombo = nullptr;
    QLabel *m_validationLabel = nullptr;

    QString m_settingsPath;
};
