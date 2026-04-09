#pragma once

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>

// Forward declare
struct SshBookmark;

class Config {
public:
    Config();

    QString theme() const;
    void setTheme(const QString &name);

    int fontSize() const;
    void setFontSize(int size);

    int windowWidth() const;
    int windowHeight() const;
    int windowX() const;
    int windowY() const;
    void setWindowGeometry(int x, int y, int w, int h);

    // Qt saveGeometry/restoreGeometry (handles WM frame offsets reliably)
    QString windowGeometryBase64() const;
    void setWindowGeometryBase64(const QString &base64);

    int scrollbackLines() const;
    void setScrollbackLines(int lines);

    double opacity() const;
    void setOpacity(double value);

    bool sessionLogging() const;
    void setSessionLogging(bool enabled);

    bool autoCopyOnSelect() const;
    void setAutoCopyOnSelect(bool enabled);

    QString editorCommand() const;
    void setEditorCommand(const QString &cmd);

    // Image paste auto-save directory
    QString imagePasteDir() const;
    void setImagePasteDir(const QString &dir);

    // Background blur (KDE/KWin)
    bool backgroundBlur() const;
    void setBackgroundBlur(bool enabled);

    // Custom keybindings (action -> key sequence string)
    QString keybinding(const QString &action, const QString &defaultKey) const;
    void setKeybinding(const QString &action, const QString &key);

    // GPU rendering
    bool gpuRendering() const;
    void setGpuRendering(bool enabled);

    // Per-pixel background alpha (0-255, separate from window opacity)
    int backgroundAlpha() const;
    void setBackgroundAlpha(int alpha);

    // Session persistence
    bool sessionPersistence() const;
    void setSessionPersistence(bool enabled);

    // AI assistant
    QString aiEndpoint() const;
    void setAiEndpoint(const QString &url);
    QString aiApiKey() const;
    void setAiApiKey(const QString &key);
    QString aiModel() const;
    void setAiModel(const QString &model);
    int aiContextLines() const;
    void setAiContextLines(int lines);
    bool aiEnabled() const;
    void setAiEnabled(bool enabled);

    // SSH bookmarks
    QJsonArray sshBookmarksJson() const;
    void setSshBookmarksJson(const QJsonArray &arr);

    // Plugin system
    QString pluginDir() const;
    void setPluginDir(const QString &dir);
    QStringList enabledPlugins() const;
    void setEnabledPlugins(const QStringList &plugins);

    // Claude Code project directories (where to look for / create projects)
    QStringList claudeProjectDirs() const;
    void setClaudeProjectDirs(const QStringList &dirs);

    // Highlight rules: [{pattern, fg, bg, enabled}]
    QJsonArray highlightRules() const;
    void setHighlightRules(const QJsonArray &rules);

    // Trigger rules: [{pattern, action_type, action_value, enabled}]
    QJsonArray triggerRules() const;
    void setTriggerRules(const QJsonArray &rules);

    // Profiles: {name -> {theme, font_size, opacity, ...}}
    QJsonObject profiles() const;
    void setProfiles(const QJsonObject &profiles);
    QString activeProfile() const;
    void setActiveProfile(const QString &name);

    // Quake mode
    bool quakeMode() const;
    void setQuakeMode(bool enabled);
    QString quakeHotkey() const;
    void setQuakeHotkey(const QString &key);

    // Broadcast mode
    bool broadcastMode() const;
    void setBroadcastMode(bool enabled);

    // Font family
    QString fontFamily() const;
    void setFontFamily(const QString &family);

    // Shell command override
    QString shellCommand() const;
    void setShellCommand(const QString &cmd);

    // Tab title format: "title", "cwd", "process", "cwd-process"
    QString tabTitleFormat() const;
    void setTabTitleFormat(const QString &fmt);

    // Raw JSON access for settings dialog
    QJsonObject rawData() const { return m_data; }
    void setRawData(const QJsonObject &data) { m_data = data; save(); }

    void save();

private:
    void load();
    QString configPath() const;

    QJsonObject m_data;
};
