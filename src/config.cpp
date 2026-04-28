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
            }
        } else {
            file.close();
            QFile::remove(tmpPath);
        }
    }
    ::umask(oldMask);
}

QString Config::theme() const {
    return m_data.value("theme").toString("Dark");
}

void Config::setTheme(const QString &name) {
    // Idempotent: skip the disk write when the value already matches.
    // Loop-prevention rationale lives at MainWindow::onConfigFileChanged.
    if (m_data.value("theme").toString() == name) return;
    m_data["theme"] = name;
    save();
}

int Config::fontSize() const {
    return qBound(4, m_data.value("font_size").toInt(11), 48);
}

void Config::setFontSize(int size) {
    size = qBound(4, size, 48);
    m_data["font_size"] = size;
    save();
}

int Config::windowWidth() const  { return m_data.value("window_w").toInt(900); }
int Config::windowHeight() const { return m_data.value("window_h").toInt(700); }
int Config::windowX() const      { return m_data.value("window_x").toInt(-1); }
int Config::windowY() const      { return m_data.value("window_y").toInt(-1); }

void Config::setWindowGeometry(int x, int y, int w, int h) {
    m_data["window_x"] = x;
    m_data["window_y"] = y;
    m_data["window_w"] = w;
    m_data["window_h"] = h;
    save();
}

QString Config::windowGeometryBase64() const {
    return m_data.value("window_geometry").toString("");
}

void Config::setWindowGeometryBase64(const QString &base64) {
    m_data["window_geometry"] = base64;
    save();
}

int Config::scrollbackLines() const {
    return qBound(1000, m_data.value("scrollback_lines").toInt(50000), 1000000);
}

void Config::setScrollbackLines(int lines) {
    m_data["scrollback_lines"] = qBound(1000, lines, 1000000);
    save();
}

double Config::opacity() const {
    return qBound(0.1, m_data.value("opacity").toDouble(1.0), 1.0);
}

void Config::setOpacity(double value) {
    m_data["opacity"] = qBound(0.1, value, 1.0);
    save();
}

bool Config::sessionLogging() const {
    return m_data.value("session_logging").toBool(false);
}

void Config::setSessionLogging(bool enabled) {
    m_data["session_logging"] = enabled;
    save();
}

bool Config::autoCopyOnSelect() const {
    return m_data.value("auto_copy_on_select").toBool(true);
}

void Config::setAutoCopyOnSelect(bool enabled) {
    m_data["auto_copy_on_select"] = enabled;
    save();
}

bool Config::confirmMultilinePaste() const {
    return m_data.value("confirm_multiline_paste").toBool(true);  // default on
}

void Config::setConfirmMultilinePaste(bool enabled) {
    m_data["confirm_multiline_paste"] = enabled;
    save();
}

QString Config::editorCommand() const {
    return m_data.value("editor_command").toString("");
}

void Config::setEditorCommand(const QString &cmd) {
    m_data["editor_command"] = cmd;
    save();
}

QString Config::imagePasteDir() const {
    return m_data.value("image_paste_dir").toString("");
}

void Config::setImagePasteDir(const QString &dir) {
    m_data["image_paste_dir"] = dir;
    save();
}

bool Config::backgroundBlur() const {
    return m_data.value("background_blur").toBool(false);
}

void Config::setBackgroundBlur(bool enabled) {
    m_data["background_blur"] = enabled;
    save();
}

QString Config::keybinding(const QString &action, const QString &defaultKey) const {
    QJsonObject kb = m_data.value("keybindings").toObject();
    return kb.value(action).toString(defaultKey);
}

void Config::setKeybinding(const QString &action, const QString &key) {
    QJsonObject kb = m_data.value("keybindings").toObject();
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
    m_data["session_persistence"] = enabled;
    save();
}

bool Config::remoteControlEnabled() const {
    return m_data.value("remote_control_enabled").toBool(false);
}

void Config::setRemoteControlEnabled(bool enabled) {
    m_data["remote_control_enabled"] = enabled;
    save();
}

bool Config::claudeTabStatusIndicator() const {
    // Default on: the feature is unobtrusive (a small dot on tabs with
    // Claude running) and its cost is negligible when no tab has Claude.
    return m_data.value("claude_tab_status_indicator").toBool(true);
}

void Config::setClaudeTabStatusIndicator(bool enabled) {
    m_data["claude_tab_status_indicator"] = enabled;
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
    store[canonicalizeProject(projectPath)] = rulePackSha256Hex(rulesBytes);
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
    m_data["ai_endpoint"] = url;
    save();
}

QString Config::aiApiKey() const {
    return m_data.value("ai_api_key").toString("");
}

void Config::setAiApiKey(const QString &key) {
    m_data["ai_api_key"] = key;
    save();
}

QString Config::aiModel() const {
    return m_data.value("ai_model").toString("llama3");
}

void Config::setAiModel(const QString &model) {
    m_data["ai_model"] = model;
    save();
}

int Config::aiContextLines() const {
    return qBound(10, m_data.value("ai_context_lines").toInt(50), 500);
}

void Config::setAiContextLines(int lines) {
    m_data["ai_context_lines"] = qBound(10, lines, 500);
    save();
}

bool Config::aiEnabled() const {
    return m_data.value("ai_enabled").toBool(false);
}

void Config::setAiEnabled(bool enabled) {
    m_data["ai_enabled"] = enabled;
    save();
}

// SSH bookmarks
QJsonArray Config::sshBookmarksJson() const {
    return m_data.value("ssh_bookmarks").toArray();
}

void Config::setSshBookmarksJson(const QJsonArray &arr) {
    m_data["ssh_bookmarks"] = arr;
    save();
}

bool Config::sshControlMaster() const {
    return m_data.value("ssh_control_master").toBool(true);
}

void Config::setSshControlMaster(bool enabled) {
    m_data["ssh_control_master"] = enabled;
    save();
}

// Plugin system
QString Config::pluginDir() const {
    return m_data.value("plugin_dir").toString("");
}

void Config::setPluginDir(const QString &dir) {
    m_data["plugin_dir"] = dir;
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
    m_data["enabled_plugins"] = arr;
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
    QJsonObject all = m_data.value("plugin_grants").toObject();
    QJsonArray arr;
    for (const QString &g : grants) arr.append(g);
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
    QJsonObject all = m_data.value("plugin_settings").toObject();
    QJsonObject plugin = all.value(pluginName).toObject();
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
    m_data["claude_project_dirs"] = arr;
    save();
}

// Highlight rules
QJsonArray Config::highlightRules() const {
    return m_data.value("highlight_rules").toArray();
}

void Config::setHighlightRules(const QJsonArray &rules) {
    m_data["highlight_rules"] = rules;
    save();
}

// Trigger rules
QJsonArray Config::triggerRules() const {
    return m_data.value("trigger_rules").toArray();
}

void Config::setTriggerRules(const QJsonArray &rules) {
    m_data["trigger_rules"] = rules;
    save();
}

// Profiles
QJsonObject Config::profiles() const {
    return m_data.value("profiles").toObject();
}

void Config::setProfiles(const QJsonObject &profiles) {
    m_data["profiles"] = profiles;
    save();
}

QString Config::activeProfile() const {
    return m_data.value("active_profile").toString("");
}

void Config::setActiveProfile(const QString &name) {
    m_data["active_profile"] = name;
    save();
}

// Quake mode
bool Config::quakeMode() const {
    return m_data.value("quake_mode").toBool(false);
}

void Config::setQuakeMode(bool enabled) {
    m_data["quake_mode"] = enabled;
    save();
}

QString Config::quakeHotkey() const {
    return m_data.value("quake_hotkey").toString("F12");
}

void Config::setQuakeHotkey(const QString &key) {
    m_data["quake_hotkey"] = key;
    save();
}

// Command-mark gutter (0.6.41).
bool Config::showCommandMarks() const {
    return m_data.value("show_command_marks").toBool(true);
}

void Config::setShowCommandMarks(bool enabled) {
    m_data["show_command_marks"] = enabled;
    save();
}

// Broadcast mode
bool Config::broadcastMode() const {
    return m_data.value("broadcast_mode").toBool(false);
}

void Config::setBroadcastMode(bool enabled) {
    m_data["broadcast_mode"] = enabled;
    save();
}

// Font family
QString Config::fontFamily() const {
    return m_data.value("font_family").toString("");
}

void Config::setFontFamily(const QString &family) {
    m_data["font_family"] = family;
    save();
}

// Shell command
QString Config::shellCommand() const {
    return m_data.value("shell_command").toString("");
}

void Config::setShellCommand(const QString &cmd) {
    m_data["shell_command"] = cmd;
    save();
}

// Tab title format
QString Config::tabTitleFormat() const {
    return m_data.value("tab_title_format").toString("title");
}

void Config::setTabTitleFormat(const QString &fmt) {
    m_data["tab_title_format"] = fmt;
    save();
}

// Visual bell
bool Config::visualBell() const {
    return m_data.value("visual_bell").toBool(true);
}

void Config::setVisualBell(bool enabled) {
    m_data["visual_bell"] = enabled;
    save();
}

// Background image
QString Config::backgroundImage() const {
    return m_data.value("background_image").toString("");
}

void Config::setBackgroundImage(const QString &path) {
    m_data["background_image"] = path;
    save();
}

// Per-style font families
QString Config::boldFontFamily() const {
    return m_data.value("bold_font_family").toString("");
}

void Config::setBoldFontFamily(const QString &family) {
    m_data["bold_font_family"] = family;
    save();
}

QString Config::italicFontFamily() const {
    return m_data.value("italic_font_family").toString("");
}

void Config::setItalicFontFamily(const QString &family) {
    m_data["italic_font_family"] = family;
    save();
}

QString Config::boldItalicFontFamily() const {
    return m_data.value("bold_italic_font_family").toString("");
}

void Config::setBoldItalicFontFamily(const QString &family) {
    m_data["bold_italic_font_family"] = family;
    save();
}

// Tab groups
QJsonObject Config::tabGroups() const {
    return m_data.value("tab_groups").toObject();
}

void Config::setTabGroups(const QJsonObject &groups) {
    m_data["tab_groups"] = groups;
    save();
}

// Tab color sequence (ordered fallback path — see header comment).
QJsonArray Config::tabColorSequence() const {
    return m_data.value("tab_color_sequence").toArray();
}

void Config::setTabColorSequence(const QJsonArray &seq) {
    m_data["tab_color_sequence"] = seq;
    save();
}

int Config::terminalPadding() const {
    return m_data.value("terminal_padding").toInt(4);
}

void Config::setTerminalPadding(int px) {
    m_data["terminal_padding"] = px;
    save();
}

// Command snippets
QJsonArray Config::snippets() const {
    return m_data.value("snippets").toArray();
}

void Config::setSnippets(const QJsonArray &snippets) {
    m_data["snippets"] = snippets;
    save();
}

// Auto-profile switching rules
QJsonArray Config::autoProfileRules() const {
    return m_data.value("auto_profile_rules").toArray();
}

void Config::setAutoProfileRules(const QJsonArray &rules) {
    m_data["auto_profile_rules"] = rules;
    save();
}

// Badge text
QString Config::badgeText() const {
    return m_data.value("badge_text").toString("");
}

void Config::setBadgeText(const QString &text) {
    m_data["badge_text"] = text;
    save();
}

// Status-bar notification timeout (default = 5 s, per user spec 2026-04-18).
int Config::notificationTimeoutMs() const {
    return m_data.value("notification_timeout_ms").toInt(5000);
}

void Config::setNotificationTimeoutMs(int ms) {
    m_data["notification_timeout_ms"] = ms;
    save();
}

// Dark/light auto-switching
bool Config::autoColorScheme() const {
    return m_data.value("auto_color_scheme").toBool(false);
}

void Config::setAutoColorScheme(bool enabled) {
    m_data["auto_color_scheme"] = enabled;
    save();
}

QString Config::darkTheme() const {
    return m_data.value("dark_theme").toString("Dark");
}

void Config::setDarkTheme(const QString &name) {
    m_data["dark_theme"] = name;
    save();
}

QString Config::lightTheme() const {
    return m_data.value("light_theme").toString("Light");
}

void Config::setLightTheme(const QString &name) {
    m_data["light_theme"] = name;
    save();
}
