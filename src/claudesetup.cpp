// ANTS-5558 — see claudesetup.h. The two hook installers moved here from
// SettingsDialog unchanged in what they write; they return an Outcome where
// they used to raise a message box, so the welcome dialog and Settings share
// one copy. Contract: docs/specs/ANTS-5558-welcome-dialog.md.

#include "claudesetup.h"

#include "configbackup.h"
#include "configpaths.h"
#include "mcpdversion.h"
#include "secureio.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace ants::claude_setup {

namespace {

Outcome fail(const QString &message) { return {false, message}; }

QString claudeSettingsPath() { return ConfigPaths::claudeSettingsJson(); }
QString claudeJsonPath() { return QDir::homePath() + QStringLiteral("/.claude.json"); }

// ANTS-2205 — single source of truth for the hook events installStatusHooks()
// writes, so statusHooksStatus() cannot drift to verifying a subset.
// ANTS-4457 — this list must also match the `hookName == "..."` chain in
// ClaudeIntegration::processHookEvent: a missing entry is silent in both
// directions. tests/features/hook_events_wired compares the two sets.
const QStringList &claudeHookEvents() {
    static const QStringList events{
        QStringLiteral("SessionStart"),   QStringLiteral("PreToolUse"),
        QStringLiteral("PostToolUse"),    QStringLiteral("Stop"),
        QStringLiteral("PreCompact"),     QStringLiteral("PermissionRequest"),
        QStringLiteral("PostToolUseFailure")};
    return events;
}

// Writes an executable helper script, creating its directory.
Outcome writeScript(const QString &scriptPath, const QString &script) {
    const QString scriptDir = QFileInfo(scriptPath).absolutePath();
    if (!QDir().mkpath(scriptDir))
        return fail(QStringLiteral("Could not create %1").arg(scriptDir));
    QSaveFile sf(scriptPath);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Could not write %1").arg(scriptPath));
    sf.write(script.toUtf8());
    if (!sf.commit())
        return fail(QStringLiteral("Write failed for %1").arg(scriptPath));
    QFile::setPermissions(scriptPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ExeOwner | QFileDevice::ReadGroup |
        QFileDevice::ReadOther | QFileDevice::ExeGroup |
        QFileDevice::ExeOther);
    return {true, {}};
}

// Reads ~/.claude/settings.json into `root`. A missing file is an empty
// object. A file that EXISTS but fails to parse is REFUSED — writing back
// would leave only `{"hooks": {...}}`, silently destroying every non-hooks
// key (model, env, permissions). 0.7.12 /indie-review cross-cutting fix.
Outcome readClaudeSettings(const QString &settingsPath, QJsonObject &root) {
    QDir().mkpath(QFileInfo(settingsPath).absolutePath());
    QFile rf(settingsPath);
    if (!rf.exists()) return {true, {}};
    if (!rf.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Could not read %1 — refusing to overwrite. "
                                   "Check file permissions and retry.")
                        .arg(settingsPath));
    const QByteArray raw = rf.readAll();
    rf.close();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (doc.isObject()) {
        root = doc.object();
        return {true, {}};
    }
    const QString backup = rotateCorruptFileAside(settingsPath);
    return fail(QStringLiteral("%1 failed to parse (%2). Refusing to overwrite "
                               "and risk clobbering non-hook keys.\n\n"
                               "A backup of the broken file was written to:\n%3\n\n"
                               "Hand-fix the file and retry.")
                    .arg(settingsPath, err.errorString(),
                         backup.isEmpty() ? QStringLiteral("(backup copy FAILED)")
                                          : backup));
}

// Writes ~/.claude/settings.json under the shared writer lock, owner-only.
// Serialised against concurrent writers (the Allowlist dialog, another
// install, `jq -i` by the user): QSaveFile's last-rename-wins would otherwise
// drop a sibling writer's permissions block.
Outcome writeClaudeSettings(const QString &settingsPath, const QJsonObject &root) {
    ConfigWriteLock writeLock(settingsPath);
    if (!writeLock.acquired())
        return fail(QStringLiteral("Another writer holds the lock on %1 — try "
                                   "again in a moment.").arg(settingsPath));
    QSaveFile settingsOut(settingsPath);
    if (!settingsOut.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Could not write %1").arg(settingsPath));
    if (!setOwnerOnlyPerms(settingsOut))  // 0600 on temp fd
        warnNotOwnerOnly(settingsPath, "Claude Code settings");
    settingsOut.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!settingsOut.commit())
        return fail(QStringLiteral("Commit failed for %1").arg(settingsPath));
    if (!setOwnerOnlyPerms(settingsPath))  // belt-and-suspenders post-rename
        warnNotOwnerOnly(settingsPath, "Claude Code settings");
    return {true, {}};
}

// Does any entry of a hook event's array run `command`?
bool eventRuns(const QJsonArray &entries, const QString &command) {
    for (const auto &v : entries)
        for (const auto &h : v.toObject().value(QStringLiteral("hooks")).toArray())
            if (h.toObject().value(QStringLiteral("command")).toString() == command)
                return true;
    return false;
}

QJsonObject hookEntry(const QString &scriptPath, bool withMatcher) {
    QJsonObject hook;
    hook[QStringLiteral("type")] = QStringLiteral("command");
    hook[QStringLiteral("command")] = scriptPath;
    hook[QStringLiteral("timeout")] = 2;
    QJsonObject wrapper;
    if (withMatcher) wrapper[QStringLiteral("matcher")] = QString();
    wrapper[QStringLiteral("hooks")] = QJsonArray{hook};
    return wrapper;
}

QJsonObject readHooks() {
    QFile sf(claudeSettingsPath());
    if (!sf.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(sf.readAll()).object()
        .value(QStringLiteral("hooks")).toObject();
}

// --- marked blocks, shared by the CLAUDE.md note and shell integration ----

QByteArray readBytes(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeBytes(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(bytes);
    return f.commit();
}

// The body between `begin` and `end`, or a null array when there is no pair.
QByteArray blockBody(const QByteArray &text, const QByteArray &begin,
                     const QByteArray &end) {
    const qsizetype b = text.indexOf(begin);
    const qsizetype e = b < 0 ? -1 : text.indexOf(end, b);
    if (b < 0 || e < 0) return {};
    const qsizetype from = b + begin.size();
    return text.mid(from, e - from);
}

// Replaces the marked block in place, or appends it; every byte outside the
// block is kept.
QByteArray withBlock(const QByteArray &text, const QByteArray &begin,
                     const QByteArray &end, const QByteArray &body) {
    const QByteArray block = begin + '\n' + body + '\n' + end;
    const qsizetype b = text.indexOf(begin);
    const qsizetype e = b < 0 ? -1 : text.indexOf(end, b);
    if (b >= 0 && e >= 0)
        return text.left(b) + block + text.mid(e + end.size());
    QByteArray out = text;
    if (!out.isEmpty() && !out.endsWith('\n')) out += '\n';
    return out + block + '\n';
}

QByteArray withoutBlock(const QByteArray &text, const QByteArray &begin,
                        const QByteArray &end) {
    const qsizetype b = text.indexOf(begin);
    const qsizetype e = b < 0 ? -1 : text.indexOf(end, b);
    if (b < 0 || e < 0) return text;
    qsizetype after = e + end.size();
    if (after < text.size() && text.at(after) == '\n') ++after;
    return text.left(b) + text.mid(after);
}

constexpr char kMdBegin[] = "<!-- ants-terminal:begin -->";
constexpr char kMdEnd[]   = "<!-- ants-terminal:end -->";
constexpr char kShBegin[] = "# >>> ants-terminal shell integration >>>";
constexpr char kShEnd[]   = "# <<< ants-terminal shell integration <<<";

}  // namespace

bool claudeCodeDetected() {
    return !QStandardPaths::findExecutable(QStringLiteral("claude")).isEmpty() ||
           QFileInfo(QDir::homePath() + QStringLiteral("/.claude")).isDir();
}

// --- status-bar hooks -----------------------------------------------------
//
// Installs ~/.config/ants-terminal/hooks/claude-forward.sh, which walks up the
// process tree to the nearest ants-terminal and forwards the hook event JSON
// on stdin to that instance's socket at /tmp/ants-claude-hooks-<pid>, and
// merges one entry per claudeHookEvents() event into ~/.claude/settings.json.
// Hooks the user already has are preserved.

Status statusHooksStatus() {
    const bool scriptPresent = QFile::exists(ConfigPaths::antsClaudeForwardScript());
    const QJsonObject hooks = readHooks();
    bool hooksWired = true;   // ANTS-2205 — EVERY event the installer writes
    for (const QString &event : claudeHookEvents())
        if (!hooks.contains(event)) { hooksWired = false; break; }
    if (scriptPresent && hooksWired)
        return {State::Installed,
                QStringLiteral("✓ Hooks installed. Status updates in real time.")};
    if (scriptPresent)
        return {State::Partial,
                QStringLiteral("Helper script present but ~/.claude/settings.json "
                               "is missing some hook entries.")};
    return {State::Missing,
            QStringLiteral("Not installed. Status updates fall back to the "
                           "~50 ms transcript-file watcher.")};
}

Outcome installStatusHooks() {
    const QString scriptPath = ConfigPaths::antsClaudeForwardScript();
    // Python3 for the socket send: every Linux desktop that runs Claude Code
    // has it, where socat is not universal. Timeouts keep a slow hook from
    // blocking Claude Code's turn.
    Outcome wrote = writeScript(scriptPath, QStringLiteral(
        "#!/bin/bash\n"
        "# Ants Terminal — Claude Code hook forwarder.\n"
        "# Walks up the process tree to find the parent ants-terminal\n"
        "# and forwards the hook event JSON on stdin to its Unix socket.\n"
        "pid=$PPID\n"
        "for _ in $(seq 1 20); do\n"
        "    comm=$(cat /proc/$pid/comm 2>/dev/null) || break\n"
        "    if [[ \"$comm\" == \"ants-terminal\" ]]; then\n"
        "        sock=\"/tmp/ants-claude-hooks-$pid\"\n"
        "        if [[ -S \"$sock\" ]]; then\n"
        "            timeout 1 python3 -c \"\n"
        "import os, socket, struct, sys\n"
        "s = socket.socket(socket.AF_UNIX)\n"
        "s.settimeout(1.0)\n"
        "s.connect('$sock')\n"
        // /tmp is world-writable: send the hook event (commands, file paths)
        // only to a socket this user's process is listening on.
        "u = struct.unpack('3i', s.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[1]\n"
        "if u != os.getuid(): sys.exit(0)\n"
        "s.sendall(sys.stdin.buffer.read())\n"
        "s.shutdown(socket.SHUT_WR)\n"
        "s.close()\n"
        "\" 2>/dev/null\n"
        "            exit 0\n"
        "        fi\n"
        "    fi\n"
        "    ppid=$(awk '{print $4}' /proc/$pid/stat 2>/dev/null) || break\n"
        "    [[ -z \"$ppid\" || \"$ppid\" -le 1 ]] && break\n"
        "    pid=$ppid\n"
        "done\n"
        "exit 0\n"));
    if (!wrote.ok) return wrote;

    const QString settingsPath = claudeSettingsPath();
    QJsonObject root;
    Outcome read = readClaudeSettings(settingsPath, root);
    if (!read.ok) return read;
    QJsonObject hooks = root.value(QStringLiteral("hooks")).toObject();
    for (const QString &event : claudeHookEvents()) {
        // Only append where our script is not already referenced, keeping
        // user-added hooks on the same event intact.
        QJsonArray existing = hooks.value(event).toArray();
        if (eventRuns(existing, scriptPath)) continue;
        existing.append(hookEntry(scriptPath, false));
        hooks[event] = existing;
    }
    root[QStringLiteral("hooks")] = hooks;
    Outcome written = writeClaudeSettings(settingsPath, root);
    if (!written.ok) return written;
    return {true, QStringLiteral("Installed Ants Terminal status-bar hooks.\n\n"
                                 "Script: %1\nSettings: %2\n\n"
                                 "Real-time Claude status updates will take "
                                 "effect for new Claude Code sessions.")
                      .arg(scriptPath, settingsPath)};
}

// --- git-context hook -----------------------------------------------------
//
// A UserPromptSubmit hook printing a <git-context> block, so Claude sees repo
// state without spending tokens on `git status`. Contract:
// tests/features/claude_git_context_hook/spec.md.

Status gitContextStatus() {
    const QString scriptPath = ConfigPaths::antsClaudeGitContextScript();
    const bool scriptPresent = QFile::exists(scriptPath);
    const bool hookWired = eventRuns(
        readHooks().value(QStringLiteral("UserPromptSubmit")).toArray(), scriptPath);
    if (scriptPresent && hookWired)
        return {State::Installed,
                QStringLiteral("✓ Installed globally. Every Claude Code prompt "
                               "carries a <git-context> block.")};
    if (scriptPresent)
        return {State::Partial,
                QStringLiteral("Helper script present but the UserPromptSubmit "
                               "entry is missing from ~/.claude/settings.json.")};
    return {State::Missing,
            QStringLiteral("Not installed. Claude Code will run `git status` "
                           "via Bash when it needs repo state (~500 tokens "
                           "per turn).")};
}

Outcome installGitContextHook() {
    const QString scriptPath = ConfigPaths::antsClaudeGitContextScript();
    // Runs on every prompt: fast (<100 ms typical) and a silent no-op outside
    // a git repo, so non-repo projects get no chatter. Spec § 3.
    Outcome wrote = writeScript(scriptPath, QStringLiteral(
        "#!/bin/bash\n"
        "# Ants Terminal — Claude Code UserPromptSubmit git-context hook.\n"
        "# Prints a <git-context> block so Claude sees repo state without\n"
        "# spending tokens on `git status`. Silent no-op outside a repo.\n"
        "set -u\n"
        "cwd=\"${CLAUDE_PROJECT_DIR:-$PWD}\"\n"
        "cd \"$cwd\" 2>/dev/null || exit 0\n"
        "command -v git >/dev/null 2>&1 || exit 0\n"
        "git rev-parse --is-inside-work-tree >/dev/null 2>&1 || exit 0\n"
        "\n"
        "branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null)\n"
        "if [[ \"$branch\" == \"HEAD\" ]]; then\n"
        "    branch=$(git rev-parse --short=7 HEAD 2>/dev/null)\n"
        "fi\n"
        "[[ -z \"$branch\" ]] && exit 0\n"
        "\n"
        "upstream=$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)\n"
        "ahead_behind=\"\"\n"
        "if [[ -n \"$upstream\" ]]; then\n"
        "    counts=$(git rev-list --left-right --count \"@{u}...HEAD\" 2>/dev/null || true)\n"
        "    if [[ -n \"$counts\" ]]; then\n"
        "        behind=$(awk '{print $1}' <<< \"$counts\")\n"
        "        ahead=$(awk '{print $2}' <<< \"$counts\")\n"
        "        ahead_behind=\" (ahead $ahead, behind $behind)\"\n"
        "    fi\n"
        "fi\n"
        "\n"
        "porcelain=$(GIT_OPTIONAL_LOCKS=0 git status --porcelain 2>/dev/null || true)\n"
        "staged=0; unstaged=0; untracked=0\n"
        "if [[ -n \"$porcelain\" ]]; then\n"
        "    while IFS= read -r line; do\n"
        "        x=\"${line:0:1}\"\n"
        "        y=\"${line:1:1}\"\n"
        "        if [[ \"$x\" == \"?\" && \"$y\" == \"?\" ]]; then\n"
        "            untracked=$((untracked + 1))\n"
        "            continue\n"
        "        fi\n"
        "        case \"$x\" in [MADRC]) staged=$((staged + 1));; esac\n"
        "        case \"$y\" in [MDARC]) unstaged=$((unstaged + 1));; esac\n"
        "    done <<< \"$porcelain\"\n"
        "fi\n"
        "\n"
        "printf '<git-context>\\n'\n"
        "printf 'Branch: %s%s\\n' \"$branch\" \"$ahead_behind\"\n"
        "printf 'Upstream: %s\\n' \"${upstream:-(none)}\"\n"
        "printf 'Staged: %s file(s)\\n' \"$staged\"\n"
        "printf 'Unstaged: %s file(s)\\n' \"$unstaged\"\n"
        "printf 'Untracked: %s file(s)\\n' \"$untracked\"\n"
        "printf '</git-context>\\n'\n"));
    if (!wrote.ok) return wrote;

    const QString settingsPath = claudeSettingsPath();
    QJsonObject root;
    Outcome read = readClaudeSettings(settingsPath, root);
    if (!read.ok) return read;
    QJsonObject hooks = root.value(QStringLiteral("hooks")).toObject();
    QJsonArray existing = hooks.value(QStringLiteral("UserPromptSubmit")).toArray();
    // Only append where our script is not already referenced, keeping the
    // user's own UserPromptSubmit hooks intact alongside ours.
    if (!eventRuns(existing, scriptPath)) {
        existing.append(hookEntry(scriptPath, true));
        hooks[QStringLiteral("UserPromptSubmit")] = existing;
        root[QStringLiteral("hooks")] = hooks;
        Outcome written = writeClaudeSettings(settingsPath, root);
        if (!written.ok) return written;
    }
    return {true, QStringLiteral("Installed globally (applies to every project).\n\n"
                                 "Script: %1\nSettings: %2\n\n"
                                 "Effective for new Claude Code sessions. Claude "
                                 "Code will see a <git-context> block on every "
                                 "user prompt.")
                      .arg(scriptPath, settingsPath)};
}

// --- MCP registration -----------------------------------------------------

QStringList mcpLaunchCommand(const QString &appImage, const QString &appDir) {
    // An AppImage's AppRun routes `--mcpd` to its bundled ants-mcpd; the mount
    // path of that binary changes every launch, so the AppImage is registered.
    if (!appImage.isEmpty()) return {appImage, QStringLiteral("--mcpd")};
    // The sibling, else PATH — never the registered command (spec § 2.4).
    const QString bin = mcpd::locateBinary(QString(), appDir);
    return bin.isEmpty() ? QStringList{} : QStringList{bin};
}

QList<QStringList> mcpRegistrationCommands(const QStringList &launch,
                                           bool alreadyRegistered) {
    QList<QStringList> commands;
    // `claude mcp add` refuses an existing name, so a re-registration removes
    // the user-scope entry first.
    if (alreadyRegistered)
        commands.append({QStringLiteral("mcp"), QStringLiteral("remove"),
                         QStringLiteral("--scope"), QStringLiteral("user"),
                         QStringLiteral("ants")});
    QStringList add{QStringLiteral("mcp"), QStringLiteral("add"),
                    QStringLiteral("--scope"), QStringLiteral("user"),
                    QStringLiteral("ants"), QStringLiteral("--")};
    add << launch;
    commands.append(add);
    return commands;
}

namespace {
QStringList thisBuildLaunch() {
    return mcpLaunchCommand(qEnvironmentVariable("APPIMAGE"),
                            QCoreApplication::applicationDirPath());
}
}  // namespace

Status mcpStatus() {
    if (QStandardPaths::findExecutable(QStringLiteral("claude")).isEmpty())
        return {State::Unavailable, QStringLiteral("the claude program is not on PATH")};
    const QStringList launch = thisBuildLaunch();
    if (launch.isEmpty())
        return {State::Unavailable,
                QStringLiteral("no ants-mcpd was found beside Ants Terminal or on PATH")};
    const mcpd::Launch reg = mcpd::registeredLaunch(claudeJsonPath());
    const int perProject = mcpd::projectRegistrationCount(claudeJsonPath());
    const QString projectNote = perProject > 0
        ? QStringLiteral(" %1 per-project registration(s) named ants still win "
                         "in their own projects.").arg(perProject)
        : QString();
    if (reg.program.isEmpty())
        return {State::Missing,
                QStringLiteral("Not connected. Claude Code does not know the Ants "
                               "toolkit yet.") + projectNote};
    if (QStringList{reg.program} + reg.args == launch)
        return {State::Installed,
                QStringLiteral("✓ Connected to this build of Ants.") + projectNote};
    return {State::Partial,
            QStringLiteral("Connected to a different ants-mcpd (%1); reinstall "
                           "to use this build.").arg(reg.program) + projectNote};
}

Outcome registerMcp() {
    const QString claude = QStandardPaths::findExecutable(QStringLiteral("claude"));
    if (claude.isEmpty()) return fail(QStringLiteral("the claude program is not on PATH"));
    const QStringList launch = thisBuildLaunch();
    if (launch.isEmpty())
        return fail(QStringLiteral("no ants-mcpd was found beside Ants Terminal or on PATH"));
    const bool registered = !mcpd::registeredLaunch(claudeJsonPath()).program.isEmpty();
    for (const QStringList &argv : mcpRegistrationCommands(launch, registered)) {
        QProcess p;   // argv form, never a shell string
        p.start(claude, argv);
        if (!p.waitForFinished(30000) || p.exitStatus() != QProcess::NormalExit ||
            p.exitCode() != 0) {
            const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
            return fail(QStringLiteral("`claude %1` failed: %2")
                            .arg(argv.join(QLatin1Char(' ')),
                                 err.isEmpty() ? p.errorString() : err));
        }
    }
    return {true, QStringLiteral("Connected. Claude Code sessions started from now "
                                 "on have the Ants toolkit; running ones get it "
                                 "after /mcp reconnects.")};
}

// --- the CLAUDE.md note ---------------------------------------------------

QString defaultClaudeMdPath() {
    return QDir::homePath() + QStringLiteral("/.claude/CLAUDE.md");
}

QString claudeMdNoteText() {
    return QStringLiteral(
        "## Ants Terminal\n"
        "\n"
        "When Claude Code runs inside Ants Terminal, its MCP tools are available "
        "as `mcp__ants__*`.\n"
        "They are usually cheaper than raw file reads and searches: prefer them "
        "where one fits.\n"
        "Call `session_orient` first in a new session.");
}

Status claudeMdNoteStatus(const QString &path) {
    const QByteArray text = readBytes(path);
    const bool begin = text.contains(kMdBegin), end = text.contains(kMdEnd);
    if (begin && end)
        return {State::Installed, QStringLiteral("✓ The Ants note is in %1.").arg(path)};
    if (begin || end)
        return {State::Partial,
                QStringLiteral("%1 holds only one of the note's markers.").arg(path)};
    return {State::Missing, QStringLiteral("Not added. Optional.")};
}

Outcome installClaudeMdNote(const QString &path) {
    const QByteArray text = readBytes(path);
    if (!writeBytes(path, withBlock(text, kMdBegin, kMdEnd,
                                    claudeMdNoteText().toUtf8())))
        return fail(QStringLiteral("Could not write %1").arg(path));
    return {true, QStringLiteral("Added the Ants note to %1.").arg(path)};
}

Outcome removeClaudeMdNote(const QString &path) {
    const QByteArray text = readBytes(path);
    if (!writeBytes(path, withoutBlock(text, kMdBegin, kMdEnd)))
        return fail(QStringLiteral("Could not write %1").arg(path));
    return {true, QStringLiteral("Removed the Ants note from %1.").arg(path)};
}

// --- shell integration ----------------------------------------------------

namespace {

struct ShellPlan {
    Status  unavailable;   // state Unavailable when the plan cannot run
    QString shell;         // bash or zsh
    QString script;        // the script found in the search list
    QString sourced;       // the path the rc file sources
    QString keyFile;       // ~/.profile or ~/.zshenv
    QString rcFile;        // ~/.bashrc or ~/.zshrc
};

ShellPlan planShell(const QStringList &scriptDirs) {
    ShellPlan p;
    p.unavailable.state = State::Missing;
    p.shell = QFileInfo(qEnvironmentVariable("SHELL")).fileName();
    if (p.shell != QLatin1String("bash") && p.shell != QLatin1String("zsh")) {
        p.unavailable = {State::Unavailable,
                         QStringLiteral("supported for bash and zsh")};
        return p;
    }
    for (const QString &dir : scriptDirs) {
        const QString candidate =
            dir + QStringLiteral("/ants-osc133.") + p.shell;
        if (QFileInfo(candidate).isFile()) { p.script = candidate; break; }
    }
    if (p.script.isEmpty()) {
        p.unavailable = {State::Unavailable,
                         QStringLiteral("the shell-integration scripts are not in "
                                        "this install")};
        return p;
    }
    const QString home = QDir::homePath();
    // An AppImage's mount path changes every launch; source a per-user copy.
    p.sourced = qEnvironmentVariableIsEmpty("APPIMAGE")
        ? p.script
        : home + QStringLiteral("/.local/share/ants-terminal/shell-integration/"
                                "ants-osc133.") + p.shell;
    const bool bash = p.shell == QLatin1String("bash");
    p.keyFile = home + (bash ? QStringLiteral("/.profile") : QStringLiteral("/.zshenv"));
    p.rcFile  = home + (bash ? QStringLiteral("/.bashrc")  : QStringLiteral("/.zshrc"));
    return p;
}

// The key an existing block exports, so a reinstall does not regenerate it.
QByteArray existingKey(const QString &keyFile) {
    static const QRegularExpression re(
        QStringLiteral(R"re(ANTS_OSC133_KEY="([0-9a-f]{64})")re"));
    const auto m = re.match(QString::fromUtf8(
        blockBody(readBytes(keyFile), kShBegin, kShEnd)));
    return m.hasMatch() ? m.captured(1).toUtf8() : QByteArray();
}

QByteArray newKey() {
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(
        reinterpret_cast<quint32 *>(raw.data()),
        reinterpret_cast<quint32 *>(raw.data() + raw.size()));
    return raw.toHex();
}

QByteArray keyLine(const QByteArray &key) {
    return "export ANTS_OSC133_KEY=\"" + key + '"';
}

QByteArray sourceLine(const QString &sourced) {
    const QByteArray q = '"' + sourced.toUtf8() + '"';
    return "[ -f " + q + " ] && source " + q;
}

}  // namespace

QStringList defaultShellScriptDirs() {
    const QString rel = QStringLiteral("/share/ants-terminal/shell-integration");
    return {QDir::cleanPath(QCoreApplication::applicationDirPath() +
                            QStringLiteral("/..") + rel),
            QStringLiteral("/usr/local") + rel, QStringLiteral("/usr") + rel};
}

QString shellIntegrationPreview(const QStringList &scriptDirs) {
    const ShellPlan p = planShell(scriptDirs);
    if (p.unavailable.state == State::Unavailable) return p.unavailable.detail;
    const QByteArray key = existingKey(p.keyFile);
    return QStringLiteral("In %1:\n%2\n%3\n%4\n\nIn %5:\n%2\n%6\n%4\n\n"
                          "The key takes effect at your next login; the source "
                          "line in a new shell.")
        .arg(p.keyFile, QString::fromUtf8(kShBegin),
             QString::fromUtf8(keyLine(key.isEmpty() ? QByteArray("<a new random key>")
                                                     : key)),
             QString::fromUtf8(kShEnd), p.rcFile,
             QString::fromUtf8(sourceLine(p.sourced)));
}

Status shellIntegrationStatus(const QStringList &scriptDirs) {
    const ShellPlan p = planShell(scriptDirs);
    if (p.unavailable.state == State::Unavailable) return p.unavailable;
    const bool keyBlock = !blockBody(readBytes(p.keyFile), kShBegin, kShEnd).isNull();
    const bool rcBlock  = !blockBody(readBytes(p.rcFile), kShBegin, kShEnd).isNull();
    if (keyBlock && rcBlock && QFileInfo(p.sourced).isFile())
        return {State::Installed, QStringLiteral("✓ Installed for %1.").arg(p.shell)};
    if (keyBlock || rcBlock)
        return {State::Partial,
                QStringLiteral("Partly installed; reinstall to repair it.")};
    return {State::Missing, QStringLiteral("Not installed.")};
}

Outcome installShellIntegration(const QStringList &scriptDirs) {
    const ShellPlan p = planShell(scriptDirs);
    if (p.unavailable.state == State::Unavailable) return fail(p.unavailable.detail);
    if (p.sourced != p.script) {
        const QByteArray body = readBytes(p.script);
        if (body.isEmpty() || !writeBytes(p.sourced, body))
            return fail(QStringLiteral("Could not copy %1 to %2").arg(p.script, p.sourced));
    }
    QByteArray key = existingKey(p.keyFile);
    if (key.isEmpty()) key = newKey();
    if (!writeBytes(p.keyFile, withBlock(readBytes(p.keyFile), kShBegin, kShEnd,
                                         keyLine(key))))
        return fail(QStringLiteral("Could not write %1").arg(p.keyFile));
    if (!writeBytes(p.rcFile, withBlock(readBytes(p.rcFile), kShBegin, kShEnd,
                                        sourceLine(p.sourced))))
        return fail(QStringLiteral("Could not write %1").arg(p.rcFile));
    return {true, QStringLiteral("Shell integration installed for %1. The key takes "
                                 "effect at your next login; the source line in a "
                                 "new shell.").arg(p.shell)};
}

}  // namespace ants::claude_setup
