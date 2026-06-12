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

// ANTS-2112 — a refusal envelope is never blanked by fields=. A narrowed
// read that hits a rate-limit / validation refusal carries its error in
// ok/code/error/retry_after_ms, none of which the caller's fields= names; the
// projection must still surface that floor so the model sees the error +
// retry hint instead of an empty {}.
TEST(McpProjection, Ants2112RefusalFloorSurvivesNarrowing) {
    const QString refusal = QStringLiteral(
        "{\"ok\":false,\"code\":\"rate_limited\","
        "\"error\":\"slow down\",\"retry_after_ms\":5000}");
    // Caller asked for an unrelated field that a refusal never carries.
    const QJsonObject o = parse(mcp::projectFields(refusal, fields({"bullets"})));
    EXPECT_FALSE(o.value("ok").toBool());
    EXPECT_EQ(o.value("code").toString(), QStringLiteral("rate_limited"));
    EXPECT_EQ(o.value("error").toString(), QStringLiteral("slow down"));
    EXPECT_EQ(o.value("retry_after_ms").toInt(), 5000);
    EXPECT_FALSE(o.contains("bullets"));
}

// ANTS-2112 — the floor is refusal-only: a successful (ok:true) narrowed read
// is untouched, so listing one field still returns exactly that field (no
// surprise `ok` injection).
TEST(McpProjection, Ants2112SuccessNarrowingUnchanged) {
    const QJsonObject o = parse(mcp::projectFields(kBody, fields({"bullets"})));
    EXPECT_EQ(o.size(), 1);
    EXPECT_FALSE(o.contains("ok"));
    EXPECT_TRUE(o.contains("bullets"));
}

// INV-8 — allowlist is exactly the eleven in-scope tools (original seven +
// read_log/ANTS-1855, model_switch_stats/ANTS-1735, read_region/ANTS-2021,
// codebase_index/ANTS-1637 — matches the makeFieldsProp() call-site count).
TEST(McpProjection, Inv8AllowlistExact) {
    for (const char *t : {"roadmap_query", "project_layout", "file_outline",
                          "get_environment", "tab_list", "subsystem",
                          "git_state", "read_log", "model_switch_stats",
                          "read_region", "codebase_index"}) {
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
    // read_log → 8; ANTS-1735 added model_switch_stats → 9; ANTS-2021
    // added read_region → 10; ANTS-1637 added codebase_index → 11). The
    // lambda definition reads `makeFieldsProp = [` so it is not counted
    // by the call-form needle.
    int count = 0;
    int idx = 0;
    const QByteArray needle = "makeFieldsProp();";
    while ((idx = s.indexOf(needle, idx)) != -1) { ++count; idx += needle.size(); }
    EXPECT_EQ(count, 11) << "expected 11 makeFieldsProp() call sites, got "
                         << count;
}

// ───────────────────────────────────────────────────────────────────
// ANTS-2081 + ANTS-2086 — mcp::appendReadHints: etag-reuse + leaner-mode
// nudges on large successful read responses.
// ───────────────────────────────────────────────────────────────────

namespace {
QString bigBodyWithEtag() {
    QString filler;
    while (filler.size() < 5000) filler += QChar('x');
    return QStringLiteral("{\"ok\":true,\"etag\":\"abc123\",\"pad\":\"")
         + filler + QStringLiteral("\"}");
}
QString bigBodyNoEtag() {
    QString filler;
    while (filler.size() < 5000) filler += QChar('x');
    return QStringLiteral("{\"ok\":true,\"pad\":\"")
         + filler + QStringLiteral("\"}");
}
}  // namespace

// Large roadmap_query body gets BOTH the etag-reuse + leaner-mode nudges.
TEST(McpReadHints, Ants2081And2086RoadmapQueryLargeBody) {
    const QJsonObject o = parse(mcp::appendReadHints(
        QStringLiteral("roadmap_query"), QJsonObject{}, bigBodyWithEtag(),
        /*etagUnchanged=*/false));
    ASSERT_TRUE(o.contains("next_call_hint"));
    EXPECT_NE(o.value("next_call_hint").toString().indexOf("abc123"), -1);
    ASSERT_TRUE(o.contains("leaner_call_hint"));
    EXPECT_NE(o.value("leaner_call_hint").toString().indexOf("headline_only"),
              -1);
}

// Below the byte threshold → untouched, byte-for-byte.
TEST(McpReadHints, Ants2086SmallBodyUntouched) {
    const QString body =
        QStringLiteral("{\"ok\":true,\"etag\":\"abc\"}");
    EXPECT_EQ(mcp::appendReadHints(QStringLiteral("roadmap_query"),
                                   QJsonObject{}, body, false),
              body);
}

// A 304 (etagUnchanged) never gets nudges.
TEST(McpReadHints, Ants2081NoHintsOn304) {
    const QString body = bigBodyWithEtag();
    EXPECT_EQ(mcp::appendReadHints(QStringLiteral("roadmap_query"),
                                   QJsonObject{}, body, /*etagUnchanged=*/true),
              body);
}

// A caller already narrowing with fields= is left alone.
TEST(McpReadHints, Ants2081NoHintsWhenFieldsPresent) {
    QJsonObject args;
    args["fields"] = QJsonArray{QStringLiteral("etag")};
    const QString body = bigBodyWithEtag();
    EXPECT_EQ(mcp::appendReadHints(QStringLiteral("roadmap_query"),
                                   args, body, false),
              body);
}

// Refusals (ok:false) are never nudged.
TEST(McpReadHints, Ants2086NoHintsOnRefusal) {
    QString filler;
    while (filler.size() < 5000) filler += QChar('x');
    const QString body = QStringLiteral("{\"ok\":false,\"error\":\"")
                       + filler + QStringLiteral("\"}");
    EXPECT_EQ(mcp::appendReadHints(QStringLiteral("roadmap_query"),
                                   QJsonObject{}, body, false),
              body);
}

// A caller already threading etag_match doesn't get the reuse nudge.
TEST(McpReadHints, Ants2081EtagMatchSuppressesReuseHint) {
    QJsonObject args;
    args["etag_match"] = QStringLiteral("old");
    const QJsonObject o = parse(mcp::appendReadHints(
        QStringLiteral("roadmap_query"), args, bigBodyWithEtag(), false));
    EXPECT_FALSE(o.contains("next_call_hint"));
}

// workspace_search names its own cheaper knob.
TEST(McpReadHints, Ants2086WorkspaceSearchLeanerHint) {
    const QJsonObject o = parse(mcp::appendReadHints(
        QStringLiteral("workspace_search"), QJsonObject{}, bigBodyNoEtag(),
        false));
    ASSERT_TRUE(o.contains("leaner_call_hint"));
    EXPECT_NE(o.value("leaner_call_hint").toString().indexOf("max_match_bytes"),
              -1);
}

// roadmap_query already in a lean mode → no leaner nudge (etag hint only).
TEST(McpReadHints, Ants2086NoLeanerHintWhenAlreadyLean) {
    QJsonObject args;
    args["mode"] = QStringLiteral("headline_only");
    const QJsonObject o = parse(mcp::appendReadHints(
        QStringLiteral("roadmap_query"), args, bigBodyWithEtag(), false));
    EXPECT_FALSE(o.contains("leaner_call_hint"));
    EXPECT_TRUE(o.contains("next_call_hint"));
}

// ───────────────────────────────────────────────────────────────────
// ANTS-2091 — mcp::compactEnvelope: drop dead-weight fields, keep the
// protected branch-on keys, recurse.
// ───────────────────────────────────────────────────────────────────

// Dead-weight scalars/collections are dropped; live values are kept.
TEST(McpCompact, Ants2091DropsDeadWeightKeepsLive) {
    const QString body = QStringLiteral(
        "{\"ok\":true,\"truncated\":false,\"walk_capped\":false,"
        "\"scope\":\"\",\"skipped\":[],\"meta\":{},\"nullish\":null,"
        "\"count\":3,\"matches\":[1,2]}");
    const QJsonObject o = parse(mcp::compactEnvelope(body));
    EXPECT_TRUE(o.value("ok").toBool());
    EXPECT_EQ(o.value("count").toInt(), 3);
    EXPECT_EQ(o.value("matches").toArray().size(), 2);
    // All the dead weight is gone.
    for (const char *k : {"truncated", "walk_capped", "scope", "skipped",
                          "meta", "nullish"})
        EXPECT_FALSE(o.contains(k)) << k << " should be dropped";
}

// A numeric 0 is load-bearing and kept (flipped_count:0 etc.).
TEST(McpCompact, Ants2091KeepsZero) {
    const QJsonObject o =
        parse(mcp::compactEnvelope(QStringLiteral("{\"flipped_count\":0}")));
    ASSERT_TRUE(o.contains("flipped_count"));
    EXPECT_EQ(o.value("flipped_count").toInt(), 0);
}

// Protected keys survive at the top level even when false / empty.
TEST(McpCompact, Ants2091ProtectsBranchKeys) {
    const QString body = QStringLiteral(
        "{\"ok\":false,\"found\":false,\"unchanged\":false,"
        "\"code\":\"bad_args\",\"error\":\"\",\"etag\":\"\"}");
    const QJsonObject o = parse(mcp::compactEnvelope(body));
    ASSERT_TRUE(o.contains("ok"));
    EXPECT_FALSE(o.value("ok").toBool());
    ASSERT_TRUE(o.contains("found"));
    EXPECT_FALSE(o.value("found").toBool());
    EXPECT_TRUE(o.contains("unchanged"));
    EXPECT_EQ(o.value("code").toString(), QStringLiteral("bad_args"));
    // error/etag are protected even though empty (verbatim).
    EXPECT_TRUE(o.contains("error"));
    EXPECT_TRUE(o.contains("etag"));
}

// Recurses into nested objects AND array elements; a child emptied by
// pruning is itself dropped.
TEST(McpCompact, Ants2091RecursesAndPrunesEmptiedChildren) {
    const QString body = QStringLiteral(
        "{\"ok\":true,"
        "\"bullets\":[{\"id\":\"A-1\",\"kind\":\"\",\"lanes\":[]},"
        "             {\"id\":\"A-2\",\"kind\":\"fix\",\"lanes\":[]}],"
        "\"deep\":{\"a\":false,\"b\":\"\"}}");
    const QJsonObject o = parse(mcp::compactEnvelope(body));
    // deep became empty after pruning → dropped entirely.
    EXPECT_FALSE(o.contains("deep"));
    const QJsonArray b = o.value("bullets").toArray();
    ASSERT_EQ(b.size(), 2);
    const QJsonObject b0 = b.at(0).toObject();
    EXPECT_EQ(b0.value("id").toString(), QStringLiteral("A-1"));
    EXPECT_FALSE(b0.contains("kind"));   // "" dropped
    EXPECT_FALSE(b0.contains("lanes"));  // [] dropped
    const QJsonObject b1 = b.at(1).toObject();
    EXPECT_EQ(b1.value("kind").toString(), QStringLiteral("fix"));
}

// A scalar array (e.g. a list of booleans) is preserved element-for-element
// — array membership is meaningful, so a `false` element is NOT dropped.
TEST(McpCompact, Ants2091KeepsScalarArrayElements) {
    const QJsonObject o = parse(mcp::compactEnvelope(
        QStringLiteral("{\"flags\":[true,false,true]}")));
    ASSERT_TRUE(o.contains("flags"));
    EXPECT_EQ(o.value("flags").toArray().size(), 3);
}

// Non-object body passes through unchanged.
TEST(McpCompact, Ants2091NonObjectPassthrough) {
    const QString raw = QStringLiteral("not json");
    EXPECT_EQ(mcp::compactEnvelope(raw), raw);
    const QString arr = QStringLiteral("[1,2,3]");
    EXPECT_EQ(mcp::compactEnvelope(arr), arr);
}

// Dispatch wiring: compactEnvelope runs after fields= projection, gated on
// the allowlist + the compact arg, and each in-scope tool declares the prop.
TEST(McpCompact, Ants2091DispatchAndSchemaWiring) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray s = f.readAll();
    const int proj = s.indexOf("mcp::projectFields(");
    const int comp = s.indexOf("mcp::compactEnvelope(");
    ASSERT_GT(comp, 0) << "compactEnvelope dispatch call site not found";
    EXPECT_LT(proj, comp) << "compaction must run after fields= projection";
    EXPECT_TRUE(s.contains("argsObj.value(QStringLiteral(\"compact\"))"))
        << "compaction must read the compact arg";
    // ANTS-2085 — when the compact arg is absent the dispatcher falls back
    // to the session/user terse default.
    EXPECT_TRUE(s.contains("mcp::terseDefault()"))
        << "compaction must fall back to mcp::terseDefault() when compact "
           "is absent";
    // 11 compact schema props, one per in-scope projection tool.
    int count = 0, idx = 0;
    const QByteArray needle = "makeCompactProp();";
    while ((idx = s.indexOf(needle, idx)) != -1) { ++count; idx += needle.size(); }
    EXPECT_EQ(count, 11) << "expected 11 makeCompactProp() call sites, got "
                         << count;
}

// ───────────────────────────────────────────────────────────────────
// ANTS-2085 — terse-by-default flag. setTerseDefault/terseDefault is the
// process-global the dispatcher reads when a call omits `compact`. The
// behaviour (compactEnvelope applied on the fallback) is covered by the
// McpCompact transform tests above + the dispatch-wiring grep; here we
// pin the getter/setter contract and the default.
// ───────────────────────────────────────────────────────────────────
TEST(McpTerseDefault, Ants2085GetterSetterRoundTrips) {
    // Module default is false (conservative for direct library/test use;
    // the application turns it on via the claude.mcp_terse_responses config
    // key, default true).
    EXPECT_FALSE(mcp::terseDefault());
    mcp::setTerseDefault(true);
    EXPECT_TRUE(mcp::terseDefault());
    mcp::setTerseDefault(false);
    EXPECT_FALSE(mcp::terseDefault());   // restore module default for siblings
}
