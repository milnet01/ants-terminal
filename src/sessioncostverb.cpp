// ANTS-5698 — session_cost seam. Contract: tests/features/session_cost/spec.md.

#include "sessioncostverb.h"
#include "mcprefusal.h"

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


// ANTS-6697 — `mcp__<server>__<verb>` → `<server>`; any other name → builtin.
QString serverOf(const QString &tool) {
    if (tool.startsWith(QLatin1String("mcp__"))) {
        const qsizetype end = tool.indexOf(QLatin1String("__"), 5);
        if (end > 5)
            return tool.mid(5, end - 5);
    }
    return QStringLiteral("builtin");
}

// An attachment's `rendered` text: an array of {content} blocks, where a
// content is a string or an array of text blocks.
qint64 renderedBytes(const QJsonValue &rendered) {
    qint64 total = 0;
    int images = 0;
    for (const auto &b : rendered.toArray())
        total += contentBytes(b.toObject().value(QStringLiteral("content")), &images);
    return total;
}

// One shape's tool definitions: bytes per tool, and per server.
struct Shape {
    QHash<QString, qint64> toolBytes;
    QHash<QString, qint64> serverBytes;
};

Shape shapeFrom(const QJsonObject &shape) {
    Shape out;
    for (const auto &t : shape.value(QStringLiteral("tools")).toArray()) {
        const QJsonObject o = t.toObject();
        const QString name = o.value(QStringLiteral("name")).toString();
        const qint64 n = QJsonDocument(o).toJson(QJsonDocument::Compact).size();
        out.toolBytes.insert(name, n);
        out.serverBytes[serverOf(name)] += n;
    }
    return out;
}

// Bytes arriving before request `request` (1-based), with a label.
struct Held {
    QString label;
    qint64 bytes = 0;
    int request = 0;
};

// Rent sums, per tool, server and hook (ANTS-6697).
struct ToolRow {
    int calls = 0;
    qint64 resultBytes = 0, rent = 0;
};
struct ServerRow {
    int calls = 0;
    qint64 resultBytes = 0, resultRent = 0, definitionRent = 0, nameRent = 0;
    qint64 total() const { return resultRent + definitionRent + nameRent; }
};
struct HookRow {
    int count = 0;
    qint64 bytes = 0, rent = 0;
};

// Rows of a hash as a JSON array, largest `key` first, ties by name.
template <typename Row, typename Key, typename Fill>
QJsonArray rankedRows(const QHash<QString, Row> &rows, int top, Key key, Fill fill) {
    QStringList names = rows.keys();
    std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) {
        const qint64 ka = key(rows.value(a)), kb = key(rows.value(b));
        return ka != kb ? ka > kb : a < b;
    });
    QJsonArray out;
    for (qsizetype i = 0; i < names.size() && i < top; ++i)
        out.append(fill(names.at(i), rows.value(names.at(i))));
    return out;
}

// Server rows of a {server: bytes} sum, as {server, tool_count, bytes}.
QJsonArray serverTable(const QHash<QString, qint64> &toolBytes) {
    QHash<QString, std::pair<int, qint64>> by;
    for (auto it = toolBytes.cbegin(); it != toolBytes.cend(); ++it) {
        auto &row = by[serverOf(it.key())];
        ++row.first;
        row.second += it.value();
    }
    QStringList names = by.keys();
    std::sort(names.begin(), names.end());
    QJsonArray out;
    for (const QString &n : names)
        out.append(QJsonObject{{QStringLiteral("server"), n},
                               {QStringLiteral("tool_count"), by.value(n).first},
                               {QStringLiteral("bytes"), by.value(n).second}});
    return out;
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

QStringList projectsDirs() {
    QStringList out;
    const QString cfg = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
    if (!cfg.isEmpty())
        out << QDir(cfg).filePath(QStringLiteral("projects"));
    out << QDir::home().filePath(QStringLiteral(".claude/projects"));
    return out;
}

QJsonObject analyse(const QString &transcriptPath, int top) {
    QFile f(transcriptPath);
    if (!f.open(QIODevice::ReadOnly))
        return mcp::refusal("not_found",
                      QStringLiteral("session_cost: cannot read %1").arg(transcriptPath));

    // ANTS-6189 — a helper agent's own transcript marks every line
    // isSidechain; there those lines are the session itself, not a
    // subagent's lines copied into a main transcript.
    const bool sidechainIsOwn =
        QFileInfo(transcriptPath).dir().dirName() == QLatin1String("subagents");
    QSet<QString> seen;
    QHash<QString, QString> toolNames;  // tool_use id -> tool name
    std::vector<Item> items;
    QJsonValue startup;  // null until the first request shape
    qint64 in = 0, read = 0, write = 0, out = 0;
    qint64 first = -1, current = 0, peak = 0;
    int requests = 0, images = 0;
    // ANTS-6697 — what rent needs: each request's context and shape, and the
    // hook text, deferred-tool lists and their added names as they arrive.
    std::vector<qint64> contexts;
    std::vector<int> requestShape;  // index into shapes, -1 before any
    std::vector<Shape> shapes;
    std::vector<Held> hooks, deferredLists, deferredNames;

    while (!f.atEnd()) {
        const QJsonObject e = QJsonDocument::fromJson(f.readLine()).object();
        if (!sidechainIsOwn && e.value(QStringLiteral("isSidechain")).toBool())
            continue;
        const QString type = e.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("api-request-shape")) {
            const QJsonObject shape = e.value(QStringLiteral("shape")).toObject();
            shapes.push_back(shapeFrom(shape));
            if (startup.isNull()) {
                QJsonObject su = startupFrom(shape, top);
                su[QStringLiteral("servers")] = serverTable(shapes.back().toolBytes);
                startup = su;
            }
            continue;
        }
        if (type == QLatin1String("attachment")) {
            const QJsonObject at = e.value(QStringLiteral("attachment")).toObject();
            const QString kind = at.value(QStringLiteral("type")).toString();
            const qint64 n = renderedBytes(e.value(QStringLiteral("rendered")));
            if (n <= 0)
                continue;
            if (kind == QLatin1String("hook_success")
                || kind == QLatin1String("hook_additional_context")) {
                hooks.push_back({at.value(QStringLiteral("hookName")).toString(), n,
                                 requests + 1});
            } else if (kind == QLatin1String("deferred_tools_delta")) {
                deferredLists.push_back({{}, n, requests + 1});
                for (const auto &name : at.value(QStringLiteral("addedNames")).toArray()) {
                    const QString s = name.toString();
                    deferredNames.push_back({serverOf(s), s.toUtf8().size() + 1, requests + 1});
                }
            }
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
            contexts.push_back(current);
            requestShape.push_back(static_cast<int>(shapes.size()) - 1);
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

    // INV-10 — a request under half the previous one's context starts a new
    // segment. segmentEnd[p] is the last request held with request p.
    const auto startsSegment = [&contexts](int p) {
        return p > 1 && 2 * contexts[p - 1] < contexts[p - 2];
    };
    int compactions = 0;
    for (int p = 2; p <= requests; ++p)
        compactions += startsSegment(p) ? 1 : 0;
    std::vector<int> segmentEnd(requests + 1, requests);
    for (int p = requests - 1; p >= 1; --p)
        segmentEnd[p] = startsSegment(p + 1) ? p : segmentEnd[p + 1];
    const auto rentOf = [&](qint64 bytes, int request) -> qint64 {
        return request >= 1 && request <= requests
                   ? bytes * (segmentEnd[request] - request + 1) : 0;
    };

    QHash<QString, ToolRow> toolRows;
    QHash<QString, ServerRow> serverRows;
    qint64 resultRent = 0;
    for (const Item &it : items) {
        if (it.kind != QLatin1String("tool_result"))
            continue;
        const qint64 rent = rentOf(it.bytes, it.request);
        resultRent += rent;
        ToolRow &t = toolRows[it.tool];
        ++t.calls;
        t.resultBytes += it.bytes;
        t.rent += rent;
        ServerRow &sv = serverRows[serverOf(it.tool)];
        ++sv.calls;
        sv.resultBytes += it.bytes;
        sv.resultRent += rent;
    }
    // INV-12 — each request rents the definitions of the shape in force.
    qint64 definitionRent = 0;
    for (const int s : requestShape) {
        if (s < 0)
            continue;
        const QHash<QString, qint64> &by = shapes[s].serverBytes;
        for (auto it = by.cbegin(); it != by.cend(); ++it) {
            serverRows[it.key()].definitionRent += it.value();
            definitionRent += it.value();
        }
    }
    // INV-13 — hook text, deferred-tool lists and their added names.
    QHash<QString, HookRow> hookRows;
    qint64 hookBytes = 0, hookRent = 0;
    for (const Held &h : hooks) {
        const qint64 rent = rentOf(h.bytes, h.request);
        HookRow &row = hookRows[h.label];
        ++row.count;
        row.bytes += h.bytes;
        row.rent += rent;
        hookBytes += h.bytes;
        hookRent += rent;
    }
    qint64 listBytes = 0, listRent = 0;
    for (const Held &d : deferredLists) {
        listBytes += d.bytes;
        listRent += rentOf(d.bytes, d.request);
    }
    for (const Held &d : deferredNames)
        serverRows[d.label].nameRent += rentOf(d.bytes, d.request);

    // INV-12 — tools a later shape names and the first does not, each sized
    // where it first appears.
    if (!startup.isNull()) {
        QHash<QString, qint64> later;
        for (size_t i = 1; i < shapes.size(); ++i)
            for (auto it = shapes[i].toolBytes.cbegin(); it != shapes[i].toolBytes.cend(); ++it)
                if (!shapes.front().toolBytes.contains(it.key()) && !later.contains(it.key()))
                    later.insert(it.key(), it.value());
        QJsonObject su = startup.toObject();
        su[QStringLiteral("loaded_later")] = serverTable(later);
        startup = su;
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

    const QJsonArray tools = rankedRows(
        toolRows, top, [](const ToolRow &r) { return r.rent; },
        [](const QString &name, const ToolRow &r) {
            return QJsonObject{{QStringLiteral("name"), name},
                               {QStringLiteral("server"), serverOf(name)},
                               {QStringLiteral("calls"), r.calls},
                               {QStringLiteral("result_bytes"), r.resultBytes},
                               {QStringLiteral("rent_bytes"), r.rent}};
        });
    const QJsonArray servers = rankedRows(
        serverRows, top, [](const ServerRow &r) { return r.total(); },
        [](const QString &name, const ServerRow &r) {
            return QJsonObject{{QStringLiteral("server"), name},
                               {QStringLiteral("calls"), r.calls},
                               {QStringLiteral("result_bytes"), r.resultBytes},
                               {QStringLiteral("result_rent_bytes"), r.resultRent},
                               {QStringLiteral("definition_rent_bytes"), r.definitionRent},
                               {QStringLiteral("deferred_name_rent_bytes"), r.nameRent},
                               {QStringLiteral("rent_bytes"), r.total()}};
        });
    const QJsonArray byHook = rankedRows(
        hookRows, top, [](const HookRow &r) { return r.rent; },
        [](const QString &name, const HookRow &r) {
            return QJsonObject{{QStringLiteral("hook"), name},
                               {QStringLiteral("count"), r.count},
                               {QStringLiteral("bytes"), r.bytes},
                               {QStringLiteral("rent_bytes"), r.rent}};
        });

    QJsonObject result{
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
        {QStringLiteral("largest_items"), largest},
        {QStringLiteral("compactions"), compactions},
        {QStringLiteral("tools"), tools},
        {QStringLiteral("servers"), servers},
        {QStringLiteral("hooks"),
         QJsonObject{{QStringLiteral("count"), static_cast<int>(hooks.size())},
                     {QStringLiteral("bytes"), hookBytes},
                     {QStringLiteral("rent_bytes"), hookRent},
                     {QStringLiteral("by_hook"), byHook}}},
        {QStringLiteral("deferred_tools"),
         QJsonObject{{QStringLiteral("count"), static_cast<int>(deferredLists.size())},
                     {QStringLiteral("bytes"), listBytes},
                     {QStringLiteral("rent_bytes"), listRent}}},
        {QStringLiteral("rent"),
         QJsonObject{{QStringLiteral("results"), resultRent},
                     {QStringLiteral("definitions"), definitionRent},
                     {QStringLiteral("hooks"), hookRent},
                     {QStringLiteral("deferred_tools"), listRent},
                     {QStringLiteral("total"),
                      resultRent + definitionRent + hookRent + listRent}}}};
    if (startup.isNull())  // INV-15
        result[QStringLiteral("notices")] = QJsonArray{QStringLiteral(
            "no api-request-shape line: start-up and tool-definition rent are unknown")};
    return result;
}

QJsonObject run(const QStringList &projectsDirs, const QString &cwd,
                const QJsonObject &args, qint64 nowMs) {
    static const QRegularExpression sid(QStringLiteral("^[0-9A-Za-z-]{8,64}$"));
    const QString sessionId = args.value(QStringLiteral("session_id")).toString();
    const bool self = sessionId == QLatin1String("self");
    if (args.contains(QStringLiteral("session_id")) && !self
        && !sid.match(sessionId).hasMatch())
        return mcp::refusal("bad_args",
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
        return mcp::refusal("not_found",
                      QStringLiteral("session_cost: no transcript folder for %1").arg(cwd));

    // ANTS-6189 — helper agents' transcripts: <session>/subagents/agent-<id>.jsonl.
    const auto agentTranscripts = [&folder](const QString &glob) {
        QFileInfoList out;
        const QStringList sessions =
            QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &s : sessions)
            out += QDir(QDir(folder).filePath(s + QStringLiteral("/subagents")))
                       .entryInfoList({glob}, QDir::Files);
        return out;
    };

    QString path;
    bool inferred = false;
    int othersActive = 0;
    if (sessionId.startsWith(QLatin1String("agent-"))) {
        const QFileInfoList hit = agentTranscripts(sessionId + QStringLiteral(".jsonl"));
        if (hit.isEmpty())
            return mcp::refusal("not_found",
                          QStringLiteral("session_cost: no helper-agent transcript %1 "
                                         "under %2").arg(sessionId, folder));
        path = hit.first().absoluteFilePath();
    } else if (!sessionId.isEmpty() && !self) {
        path = QDir(folder).filePath(sessionId + QStringLiteral(".jsonl"));
        if (!QFileInfo(path).isFile())
            return mcp::refusal("not_found",
                          QStringLiteral("session_cost: no transcript %1").arg(path));
    } else {
        // No id: the newest session. "self": the newest session or helper
        // agent, which is the caller when it is the one writing now; every
        // agent shares one MCP connection, so nothing names it better.
        QFileInfoList all = QDir(folder).entryInfoList(
            {QStringLiteral("*.jsonl")}, QDir::Files);
        if (self)
            all += agentTranscripts(QStringLiteral("agent-*.jsonl"));
        std::stable_sort(all.begin(), all.end(), [](const QFileInfo &a, const QFileInfo &b) {
            return a.lastModified() > b.lastModified();
        });
        if (all.isEmpty())
            return mcp::refusal("not_found",
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
    if (QFileInfo(path).dir().dirName() == QLatin1String("subagents"))
        r[QStringLiteral("parent_session_id")] =
            QFileInfo(QFileInfo(path).path()).dir().dirName();
    r[QStringLiteral("transcript")] = path;
    r[QStringLiteral("session_inferred")] = inferred;
    if (inferred)
        r[QStringLiteral("other_active_sessions")] = othersActive;
    // INV-16 — a session's helper agents, summed. A helper has none.
    if (args.value(QStringLiteral("include_agents")).toBool()
        && !r.contains(QStringLiteral("parent_session_id"))) {
        const QFileInfoList helpers =
            QDir(QDir(folder).filePath(QFileInfo(path).completeBaseName()
                                       + QStringLiteral("/subagents")))
                .entryInfoList({QStringLiteral("agent-*.jsonl")}, QDir::Files);
        int count = 0, requests = 0;
        qint64 rentTotal = 0;
        QJsonObject totals;
        for (const QFileInfo &h : helpers) {
            const QJsonObject a = analyse(h.absoluteFilePath(), 1);
            if (!a.value(QStringLiteral("ok")).toBool())
                continue;
            ++count;
            requests += a.value(QStringLiteral("requests")).toInt();
            rentTotal += static_cast<qint64>(
                a.value(QStringLiteral("rent")).toObject().value(QStringLiteral("total")).toDouble());
            const QJsonObject t = a.value(QStringLiteral("totals")).toObject();
            for (auto it = t.constBegin(); it != t.constEnd(); ++it)
                totals[it.key()] = static_cast<qint64>(totals.value(it.key()).toDouble()
                                                       + it.value().toDouble());
        }
        r[QStringLiteral("agents")] =
            QJsonObject{{QStringLiteral("count"), count},
                        {QStringLiteral("requests"), requests},
                        {QStringLiteral("totals"), totals},
                        {QStringLiteral("rent_total"), rentTotal}};
    }
    return r;
}

}  // namespace SessionCost
