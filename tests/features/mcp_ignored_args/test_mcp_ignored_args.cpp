// ANTS-2175 — feature-conformance test for the MCP unknown-arg advisory.
// Behavioural coverage of the pure mcp::ignoredArgs diff (Qt6::Core, in
// ants_core_lib) plus source-scrapes of claudeintegration.cpp for the
// m_toolParamKeys cache population and the dispatch-site injection.
// See tests/features/mcp_ignored_args/spec.md.

#include "mcpprojection.h"   // mcp::ignoredArgs

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
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
    // The field literal itself moved into mcp::withIgnoredArgs with ANTS-4525
    // and is covered behaviourally by INV-7 below, which is stronger than a
    // scrape: it reads the attached field out of a real envelope.
    EXPECT_TRUE(src.contains(QStringLiteral("m_toolParamKeys.contains(toolName)")))
        << "advisory gated on a known verb";
    EXPECT_TRUE(src.contains(QStringLiteral("mcp::withIgnoredArgs(")))
        << "the attach is the shared seam, so the dispatch site and INV-7's "
           "tests cannot disagree about what gets annotated";

    // ANTS-4525 — the ok:false suppression guard is GONE. This is the half of
    // INV-7 that is falsifiable against the pre-fix tree: the helper is new, so
    // its own tests could only fail to compile there, while this line reds
    // against the guard that used to sit at the dispatch site.
    EXPECT_FALSE(src.contains(QStringLiteral("!= QJsonValue(false)")))
        << "a refusal must not be excluded from the advisory — that suppresses "
           "the correction precisely when the caller's mental model is wrong";
}

// INV-7 (ANTS-4525) — the advisory is attached to REFUSALS as well.
//
// INV-6 used to say "never annotates a refusal (ok:false)", which is backwards
// for the case it matters most in: a caller holding a wrong mental model of a
// verb passes wrong ARGUMENTS and gets a refusal, so the reply that would
// correct the model is suppressed precisely because it refused.
//
// Measured, and it is what made ANTS-4510 cost a round trip. A DOOM session
// called read_log {max_commits:3, body:true} believing it read git log. Both
// args are unknown to the verb. Nothing said so, and the reply was `not_found`
// on the Ants debug-log path — which explains the file, not the misconception:
// "it ACCEPTED max_commits and body without an argument error, which confirms
// the wrong mental model before the file-open contradicts it."
TEST(McpIgnoredArgs, Ants4525RefusalsCarryTheAdvisoryToo) {
    const QStringList ignored{QStringLiteral("body"),
                              QStringLiteral("max_commits")};

    // The reported shape: a refusal for an UNRELATED reason (the log file is
    // missing), with the wrong args unmentioned.
    const QString refusal = QStringLiteral(
        R"({"ok":false,"code":"not_found","error":"read_log: no log file",)"
        R"("retry_after_ms":0})");
    const QJsonObject out =
        QJsonDocument::fromJson(
            mcp::withIgnoredArgs(refusal, ignored).toUtf8()).object();
    ASSERT_FALSE(out.value(QStringLiteral("ok")).toBool());
    const QJsonArray adv = out.value(QStringLiteral("ignored_args")).toArray();
    ASSERT_EQ(adv.size(), 2);
    EXPECT_EQ(adv.at(0).toString(), QStringLiteral("body"));
    EXPECT_EQ(adv.at(1).toString(), QStringLiteral("max_commits"));

    // The ANTS-2112 refusal floor survives — a caller who cannot see the error
    // is worse off than one who cannot see the advisory.
    EXPECT_EQ(out.value(QStringLiteral("code")).toString(),
              QStringLiteral("not_found"));
    EXPECT_EQ(out.value(QStringLiteral("error")).toString(),
              QStringLiteral("read_log: no log file"));
    EXPECT_TRUE(out.contains(QStringLiteral("retry_after_ms")));

    // Nothing to say ⟹ byte-identical, so the steady state is untouched.
    EXPECT_EQ(mcp::withIgnoredArgs(refusal, QStringList()), refusal);

    // A body this cannot parse is returned verbatim rather than replaced: the
    // advisory is worth less than the response it would destroy.
    const QString notJson = QStringLiteral("not a json envelope");
    EXPECT_EQ(mcp::withIgnoredArgs(notJson, ignored), notJson);

    // And a success envelope still gets it, which is what INV-6 shipped for.
    const QJsonObject okOut =
        QJsonDocument::fromJson(
            mcp::withIgnoredArgs(QStringLiteral(R"({"ok":true,"count":3})"),
                                 ignored).toUtf8()).object();
    EXPECT_TRUE(okOut.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(okOut.value(QStringLiteral("count")).toInt(), 3);
    EXPECT_EQ(okOut.value(QStringLiteral("ignored_args")).toArray().size(), 2);
}
