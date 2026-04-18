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

    // Confirm pastes that look dangerous (multi-line, sudo, curl | sh, control chars)
    bool confirmMultilinePaste() const;
    void setConfirmMultilinePaste(bool enabled);

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

    // Manifest v2: per-plugin permission grants, persisted across runs.
    QStringList pluginGrants(const QString &pluginName) const;
    void setPluginGrants(const QString &pluginName, const QStringList &grants);
    // Plugin-owned key/value settings (backing store for ants.settings.get/set)
    QString pluginSetting(const QString &pluginName, const QString &key) const;
    void setPluginSetting(const QString &pluginName, const QString &key, const QString &value);

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

    // Visual bell
    bool visualBell() const;
    void setVisualBell(bool enabled);

    // Background image
    QString backgroundImage() const;
    void setBackgroundImage(const QString &path);

    // Per-style font families
    QString boldFontFamily() const;
    void setBoldFontFamily(const QString &family);
    QString italicFontFamily() const;
    void setItalicFontFamily(const QString &family);
    QString boldItalicFontFamily() const;
    void setBoldItalicFontFamily(const QString &family);

    // Terminal padding
    int terminalPadding() const;
    void setTerminalPadding(int px);

    // Tab groups (color labels)
    QJsonObject tabGroups() const;
    void setTabGroups(const QJsonObject &groups);

    // Tab color sequence — an ordered list of "#rrggbb" strings (empty
    // string for "no color") matching the tab order at save time. This
    // is the fallback path used when session_persistence is disabled —
    // the UUID-keyed tab_groups map relies on the same UUID being
    // regenerated on restore, which only happens with session
    // persistence on. The ordered sequence matches by index instead,
    // so tab colors survive restart regardless of whether scrollback
    // is persisted. User spec 2026-04-18.
    QJsonArray tabColorSequence() const;
    void setTabColorSequence(const QJsonArray &seq);

    // Command snippets
    QJsonArray snippets() const;
    void setSnippets(const QJsonArray &snippets);

    // Auto-profile switching rules: [{pattern, type, profile}]
    QJsonArray autoProfileRules() const;
    void setAutoProfileRules(const QJsonArray &rules);

    // Badge text (displayed as watermark in terminal background)
    QString badgeText() const;
    void setBadgeText(const QString &text);

    // Status-bar notification display duration for showStatusMessage()
    // calls that omit an explicit timeout. Default 5000ms matches
    // desktop-notification conventions. Callers that need a permanent
    // pinned notification (e.g. "Claude waiting for permission") pass
    // 0 explicitly; callers that need a custom timeout pass that value
    // in milliseconds.
    int notificationTimeoutMs() const;
    void setNotificationTimeoutMs(int ms);

    // Dark/light mode auto-switching
    bool autoColorScheme() const;
    void setAutoColorScheme(bool enabled);
    QString darkTheme() const;
    void setDarkTheme(const QString &name);
    QString lightTheme() const;
    void setLightTheme(const QString &name);

    // Raw JSON access for settings dialog
    QJsonObject rawData() const { return m_data; }
    void setRawData(const QJsonObject &data) { m_data = data; save(); }

    void save();

private:
    void load();
    static QString configPath();

    QJsonObject m_data;
};
