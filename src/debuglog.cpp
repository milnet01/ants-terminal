#include "debuglog.h"

#include "secureio.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

#include <sys/stat.h>
#include <sys/types.h>

namespace {
// ANTS-1170: cap on-disk debug log size. When a fresh open() finds an
// existing log larger than this, it's renamed to debug.log.1 (replacing
// any prior .1) before the new log is opened. One generation is enough
// for typical "I just hit the bug, let me grab the log" workflows
// without growing unboundedly across forgotten ANTS_DEBUG=all sessions.
constexpr qint64 kMaxLogBytes = 10 * 1024 * 1024;
} // namespace

std::mutex DebugLog::s_mutex;
QFile DebugLog::s_file;
quint32 DebugLog::s_active = 0;
bool DebugLog::s_alsoStderr = false;

static const struct {
    DebugLog::Category cat;
    const char *name;
} kCategoryTable[] = {
    {DebugLog::Paint,    "paint"},
    {DebugLog::Events,   "events"},
    {DebugLog::Input,    "input"},
    {DebugLog::Pty,      "pty"},
    {DebugLog::Vt,       "vt"},
    {DebugLog::Render,   "render"},
    {DebugLog::Plugins,  "plugins"},
    {DebugLog::Network,  "network"},
    {DebugLog::Config,   "config"},
    {DebugLog::Audit,    "audit"},
    {DebugLog::Claude,   "claude"},
    {DebugLog::Signals,  "signals"},
    {DebugLog::Shell,    "shell"},
    {DebugLog::Session,  "session"},
    {DebugLog::Perf,        "perf"},
    {DebugLog::Scrollback,  "scrollback"},
    {DebugLog::AutoSwitch,  "autoswitch"},
};

quint32 DebugLog::parseCategories(const QString &spec) {
    quint32 mask = 0;
    if (spec.isEmpty()) return mask;
    // Accept numeric (legacy: ANTS_DEBUG=1 means paint) and named.
    bool numOk = false;
    int numeric = spec.toInt(&numOk);
    if (numOk) {
        if (numeric == 1) return Paint;
        if (numeric >= 2) return Paint | Events;
        return 0;
    }
    const QStringList tokens = spec.split(',', Qt::SkipEmptyParts);
    for (const QString &tRaw : tokens) {
        QString t = tRaw.trimmed().toLower();
        if (t == "all") return All;
        if (t == "none" || t == "off") return 0;
        Category c = categoryFor(t);
        if (c != None) mask |= c;
    }
    return mask;
}

quint32 DebugLog::active() { return s_active; }

void DebugLog::openLogFileLocked() {
    if (s_active == 0 || s_file.isOpen()) return;
    const QString path = logFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    // ANTS-1170: rotate when the existing log exceeds the cap, so a
    // forgotten ANTS_DEBUG=all session can't write secrets
    // indefinitely. Single-generation rotation: debug.log → .log.1
    // (replacing any prior .1).
    QFileInfo existing(path);
    if (existing.exists() && existing.size() > kMaxLogBytes) {
        const QString rotated = path + QStringLiteral(".1");
        QFile::remove(rotated);
        QFile::rename(path, rotated);
    }
    s_file.setFileName(path);
    // ANTS-1142 — tighten umask BEFORE create so the kernel
    // applies 0600 perms at file-creation time, eliminating
    // the same-UID race window between open() and the
    // post-open setOwnerOnlyPerms call. Save/restore so the
    // tighter umask doesn't leak into other parts of the
    // process. The 0077 mask zeros every group/other perm
    // bit at create; combined with the QFile::open default
    // 0666 request, the kernel produces 0600.
    const mode_t prevMask = ::umask(0077);
    // Append — don't truncate; user can clear() on demand.
    if (!s_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        fprintf(stderr, "DebugLog: could not open %s\n", qPrintable(path));
    }
    ::umask(prevMask);
    if (s_file.isOpen()) {
        // Owner-only. The log can contain PTY output (keystrokes),
        // network responses (API bodies), and HMAC material from
        // OSC 133 forgery detection. Apply both via the open fd
        // and via path — the fd-level call covers the just-opened
        // descriptor; the path-level call covers the on-disk inode
        // in case append reused a pre-existing file that a prior
        // (pre-fix) run left at the process umask.
        setOwnerOnlyPerms(s_file);
        setOwnerOnlyPerms(path);
        const QString header = QStringLiteral(
            "\n=== debug log opened at %1 (pid %2, categories=0x%3) ===\n")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
            .arg(QCoreApplication::applicationPid())
            .arg(s_active, 0, 16);
        s_file.write(header.toUtf8());
        s_file.flush();
    }
}

void DebugLog::setActive(quint32 mask) {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_active = mask;
    // Mirror to stderr only when the env var kicked us on at startup
    // (so `2> file.log` still captures output). Runtime toggles from
    // the menu only write to the file — we don't want to spam stderr
    // on a user-initiated debug session.
    s_alsoStderr = qEnvironmentVariableIsSet("ANTS_DEBUG") && mask != 0;
    openLogFileLocked();
}

QString DebugLog::logFilePath() {
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation) + "/ants-terminal";
    return dir + "/debug.log";
}

void DebugLog::clear() {
    std::lock_guard<std::mutex> lk(s_mutex);
    if (s_file.isOpen()) s_file.close();
    QFile::remove(logFilePath());
    // ANTS-1190 — reopen if categories remain active. Without this,
    // the menu's "Clear Log File" action permanently silences future
    // ANTS_LOG calls until the user toggles a category off-and-on
    // (which is the only other path that runs setActive). User repro
    // 2026-05-08: enabled VT/Claude/Perf, clicked Clear, ran Flask,
    // saw an empty log file (actually no file at all). Now we keep
    // the sink open so the next write lands on disk.
    openLogFileLocked();
}

void DebugLog::write(Category c, const QString &message) {
    // The enabled() check happens in the macro — by the time we're
    // here, we know the category is active or it's an ANTS_LOG_ALWAYS.
    // Still, guard against stray calls with no active bits.
    if (c != None && (s_active & c) == 0) return;

    std::lock_guard<std::mutex> lk(s_mutex);
    const QByteArray line = QStringLiteral("[%1] %2  %3\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-ddTHH:mm:ss.zzz"))
        .arg(QString::asprintf("%-8s", nameFor(c)))
        .arg(message)
        .toUtf8();

    if (s_file.isOpen()) {
        s_file.write(line);
        s_file.flush();
    }
    if (s_alsoStderr) {
        fwrite(line.constData(), 1, line.size(), stderr);
    }
}

QStringList DebugLog::categoryNames() {
    QStringList out;
    for (const auto &e : kCategoryTable) out << QString::fromLatin1(e.name);
    return out;
}

DebugLog::Category DebugLog::categoryFor(const QString &name) {
    for (const auto &e : kCategoryTable) {
        if (QString::fromLatin1(e.name) == name) return e.cat;
    }
    return None;
}

const char *DebugLog::nameFor(Category c) {
    if (c == None) return "info";
    for (const auto &e : kCategoryTable) {
        if (e.cat == c) return e.name;
    }
    return "?";
}
