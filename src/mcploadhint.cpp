// ANTS-5736 — see mcploadhint.h.

#include "mcploadhint.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QtGlobal>

#include <algorithm>

namespace mcp {

namespace {

// A session loads its tools near its start, and a transcript can run to tens
// of megabytes; reading only the head keeps `initialize` fast.
constexpr qint64 kHeadBytes = 4 * 1024 * 1024;

// The tool names in every ToolSearch `select:` query in `text`.
QSet<QString> selectedNames(const QByteArray &text) {
    static const QByteArray kMarker = QByteArrayLiteral("\"query\":\"select:");
    QSet<QString> names;
    for (qsizetype at = text.indexOf(kMarker); at >= 0;
         at = text.indexOf(kMarker, at)) {
        at += kMarker.size();
        const qsizetype end = text.indexOf('"', at);
        if (end < 0)
            break;
        for (const QByteArray &name : text.mid(at, end - at).split(','))
            if (const QString n = QString::fromUtf8(name.trimmed()); !n.isEmpty())
                names.insert(n);
        at = end;
    }
    return names;
}

}  // namespace

QString transcriptDirFor(const QString &projectDir) {
    QString config = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
    if (config.isEmpty())
        config = QDir::homePath() + QStringLiteral("/.claude");
    static const QRegularExpression kOther(QStringLiteral("[^A-Za-z0-9]"));
    QString slug = projectDir;
    slug.replace(kOther, QStringLiteral("-"));
    return config + QStringLiteral("/projects/") + slug;
}

QStringList recentlyLoadedTools(const QString &dir, const QSet<QString> &known,
                                int maxFiles, int maxNames) {
    const QFileInfoList files = QDir(dir).entryInfoList(
        {QStringLiteral("*.jsonl")}, QDir::Files, QDir::Time);
    QHash<QString, int> sessions;
    for (const QFileInfo &fi : files.mid(0, maxFiles)) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        for (const QString &name : selectedNames(f.read(kHeadBytes))) {
            const qsizetype cut = name.lastIndexOf(QStringLiteral("__"));
            if (name.startsWith(QStringLiteral("mcp__")) && cut > 3
                && known.contains(name.mid(cut + 2)))
                ++sessions[name];
        }
    }
    QStringList ranked = sessions.keys();
    std::sort(ranked.begin(), ranked.end(), [&sessions](const QString &a, const QString &b) {
        const int na = sessions.value(a), nb = sessions.value(b);
        return na != nb ? na > nb : a < b;
    });
    return ranked.mid(0, maxNames);
}

QString loadHintText(const QStringList &tools) {
    if (tools.isEmpty())
        return {};
    return QStringLiteral("This project's recent sessions loaded these Ants tools; "
                          "load them in one call: ToolSearch query \"select:%1\".")
        .arg(tools.join(QLatin1Char(',')));
}

}  // namespace mcp
