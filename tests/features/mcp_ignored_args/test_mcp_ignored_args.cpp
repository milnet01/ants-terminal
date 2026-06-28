// ANTS-2175 — feature-conformance test for the MCP unknown-arg advisory.
// Behavioural coverage of the pure mcp::ignoredArgs diff (Qt6::Core, in
// ants_core_lib) plus source-scrapes of claudeintegration.cpp for the
// m_toolParamKeys cache population and the dispatch-site injection.
// See tests/features/mcp_ignored_args/spec.md.

#include "mcpprojection.h"   // mcp::ignoredArgs

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace {

QString readSource(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// The roadmap_query property set, as declared in its inputSchema.
QSet<QString> roadmapQueryKnown() {
    return {QStringLiteral("status"),  QStringLiteral("section"),
            QStringLiteral("id"),      QStringLiteral("ids"),
            QStringLiteral("mode"),    QStringLiteral("include_body")};
}

}  // namespace

// INV-1 — the unrecognised key is reported; sorted ascending.
TEST(McpIgnoredArgs, Inv1ReportsUnknownSorted) {
    QJsonObject args;
    args[QStringLiteral("query")]   = QStringLiteral("token");  // the typo
    args[QStringLiteral("aaa")]     = 1;
    args[QStringLiteral("status")]  = QStringLiteral("active"); // known
    const QStringList got = mcp::ignoredArgs(args, roadmapQueryKnown());
    // "aaa" sorts before "query"; "status" (known) is excluded.
    EXPECT_EQ(got, (QStringList{QStringLiteral("aaa"),
                                QStringLiteral("query")}));
}

// INV-2 — universal dispatch-layer args are never flagged, even though
// roadmap_query's `known` set here does not include them all.
TEST(McpIgnoredArgs, Inv2UniversalArgsNeverFlagged) {
    QJsonObject args;
    for (const char *k : {"caller_cwd", "etag_match", "fields",
                          "compact", "offload"})
        args[QString::fromUtf8(k)] = true;
    EXPECT_TRUE(mcp::ignoredArgs(args, roadmapQueryKnown()).isEmpty());
    // Even against an empty known set (INV-4 corollary).
    EXPECT_TRUE(mcp::ignoredArgs(args, QSet<QString>{}).isEmpty());
}

// INV-3 — all-known args (declared + universal) → empty.
TEST(McpIgnoredArgs, Inv3AllKnownEmpty) {
    QJsonObject args;
    args[QStringLiteral("status")]     = QStringLiteral("active");
    args[QStringLiteral("section")]    = QStringLiteral("x");
    args[QStringLiteral("caller_cwd")] = QStringLiteral("/p");
    EXPECT_TRUE(mcp::ignoredArgs(args, roadmapQueryKnown()).isEmpty());
}

// INV-4 — an empty known set reports every non-universal arg.
TEST(McpIgnoredArgs, Inv4EmptyKnownReportsNonUniversal) {
    QJsonObject args;
    args[QStringLiteral("foo")]        = 1;
    args[QStringLiteral("bar")]        = 2;
    args[QStringLiteral("caller_cwd")] = QStringLiteral("/p");  // universal
    EXPECT_EQ(mcp::ignoredArgs(args, QSet<QString>{}),
              (QStringList{QStringLiteral("bar"), QStringLiteral("foo")}));
}

// INV-5 — the tools/list handler populates m_toolParamKeys from each
// tool's inputSchema.properties, alongside the m_lastToolsList snapshot.
TEST(McpIgnoredArgs, Inv5CachePopulatedFromSchema) {
    const QString src = readSource(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(src.isEmpty());
    EXPECT_TRUE(src.contains(QStringLiteral("m_toolParamKeys.insert")))
        << "tools/list must populate the param-key cache";
    EXPECT_TRUE(src.contains(QStringLiteral("m_toolParamKeys.clear")))
        << "cache rebuilt in lockstep with m_lastToolsList";
    // Derived from inputSchema.properties.
    EXPECT_TRUE(src.contains(QStringLiteral("\"inputSchema\""))
                && src.contains(QStringLiteral("\"properties\"")));
}

// INV-6 — the dispatch site calls mcp::ignoredArgs, attaches ignored_args,
// and is gated on a fresh (non-cached) call.
TEST(McpIgnoredArgs, Inv6DispatchInjection) {
    const QString src = readSource(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(src.isEmpty());
    EXPECT_TRUE(src.contains(QStringLiteral("mcp::ignoredArgs(")))
        << "dispatcher must call the shared diff";
    EXPECT_TRUE(src.contains(QStringLiteral("\"ignored_args\"")))
        << "advisory field must be attached";
    EXPECT_TRUE(src.contains(QStringLiteral("m_toolParamKeys.contains(toolName)")))
        << "advisory gated on a known verb";
}
