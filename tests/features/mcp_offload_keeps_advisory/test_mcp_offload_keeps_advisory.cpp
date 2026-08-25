// ANTS-4626 — the unknown-arg advisory must survive the result offload.
// See tests/features/mcp_offload_keeps_advisory/spec.md.

#include "mcpprojection.h"
#include "mcpspill.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace {

// Big enough that the real dispatcher would have spilled it, so the case
// under test is the one that actually reaches callers.
QString bigBody() {
    QString rows;
    while (rows.size() < 40000)
        rows += QStringLiteral(R"({"id":"ANTS-0000","headline":"filler"},)");
    rows.chop(1);
    return QStringLiteral(R"({"ok":true,"bullets":[)") + rows +
           QStringLiteral(R"(]})");
}

QJsonObject parse(const QString &s) {
    return QJsonDocument::fromJson(s.toUtf8()).object();
}

QStringList oneIgnored() {
    return QStringList{QStringLiteral("section_filter")};
}

}  // namespace

// INV-2 first, because it is the premise the others rest on: offloadBody
// builds a fresh envelope from its own keys, so anything attached to the
// body beforehand is gone from what the caller reads. Pinned so a later
// change that makes offloadBody preserve keys cannot quietly turn INV-1
// into an assertion that proves nothing.
TEST(McpOffloadKeepsAdvisory, Inv2OffloadBodyAloneDropsIt) {
    const QString withAdvisory =
        mcp::withIgnoredArgs(bigBody(), oneIgnored());
    ASSERT_TRUE(parse(withAdvisory).contains("ignored_args"))
        << "precondition: the advisory is on the body before the offload";

    const QString spilled = mcp::offloadBody(QStringLiteral("roadmap_query"),
                                             withAdvisory);
    const QJsonObject o = parse(spilled);
    // Guard against a fail-open offload (unwritable spill dir), which would
    // return the body unchanged and pass this test for the wrong reason.
    ASSERT_TRUE(o.value("offloaded").toBool())
        << "offload did not fire — nothing was being tested";
    EXPECT_FALSE(o.contains("ignored_args"))
        << "offloadBody is expected to drop it; INV-1 is what puts it back";
}

// INV-1 — re-applying after the offload is what reaches the caller. This is
// the dispatcher's sequence, run end to end.
TEST(McpOffloadKeepsAdvisory, Inv1AdvisorySurvivesOffload) {
    const QStringList ignored = oneIgnored();

    QString body = mcp::withIgnoredArgs(bigBody(), ignored);
    body = mcp::offloadBody(QStringLiteral("roadmap_query"), body);
    ASSERT_TRUE(parse(body).value("offloaded").toBool());
    body = mcp::withIgnoredArgs(body, ignored);

    const QJsonObject o = parse(body);
    ASSERT_TRUE(o.contains("ignored_args"))
        << "a spilled envelope must still name the argument that was dropped";
    EXPECT_EQ(o.value("ignored_args").toArray().size(), 1);
    EXPECT_EQ(o.value("ignored_args").toArray().at(0).toString(),
              QStringLiteral("section_filter"));
    // The pointer envelope is otherwise intact — the advisory is additive.
    EXPECT_TRUE(o.value("offloaded").toBool());
    EXPECT_TRUE(o.contains("handle"));
}

// INV-3 — a correct call gains nothing, so the advisory cannot become noise.
TEST(McpOffloadKeepsAdvisory, Inv3CleanCallGainsNoKey) {
    QString body = mcp::withIgnoredArgs(bigBody(), QStringList{});
    body = mcp::offloadBody(QStringLiteral("roadmap_query"), body);
    body = mcp::withIgnoredArgs(body, QStringList{});
    EXPECT_FALSE(parse(body).contains("ignored_args"));
}

// INV-4 — the dispatcher really does apply it on both sides. The pure
// helpers above cannot see the call order, and the defect WAS the call
// order. Source-scrape, since the dispatch path is GUI-coupled.
TEST(McpOffloadKeepsAdvisory, Inv4DispatchAppliesAfterOffload) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray s = f.readAll();

    const int offload = s.indexOf("mcp::offloadBody(");
    ASSERT_GT(offload, 0) << "offloadBody call site not found";
    const int after = s.indexOf("mcp::withIgnoredArgs(", offload);
    EXPECT_GT(after, 0)
        << "the advisory must be re-applied AFTER the offload, or a spilled "
           "envelope silently drops it";
}
