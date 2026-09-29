// ANTS-5254 — roadmap_log's published op enum matches its dispatch.
// See spec.md.

#include "claudeintegration.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <gtest/gtest.h>

namespace {

// The op names cmdRoadmapLogDispatch compares against, scoped to its body.
QSet<QString> dispatchedOps() {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/remotecontrol_roadmap_query_verb.cpp"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ADD_FAILURE() << "cannot read " << f.fileName().toStdString();
        return {};
    }
    const QString src = QString::fromUtf8(f.readAll());
    const qsizetype start =
        src.indexOf(QStringLiteral("QJsonDocument RemoteControl::cmdRoadmapLogDispatch("));
    if (start < 0) {
        ADD_FAILURE() << "cmdRoadmapLogDispatch not found";
        return {};
    }
    // The body ends at the first column-0 closing brace after it.
    const qsizetype end = src.indexOf(QStringLiteral("\n}\n"), start);
    const QString body = src.mid(start, end < 0 ? -1 : end - start);
    static const QRegularExpression rx(
        QStringLiteral("\\bop [=!]= QStringLiteral\\(\"([a-z_]+)\"\\)"));
    QSet<QString> ops;
    for (auto it = rx.globalMatch(body); it.hasNext();)
        ops.insert(it.next().captured(1));
    ops.remove(QStringLiteral("add"));        // aliases of append / append_batch
    ops.remove(QStringLiteral("add_batch"));
    return ops;
}

QSet<QString> publishedOps() {
    ClaudeIntegration ci;
    QByteArray line;
    McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                        [] { return true; });
    ci.handleMcpLine(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", &out);
    const QJsonArray tools = QJsonDocument::fromJson(line).object()
                                 .value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("tools")).toArray();
    QSet<QString> ops;
    for (const auto &t : tools) {
        const QJsonObject o = t.toObject();
        if (o.value(QStringLiteral("name")).toString() != QLatin1String("roadmap_log"))
            continue;
        const QJsonArray e = o.value(QStringLiteral("inputSchema")).toObject()
                                 .value(QStringLiteral("properties")).toObject()
                                 .value(QStringLiteral("op")).toObject()
                                 .value(QStringLiteral("enum")).toArray();
        for (const auto &v : e) ops.insert(v.toString());
    }
    return ops;
}

}  // namespace

TEST(RoadmapLogOpEnum, EnumMatchesDispatch) {
    const QSet<QString> dispatched = dispatchedOps();
    const QSet<QString> published  = publishedOps();
    ASSERT_GT(dispatched.size(), 10) << "the scrape found too few ops to be real";
    ASSERT_FALSE(published.isEmpty()) << "roadmap_log has no op enum in tools/list";

    for (const QString &op : dispatched)
        EXPECT_TRUE(published.contains(op))
            << "INV-1: dispatched but not in the published enum: " << op.toStdString();
    for (const QString &op : published)
        EXPECT_TRUE(dispatched.contains(op))
            << "INV-2: in the published enum but never dispatched: " << op.toStdString();

    EXPECT_FALSE(dispatched.contains(QStringLiteral("rotate_minor"))) << "INV-3";
    EXPECT_FALSE(published.contains(QStringLiteral("rotate_minor"))) << "INV-3";

    for (const char *op : {"link", "unlink"}) {   // INV-4 (ANTS-4079 INV-11)
        EXPECT_TRUE(dispatched.contains(QString::fromLatin1(op))) << "INV-4: " << op;
        EXPECT_TRUE(published.contains(QString::fromLatin1(op))) << "INV-4: " << op;
    }
}
