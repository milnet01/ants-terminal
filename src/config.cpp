#include "config.h"

#include "configbackup.h"
#include "debuglog.h"
#include "secureio.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStandardPaths>

#include <cerrno>      // errno — report rename() failure causes
#include <cstdio>      // std::rename — atomic POSIX rename (overwrites dest)
#include <cstring>     // std::strerror — rename() failure diagnostics
#include <sys/stat.h>
#include <unistd.h>    // fsync — durability guarantee before atomic rename

namespace {
// ANTS-1169: PATH_MAX-sized cap on user-typable path settings. The
// six path/command setters below run user-supplied strings through
// this gate. A nul-embedded value is also rejected — POSIX path
// semantics treat nul as a terminator, so a value like
// "/safe\0;/etc/shadow" would otherwise be saved to JSON intact and
// reinterpreted differently at downstream read sites.
constexpr int kMaxPathSetterLen = 4096;

bool acceptablePathValue(const QString &v) {
    if (v.size() > kMaxPathSetterLen) return false;
    if (v.contains(QChar(0))) return false;
    return true;
}
} // namespace

Config::Config() {
    load();
}

void Config::migrate(int from, int to) {
    // ANTS-1183: placeholder. Future on-disk renames ship as a new
    // arm:
    //
    //   if (from < 2) { ... rename "opacity" → "terminal_opacity"; }
    //   if (from < 3) { ... split "shell_command" into argv array; }
    //
    // Migrations stack: a v0 → v3 jump runs each arm in order. The
    // top-level `_schema` key is stamped on the next save().
    Q_UNUSED(to);
    if (from < 1) {
        // No-op for the v0 → v1 transition: the keys that exist at
        // v1 are exactly the keys that existed before the schema
        // stamp landed. Future bumps add real arms here.
    }
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
        // ANTS-1183: schema migration. Pre-stamp configs (no
        // `_schema` key) are v0; everything written by 0.7.78+
        // carries the current version. Newer-than-current values
        // are read as-is — the binary that wrote them owned the
        // forward-compat decision.
        const int onDisk = m_data.value(QStringLiteral("_schema")).toInt(0);
        if (onDisk < kSchemaVersion) {
            migrate(onDisk, kSchemaVersion);
        }
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

    // ANTS-1183: stamp schema version on every save so the next load
    // can decide whether to migrate. Cheap; idempotent.
    m_data[QStringLiteral("_schema")] = kSchemaVersion;

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
                // Record exactly what we wrote so MainWindow's config
                // watcher can recognise this event as our own echo and
                // skip the external-edit hot-reload (which would tear
                // down an open Settings dialog on Apply / tab-switch).
                // ANTS-1981.
                m_lastWrittenBytes = json;
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

// ANTS-1842 — per-dialog size map. Width/height only (position is never
// persisted; the dialog re-centers per D4).
QSize Config::dialogSize(const QString &key) const {
    const QJsonArray wh =
        m_data.value("dialog_sizes").toObject().value(key).toArray();
    if (wh.size() != 2) return QSize();
    return QSize(wh.at(0).toInt(), wh.at(1).toInt());
}

void Config::setDialogSize(const QString &key, const QSize &size) {
    if (key.isEmpty() || !size.isValid() || size.isEmpty()) return;
    QJsonObject sizes = m_data.value("dialog_sizes").toObject();
    QJsonArray wh;
    wh.append(size.width());
    wh.append(size.height());
    sizes[key] = wh;
    if (!storeIfChanged("dialog_sizes", sizes)) return;
    save();
}

// ANTS-1150 — UI / chrome state persistence Phase 1.

int Config::settingsDialogLastTab() const {
    return m_data.value("settings_dialog_last_tab").toInt(0);
}

void Config::setSettingsDialogLastTab(int index) {
    if (!storeIfChanged("settings_dialog_last_tab", index)) return;
    save();
}

QString Config::roadmapActivePreset() const {
    const QString name = m_data.value("roadmap_active_preset").toString("full");
    // Validate against the known set; unknown strings (corrupt
    // hand-edits, older Ants writing a future enum value) fall back
    // to "full".
    if (name == "full" || name == "history" || name == "current" ||
        name == "next" || name == "far_future" || name == "custom") {
        return name;
    }
    return QStringLiteral("full");
}

void Config::setRoadmapActivePreset(const QString &name) {
    // ANTS-1179: mirror the getter's known-set validation. Without
    // this, setRoadmapActivePreset("zombie") writes through, the
    // getter then silently returns "full" but the JSON file keeps
    // the zombie value forever — a future KindEntry rename / preset
    // rename would leave dead values stranded on disk.
    static const QSet<QString> kKnown = {
        QStringLiteral("full"), QStringLiteral("history"),
        QStringLiteral("current"), QStringLiteral("next"),
        QStringLiteral("far_future"), QStringLiteral("custom"),
    };
    if (!kKnown.contains(name)) return;
    if (!storeIfChanged("roadmap_active_preset", name)) return;
    save();
}

QStringList Config::roadmapKindFilters() const {
    static const QStringList kKnown = {
        QStringLiteral("implement"),  QStringLiteral("fix"),
        QStringLiteral("audit-fix"),  QStringLiteral("review-fix"),
        QStringLiteral("doc"),        QStringLiteral("doc-fix"),
        QStringLiteral("refactor"),   QStringLiteral("test"),
        QStringLiteral("chore"),      QStringLiteral("release"),
        QStringLiteral("research"),   QStringLiteral("ux"),
    };
    const QJsonArray arr = m_data.value("roadmap_kind_filters").toArray();
    QStringList result;
    result.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QString s = v.toString();
        // Drop unknowns silently — defends against a stale entry
        // surviving a future KindEntry rename. Spec says known set
        // is the KindEntry table at roadmapdialog.cpp:846.
        if (kKnown.contains(s)) result.append(s);
    }
    return result;
}

void Config::setRoadmapKindFilters(const QStringList &kinds) {
    // ANTS-1179: filter to the known kind set so unknowns can't
    // sneak through the setter and live on as zombies in the JSON
    // file. Mirrors the getter's validation list so the in-memory
    // and on-disk views stay in sync.
    static const QSet<QString> kKnown = {
        QStringLiteral("implement"),  QStringLiteral("fix"),
        QStringLiteral("audit-fix"),  QStringLiteral("review-fix"),
        QStringLiteral("doc"),        QStringLiteral("doc-fix"),
        QStringLiteral("refactor"),   QStringLiteral("test"),
        QStringLiteral("chore"),      QStringLiteral("release"),
        QStringLiteral("research"),   QStringLiteral("ux"),
    };
    QStringList filtered;
    filtered.reserve(kinds.size());
    for (const QString &k : kinds) {
        if (kKnown.contains(k)) filtered.append(k);
    }
    // Sort ASCII codepoint-wise for stable on-disk ordering. Diffs
    // across kind-toggle saves stay readable; QSet iteration order
    // is unspecified, so an unsorted setter would re-write the
    // same-content array in different orders. ASCII-only — every
    // KindEntry value at roadmapdialog.cpp:846 is ASCII.
    filtered.sort();
    QJsonArray arr;
    for (const QString &k : filtered) arr.append(k);
    if (!storeIfChanged("roadmap_kind_filters", arr)) return;
    save();
}

QJsonObject Config::roadmapStatusFilters() const {
    return m_data.value("roadmap_status_filters").toObject();
}

void Config::setRoadmapStatusFilters(const QJsonObject &filters) {
    if (!storeIfChanged("roadmap_status_filters", filters)) return;
    save();
}

// ANTS-1735 §2.7 — typed accessor mirrors the roadmapStatusFilters() shape.
// On-disk keys: claude.auto_model_switch / claude.auto_model_min_dwell_sec /
// claude.auto_model_floor / claude.auto_model_composer_stale_ms (ANTS-1914) /
// claude.auto_model_idle_ceiling_sec (ANTS-1917). Defaults applied here so
// callers always see a fully-populated object. This object is a WHITELIST —
// only keys inserted here are visible to claudestatuswidgets; a new tuning
// knob MUST be surfaced here or the controller read silently gets the default.
QJsonObject Config::claudeAutoModel() const {
    QJsonObject out;
    out.insert("switch_enabled",
               m_data.value("claude.auto_model_switch").toBool(false));
    int dwell = m_data.value("claude.auto_model_min_dwell_sec").toInt(90);
    if (dwell < 30)   dwell = 30;
    if (dwell > 1800) dwell = 1800;
    out.insert("min_dwell_sec", dwell);
    QString floor = m_data.value("claude.auto_model_floor").toString("haiku");
    if (floor != QLatin1String("haiku") && floor != QLatin1String("sonnet"))
        floor = QStringLiteral("haiku");
    out.insert("floor", floor);
    // ANTS-1974 — home/baseline tier: the neutral-band recommendation. Default
    // "sonnet" reproduces the pre-1974 mapping. An unrecognised value coerces to
    // "sonnet". Clamped up to the floor here (effectiveHome = max(home, floor))
    // so the envelope never advertises a home the floor forbids.
    QString home = m_data.value("claude.auto_model_home_tier").toString("sonnet");
    if (home != QLatin1String("haiku") && home != QLatin1String("sonnet")
        && home != QLatin1String("opus"))
        home = QStringLiteral("sonnet");
    // One-sided clamp: rank order haiku < sonnet < opus. If home is below the
    // floor, raise it to the floor (the floor is the hard lower bound).
    auto rank = [](const QString &t) {
        return t == QLatin1String("opus") ? 2 : (t == QLatin1String("sonnet") ? 1 : 0);
    };
    if (rank(home) < rank(floor)) home = floor;
    out.insert("home_tier", home);
    // ANTS-1914 — composer-stale veto threshold (ms). -1 sentinel (default)
    // tells the gate to use kComposerStaleVetoMs (~5 min). Advanced users set
    // a smaller positive value to unblock queued slash-commands sooner.
    // Passed through verbatim; the gate clamps semantics (>= 0 applies).
    out.insert("composer_stale_ms",
               m_data.value("claude.auto_model_composer_stale_ms").toInt(-1));
    // ANTS-1917 — idle end-of-session ceiling (seconds). Default 180 s
    // (== kIdleEndOfSessionMs); a value <= 0 disables the gate. Clamped to a
    // sane upper bound to avoid runaway values.
    int idleCeil = m_data.value("claude.auto_model_idle_ceiling_sec").toInt(180);
    if (idleCeil > 86'400) idleCeil = 86'400;   // 1 day ceiling guard
    out.insert("idle_ceiling_sec", idleCeil);
    return out;
}

void Config::setClaudeAutoModelSwitch(bool enabled) {
    if (!storeIfChanged("claude.auto_model_switch", enabled)) return;
    save();
}

// ANTS-1974 — persist the home/baseline tier. Accepts "haiku"|"sonnet"|"opus";
// anything else is normalised to "sonnet" before storing.
void Config::setClaudeAutoModelHomeTier(const QString &tier) {
    QString t = tier;
    if (t != QLatin1String("haiku") && t != QLatin1String("sonnet")
        && t != QLatin1String("opus"))
        t = QStringLiteral("sonnet");
    if (!storeIfChanged("claude.auto_model_home_tier", t)) return;
    save();
}

bool Config::claudeAutoModelNudgeShown() const {
    return m_data.value("claude.auto_model_nudge_shown").toBool(false);
}

void Config::setClaudeAutoModelNudgeShown(bool shown) {
    if (!storeIfChanged("claude.auto_model_nudge_shown", shown)) return;
    save();
}

// ANTS-1893 — switch-event surfacing mute toggles. Default TRUE
// (the three surfaces are the primary trust-building affordance per
// the spec §1; users who find any noisy flip one off in Settings).
bool Config::claudeAutoModelToastEnabled() const {
    return m_data.value("claude.auto_model_toast_enabled").toBool(true);
}

void Config::setClaudeAutoModelToastEnabled(bool enabled) {
    if (!storeIfChanged("claude.auto_model_toast_enabled", enabled)) return;
    save();
}

bool Config::claudeAutoModelChipPulseEnabled() const {
    return m_data.value("claude.auto_model_chip_pulse_enabled").toBool(true);
}

void Config::setClaudeAutoModelChipPulseEnabled(bool enabled) {
    if (!storeIfChanged("claude.auto_model_chip_pulse_enabled", enabled)) return;
    save();
}

bool Config::claudeAutoModelUndoEnabled() const {
    return m_data.value("claude.auto_model_undo_enabled").toBool(true);
}

void Config::setClaudeAutoModelUndoEnabled(bool enabled) {
    if (!storeIfChanged("claude.auto_model_undo_enabled", enabled)) return;
    save();
}

// ANTS-1951 — auto-confirm Claude Code's "Switch model?" dialog even when
// the /model command was typed by the user (not injected by the auto-switcher
// or a chip-click). The user typed it deliberately, so the confirm prompt is
// pure friction; a mistyped tier never reaches this dialog (CC errors on an
// invalid /model arg), so there is no typo to guard. Config-file-only key,
// default on; flip to false to keep the manual confirmation.
bool Config::claudeAutoModelConfirmUserSwitch() const {
    return m_data.value("claude.auto_model_confirm_user_switch").toBool(true);
}

// ANTS-1924 — text injected into the PTY after a model switch completes
// so CC resumes without the user having to type anything. Empty = no
// prompt. PTY layer expects `\r` (CR) as the submit key — `\n` only
// inserts a newline in CC's composer without submitting.
QString Config::claudeAutoModelContinuationPrompt() const {
    return m_data.value("claude.auto_model_switch_continuation_prompt")
               .toString(QStringLiteral("please continue"));
}

// ANTS-1897.
bool Config::claudeMcpOrientationEnabled() const {
    return m_data.value("claude.mcp_orientation_enabled").toBool(true);
}

void Config::setClaudeMcpOrientationEnabled(bool enabled) {
    if (!storeIfChanged("claude.mcp_orientation_enabled", enabled)) return;
    save();
}

bool Config::claudeMcpOrientationNudgeShown() const {
    return m_data.value("claude.mcp_orientation_nudge_shown").toBool(false);
}

void Config::setClaudeMcpOrientationNudgeShown(bool shown) {
    if (!storeIfChanged("claude.mcp_orientation_nudge_shown", shown)) return;
    save();
}

// ANTS-1154 v2 card-renderer state. Three string-set keys for
// per-item / per-section expand state and table-mode toggles.
// Stored as JSON arrays of strings for diffability — the dialog
// reads them into QSet<QString> at open and writes them sorted
// at close, same pattern as roadmap_kind_filters.

static QStringList readStringList(const QJsonObject &data,
                                  const char *key) {
    const QJsonArray arr = data.value(QLatin1String(key)).toArray();
    QStringList out;
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QString s = v.toString();
        if (!s.isEmpty()) out.append(s);
    }
    return out;
}

QStringList Config::roadmapExpandedItems() const {
    return readStringList(m_data, "roadmap_expanded_items");
}

void Config::setRoadmapExpandedItems(const QStringList &ids) {
    QStringList sorted = ids;
    sorted.removeDuplicates();
    sorted.sort();
    QJsonArray arr;
    for (const QString &s : sorted) arr.append(s);
    if (!storeIfChanged("roadmap_expanded_items", arr)) return;
    save();
}

QStringList Config::roadmapExpandedSections() const {
    return readStringList(m_data, "roadmap_expanded_sections");
}

void Config::setRoadmapExpandedSections(const QStringList &slugs) {
    QStringList sorted = slugs;
    sorted.removeDuplicates();
    sorted.sort();
    QJsonArray arr;
    for (const QString &s : sorted) arr.append(s);
    if (!storeIfChanged("roadmap_expanded_sections", arr)) return;
    save();
}

QStringList Config::roadmapTableSections() const {
    return readStringList(m_data, "roadmap_table_sections");
}

void Config::setRoadmapTableSections(const QStringList &slugs) {
    QStringList sorted = slugs;
    sorted.removeDuplicates();
    sorted.sort();
    QJsonArray arr;
    for (const QString &s : sorted) arr.append(s);
    if (!storeIfChanged("roadmap_table_sections", arr)) return;
    save();
}

QJsonObject Config::roadmapScrollAnchors() const {
    return m_data.value("roadmap_scroll_anchors").toObject();
}

void Config::setRoadmapScrollAnchors(const QJsonObject &anchors) {
    if (!storeIfChanged("roadmap_scroll_anchors", anchors)) return;
    save();
}

// ANTS-1238 — density tier selector. INV-4: unknown / missing /
// non-string JSON value → "cozy" fallback.
QString Config::roadmapDensity() const {
    const QString name = m_data.value("roadmap_density").toString("cozy");
    if (name == QStringLiteral("compact") ||
        name == QStringLiteral("cozy") ||
        name == QStringLiteral("comfortable")) {
        return name;
    }
    return QStringLiteral("cozy");
}

void Config::setRoadmapDensity(const QString &name) {
    // Mirror getter's known-set validation. Unknown values are
    // silently dropped on write — same pattern as
    // setRoadmapActivePreset (ANTS-1179).
    static const QSet<QString> kKnown = {
        QStringLiteral("compact"),
        QStringLiteral("cozy"),
        QStringLiteral("comfortable"),
    };
    if (!kKnown.contains(name)) return;
    if (!storeIfChanged("roadmap_density", name)) return;
    save();
}

QJsonObject Config::auditSeverityFilters() const {
    return m_data.value("audit_severity_filters").toObject();
}

void Config::setAuditSeverityFilters(const QJsonObject &filters) {
    if (!storeIfChanged("audit_severity_filters", filters)) return;
    save();
}

bool Config::auditShowNewOnly() const {
    return m_data.value("audit_show_new_only").toBool(false);
}

void Config::setAuditShowNewOnly(bool enabled) {
    if (!storeIfChanged("audit_show_new_only", enabled)) return;
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
    if (!acceptablePathValue(cmd)) return;
    if (!storeIfChanged("editor_command", cmd)) return;
    save();
}

QString Config::imagePasteDir() const {
    return m_data.value("image_paste_dir").toString("");
}

void Config::setImagePasteDir(const QString &dir) {
    if (!acceptablePathValue(dir)) return;
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

// ANTS-1727 — bounded-concurrency cap for the review-dialog LlmDispatcher.
// Default 2, clamped [1, 4] so a large partition can't open more sockets
// or exceed the ~40 MiB transient RAM budget (spec § 4).
int Config::aiReviewConcurrency() const {
    return qBound(1, m_data.value("ai_review_concurrency").toInt(2), 4);
}

void Config::setAiReviewConcurrency(int n) {
    if (!storeIfChanged("ai_review_concurrency", qBound(1, n, 4))) return;
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
    if (!acceptablePathValue(dir)) return;
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
    for (const QString &d : dirs) {
        if (!acceptablePathValue(d)) continue;
        arr.append(d);
    }
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
    if (!acceptablePathValue(cmd)) return;
    if (!storeIfChanged("shell_command", cmd)) return;
    save();
}

// Tab title format — fixed enum. The consumer (updateTabTitles in
// mainwindow.cpp) matches exactly these four values; any other string
// degrades every tab to the "Shell" fallback. Validate at both ends
// (ANTS-1764, same pattern as setRoadmapActivePreset/-Density per
// ANTS-1179) so a zombie value can't linger on disk.
static const QSet<QString> &knownTabTitleFormats() {
    static const QSet<QString> kKnown = {
        QStringLiteral("title"), QStringLiteral("cwd"),
        QStringLiteral("process"), QStringLiteral("cwd-process"),
    };
    return kKnown;
}

QString Config::tabTitleFormat() const {
    const QString fmt = m_data.value("tab_title_format").toString("title");
    if (knownTabTitleFormats().contains(fmt)) return fmt;
    return QStringLiteral("title");
}

void Config::setTabTitleFormat(const QString &fmt) {
    if (!knownTabTitleFormats().contains(fmt)) return;
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
    if (!acceptablePathValue(path)) return;
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
    // ANTS-1138 — bump the generation counter so caches keyed on
    // pattern strings can invalidate stale entries.
    ++m_autoProfileRulesGen;
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
