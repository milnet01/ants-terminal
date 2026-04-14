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

class Config;

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(Config *config, QWidget *parent = nullptr);

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
};
