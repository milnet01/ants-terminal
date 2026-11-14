// ANTS-2175 — which arguments a verb does not declare. Behavioural coverage
// of the pure mcp::ignoredArgs diff (Qt6::Core, in ants_core_lib) plus
// source-scrapes of claudeintegration.cpp for the m_toolParamKeys cache
// population and the dispatch-site refusal (ANTS-4659).
// See tests/features/mcp_ignored_args/spec.md.

#include "../../_support/srcgrep.h"
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
    const QStringList got =
        mcp::ignoredArgs(args, roadmapQueryKnown(), QSet<QString>{});
    // "aaa" sorts before "query"; "status" (known) is excluded.
    EXPECT_EQ(got, (QStringList{QStringLiteral("aaa"),
                                QStringLiteral("query")}));
}

// INV-2 — a dispatch-layer arg the verb HONOURS is never flagged.
//
// ANTS-4578 — this used to exempt `fields`, `compact`, `offload` and
// `etag_match` on every verb, while the dispatcher acts on them only for the
// verbs on its allowlists. So passing `fields` to a verb that does not
// support it did nothing AND said nothing, which is the worst of the three
// possible outcomes: the caller believes the narrowing worked and makes the
// same call again. `raw` was never on the list and was correctly reported all
// along — that asymmetry is what identified the defect.
TEST(McpIgnoredArgs, Inv2HonouredArgsNeverFlagged) {
    QJsonObject args;
    for (const char *k : {"caller_cwd", "encoding", "etag_match", "fields",
                          "compact", "offload"})
        args[QString::fromUtf8(k)] = true;
    const QSet<QString> honoured{
        QStringLiteral("etag_match"), QStringLiteral("fields"),
        QStringLiteral("compact"),    QStringLiteral("offload")};
    EXPECT_TRUE(
        mcp::ignoredArgs(args, roadmapQueryKnown(), honoured).isEmpty());
    // Even against an empty known set (INV-4 corollary).
    EXPECT_TRUE(mcp::ignoredArgs(args, QSet<QString>{}, honoured).isEmpty());
}

// INV-2b — the remaining per-verb args ARE flagged on a verb that honours
// none of them. This is the case ANTS-4578 reported, from two different
// projects: `fields` to feedback_query, and `fields` to session_orient before
// it joined the projection allowlist. Both returned the full payload in
// silence.
//
// ANTS-4524 — `fields` is no longer among them, and the reason is the reverse
// of an exemption: it is honoured on EVERY verb now, so there is no verb on
// which it could be the dropped argument. `caller_cwd` and `encoding` are
// exempt for the same kind of reason rather than by omission — every verb
// reads caller_cwd, and tabularize is self-guarding with no tool-name
// predicate. ANTS-4659 — `compact` followed `fields`: honoured on every
// verb. The two that remain are honoured per allowlist and stay
// reportable; test_fieldsIsUniversal below pins the difference, because
// "exempt because it always works" and "exempt because nobody checked" look
// identical from inside this function.
TEST(McpIgnoredArgs, Inv2bUnhonouredDispatchArgsAreFlagged) {
    QJsonObject args;
    for (const char *k : {"caller_cwd", "encoding", "etag_match", "fields",
                          "compact", "offload"})
        args[QString::fromUtf8(k)] = true;
    EXPECT_EQ(mcp::ignoredArgs(args, QSet<QString>{}, QSet<QString>{}),
              (QStringList{QStringLiteral("etag_match"),
                           QStringLiteral("offload")}));
}

// ANTS-4524 — `fields` is universal, so it is never reported however the
// caller arrives: no declaration, no honoured set, any verb. Until ANTS-4578
// INV-2's premise was false in the dangerous direction — the advisory called
// `fields` a universal dispatch arg while the dispatcher honoured it on an
// allowlist, so the verbs that dropped it were the ones barred from saying so.
TEST(McpIgnoredArgs, Ants4524FieldsIsUniversalAndNeverFlagged) {
    QJsonObject args;
    args[QStringLiteral("fields")] = true;
    args[QStringLiteral("bogus")]  = true;
    EXPECT_EQ(mcp::ignoredArgs(args, QSet<QString>{}, QSet<QString>{}),
              (QStringList{QStringLiteral("bogus")}));
}

// INV-3 — all-known args (declared + universal) → empty.
TEST(McpIgnoredArgs, Inv3AllKnownEmpty) {
    QJsonObject args;
    args[QStringLiteral("status")]     = QStringLiteral("active");
    args[QStringLiteral("section")]    = QStringLiteral("x");
    args[QStringLiteral("caller_cwd")] = QStringLiteral("/p");
    EXPECT_TRUE(
        mcp::ignoredArgs(args, roadmapQueryKnown(), QSet<QString>{}).isEmpty());
}

// INV-4 — an empty known set reports every non-universal arg.
TEST(McpIgnoredArgs, Inv4EmptyKnownReportsNonUniversal) {
    QJsonObject args;
    args[QStringLiteral("foo")]        = 1;
    args[QStringLiteral("bar")]        = 2;
    args[QStringLiteral("caller_cwd")] = QStringLiteral("/p");  // universal
    EXPECT_EQ(mcp::ignoredArgs(args, QSet<QString>{}, QSet<QString>{}),
              (QStringList{QStringLiteral("bar"), QStringLiteral("foo")}));
}

// INV-5 — the tools/list handler populates m_toolParamKeys from each
// tool's inputSchema.properties, alongside the m_lastToolsList snapshot.
TEST(McpIgnoredArgs, Inv5CachePopulatedFromSchema) {
    const QString src = QString::fromStdString(ants_test::slurpClaudeIntegration());
    ASSERT_FALSE(src.isEmpty());
    EXPECT_TRUE(src.contains(QStringLiteral("m_toolParamKeys.insert")))
        << "tools/list must populate the param-key cache";
    EXPECT_TRUE(src.contains(QStringLiteral("m_toolParamKeys.clear")))
        << "cache rebuilt in lockstep with m_lastToolsList";
    // Derived from inputSchema.properties.
    EXPECT_TRUE(src.contains(QStringLiteral("\"inputSchema\""))
                && src.contains(QStringLiteral("\"properties\"")));
}

// INV-6 — the dispatch site calls mcp::ignoredArgs for a verb whose schema
// it knows.
TEST(McpIgnoredArgs, Inv6DispatchInjection) {
    const QString src = QString::fromStdString(ants_test::slurpClaudeIntegration());
    ASSERT_FALSE(src.isEmpty());
    EXPECT_TRUE(src.contains(QStringLiteral("mcp::ignoredArgs(")))
        << "dispatcher must call the shared diff";
    EXPECT_TRUE(src.contains(QStringLiteral("m_toolParamKeys.contains(toolName)")))
        << "the check is gated on a verb whose schema is known";
    // ANTS-4659 — the call is refused instead; mcp_undeclared_args_refused
    // covers the refusal, and the absence of the advisory, behaviourally.
}
