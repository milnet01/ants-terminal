// ANTS-1251 — subsystemmap implementation. See subsystemmap.h.

#include "subsystemmap.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>

namespace SubsystemMap {

namespace {

struct CacheEntry {
    qint64        mtimeMs = -1;
    QVector<Lane> lanes;
};

// Cache keyed on canonical CLAUDE.md path. Multiple projects in one
// session would each get an entry; bounded by number of projects
// (small).
QHash<QString, CacheEntry> &cache() {
    static QHash<QString, CacheEntry> g;
    return g;
}

// Bullet shape:  "- `name` [(qualifier)] [/ `name2`] — summary..."
// Strategy: split on the first ` — ` (em-dash U+2014) or ` -- ` in
// the line, harvest every backticked token in the prefix, and emit
// one Lane per harvested name with the shared summary as the body.
// Tolerates qualifier parens ("(QOpenGLWidget)") and slash-multi
// names ("`luaengine` / `pluginmanager`"). The summary may wrap
// across continuation lines (indented two spaces).
// ANTS-1251-INV-3: lines without a name+separator drop silently.
const QRegularExpression &bulletStartRe() {
    static const QRegularExpression re(QStringLiteral(R"(^-\s+`)"));
    return re;
}

// Captures every backticked identifier-shaped token in a string.
const QRegularExpression &nameTokenRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(`([A-Za-z][A-Za-z0-9_./-]*)`)"));
    return re;
}

// Matches " — " (U+2014, with surrounding spaces) or " -- " ASCII.
const QRegularExpression &separatorRe() {
    static const QRegularExpression re(QStringLiteral(R"( (?:—|--) )"));
    return re;
}

// ANTS-1796: a parenthetical qualifier ("(Qt6::Core, `ants_core_lib`)")
// may itself contain a backticked token (a CMake library target). Strip
// paren spans from the name-prefix before harvesting so the qualifier's
// lib name is not emitted as a bogus subsystem lane. The format contract
// (docs/subsystems.md) tolerates qualifier-paren forms = ignores them.
const QRegularExpression &parenQualifierRe() {
    static const QRegularExpression re(QStringLiteral(R"(\([^)]*\))"));
    return re;
}

}  // namespace

QVector<Lane> parse(const QString &claudeMdBody) {
    QVector<Lane> out;
    const QStringList lines = claudeMdBody.split(QLatin1Char('\n'));

    int i = 0;
    const int n = lines.size();

    // Find the `## Module map (src/)` H2.
    bool inSection = false;
    for (; i < n; ++i) {
        const QString &ln = lines.at(i);
        if (ln.startsWith(QStringLiteral("## Module map"))) {
            inSection = true;
            ++i;
            break;
        }
    }
    if (!inSection) return out;  // ANTS-1251-INV-7: empty when missing

    QStringList   pendingNames;
    QString       pendingSummary;
    bool          haveCurrent = false;
    auto flush = [&]() {
        if (!haveCurrent) return;
        const QString summary = pendingSummary.trimmed();
        for (const QString &nm : pendingNames) {
            if (!nm.isEmpty() && !summary.isEmpty()) {
                Lane l;
                l.name    = nm;
                l.summary = summary;
                out.push_back(l);
            }
        }
        pendingNames.clear();
        pendingSummary.clear();
        haveCurrent = false;
    };

    for (; i < n; ++i) {
        const QString &ln = lines.at(i);
        // Stop at next H2 (or H1).
        if (ln.startsWith(QStringLiteral("## ")) || ln.startsWith(QStringLiteral("# "))) {
            break;
        }
        // Bullet start?
        if (bulletStartRe().match(ln).hasMatch()) {
            flush();
            const auto sep = separatorRe().match(ln);
            if (!sep.hasMatch()) continue;  // INV-3 drop
            QString       prefix = ln.left(sep.capturedStart());
            prefix.remove(parenQualifierRe());  // ANTS-1796
            const QString suffix = ln.mid(sep.capturedEnd());
            QStringList   names;
            auto it = nameTokenRe().globalMatch(prefix);
            while (it.hasNext()) {
                names.push_back(it.next().captured(1));
            }
            if (names.isEmpty() || suffix.trimmed().isEmpty()) continue;
            pendingNames   = names;
            pendingSummary = suffix;
            haveCurrent    = true;
            continue;
        }
        // Indented continuation (two spaces) — append to running summary.
        if (haveCurrent && ln.startsWith(QStringLiteral("  ")) && !ln.trimmed().isEmpty()) {
            pendingSummary.append(QLatin1Char(' '));
            pendingSummary.append(ln.trimmed());
            continue;
        }
        // Blank line ends a bullet; non-matching, non-indented content
        // also flushes.
        flush();
    }
    flush();
    return out;
}

QVector<Lane> cachedLanes(const QString &claudeMdPath) {
    QFileInfo fi(claudeMdPath);
    const QString key = fi.canonicalFilePath().isEmpty()
                            ? claudeMdPath : fi.canonicalFilePath();
    if (key.isEmpty()) return {};

    const qint64 mtimeMs = fi.lastModified().toMSecsSinceEpoch();
    auto &c = cache();
    auto it = c.find(key);
    // ANTS-1251-INV-2: invalidate on mtime change only — no wall-clock TTL.
    if (it != c.end() && it->mtimeMs == mtimeMs) {
        return it->lanes;
    }

    QFile f(claudeMdPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QString body = QString::fromUtf8(f.readAll());
    f.close();

    CacheEntry entry;
    entry.mtimeMs = mtimeMs;
    entry.lanes   = parse(body);
    c.insert(key, entry);
    return entry.lanes;
}

QString resolveSource(const QString &claudeMdPath) {
    if (claudeMdPath.isEmpty()) return claudeMdPath;
    // ANTS-1292: prefer the dedicated module-map file when present so the
    // catalogue stays out of the always-loaded CLAUDE.md.
    const QString root = QFileInfo(claudeMdPath).absolutePath();
    const QString candidate =
        root + QStringLiteral("/docs/subsystems.md");
    if (QFileInfo::exists(candidate)) return candidate;
    return claudeMdPath;
}

void clearCacheForTests() {
    cache().clear();
}

}  // namespace SubsystemMap
