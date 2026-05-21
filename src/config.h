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

    // ANTS-1150 — UI / chrome state persistence Phase 1.
    //
    // SettingsDialog last-active tab index. Caller (SettingsDialog
    // ctor) clamps the read int to [0, m_tabs->count()-1]; Config
    // doesn't know how many tabs the dialog has.
    int settingsDialogLastTab() const;
    void setSettingsDialogLastTab(int index);

    // RoadmapDialog active preset — one of "full", "history",
    // "current", "next", "far_future", "custom". Unknown / missing
    // → "full". For Custom-preset persistence to be honoured, the
    // RoadmapDialog ctor must also have valid roadmap_status_filters
    // (otherwise the dialog falls back to Full to avoid an empty-
    // render dead-end — see docs/specs/ANTS-1150.md).
    QString roadmapActivePreset() const;
    void setRoadmapActivePreset(const QString &name);

    // RoadmapDialog Kind facet checkbox set. Values from the
    // KindEntry table in roadmapdialog.cpp ("implement", "fix",
    // "audit-fix", "review-fix", "doc", "doc-fix", "refactor",
    // "test", "chore", "release", "research", "ux"). Persisted
    // sorted (ASCII codepoint) for diffability. Unknown values
    // are silently dropped on read.
    QStringList roadmapKindFilters() const;
    void setRoadmapKindFilters(const QStringList &kinds);

    // RoadmapDialog 5 status-emoji checkboxes — applies only when
    // roadmap_active_preset == "custom". JSON shape:
    //   { "done", "planned", "in_progress", "considered", "current" }
    // each value bool. Empty/missing object → caller falls back
    // to Preset::Full defaults.
    QJsonObject roadmapStatusFilters() const;
    void setRoadmapStatusFilters(const QJsonObject &filters);

    // ANTS-1154 v2 card-renderer state. Each set stores the IDs /
    // slugs the user has manually toggled to "expanded" or
    // "table-view". Restored on dialog open; updated on close.
    //
    // - roadmap_expanded_items: ANTS-NNNN ids of cards the user
    //   has opened. Missing → all cards start collapsed.
    // - roadmap_expanded_sections: slugified ## / ### heading
    //   text (lowercase, non-alnum → `-`). Missing → all sections
    //   start collapsed.
    // - roadmap_table_sections: section slugs toggled to compact
    //   table view. Missing → card-stack view (default).
    QStringList roadmapExpandedItems() const;
    void setRoadmapExpandedItems(const QStringList &ids);

    QStringList roadmapExpandedSections() const;
    void setRoadmapExpandedSections(const QStringList &slugs);

    QStringList roadmapTableSections() const;
    void setRoadmapTableSections(const QStringList &slugs);

    // RoadmapDialog per-tab scroll anchor. JSON shape:
    //   {
    //     "full": { "section": "0-7-82", "id": "ANTS-1145", "offset_px": 14 },
    //     "history": { ... },
    //     ...
    //   }
    // Restored on open: if `id` still exists, scroll its top - offset_px
    // to viewport top; else scroll to section; else top. Missing/empty
    // → scroll to top.
    QJsonObject roadmapScrollAnchors() const;
    void setRoadmapScrollAnchors(const QJsonObject &anchors);

    // ANTS-1238 — RoadmapDialog density tier. Values: "compact" /
    // "cozy" / "comfortable". Unknown / missing / non-string-JSON
    // → "cozy" (graceful fallback — spec INV-4). Persistence-write
    // failure is silent — see ANTS-1238 § 3.h.
    QString roadmapDensity() const;
    void setRoadmapDensity(const QString &name);

    // AuditDialog severity-filter pills. JSON shape:
    //   { "blocker", "critical", "major", "minor", "info" }
    // each value bool. Empty/missing → all 5 on.
    QJsonObject auditSeverityFilters() const;
    void setAuditSeverityFilters(const QJsonObject &filters);

    // AuditDialog "Show new since baseline" toggle. Persisted bool
    // is honoured at restore only when m_hasBaseline is true; the
    // bool stays through baseline deletion (lazy-invalidate — re-
    // honours on next saveBaseline).
    bool auditShowNewOnly() const;
    void setAuditShowNewOnly(bool enabled);

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
    int aiReviewConcurrency() const;        // ANTS-1727; default 2, clamp [1,4]
    void setAiReviewConcurrency(int n);
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

    // ANTS-1183: schema version stamp. Bump kSchemaVersion when a
    // breaking on-disk rename / type change ships, and add the
    // corresponding migration arm in migrate(). Pre-stamp configs
    // (those written by Ants ≤ 0.7.77 with no `_schema` key) are
    // treated as v0 and migrated to v1 on first load. Persisted as
    // a top-level int key `"_schema"` — leading underscore so it
    // sorts ahead of user keys in any pretty-print and signals
    // "internal metadata, do not hand-edit."
    static constexpr int kSchemaVersion = 1;

private:
    void load();
    // Apply on-disk schema migrations from `from` to `to`. Empty
    // arms today; placeholder so a future rename has a canonical
    // home rather than ad-hoc per-getter compatibility shims.
    void migrate(int from, int to);
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
