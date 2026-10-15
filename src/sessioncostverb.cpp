// ANTS-5698 — session_cost seam. Contract: tests/features/session_cost/spec.md.

#include "sessioncostverb.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <vector>

namespace SessionCost {

namespace {

QJsonObject refuse(const char *code, const QString &message) {
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("code"), QString::fromLatin1(code)},
                       {QStringLiteral("error"), message}};
}

struct Item {
    QString kind;
    QString tool;
    qint64 bytes = 0;
    int request = 0;
};

qint64 textBytes(const QJsonValue &v) {
    return static_cast<qint64>(v.toString().toUtf8().size());
}

// A content value's text bytes; image blocks are counted, never sized by
// their base64 (INV-2).
qint64 contentBytes(const QJsonValue &content, int *images) {
    if (content.isString())
        return textBytes(content);
    qint64 total = 0;
    for (const auto &b : content.toArray()) {
        const QJsonObject o = b.toObject();
        const QString type = o.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("image"))
            ++*images;
        else if (type == QLatin1String("text"))
            total += textBytes(o.value(QStringLiteral("text")));
    }
    return total;
}

QJsonObject startupFrom(const QJsonObject &shape, int top) {
    qint64 systemBytes = 0;
    for (const auto &b : shape.value(QStringLiteral("system")).toArray())
        systemBytes += textBytes(b.toObject().value(QStringLiteral("text")));
    std::vector<std::pair<qint64, QString>> tools;
    qint64 toolBytes = 0;
    for (const auto &t : shape.value(QStringLiteral("tools")).toArray()) {
        const QJsonObject o = t.toObject();
        const qint64 n = QJsonDocument(o).toJson(QJsonDocument::Compact).size();
        toolBytes += n;
        tools.emplace_back(n, o.value(QStringLiteral("name")).toString());
    }
    std::stable_sort(tools.begin(), tools.end(),
                     [](const auto &a, const auto &b) { return a.first > b.first; });
    QJsonArray largest;
    for (size_t i = 0; i < tools.size() && static_cast<int>(i) < top; ++i)
        largest.append(QJsonObject{{QStringLiteral("name"), tools[i].second},
                                   {QStringLiteral("bytes"), tools[i].first}});
    return QJsonObject{{QStringLiteral("system_bytes"), systemBytes},
                       {QStringLiteral("tool_bytes"), toolBytes},
                       {QStringLiteral("tool_count"), static_cast<int>(tools.size())},
                       {QStringLiteral("largest_tools"), largest}};
}

}  // namespace

QString folderNameFor(const QString &cwd) {
    QString out = cwd;
    for (QChar &c : out) {
        const ushort u = c.unicode();
        const bool keep = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z')
                          || (u >= 'A' && u <= 'Z');
        if (!keep)
            c = QLatin1Char('-');
    }
    return out;
}

QJsonObject analyse(const QString &transcriptPath, int top) {
    QFile f(transcriptPath);
    if (!f.open(QIODevice::ReadOnly))
        return refuse("not_found",
                      QStringLiteral("session_cost: cannot read %1").arg(transcriptPath));

    QSet<QString> seen;
    QHash<QString, QString> toolNames;  // tool_use id -> tool name
    std::vector<Item> items;
    QJsonValue startup;  // null until the first request shape
    qint64 in = 0, read = 0, write = 0, out = 0;
    qint64 first = -1, current = 0, peak = 0;
    int requests = 0, images = 0;

    while (!f.atEnd()) {
        const QJsonObject e = QJsonDocument::fromJson(f.readLine()).object();
        if (e.value(QStringLiteral("isSidechain")).toBool())
            continue;
        const QString type = e.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("api-request-shape")) {
            if (startup.isNull())
                startup = startupFrom(e.value(QStringLiteral("shape")).toObject(), top);
            continue;
        }
        const QJsonObject m = e.value(QStringLiteral("message")).toObject();
        if (type == QLatin1String("assistant")) {
            for (const auto &b : m.value(QStringLiteral("content")).toArray()) {
                const QJsonObject o = b.toObject();
                if (o.value(QStringLiteral("type")).toString() == QLatin1String("tool_use"))
                    toolNames.insert(o.value(QStringLiteral("id")).toString(),
                                     o.value(QStringLiteral("name")).toString());
            }
            // INV-1: the harness writes one line per content block, each with
            // the whole message's usage, so a message counts once by its id.
            const QString id = m.value(QStringLiteral("id")).toString();
            const QJsonObject us = m.value(QStringLiteral("usage")).toObject();
            if (id.isEmpty() || us.isEmpty() || seen.contains(id))
                continue;
            seen.insert(id);
            const auto n = [&](const char *k) {
                return static_cast<qint64>(us.value(QLatin1String(k)).toDouble());
            };
            in += n("input_tokens");
            read += n("cache_read_input_tokens");
            write += n("cache_creation_input_tokens");
            out += n("output_tokens");
            current = n("input_tokens") + n("cache_read_input_tokens")
                      + n("cache_creation_input_tokens");
            if (first < 0)
                first = current;
            peak = std::max(peak, current);
            ++requests;
            continue;
        }
        if (type != QLatin1String("user"))
            continue;
        const QString textKind = e.value(QStringLiteral("isMeta")).toBool()
                                     ? QStringLiteral("meta_text")
                                     : QStringLiteral("user_text");
        const QJsonValue content = m.value(QStringLiteral("content"));
        if (content.isString()) {
            items.push_back({textKind, {}, textBytes(content), requests + 1});
            continue;
        }
        for (const auto &b : content.toArray()) {
            const QJsonObject o = b.toObject();
            const QString bt = o.value(QStringLiteral("type")).toString();
            if (bt == QLatin1String("tool_result")) {
                items.push_back({QStringLiteral("tool_result"),
                                 toolNames.value(o.value(QStringLiteral("tool_use_id")).toString()),
                                 contentBytes(o.value(QStringLiteral("content")), &images),
                                 requests + 1});
            } else if (bt == QLatin1String("text")) {
                items.push_back({textKind, {}, textBytes(o.value(QStringLiteral("text"))),
                                 requests + 1});
            } else if (bt == QLatin1String("image")) {
                ++images;
            }
        }
    }

    std::stable_sort(items.begin(), items.end(),
                     [](const Item &a, const Item &b) { return a.bytes > b.bytes; });
    QJsonArray largest;
    for (size_t i = 0; i < items.size() && static_cast<int>(i) < top; ++i) {
        QJsonObject o{{QStringLiteral("kind"), items[i].kind},
                      {QStringLiteral("bytes"), items[i].bytes},
                      {QStringLiteral("request"), items[i].request}};
        if (!items[i].tool.isEmpty())
            o[QStringLiteral("tool")] = items[i].tool;
        largest.append(o);
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("requests"), requests},
        {QStringLiteral("context"),
         QJsonObject{{QStringLiteral("first"), std::max<qint64>(first, 0)},
                     {QStringLiteral("current"), current},
                     {QStringLiteral("peak"), peak}}},
        {QStringLiteral("totals"),
         QJsonObject{{QStringLiteral("input"), in},
                     {QStringLiteral("cache_read"), read},
                     {QStringLiteral("cache_write"), write},
                     {QStringLiteral("output"), out}}},
        {QStringLiteral("images"), images},
        {QStringLiteral("startup"), startup},
        {QStringLiteral("largest_items"), largest}};
}

QJsonObject run(const QStringList &projectsDirs, const QString &cwd,
                const QJsonObject &args, qint64 nowMs) {
    static const QRegularExpression sid(QStringLiteral("^[0-9A-Za-z-]{8,64}$"));
    const QString sessionId = args.value(QStringLiteral("session_id")).toString();
    if (args.contains(QStringLiteral("session_id")) && !sid.match(sessionId).hasMatch())
        return refuse("bad_args",
                      QStringLiteral("session_cost: session_id \"%1\" is not a session id")
                          .arg(sessionId));
    const int top = std::clamp(args.value(QStringLiteral("top")).toInt(kDefaultTop), 1, kMaxTop);

    // The folder for cwd, else for its nearest ancestor that has one.
    // Walked by path text: QDir::cdUp() stops at a directory that is gone.
    QString folder;
    QString c = QDir::cleanPath(cwd);
    while (folder.isEmpty() && !c.isEmpty()) {
        for (const QString &base : projectsDirs) {
            const QString cand = QDir(base).filePath(folderNameFor(c));
            if (QFileInfo(cand).isDir()) {
                folder = cand;
                break;
            }
        }
        const QString parent = QFileInfo(c).path();
        c = parent == c ? QString() : parent;
    }
    if (folder.isEmpty())
        return refuse("not_found",
                      QStringLiteral("session_cost: no transcript folder for %1").arg(cwd));

    QString path;
    bool inferred = false;
    int othersActive = 0;
    if (!sessionId.isEmpty()) {
        path = QDir(folder).filePath(sessionId + QStringLiteral(".jsonl"));
        if (!QFileInfo(path).isFile())
            return refuse("not_found",
                          QStringLiteral("session_cost: no transcript %1").arg(path));
    } else {
        const QFileInfoList all = QDir(folder).entryInfoList(
            {QStringLiteral("*.jsonl")}, QDir::Files, QDir::Time);
        if (all.isEmpty())
            return refuse("not_found",
                          QStringLiteral("session_cost: no transcript in %1").arg(folder));
        path = all.first().absoluteFilePath();
        inferred = true;
        for (qsizetype i = 1; i < all.size(); ++i)
            if (all.at(i).lastModified().toMSecsSinceEpoch() >= nowMs - kActiveWindowMs)
                ++othersActive;
    }

    QJsonObject r = analyse(path, top);
    if (!r.value(QStringLiteral("ok")).toBool())
        return r;
    r[QStringLiteral("session_id")] = QFileInfo(path).completeBaseName();
    r[QStringLiteral("transcript")] = path;
    r[QStringLiteral("session_inferred")] = inferred;
    if (inferred)
        r[QStringLiteral("other_active_sessions")] = othersActive;
    return r;
}

}  // namespace SessionCost
