#pragma once

#include <QDialog>
#include <QTabWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QSlider>
#include <QFontComboBox>
#include <QTableWidget>
#include <QKeySequenceEdit>
#include <QPushButton>
#include <QLabel>
#include <QJsonObject>
#include <QList>
#include <QMap>

class Config;

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    // Lightweight plugin display struct — decouples SettingsDialog from
    // pluginmanager.h (which pulls in luaengine.h and the Lua-gated build).
    // MainWindow translates its PluginInfo list into this form before
    // handing it to the dialog.
    struct PluginDisplay {
        QString name;
        QString version;
        QString description;
        QString author;
        QStringList permissions;
    };

    explicit SettingsDialog(Config *config, QWidget *parent = nullptr);

    // Populate the Plugins tab. Must be called before show() (or the tab
    // will display "no plugins discovered"). Tab is always present; empty
    // list just changes the message.
    void setPlugins(const QList<PluginDisplay> &plugins);

signals:
    void settingsChanged();

private:
    void setupGeneralTab(QWidget *tab);
    void setupAppearanceTab(QWidget *tab);
    void setupTerminalTab(QWidget *tab);
    void setupAiTab(QWidget *tab);
    void setupHighlightsTab(QWidget *tab);
    void setupTriggersTab(QWidget *tab);
    void setupKeybindingsTab(QWidget *tab);
    void setupProfilesTab(QWidget *tab);
    void setupPluginsTab(QWidget *tab);
    void populatePluginsTab();

    void loadSettings();
    void applySettings();
    void addHighlightRow(const QString &pattern = {}, const QString &fg = "#FF0000",
                         const QString &bg = {}, bool enabled = true);
    void addTriggerRow(const QString &pattern = {}, const QString &actionType = "notify",
                       const QString &actionValue = {}, bool enabled = true);

    Config *m_config;
    QTabWidget *m_tabs;

    // General
    QComboBox *m_shellCombo;
    QLineEdit *m_shellCustom;
    QCheckBox *m_sessionPersistence;
    QCheckBox *m_sessionLogging;
    QCheckBox *m_autoCopy;
    QCheckBox *m_confirmMultilinePaste;
    QLineEdit *m_editorCmd;
    QLineEdit *m_imagePasteDir;
    QComboBox *m_tabTitleFormat;
    QSpinBox *m_notificationTimeout = nullptr;    // status-bar toast timeout
    QPushButton *m_installClaudeHooksBtn = nullptr;
    QLabel *m_claudeHooksStatus = nullptr;
    QPushButton *m_installClaudeGitContextBtn = nullptr;
    QLabel *m_claudeGitContextStatus = nullptr;
    // Per-tab Claude activity glyph gate. See Config::claudeTabStatusIndicator.
    QCheckBox *m_claudeTabStatusIndicator = nullptr;
    void installClaudeHooks();
    void refreshClaudeHooksStatus();
    void installClaudeGitContextHook();
    void refreshClaudeGitContextStatus();

    // Appearance
    QFontComboBox *m_fontFamily;
    QSpinBox *m_fontSize;
    QComboBox *m_themeCombo;
    QSlider *m_opacitySlider;
    QLabel *m_opacityLabel;
    QCheckBox *m_backgroundBlur;
    QCheckBox *m_gpuRendering;
    QSpinBox *m_paddingSpinner;
    QLineEdit *m_badgeEdit;
    QCheckBox *m_autoColorScheme;
    QComboBox *m_darkThemeCombo;
    QComboBox *m_lightThemeCombo;

    // Terminal
    QSpinBox *m_scrollbackLines;
    QCheckBox *m_showCommandMarks;
    QCheckBox *m_quakeMode;
    QLineEdit *m_quakeHotkey;

    // AI
    QCheckBox *m_aiEnabled;
    QLineEdit *m_aiEndpoint;
    QLineEdit *m_aiApiKey;
    QLineEdit *m_aiModel;
    QSpinBox *m_aiContextLines;

    // Highlights
    QTableWidget *m_highlightTable;

    // Triggers
    QTableWidget *m_triggerTable;

    // Keybindings
    QTableWidget *m_keybindingTable;

    // Profiles
    QComboBox *m_profileCombo;
    QLineEdit *m_profileName;
    QPushButton *m_profileSave;
    QPushButton *m_profileDelete;
    QPushButton *m_profileLoad;
    // Pending (uncommitted) profile state — Save/Delete buttons stage
    // edits here so Cancel can roll back. Initialized from m_config in
    // loadSettings(); committed back via applySettings(). Without
    // staging, Save/Delete bypassed the OK/Cancel pattern entirely.
    QJsonObject m_pendingProfiles;
    QString     m_pendingActiveProfile;

    // Plugins — manifest v2 capability audit UI (0.6.11)
    // m_pluginsContainer holds the scrollable vertical list of plugin
    // group boxes that is rebuilt whenever setPlugins() is called.
    // m_pluginPermissionChecks maps plugin name → (permission → checkbox)
    // so applySettings() can collect granted permissions without re-parsing
    // the layout.
    QWidget *m_pluginsContainer = nullptr;
    QLabel *m_pluginsEmptyLabel = nullptr;
    QList<PluginDisplay> m_plugins;
    QMap<QString, QMap<QString, QCheckBox *>> m_pluginPermissionChecks;
};
