#include "debuglog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>

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
    {DebugLog::Perf,     "perf"},
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

void DebugLog::setActive(quint32 mask) {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_active = mask;
    // Mirror to stderr only when the env var kicked us on at startup
    // (so `2> file.log` still captures output). Runtime toggles from
    // the menu only write to the file — we don't want to spam stderr
    // on a user-initiated debug session.
    s_alsoStderr = qEnvironmentVariableIsSet("ANTS_DEBUG") && mask != 0;
    if (mask != 0 && !s_file.isOpen()) {
        const QString path = logFilePath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        s_file.setFileName(path);
        // Append — don't truncate; user can clear() on demand.
        if (!s_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            fprintf(stderr, "DebugLog: could not open %s\n", qPrintable(path));
        }
        if (s_file.isOpen()) {
            const QString header = QStringLiteral(
                "\n=== debug log opened at %1 (pid %2, categories=0x%3) ===\n")
                .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
                .arg(QCoreApplication::applicationPid())
                .arg(mask, 0, 16);
            s_file.write(header.toUtf8());
            s_file.flush();
        }
    }
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
