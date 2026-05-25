// ANTS-1720 — feature-conformance test for MCP `fields=` response
// projection. Behavioural coverage of mcp::projectFields +
// mcp::isFieldProjectionTool (pure, Qt6::Core), plus a source-scrape of
// the dispatch ordering invariant in claudeintegration.cpp.
// See tests/features/mcp_projection/spec.md.

#include "mcpprojection.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace {

QJsonObject parse(const QString &s) {
    return QJsonDocument::fromJson(s.toUtf8()).object();
}

QJsonArray fields(std::initializer_list<const char *> names) {
    QJsonArray a;
    for (const char *n : names) a.append(QString::fromUtf8(n));
    return a;
}

const QString kBody = QStringLiteral(
    "{\"ok\":true,\"bullets\":[1,2,3],\"count\":3,"
    "\"path\":\"/x/ROADMAP.md\",\"etag\":\"abc123\"}");

}  // namespace

// INV-1 — empty fields returns the body unchanged, byte-for-byte.
TEST(McpProjection, Inv1EmptyFieldsPassthrough) {
    EXPECT_EQ(mcp::projectFields(kBody, QJsonArray{}), kBody);
}

// INV-2 — single field subset returns exactly that key, value verbatim.
TEST(McpProjection, Inv2SingleFieldSubset) {
    const QJsonObject o = parse(mcp::projectFields(kBody, fields({"bullets"})));
    EXPECT_EQ(o.size(), 1);
    ASSERT_TRUE(o.contains("bullets"));
    EXPECT_TRUE(o.value("bullets").isArray());
    EXPECT_EQ(o.value("bullets").toArray().size(), 3);
}

// INV-3 — multi-field subset keeps only the named keys, verbatim.
TEST(McpProjection, Inv3MultiFieldSubset) {
    const QJsonObject o =
        parse(mcp::projectFields(kBody, fields({"ok", "count"})));
    EXPECT_EQ(o.size(), 2);
    EXPECT_TRUE(o.value("ok").toBool());
    EXPECT_EQ(o.value("count").toInt(), 3);
    EXPECT_FALSE(o.contains("bullets"));
}

// INV-4 — unknown field yields {}; known+unknown yields only the known.
TEST(McpProjection, Inv4UnknownFieldEmptyNotError) {
    EXPECT_EQ(mcp::projectFields(kBody, fields({"nonexistent"})),
              QStringLiteral("{}"));
    const QJsonObject o =
        parse(mcp::projectFields(kBody, fields({"count", "nope"})));
    EXPECT_EQ(o.size(), 1);
    EXPECT_EQ(o.value("count").toInt(), 3);
}

// INV-5 — non-string / empty entries are ignored, not faulted.
TEST(McpProjection, Inv5NonStringEntriesIgnored) {
    QJsonArray mixed;
    mixed.append(QStringLiteral("count"));
    mixed.append(42);                 // non-string
    mixed.append(QStringLiteral(""));  // empty
    const QJsonObject o = parse(mcp::projectFields(kBody, mixed));
    EXPECT_EQ(o.size(), 1);
    EXPECT_EQ(o.value("count").toInt(), 3);
}

// INV-6 — a non-object body passes through unchanged.
TEST(McpProjection, Inv6NonObjectPassthrough) {
    const QString raw = QStringLiteral("not json at all");
    EXPECT_EQ(mcp::projectFields(raw, fields({"x"})), raw);
    const QString arr = QStringLiteral("[1,2,3]");
    EXPECT_EQ(mcp::projectFields(arr, fields({"x"})), arr);
}

// INV-7 — etag survives only when explicitly requested (canonical body
// behaviour: the dispatch computes the etag pre-projection, so listing
// "etag" carries the same value a full call returns).
TEST(McpProjection, Inv7EtagRetainedOnlyWhenListed) {
    const QJsonObject without =
        parse(mcp::projectFields(kBody, fields({"bullets"})));
    EXPECT_FALSE(without.contains("etag"));

    const QJsonObject with =
        parse(mcp::projectFields(kBody, fields({"bullets", "etag"})));
    ASSERT_TRUE(with.contains("etag"));
    EXPECT_EQ(with.value("etag").toString(), QStringLiteral("abc123"));
}

// INV-8 — allowlist is exactly the seven in-scope tools.
TEST(McpProjection, Inv8AllowlistExact) {
    for (const char *t : {"roadmap_query", "project_layout", "file_outline",
                          "get_environment", "tab_list", "subsystem",
                          "git_state"}) {
        EXPECT_TRUE(mcp::isFieldProjectionTool(QString::fromUtf8(t)))
            << t << " should be field-projectable";
    }
    for (const char *t : {"get_scrollback", "session_brief", "current_state",
                          "last_audit_summary", "roadmap_branch_drift",
                          "build_status", "test_results", ""}) {
        EXPECT_FALSE(mcp::isFieldProjectionTool(QString::fromUtf8(t)))
            << t << " should NOT be field-projectable";
    }
}

// INV-9 — dispatch ordering: projectFields is called after
// applyEtagPattern and before wrapMcpData, and skipped on the etag
// short-circuit. Source-scrape (the dispatch path is GUI-coupled).
TEST(McpProjection, Inv9DispatchOrdering) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray s = f.readAll();

    const int etag = s.indexOf("applyEtagPattern(");
    const int proj = s.indexOf("mcp::projectFields(");
    const int wrap = s.indexOf("wrapMcpData(toolName, responseText)");
    ASSERT_GT(etag, 0) << "applyEtagPattern call site not found";
    ASSERT_GT(proj, 0) << "mcp::projectFields call site not found";
    ASSERT_GT(wrap, 0) << "wrapMcpData call site not found";
    EXPECT_LT(etag, proj) << "projection must run after the etag step";
    EXPECT_LT(proj, wrap) << "projection must run before the wrap";

    // Guarded against the etag short-circuit so {ok,unchanged,etag} is
    // never narrowed.
    EXPECT_TRUE(s.contains("!etagUnchanged"))
        << "projection must be guarded by !etagUnchanged";
    EXPECT_TRUE(s.contains("mcp::isFieldProjectionTool(toolName)"))
        << "dispatch must gate projection on the allowlist";
}

// INV-10 — each in-scope tool declares a `fields` schema property.
TEST(McpProjection, Inv10SchemaDeclaresFields) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray s = f.readAll();
    EXPECT_TRUE(s.contains("auto makeFieldsProp"))
        << "the shared `fields` schema fragment must be defined once";
    // One call site per in-scope projection tool (ANTS-1855 added
    // read_log → 8). The lambda definition reads `makeFieldsProp = [`
    // so it is not counted by the call-form needle.
    int count = 0;
    int idx = 0;
    const QByteArray needle = "makeFieldsProp();";
    while ((idx = s.indexOf(needle, idx)) != -1) { ++count; idx += needle.size(); }
    EXPECT_EQ(count, 8) << "expected 8 makeFieldsProp() call sites, got "
                        << count;
}
