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

    // Persisted RoadmapDialog geometry (ANTS-1100). Stored as
    // base64-encoded saveGeometry() bytes.
    QString roadmapDialogGeometry() const;
    void setRoadmapDialogGeometry(const QString &base64);

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

    // Confirm before closing a tab whose shell has non-shell descendant
    // processes (vim, top, claude, tail -f, etc.). Default on.
    bool confirmCloseWithProcesses() const;
    void setConfirmCloseWithProcesses(bool enabled);

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

    // Session persistence
    bool sessionPersistence() const;
    void setSessionPersistence(bool enabled);

    // Remote-control listener (Kitty-style rc_protocol socket).
    // Defaults to FALSE — any process under the user's UID can otherwise
    // drive the terminal via the socket, including injecting arbitrary
    // keystrokes via send-text. Opt-in per the 0.7.12 /indie-review
    // finding. X25519 auth deferred to 0.8.0.
    bool remoteControlEnabled() const;
    void setRemoteControlEnabled(bool enabled);

    // Per-tab Claude Code activity indicator. When true (default), each
    // tab whose shell has a Claude Code child process draws a small
    // state-dependent dot on the tab chrome — idle, thinking, tool-use,
    // planning, compacting, or awaiting-input. When false, no per-tab
    // tracking runs and no glyph renders (the bottom status bar still
    // shows the active tab's state). See
    // tests/features/claude_tab_status_indicator/spec.md.
    bool claudeTabStatusIndicator() const;
    void setClaudeTabStatusIndicator(bool enabled);

    // Per-project trust store for <project>/audit_rules.json rule packs
    // that carry `command` fields. `command` strings are bash-exec'd
    // verbatim when the Audit dialog runs, so an untrusted cloned repo
    // with a hostile rule pack is a local-RCE chain. Trust is scoped to
    // (canonical projectPath → sha256 of the rule-pack bytes), so a user
    // who trusts project A does not implicitly trust project B, and any
    // edit to the rule pack invalidates trust (re-prompt on next open).
    // 0.7.13: replaces the global audit_trust_command_rules bool.
    bool isAuditRulePackTrusted(const QString &projectPath,
                                const QByteArray &rulesBytes) const;
    void trustAuditRulePack(const QString &projectPath,
                            const QByteArray &rulesBytes);
    void untrustAuditRulePack(const QString &projectPath);

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

    // SSH ControlMaster auto-multiplexing. When true (default), ssh
    // invocations from the bookmark dialog add -o ControlMaster=auto /
    // -o ControlPath=~/.ssh/cm-%r@%h:%p / -o ControlPersist=10m so a
    // second tab to the same host reuses the first connection's auth.
    bool sshControlMaster() const;
    void setSshControlMaster(bool enabled);

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

    // Command-mark gutter (0.6.41): draw tick marks next to the
    // scrollbar for OSC 133 A prompt boundaries. Default on so the
    // feature is discoverable when shell integration is installed; the
    // gutter is a no-op (width 0) when promptRegions() is empty, so
    // users without shell integration see no change.
    bool showCommandMarks() const;
    void setShowCommandMarks(bool enabled);

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
    // ANTS-1138 — generation counter bumped on every successful
    // setAutoProfileRules write. MainWindow's checkAutoProfileRules
    // compares this to its locally-cached generation to invalidate
    // the s_patternCache + s_warnedInvalid statics when the rules
    // change. Without this, retired patterns linger in the cache
    // and `s_warnedInvalid` never re-warns when a fixed pattern
    // gets edited again.
    quint64 autoProfileRulesGeneration() const { return m_autoProfileRulesGen; }

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
    void setRawData(const QJsonObject &data) {
        // ANTS-1141 — short-circuit on m_loadFailed; same UX
        // rationale as setKeybinding/setPluginGrants/setPluginSetting.
        if (m_loadFailed) return;
        if (m_data == data) return;
        m_data = data;
        save();
    }

    void save();

    // True when the on-disk config.json existed at construction time but
    // failed to parse as a JSON object. In that mode save() becomes a
    // no-op so the next setter doesn't overwrite the user's (corrupt
    // but possibly recoverable) data with fresh defaults. The broken
    // file is rotated to `config.json.corrupt-<timestamp>` on load so
    // the user can hand-fix or copy-back. UI can surface this via a
    // one-time nag; the getter is the hook.
    bool loadFailed() const { return m_loadFailed; }
    const QString &loadFailureBackupPath() const { return m_loadFailureBackupPath; }

private:
    void load();
    static QString configPath();

    // Idempotent setter helper. Compares `value` against m_data[key] and
    // assigns + returns true only when they differ; returns false (leaving
    // m_data untouched) when they match. Lets every public setter
    // short-circuit a no-op save() — the primary defense against the
    // inotify-loop class of bug (see MainWindow::onConfigFileChanged).
    bool storeIfChanged(const QString &key, const QJsonValue &value);

    QJsonObject m_data;
    bool m_loadFailed = false;
    // ANTS-1138 — see autoProfileRulesGeneration().
    quint64 m_autoProfileRulesGen = 0;
    QString m_loadFailureBackupPath;
};
