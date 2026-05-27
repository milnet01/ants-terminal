// ANTS-1897 — implementation. See mcporientation.h + docs/specs/ANTS-1897.md.

#include "mcporientation.h"

#include "secureio.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringLiteral>

namespace ants {
namespace mcp_orientation {

namespace {

// INV-3 — substring that identifies an Ants-owned SessionStart hook
// entry. Substring match on `command`. Same string used at write time
// to construct the entry; never abbreviate or alias.
constexpr const char *kMarkerSubstring = "ants-terminal/hooks/mcp-orientation.sh";

// INV-1 — marker prefix at the start of the second line of the script.
// The version token that follows is parsed and compared.
constexpr const char *kScriptMarkerPrefix = "# ants-orientation-version:";

// Script body template. INV-12 byte-equality target after
// `arg(ANTS_VERSION)`. Contains exactly one `%1`; no other `%N`.
constexpr const char *kOrientationScriptBody =
    "#!/bin/bash\n"
    "# ants-orientation-version: %1\n"
    "# ants-terminal MCP orientation prelude (ANTS-1897).\n"
    "# This file is managed by Ants Terminal — your edits will be\n"
    "# overwritten on the next Ants version bump.\n"
    "# Disable: Settings → General → uncheck \"Show MCP cheat-sheet\n"
    "# at Claude session start\", OR delete this file and restart\n"
    "# Ants Terminal.\n"
    "if [ -z \"${ANTS_MCP_SOCKET:-}\" ] || [ ! -S \"$ANTS_MCP_SOCKET\" ]; then\n"
    "  exit 0   # Ants MCP not reachable; stay silent\n"
    "fi\n"
    "cat <<'EOF'\n"
    "Ants MCP is connected. Before reaching for Edit/Write/Bash, check\n"
    "if a dedicated MCP tool exists — they are typically 5–10× cheaper:\n"
    "\n"
    "  • changelog_log    → CHANGELOG.md append (Keep-a-Changelog aware)\n"
    "  • roadmap_log      → ROADMAP.md append / status flip / edit\n"
    "  • file_outline     → scan a file's structure cheaply\n"
    "  • find_definition  → \"where is foo defined?\"\n"
    "  • find_sources     → \"who calls bar?\"\n"
    "  • git_state        → status + branch + ahead/behind in one call\n"
    "  • read_log         → filtered log tail (vs full Read)\n"
    "  • session_orient   → first-call session bootstrap (state + layout)\n"
    "  • model_switch_stats → switcher trust signal + envelope\n"
    "\n"
    "Full catalog: load with ToolSearch query \"mcp__ants\" (or per-name\n"
    "with \"select:mcp__ants__<name>\"). Tool descriptors carry\n"
    "selection_hint lines describing when to reach for each.\n"
    "EOF\n";

// Resolve `<home>/.config/ants-terminal/hooks/mcp-orientation.sh`.
// Caller may pass an explicit home to bypass $HOME (test seam).
QString resolveScriptPath(const QString &homeOverride) {
    QString configBase;
    if (!homeOverride.isEmpty()) {
        configBase = homeOverride + QLatin1String("/.config/ants-terminal");
    } else {
        configBase = QStandardPaths::writableLocation(
                         QStandardPaths::AppConfigLocation);
        if (configBase.isEmpty()) {
            // Fallback: HOME/.config/ants-terminal — matches
            // QStandardPaths default for AppConfigLocation when the
            // app's organizationName/applicationName are unset.
            const QString home = QDir::homePath();
            configBase = home + QLatin1String("/.config/ants-terminal");
        }
    }
    return configBase + QLatin1String("/hooks/mcp-orientation.sh");
}

QString resolveSettingsPath(const QString &homeOverride) {
    const QString home = homeOverride.isEmpty()
        ? QDir::homePath()
        : homeOverride;
    return home + QLatin1String("/.claude/settings.json");
}

// Parse the marker version from the second line of an existing script.
// Returns empty when the file is absent, unreadable, or the marker line
// is missing (case 3: user-owned, do not overwrite — INV-2).
QString parseScriptMarkerVersion(const QString &path) {
    QFile f(path);
    if (!f.exists()) return QString();
    if (!f.open(QIODevice::ReadOnly)) return QString();
    // Read first ~256 bytes — the marker is always on line 2.
    const QByteArray head = f.read(256);
    f.close();
    const QList<QByteArray> lines = head.split('\n');
    if (lines.size() < 2) return QString();
    const QByteArray &markerLine = lines.at(1);
    const QByteArray prefix(kScriptMarkerPrefix);
    if (!markerLine.startsWith(prefix)) return QString();
    QByteArray rest = markerLine.mid(prefix.size()).trimmed();
    return QString::fromUtf8(rest);
}

bool writeScriptFile(const QString &path, const QByteArray &body,
                     QString *warn) {
    QFileInfo fi(path);
    QDir dir;
    if (!dir.mkpath(fi.absolutePath())) {
        if (warn) {
            *warn = QStringLiteral("could not create script dir: %1")
                    .arg(fi.absolutePath());
        }
        return false;
    }
    QSaveFile saver(path);
    if (!saver.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (warn) {
            *warn = QStringLiteral("could not open script for write: %1")
                    .arg(path);
        }
        return false;
    }
    if (saver.write(body) != body.size()) {
        if (warn) *warn = QStringLiteral("short write to %1").arg(path);
        return false;
    }
    if (!saver.commit()) {
        if (warn) {
            *warn = QStringLiteral("commit failed for %1").arg(path);
        }
        return false;
    }
    // INV-11 — owner-only perms (0600 baseline + +x for the user since
    // the hook runner invokes the script directly per the bash-prefix
    // schema; the file needs +x even though `bash <path>` would honour
    // mode-bit-stripped runs, future re-shapings may need it).
    setOwnerOnlyPerms(path);
    QFile::setPermissions(path,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return true;
}

// Build the `command` value Ants writes into settings.json. Matches the
// kMarkerSubstring contract — substring match anchors on the resolved
// install path.
QString hookCommand(const QString &scriptPath) {
    return QStringLiteral("bash ") + scriptPath;
}

// Build the {"type":"command", "command":"...", "timeout":3} entry.
QJsonObject hookCommandEntry(const QString &scriptPath) {
    QJsonObject entry;
    entry.insert(QStringLiteral("type"), QStringLiteral("command"));
    entry.insert(QStringLiteral("command"), hookCommand(scriptPath));
    entry.insert(QStringLiteral("timeout"), 3);
    return entry;
}

// INV-3 — substring match. Walks every SessionStart entry's inner
// `hooks[]` array and returns the (i, j) index of the first match, or
// {-1, -1} if absent.
struct EntryLocation { int outerIdx = -1; int innerIdx = -1; };
EntryLocation findAntsEntry(const QJsonArray &sessionStart) {
    const QString marker = QString::fromUtf8(kMarkerSubstring);
    for (int i = 0; i < sessionStart.size(); ++i) {
        const QJsonObject outer = sessionStart.at(i).toObject();
        const QJsonArray inner = outer.value(QStringLiteral("hooks"))
                                      .toArray();
        for (int j = 0; j < inner.size(); ++j) {
            const QJsonObject e = inner.at(j).toObject();
            const QString cmd = e.value(QStringLiteral("command"))
                                 .toString();
            if (cmd.contains(marker)) return {i, j};
        }
    }
    return {-1, -1};
}

// Merge / unmerge the Ants SessionStart entry into settings.json.
// `add=true` inserts (idempotent); `add=false` removes.
// Returns: kept, added, removed, parse_fail.
enum class MergeOutcome { Kept, Added, Removed, ParseFail, WriteFail, Created };

MergeOutcome mergeSettings(const QString &settingsPath,
                           const QString &scriptPath,
                           bool add,
                           QString *warn) {
    QFile f(settingsPath);
    QJsonObject root;
    bool fileExisted = f.exists();
    if (fileExisted) {
        if (!f.open(QIODevice::ReadOnly)) {
            if (warn) {
                *warn = QStringLiteral("could not open settings.json: %1")
                        .arg(settingsPath);
            }
            return MergeOutcome::ParseFail;
        }
        const QByteArray raw = f.readAll();
        f.close();
        if (!raw.trimmed().isEmpty()) {
            QJsonParseError pe;
            QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
                // INV-13 — refuse to overwrite a broken settings.json.
                if (warn) {
                    *warn = QStringLiteral(
                        "settings.json failed to parse (%1) — Ants "
                        "will not overwrite. Fix the file or delete "
                        "it to retry the merge.").arg(pe.errorString());
                }
                return MergeOutcome::ParseFail;
            }
            root = doc.object();
        }
    } else if (!add) {
        // Nothing to remove from a non-existent file.
        return MergeOutcome::Kept;
    }

    QJsonObject hooks = root.value(QStringLiteral("hooks")).toObject();
    QJsonArray sessionStart = hooks.value(QStringLiteral("SessionStart"))
                                   .toArray();
    EntryLocation existing = findAntsEntry(sessionStart);

    if (add) {
        if (existing.outerIdx >= 0) {
            // Already present — INV-3 idempotent no-op. Update the
            // command in case the resolved scriptPath changed (e.g.
            // home moved); preserves the surrounding structure.
            QJsonObject outer = sessionStart.at(existing.outerIdx).toObject();
            QJsonArray inner = outer.value(QStringLiteral("hooks"))
                                    .toArray();
            QJsonObject entry = inner.at(existing.innerIdx).toObject();
            const QString currentCmd =
                entry.value(QStringLiteral("command")).toString();
            const QString desiredCmd = hookCommand(scriptPath);
            if (currentCmd == desiredCmd) {
                return MergeOutcome::Kept;
            }
            entry.insert(QStringLiteral("command"), desiredCmd);
            inner.replace(existing.innerIdx, entry);
            outer.insert(QStringLiteral("hooks"), inner);
            sessionStart.replace(existing.outerIdx, outer);
        } else {
            // Append a fresh outer container with our one hook entry.
            QJsonObject outer;
            QJsonArray inner;
            inner.append(hookCommandEntry(scriptPath));
            outer.insert(QStringLiteral("hooks"), inner);
            sessionStart.append(outer);
        }
    } else {
        if (existing.outerIdx < 0) return MergeOutcome::Kept;
        QJsonObject outer = sessionStart.at(existing.outerIdx).toObject();
        QJsonArray inner = outer.value(QStringLiteral("hooks")).toArray();
        inner.removeAt(existing.innerIdx);
        if (inner.isEmpty()) {
            sessionStart.removeAt(existing.outerIdx);
        } else {
            outer.insert(QStringLiteral("hooks"), inner);
            sessionStart.replace(existing.outerIdx, outer);
        }
    }

    hooks.insert(QStringLiteral("SessionStart"), sessionStart);
    root.insert(QStringLiteral("hooks"), hooks);

    // Write back atomically.
    QFileInfo fi(settingsPath);
    QDir dir;
    if (!dir.mkpath(fi.absolutePath())) {
        if (warn) {
            *warn = QStringLiteral("could not create settings dir: %1")
                    .arg(fi.absolutePath());
        }
        return MergeOutcome::WriteFail;
    }
    QSaveFile saver(settingsPath);
    if (!saver.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (warn) {
            *warn = QStringLiteral("could not open settings.json "
                                   "for write: %1").arg(settingsPath);
        }
        return MergeOutcome::WriteFail;
    }
    const QByteArray out = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (saver.write(out) != out.size()) {
        if (warn) *warn = QStringLiteral("short write to %1").arg(settingsPath);
        return MergeOutcome::WriteFail;
    }
    if (!saver.commit()) {
        if (warn) {
            *warn = QStringLiteral("commit failed for %1").arg(settingsPath);
        }
        return MergeOutcome::WriteFail;
    }
    setOwnerOnlyPerms(settingsPath);

    if (!fileExisted) return MergeOutcome::Created;
    return add ? MergeOutcome::Added : MergeOutcome::Removed;
}

}  // namespace

QString orientationScriptTemplate() {
    return QString::fromUtf8(kOrientationScriptBody);
}

QString scriptInstallPath() { return resolveScriptPath(QString()); }
QString claudeSettingsPath() { return resolveSettingsPath(QString()); }
QString settingsMarkerSubstring() { return QString::fromUtf8(kMarkerSubstring); }

Result install() { return installAt(QString()); }
Result uninstall() { return uninstallAt(QString()); }

Result installAt(const QString &homeDir) {
    Result r;
    r.scriptPath = resolveScriptPath(homeDir);
    const QString settingsPath = resolveSettingsPath(homeDir);

    // Decide the script-write disposition per § 2.1.1.
    const QString runningVersion = QStringLiteral(ANTS_VERSION);
    const QString markerVersion = parseScriptMarkerVersion(r.scriptPath);
    bool scriptWritten = false;
    if (QFile::exists(r.scriptPath)) {
        if (markerVersion.isEmpty()) {
            // INV-2 — user-owned. Log + skip script write.
            r.warning = QStringLiteral(
                "user-owned %1 — Ants will not overwrite. Delete it "
                "to let Ants reinstall.").arg(r.scriptPath);
            qWarning().noquote() << "[mcp-orientation]" << (r.warning);
        } else if (markerVersion == runningVersion) {
            // Matches — idempotent skip.
        } else {
            // Version bump — overwrite.
            const QByteArray body = orientationScriptTemplate()
                                    .arg(runningVersion).toUtf8();
            QString w;
            if (writeScriptFile(r.scriptPath, body, &w)) {
                scriptWritten = true;
            } else {
                r.warning = w;
            }
        }
    } else {
        // Case 1: file does not exist — fresh install.
        const QByteArray body = orientationScriptTemplate()
                                .arg(runningVersion).toUtf8();
        QString w;
        if (writeScriptFile(r.scriptPath, body, &w)) {
            scriptWritten = true;
        } else {
            r.warning = w;
        }
    }
    (void)scriptWritten;  // used by tests via file inspection

    // Always attempt the settings merge — even if the script is
    // user-owned (case 3), the hook entry should still point at the
    // user's customised script.
    QString settingsWarn;
    MergeOutcome outcome = mergeSettings(settingsPath, r.scriptPath,
                                         /*add=*/true, &settingsWarn);
    if (outcome == MergeOutcome::ParseFail
        || outcome == MergeOutcome::WriteFail) {
        if (r.warning.isEmpty()) r.warning = settingsWarn;
        else r.warning += QLatin1String("; ") + settingsWarn;
        qWarning().noquote() << "[mcp-orientation]" << (settingsWarn);
        r.ok = false;
        return r;
    }
    r.ok = true;
    return r;
}

Result uninstallAt(const QString &homeDir) {
    Result r;
    r.scriptPath = resolveScriptPath(homeDir);
    const QString settingsPath = resolveSettingsPath(homeDir);

    QString settingsWarn;
    MergeOutcome outcome = mergeSettings(settingsPath, r.scriptPath,
                                         /*add=*/false, &settingsWarn);
    if (outcome == MergeOutcome::ParseFail
        || outcome == MergeOutcome::WriteFail) {
        r.warning = settingsWarn;
        r.ok = false;
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace mcp_orientation
}  // namespace ants
