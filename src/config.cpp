#include "config.h"

#include "configbackup.h"
#include "debuglog.h"
#include "secureio.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStandardPaths>

#include <cerrno>      // errno — report rename() failure causes
#include <cstdio>      // std::rename — atomic POSIX rename (overwrites dest)
#include <cstring>     // std::strerror — rename() failure diagnostics
#include <sys/stat.h>
#include <unistd.h>    // fsync — durability guarantee before atomic rename

Config::Config() {
    load();
}

QString Config::configPath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                  + "/ants-terminal";
    QDir().mkpath(dir);
    return dir + "/config.json";
}

void Config::load() {
    const QString path = configPath();
    QFile file(path);
    if (!file.exists()) return;  // first run — fall through to defaults

    if (!file.open(QIODevice::ReadOnly)) {
        // File exists but we can't read it — treat as a load failure
        // so we don't clobber it with defaults on next save(). Same
        // policy as the parse-failure path below.
        m_loadFailed = true;
        ANTS_LOG(DebugLog::Config,
                 "config.json exists but could not be opened for reading "
                 "— save() suppressed");
        return;
    }

    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseErr{};
    QJsonDocument doc = QJsonDocument::fromJson(raw, &parseErr);
    if (doc.isObject()) {
        m_data = doc.object();
        return;
    }

    // Parse failed — don't silently fall through to empty defaults and
    // have the next save() overwrite the user's (corrupt but possibly
    // hand-fixable) config. Instead: rotate the broken file aside,
    // latch a "load failed" flag, and refuse to save for this session
    // until the user intervenes. See
    // tests/features/config_parse_failure_guard/spec.md.
    m_loadFailed = true;

    // Rotate the broken bytes aside via the shared helper (see
    // secureio.h — millisecond timestamp + retry-on-collision).
    m_loadFailureBackupPath = rotateCorruptFileAside(path);

    if (!m_loadFailureBackupPath.isEmpty()) {
        ANTS_LOG(DebugLog::Config,
                 "config.json failed to parse at offset %d: %s — rotated to "
                 "%s, save() suppressed until next successful load",
                 parseErr.offset,
                 qUtf8Printable(parseErr.errorString()),
                 qUtf8Printable(m_loadFailureBackupPath));
    } else {
        // Backup copy failed entirely (disk full, perms, 10 collisions
        // in one ms — pathological). The live config.json is still
        // untouched thanks to the save() suppression, but the user has
        // no separate recovery file. Log the failure distinctly so
        // debugging doesn't mistake this for the success path.
        ANTS_LOG(DebugLog::Config,
                 "config.json failed to parse at offset %d: %s — backup copy "
                 "FAILED (no .corrupt-* written); save() still suppressed, "
                 "live file left in place for the user to inspect",
                 parseErr.offset,
                 qUtf8Printable(parseErr.errorString()));
    }
}

void Config::save() {
    // Refuse to save when load() latched a parse failure — otherwise a
    // setter-triggered save would overwrite the user's (corrupt but
    // possibly hand-fixable) config.json with whatever empty/default
    // state m_data has now. The corrupt file was already rotated aside
    // by load(); this is the write-side guard. See
    // tests/features/config_parse_failure_guard/spec.md.
    if (m_loadFailed) {
        ANTS_LOG(DebugLog::Config,
                 "save() skipped — earlier load failure latched "
                 "(backup at %s)",
                 qUtf8Printable(m_loadFailureBackupPath));
        return;
    }

    QString path = configPath();
    QString tmpPath = path + QStringLiteral(".tmp");

    // Serialize against concurrent Ants instances writing the same
    // config.json. Without this, two simultaneous saves race over the
    // shared `.tmp` file and last-rename-wins silently drops one
    // process's keystrokes/settings/profile changes. flock(2) on a
    // sibling .lock file is advisory but covers cooperating callers.
    ConfigWriteLock writeLock(path);
    if (!writeLock.acquired()) {
        ANTS_LOG(DebugLog::Config,
                 "save() skipped — could not acquire write lock on %s "
                 "within 5 s (another Ants process likely mid-save)",
                 qUtf8Printable(path));
        return;
    }

    // Set restrictive umask before creating file to avoid brief world-readable window
    mode_t oldMask = ::umask(0077);
    QFile file(tmpPath);
    if (file.open(QIODevice::WriteOnly)) {
        setOwnerOnlyPerms(file);
        QByteArray json = QJsonDocument(m_data).toJson();
        if (file.write(json) == json.size()) {
            // 0.6.28 — fsync before rename. QFile::close flushes userspace
            // buffers but doesn't force a kernel flush to disk; a kernel
            // crash or power loss between close() and rename() on ext4
            // with data=ordered can leave a zero-sized config.json after
            // reboot. fsync on the temp fd, then rename — matches the
            // classic "write-rename-fsync" pattern used by SQLite/Git.
            ::fsync(file.handle());
            file.close();
            // Atomic rename — on POSIX rename(2) atomically replaces the
            // destination, so no need to remove first (avoids a crash window
            // where the file is deleted but not yet renamed).
            //
            // MUST use std::rename (POSIX rename(2)) rather than QFile::rename:
            // QFile::rename refuses to overwrite an existing destination and
            // returns false, silently leaving config.json untouched while
            // config.json.tmp accumulates on disk. That path broke every save
            // after the first — theme / font / tab_groups changes all
            // evaporated on restart. POSIX rename(2) atomically replaces.
            //
            // 0.7.12: check the return value. ENOSPC, EACCES on the dest
            // dir, or EXDEV (cross-device, shouldn't happen here but
            // defensive) leave the tmp file orphaned and the user's prior
            // config silently unchanged. Log + remove the tmp on failure
            // so the user sees the error in debug.log and the tmp doesn't
            // accumulate across session lifetimes.
            const int rc = std::rename(tmpPath.toLocal8Bit().constData(),
                                       path.toLocal8Bit().constData());
            if (rc != 0) {
                ANTS_LOG(DebugLog::Config,
                         "rename(%s -> %s) failed: errno=%d (%s) — "
                         "prior config.json unchanged, tmp removed",
                         qUtf8Printable(tmpPath),
                         qUtf8Printable(path),
                         errno, std::strerror(errno));
                QFile::remove(tmpPath);
            } else {
                // Belt-and-suspenders: rename(2) preserves perms on
                // most local filesystems, but FAT/exFAT/SMB/NFS edges
                // and Qt's copy+unlink fallback path can drop the
                // 0600 set on the temp fd. config.json may hold
                // ai_api_key — re-chmod the final inode after the
                // atomic rename succeeds.
                setOwnerOnlyPerms(path);
                // ANTS-1141 — fsync the parent directory so the
                // rename's directory-entry update is durable
                // across power loss / kernel panic. Postgres /
                // SQLite / Git pattern; ext4 with data=ordered
                // can otherwise lose the rename on a crash
                // between rename(2) returning and the journal
                // commit.
                fsyncParentDir(path);
            }
        } else {
            file.close();
            QFile::remove(tmpPath);
        }
    }
    ::umask(oldMask);
}

// Idempotent helper. Compare-then-assign on m_data; returns true only when
// the stored value differed from the new value (in which case caller does
// the save()). Used by every setter below. Loop-prevention rationale lives
// at MainWindow::onConfigFileChanged — short-circuiting a no-op save() at
// the setter is the primary defense against the inotify-fileChanged-loop
// class of bug.
bool Config::storeIfChanged(const QString &key, const QJsonValue &value) {
    if (m_data.value(key) == value) return false;
    m_data[key] = value;
    return true;
}

QString Config::theme() const {
    return m_data.value("theme").toString("Dark");
}

void Config::setTheme(const QString &name) {
    if (!storeIfChanged("theme", name)) return;
    save();
}

int Config::fontSize() const {
    return qBound(4, m_data.value("font_size").toInt(11), 48);
}

void Config::setFontSize(int size) {
    if (!storeIfChanged("font_size", qBound(4, size, 48))) return;
    save();
}

int Config::windowWidth() const  { return m_data.value("window_w").toInt(900); }
int Config::windowHeight() const { return m_data.value("window_h").toInt(700); }
int Config::windowX() const      { return m_data.value("window_x").toInt(-1); }
int Config::windowY() const      { return m_data.value("window_y").toInt(-1); }

void Config::setWindowGeometry(int x, int y, int w, int h) {
    // Four-field setter: write all four through the helper, save only when
    // at least one differed. The OR-fold is intentional — `||` short-
    // circuits, but each storeIfChanged call has the side effect of
    // writing when changed, so we use `|` (bitwise) so every call runs.
    const bool dirty =
        int(storeIfChanged("window_x", x)) |
        int(storeIfChanged("window_y", y)) |
        int(storeIfChanged("window_w", w)) |
        int(storeIfChanged("window_h", h));
    if (!dirty) return;
    save();
}

QString Config::windowGeometryBase64() const {
    return m_data.value("window_geometry").toString("");
}

void Config::setWindowGeometryBase64(const QString &base64) {
    if (!storeIfChanged("window_geometry", base64)) return;
    save();
}

QString Config::roadmapDialogGeometry() const {
    return m_data.value("roadmap_dialog_geometry").toString("");
}

void Config::setRoadmapDialogGeometry(const QString &base64) {
    if (!storeIfChanged("roadmap_dialog_geometry", base64)) return;
    save();
}

int Config::scrollbackLines() const {
    return qBound(1000, m_data.value("scrollback_lines").toInt(50000), 1000000);
}

void Config::setScrollbackLines(int lines) {
    if (!storeIfChanged("scrollback_lines", qBound(1000, lines, 1000000))) return;
    save();
}

double Config::opacity() const {
    return qBound(0.1, m_data.value("opacity").toDouble(1.0), 1.0);
}

void Config::setOpacity(double value) {
    if (!storeIfChanged("opacity", qBound(0.1, value, 1.0))) return;
    save();
}

bool Config::sessionLogging() const {
    return m_data.value("session_logging").toBool(false);
}

void Config::setSessionLogging(bool enabled) {
    if (!storeIfChanged("session_logging", enabled)) return;
    save();
}

bool Config::autoCopyOnSelect() const {
    return m_data.value("auto_copy_on_select").toBool(true);
}

void Config::setAutoCopyOnSelect(bool enabled) {
    if (!storeIfChanged("auto_copy_on_select", enabled)) return;
    save();
}

bool Config::confirmMultilinePaste() const {
    return m_data.value("confirm_multiline_paste").toBool(true);  // default on
}

void Config::setConfirmMultilinePaste(bool enabled) {
    if (!storeIfChanged("confirm_multiline_paste", enabled)) return;
    save();
}

bool Config::confirmCloseWithProcesses() const {
    return m_data.value("confirm_close_with_processes").toBool(true);  // default on
}

void Config::setConfirmCloseWithProcesses(bool enabled) {
    if (!storeIfChanged("confirm_close_with_processes", enabled)) return;
    save();
}

QString Config::editorCommand() const {
    return m_data.value("editor_command").toString("");
}

void Config::setEditorCommand(const QString &cmd) {
    if (!storeIfChanged("editor_command", cmd)) return;
    save();
}

QString Config::imagePasteDir() const {
    return m_data.value("image_paste_dir").toString("");
}

void Config::setImagePasteDir(const QString &dir) {
    if (!storeIfChanged("image_paste_dir", dir)) return;
    save();
}

bool Config::backgroundBlur() const {
    return m_data.value("background_blur").toBool(false);
}

void Config::setBackgroundBlur(bool enabled) {
    if (!storeIfChanged("background_blur", enabled)) return;
    save();
}

QString Config::keybinding(const QString &action, const QString &defaultKey) const {
    QJsonObject kb = m_data.value("keybindings").toObject();
    return kb.value(action).toString(defaultKey);
}

void Config::setKeybinding(const QString &action, const QString &key) {
    // ANTS-1141 — short-circuit on m_loadFailed. save() bails
    // anyway, but the in-memory mutation otherwise leaves a
    // fictional "your keybinding worked" state visible to
    // subsequent getters in the session — UX-confusing
    // because it never persists.
    if (m_loadFailed) return;
    QJsonObject kb = m_data.value("keybindings").toObject();
    if (kb.value(action).toString() == key) return;
    kb[action] = key;
    m_data["keybindings"] = kb;
    save();
}

// Session persistence — default ON. Modern terminals (iTerm2, WezTerm,
// Kitty, Konsole) restore tabs out of the box; users expect the same here
// without hunting through Settings. The only cost is ~a few KB of
// scrollback per tab on disk, gated by a 5-second uptime floor in
// saveAllSessions() so crash-test launches don't wipe real state.
bool Config::sessionPersistence() const {
    return m_data.value("session_persistence").toBool(true);
}

void Config::setSessionPersistence(bool enabled) {
    if (!storeIfChanged("session_persistence", enabled)) return;
    save();
}

bool Config::remoteControlEnabled() const {
    return m_data.value("remote_control_enabled").toBool(false);
}

void Config::setRemoteControlEnabled(bool enabled) {
    if (!storeIfChanged("remote_control_enabled", enabled)) return;
    save();
}

bool Config::claudeTabStatusIndicator() const {
    // Default on: the feature is unobtrusive (a small dot on tabs with
    // Claude running) and its cost is negligible when no tab has Claude.
    return m_data.value("claude_tab_status_indicator").toBool(true);
}

void Config::setClaudeTabStatusIndicator(bool enabled) {
    if (!storeIfChanged("claude_tab_status_indicator", enabled)) return;
    save();
}

// Per-project audit rule-pack trust store. Key:
//   audit_rule_pack_trust = { <canonical projectPath> : <sha256Hex> }
// A project is trusted iff its canonicalized path appears and the stored
// hash matches the current bytes. Any rule-pack edit invalidates trust.
namespace {
QString canonicalizeProject(const QString &projectPath) {
    const QString c = QFileInfo(projectPath).canonicalFilePath();
    return c.isEmpty() ? projectPath : c;
}
QString rulePackSha256Hex(const QByteArray &rulesBytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(rulesBytes, QCryptographicHash::Sha256).toHex());
}
}

bool Config::isAuditRulePackTrusted(const QString &projectPath,
                                    const QByteArray &rulesBytes) const {
    const QJsonObject store = m_data.value("audit_rule_pack_trust").toObject();
    const QString stored = store.value(canonicalizeProject(projectPath)).toString();
    if (stored.isEmpty()) return false;
    return stored == rulePackSha256Hex(rulesBytes);
}

void Config::trustAuditRulePack(const QString &projectPath,
                                const QByteArray &rulesBytes) {
    QJsonObject store = m_data.value("audit_rule_pack_trust").toObject();
    const QString key = canonicalizeProject(projectPath);
    const QString hash = rulePackSha256Hex(rulesBytes);
    if (store.value(key).toString() == hash) return;
    store[key] = hash;
    m_data["audit_rule_pack_trust"] = store;
    save();
}

void Config::untrustAuditRulePack(const QString &projectPath) {
    QJsonObject store = m_data.value("audit_rule_pack_trust").toObject();
    const QString key = canonicalizeProject(projectPath);
    if (!store.contains(key)) return;
    store.remove(key);
    m_data["audit_rule_pack_trust"] = store;
    save();
}

// AI assistant
QString Config::aiEndpoint() const {
    return m_data.value("ai_endpoint").toString("");
}

void Config::setAiEndpoint(const QString &url) {
    if (!storeIfChanged("ai_endpoint", url)) return;
    save();
}

QString Config::aiApiKey() const {
    return m_data.value("ai_api_key").toString("");
}

void Config::setAiApiKey(const QString &key) {
    if (!storeIfChanged("ai_api_key", key)) return;
    save();
}

QString Config::aiModel() const {
    return m_data.value("ai_model").toString("llama3");
}

void Config::setAiModel(const QString &model) {
    if (!storeIfChanged("ai_model", model)) return;
    save();
}

int Config::aiContextLines() const {
    return qBound(10, m_data.value("ai_context_lines").toInt(50), 500);
}

void Config::setAiContextLines(int lines) {
    if (!storeIfChanged("ai_context_lines", qBound(10, lines, 500))) return;
    save();
}

bool Config::aiEnabled() const {
    return m_data.value("ai_enabled").toBool(false);
}

void Config::setAiEnabled(bool enabled) {
    if (!storeIfChanged("ai_enabled", enabled)) return;
    save();
}

// SSH bookmarks
QJsonArray Config::sshBookmarksJson() const {
    return m_data.value("ssh_bookmarks").toArray();
}

void Config::setSshBookmarksJson(const QJsonArray &arr) {
    if (!storeIfChanged("ssh_bookmarks", arr)) return;
    save();
}

bool Config::sshControlMaster() const {
    return m_data.value("ssh_control_master").toBool(true);
}

void Config::setSshControlMaster(bool enabled) {
    if (!storeIfChanged("ssh_control_master", enabled)) return;
    save();
}

// Plugin system
QString Config::pluginDir() const {
    return m_data.value("plugin_dir").toString("");
}

void Config::setPluginDir(const QString &dir) {
    if (!storeIfChanged("plugin_dir", dir)) return;
    save();
}

QStringList Config::enabledPlugins() const {
    QStringList result;
    QJsonArray arr = m_data.value("enabled_plugins").toArray();
    for (const QJsonValue &v : arr)
        result.append(v.toString());
    return result;
}

void Config::setEnabledPlugins(const QStringList &plugins) {
    QJsonArray arr;
    for (const QString &p : plugins)
        arr.append(p);
    if (!storeIfChanged("enabled_plugins", arr)) return;
    save();
}

// --- Manifest v2 per-plugin grants + settings ---
// Config shape (JSON):
//   "plugin_grants": { "<pluginName>": ["perm1", "perm2"], ... }
//   "plugin_settings": { "<pluginName>": { "key": "value", ... }, ... }

QStringList Config::pluginGrants(const QString &pluginName) const {
    QStringList out;
    QJsonObject grants = m_data.value("plugin_grants").toObject();
    QJsonArray arr = grants.value(pluginName).toArray();
    for (const QJsonValue &v : arr) out.append(v.toString());
    return out;
}

void Config::setPluginGrants(const QString &pluginName, const QStringList &grants) {
    if (m_loadFailed) return;  // ANTS-1141 — see setKeybinding
    QJsonObject all = m_data.value("plugin_grants").toObject();
    QJsonArray arr;
    for (const QString &g : grants) arr.append(g);
    if (all.value(pluginName).toArray() == arr) return;
    all[pluginName] = arr;
    m_data["plugin_grants"] = all;
    save();
}

QString Config::pluginSetting(const QString &pluginName, const QString &key) const {
    QJsonObject all = m_data.value("plugin_settings").toObject();
    QJsonObject plugin = all.value(pluginName).toObject();
    QJsonValue v = plugin.value(key);
    if (!v.isString()) return QString();
    return v.toString();
}

void Config::setPluginSetting(const QString &pluginName, const QString &key, const QString &value) {
    if (m_loadFailed) return;  // ANTS-1141 — see setKeybinding
    QJsonObject all = m_data.value("plugin_settings").toObject();
    QJsonObject plugin = all.value(pluginName).toObject();
    if (plugin.value(key).toString() == value) return;
    plugin[key] = value;
    all[pluginName] = plugin;
    m_data["plugin_settings"] = all;
    save();
}

// Claude Code project directories
QStringList Config::claudeProjectDirs() const {
    QStringList result;
    QJsonArray arr = m_data.value("claude_project_dirs").toArray();
    for (const QJsonValue &v : arr)
        result.append(v.toString());
    return result;
}

void Config::setClaudeProjectDirs(const QStringList &dirs) {
    QJsonArray arr;
    for (const QString &d : dirs)
        arr.append(d);
    if (!storeIfChanged("claude_project_dirs", arr)) return;
    save();
}

// Highlight rules
QJsonArray Config::highlightRules() const {
    return m_data.value("highlight_rules").toArray();
}

void Config::setHighlightRules(const QJsonArray &rules) {
    if (!storeIfChanged("highlight_rules", rules)) return;
    save();
}

// Trigger rules
QJsonArray Config::triggerRules() const {
    return m_data.value("trigger_rules").toArray();
}

void Config::setTriggerRules(const QJsonArray &rules) {
    if (!storeIfChanged("trigger_rules", rules)) return;
    save();
}

// Profiles
QJsonObject Config::profiles() const {
    return m_data.value("profiles").toObject();
}

void Config::setProfiles(const QJsonObject &profiles) {
    if (!storeIfChanged("profiles", profiles)) return;
    save();
}

QString Config::activeProfile() const {
    return m_data.value("active_profile").toString("");
}

void Config::setActiveProfile(const QString &name) {
    if (!storeIfChanged("active_profile", name)) return;
    save();
}

// Quake mode
bool Config::quakeMode() const {
    return m_data.value("quake_mode").toBool(false);
}

void Config::setQuakeMode(bool enabled) {
    if (!storeIfChanged("quake_mode", enabled)) return;
    save();
}

QString Config::quakeHotkey() const {
    return m_data.value("quake_hotkey").toString("F12");
}

void Config::setQuakeHotkey(const QString &key) {
    if (!storeIfChanged("quake_hotkey", key)) return;
    save();
}

// Command-mark gutter (0.6.41).
bool Config::showCommandMarks() const {
    return m_data.value("show_command_marks").toBool(true);
}

void Config::setShowCommandMarks(bool enabled) {
    if (!storeIfChanged("show_command_marks", enabled)) return;
    save();
}

// Broadcast mode
bool Config::broadcastMode() const {
    return m_data.value("broadcast_mode").toBool(false);
}

void Config::setBroadcastMode(bool enabled) {
    if (!storeIfChanged("broadcast_mode", enabled)) return;
    save();
}

// Font family
QString Config::fontFamily() const {
    return m_data.value("font_family").toString("");
}

void Config::setFontFamily(const QString &family) {
    if (!storeIfChanged("font_family", family)) return;
    save();
}

// Shell command
QString Config::shellCommand() const {
    return m_data.value("shell_command").toString("");
}

void Config::setShellCommand(const QString &cmd) {
    if (!storeIfChanged("shell_command", cmd)) return;
    save();
}

// Tab title format
QString Config::tabTitleFormat() const {
    return m_data.value("tab_title_format").toString("title");
}

void Config::setTabTitleFormat(const QString &fmt) {
    if (!storeIfChanged("tab_title_format", fmt)) return;
    save();
}

// Visual bell
bool Config::visualBell() const {
    return m_data.value("visual_bell").toBool(true);
}

void Config::setVisualBell(bool enabled) {
    if (!storeIfChanged("visual_bell", enabled)) return;
    save();
}

// Background image
QString Config::backgroundImage() const {
    return m_data.value("background_image").toString("");
}

void Config::setBackgroundImage(const QString &path) {
    if (!storeIfChanged("background_image", path)) return;
    save();
}

// Per-style font families
QString Config::boldFontFamily() const {
    return m_data.value("bold_font_family").toString("");
}

void Config::setBoldFontFamily(const QString &family) {
    if (!storeIfChanged("bold_font_family", family)) return;
    save();
}

QString Config::italicFontFamily() const {
    return m_data.value("italic_font_family").toString("");
}

void Config::setItalicFontFamily(const QString &family) {
    if (!storeIfChanged("italic_font_family", family)) return;
    save();
}

QString Config::boldItalicFontFamily() const {
    return m_data.value("bold_italic_font_family").toString("");
}

void Config::setBoldItalicFontFamily(const QString &family) {
    if (!storeIfChanged("bold_italic_font_family", family)) return;
    save();
}

// Tab groups
QJsonObject Config::tabGroups() const {
    return m_data.value("tab_groups").toObject();
}

void Config::setTabGroups(const QJsonObject &groups) {
    if (!storeIfChanged("tab_groups", groups)) return;
    save();
}

// Tab color sequence (ordered fallback path — see header comment).
QJsonArray Config::tabColorSequence() const {
    return m_data.value("tab_color_sequence").toArray();
}

void Config::setTabColorSequence(const QJsonArray &seq) {
    if (!storeIfChanged("tab_color_sequence", seq)) return;
    save();
}

int Config::terminalPadding() const {
    return m_data.value("terminal_padding").toInt(4);
}

void Config::setTerminalPadding(int px) {
    if (!storeIfChanged("terminal_padding", px)) return;
    save();
}

// Command snippets
QJsonArray Config::snippets() const {
    return m_data.value("snippets").toArray();
}

void Config::setSnippets(const QJsonArray &snippets) {
    if (!storeIfChanged("snippets", snippets)) return;
    save();
}

// Auto-profile switching rules
QJsonArray Config::autoProfileRules() const {
    return m_data.value("auto_profile_rules").toArray();
}

void Config::setAutoProfileRules(const QJsonArray &rules) {
    if (!storeIfChanged("auto_profile_rules", rules)) return;
    save();
}

// Badge text
QString Config::badgeText() const {
    return m_data.value("badge_text").toString("");
}

void Config::setBadgeText(const QString &text) {
    if (!storeIfChanged("badge_text", text)) return;
    save();
}

// Status-bar notification timeout (default = 5 s, per user spec 2026-04-18).
int Config::notificationTimeoutMs() const {
    return m_data.value("notification_timeout_ms").toInt(5000);
}

void Config::setNotificationTimeoutMs(int ms) {
    if (!storeIfChanged("notification_timeout_ms", ms)) return;
    save();
}

// Dark/light auto-switching
bool Config::autoColorScheme() const {
    return m_data.value("auto_color_scheme").toBool(false);
}

void Config::setAutoColorScheme(bool enabled) {
    if (!storeIfChanged("auto_color_scheme", enabled)) return;
    save();
}

QString Config::darkTheme() const {
    return m_data.value("dark_theme").toString("Dark");
}

void Config::setDarkTheme(const QString &name) {
    if (!storeIfChanged("dark_theme", name)) return;
    save();
}

QString Config::lightTheme() const {
    return m_data.value("light_theme").toString("Light");
}

void Config::setLightTheme(const QString &name) {
    if (!storeIfChanged("light_theme", name)) return;
    save();
}
