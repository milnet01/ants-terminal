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
// entry. Suffix-only (the filename + parent dir) so the match is
// robust to directory-name variants: `QStandardPaths::AppConfigLocation`
// uses Qt's `applicationName` verbatim ("Ants Terminal" — capital +
// space) while tests and the doc-comment fallback path use the
// lowercase-hyphenated `ants-terminal`. Both forms end in the same
// `/hooks/mcp-orientation.sh` suffix. ANTS-1901.
constexpr const char *kMarkerSubstring = "/hooks/mcp-orientation.sh";

// INV-1 — marker prefix at the start of the second line of the script.
// The version token that follows is parsed only to detect marker
// PRESENCE (Ants-managed vs user-owned); its value is not compared —
// the ANTS-2038 rewrite gate is a full-body byte compare.
constexpr const char *kScriptMarkerPrefix = "# ants-orientation-version:";

// Script body template. INV-12 byte-equality target after
// `arg(ANTS_VERSION)`. Contains exactly one `%1`; no other `%N`.
constexpr const char *kOrientationScriptBody =
    "#!/bin/bash\n"
    "# ants-orientation-version: %1\n"
    "# ants-terminal MCP orientation prelude (ANTS-1897).\n"
    "# This file is managed by Ants Terminal — edits to this managed\n"
    "# body are overwritten on the next launch. To take ownership,\n"
    "# delete the '# ants-orientation-version:' marker line above.\n"
    "# Disable: Settings → General → uncheck \"Show MCP cheat-sheet\n"
    "# at Claude session start\", OR delete this file and restart\n"
    "# Ants Terminal.\n"
    "if [ -z \"${ANTS_MCP_SOCKET:-}\" ] || [ ! -S \"$ANTS_MCP_SOCKET\" ]; then\n"
    "  exit 0   # Ants MCP not reachable; stay silent\n"
    "fi\n"
    "cat <<'EOF'\n"
    "Ants MCP is connected. Before reaching for Edit/Write/Bash, check\n"
    "if a dedicated MCP tool exists — they are 5–10× cheaper. Call each\n"
    "by its full name, e.g. mcp__ants__workspace_search:\n"
    "\n"
    "  • session_orient     → first-call bootstrap (state + layout + codebase index)\n"
    "  • codebase_index     → project map: symbols / lanes / files — query before grep\n"
    "  • roadmap_query      → look up roadmap bullets by ID / status / section\n"
    "  • roadmap_log        → ROADMAP.md append / status flip / annotate\n"
    "  • changelog_log      → CHANGELOG.md append (Keep-a-Changelog aware)\n"
    "  • workspace_search   → project-wide code search (saves 250–4500 tokens vs grep)\n"
    "  • file_outline       → outline a file; read_region(s) fetch its slices\n"
    "  • find_definition    → \"where is foo defined?\"\n"
    "  • find_sources       → \"who calls bar?\"\n"
    "  • git_state          → status + branch + ahead/behind in one call\n"
    "  • read_log           → filtered log tail (vs full Read)\n"
    "  • model_switch_stats → auto-switcher trust signal + near-miss breakdown\n"
    "  • spec_log           → write a spec's Status / cold-eyes loop / INV entries\n"
    "\n"
    "Full catalog (every verb + one-line \"when to use\"): call tool_info\n"
    "with {\"catalog\":true}. ToolSearch query \"mcp__ants\" loads schemas.\n"
    "EOF\n"
    // ANTS-1971 — feedback_query / feedback_log are kept OUT of the
    // always-on prelude (it must stay under the INV-10 1200-byte cap,
    // ANTS-1970) and surfaced CONDITIONALLY: only when the project
    // CC launched in keeps a cross-session *_Ants_MCP_Feedback.md
    // file, point contributors at the read/write tools so they stop
    // hand-editing it (format-drift risk). The glob is anchored to
    // $CLAUDE_PROJECT_DIR (CC sets it to the project root) falling
    // back to $PWD. Prints nothing — and so does not count against the
    // INV-10 cap — when no such file is present (the test case).\n"
    "if compgen -G \"${CLAUDE_PROJECT_DIR:-$PWD}/*_Ants_MCP_Feedback.md\""
    " >/dev/null 2>&1; then\n"
    "  echo\n"
    "  echo \"This project keeps a *_Ants_MCP_Feedback.md — use"
    " feedback_query (read the\"\n"
    "  echo \"un-triaged tail) / feedback_log (append a finding or"
    " tracking block)\"\n"
    "  echo \"instead of hand-editing it.\"\n"
    "fi\n";

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

// Bash-quote a path: wrap in single quotes; any embedded `'` becomes
// `'\''`. Required because `QStandardPaths::AppConfigLocation` builds
// `<home>/.config/Ants Terminal/...` from Qt's applicationName (capital
// + space) — unquoted, bash would split at the space. ANTS-1901.
QString bashQuote(const QString &s) {
    QString escaped = s;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// Build the `command` value Ants writes into settings.json. The path
// is bash-single-quoted so spaces survive hook-runner tokenisation.
QString hookCommand(const QString &scriptPath) {
    return QStringLiteral("bash ") + bashQuote(scriptPath);
}

// Build the {"type":"command", "command":"...", "timeout":3} entry.
QJsonObject hookCommandEntry(const QString &scriptPath) {
    QJsonObject entry;
    entry.insert(QStringLiteral("type"), QStringLiteral("command"));
    entry.insert(QStringLiteral("command"), hookCommand(scriptPath));
    entry.insert(QStringLiteral("timeout"), 3);
    return entry;
}

// INV-3 — sweep every SessionStart entry whose inner `hooks[]` array
// contains an entry with `command` matching the marker substring. The
// caller appends a single canonical entry afterwards. Walks back-to-
// front so removals don't invalidate later indices, and drops outer
// containers that become empty as their last inner entry is removed.
// Returns the number of entries removed. ANTS-1901 — earlier versions
// only updated the first match, leaving prior duplicates intact.
int removeAllAntsEntries(QJsonArray &sessionStart) {
    const QString marker = QString::fromUtf8(kMarkerSubstring);
    int removed = 0;
    for (int i = sessionStart.size() - 1; i >= 0; --i) {
        QJsonObject outer = sessionStart.at(i).toObject();
        QJsonArray inner = outer.value(QStringLiteral("hooks")).toArray();
        bool innerMutated = false;
        for (int j = inner.size() - 1; j >= 0; --j) {
            const QJsonObject e = inner.at(j).toObject();
            const QString cmd = e.value(QStringLiteral("command"))
                                 .toString();
            if (cmd.contains(marker)) {
                inner.removeAt(j);
                innerMutated = true;
                ++removed;
            }
        }
        if (!innerMutated) continue;
        if (inner.isEmpty()) {
            sessionStart.removeAt(i);
        } else {
            outer.insert(QStringLiteral("hooks"), inner);
            sessionStart.replace(i, outer);
        }
    }
    return removed;
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

    // ANTS-1901 — sweep every existing entry whose `command` carries
    // the marker. The add-path then appends a single canonical entry;
    // the remove-path simply stops after the sweep. Both operations
    // are idempotent and self-healing against settings.json files
    // that accumulated duplicates from buggy prior versions.
    const int removed = removeAllAntsEntries(sessionStart);

    if (add) {
        QJsonObject outer;
        QJsonArray inner;
        inner.append(hookCommandEntry(scriptPath));
        outer.insert(QStringLiteral("hooks"), inner);
        sessionStart.append(outer);
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
    if (add) return MergeOutcome::Added;
    return removed > 0 ? MergeOutcome::Removed : MergeOutcome::Kept;
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

    // ANTS-3562 — refuse to touch ~/.claude/settings.json when the resolved
    // orientation-script path lives OUTSIDE the settings file's home tree.
    // resolveScriptPath uses QStandardPaths::AppConfigLocation, which under
    // --e2e is redirected to a throwaway /tmp config dir; resolveSettingsPath
    // keeps the REAL ~/.claude. A naive install then wrote the user's real
    // settings a SessionStart hook pointing at the temp script, which dangles
    // the moment the e2e dir is reaped — a non-blocking "No such file" hook
    // error on every later Claude Code session until a normal relaunch rewrote
    // the path. Skip the whole install in that split-tree case; a normal launch
    // (config dir + ~/.claude under the same home) is unaffected. Self-contained
    // guard so any config redirection — not just --e2e — is covered by
    // construction. ok:true — this is a deliberate no-op, not a failure.
    {
        const QString homeRoot = QDir::cleanPath(
            homeDir.isEmpty() ? QDir::homePath() : homeDir);
        const QString cleanScript = QDir::cleanPath(r.scriptPath);
        const bool scriptUnderHome =
            cleanScript == homeRoot ||
            cleanScript.startsWith(homeRoot + QLatin1Char('/'));
        if (!scriptUnderHome) {
            r.ok = true;
            r.warning = QStringLiteral(
                "skipped SessionStart hook install: orientation script path "
                "(%1) is outside the settings-file home tree (%2) — refusing to "
                "write a cross-tree hook that would dangle (ANTS-3562; e.g. an "
                "--e2e run with a redirected config dir).").arg(cleanScript, homeRoot);
            qWarning().noquote() << "[mcp-orientation]" << (r.warning);
            return r;
        }
    }

    // Decide the script-write disposition per § 2.1.1.
    const QString runningVersion = QStringLiteral(ANTS_VERSION);
    const QString markerVersion = parseScriptMarkerVersion(r.scriptPath);
    bool scriptWritten = false;
    if (QFile::exists(r.scriptPath)) {
        if (markerVersion.isEmpty()) {
            // INV-2 — user-owned (no marker). Log + skip script write.
            r.warning = QStringLiteral(
                "user-owned %1 — Ants will not overwrite. Delete it "
                "to let Ants reinstall.").arg(r.scriptPath);
            qWarning().noquote() << "[mcp-orientation]" << (r.warning);
        } else {
            // ANTS-2038 — marker present → Ants-managed. Keep the body
            // in lockstep with the rendered template: rewrite on ANY
            // byte difference. This covers a version bump (the marker
            // line itself changes) AND an in-version template edit — a
            // prelude reword shipped without a version bump, which the
            // prior version-only gate (markerVersion == runningVersion
            // → skip) left stale on disk until the next bump (e.g.
            // ANTS-1985's catalog repoint). A byte-identical body is
            // the idempotent skip. INV-1 / INV-12.
            const QByteArray body = orientationScriptTemplate()
                                    .arg(runningVersion).toUtf8();
            QByteArray onDisk;
            QFile rf(r.scriptPath);
            if (rf.open(QIODevice::ReadOnly)) {
                onDisk = rf.readAll();
                rf.close();
            }
            if (onDisk != body) {
                QString w;
                if (writeScriptFile(r.scriptPath, body, &w)) {
                    scriptWritten = true;
                } else {
                    r.warning = w;
                }
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
