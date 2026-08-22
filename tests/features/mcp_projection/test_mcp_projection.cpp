// ANTS-1720 — feature-conformance test for MCP `fields=` response
// projection. Behavioural coverage of mcp::projectFields +
// mcp::isFieldProjectionTool (pure, Qt6::Core), plus a source-scrape of
// the dispatch ordering invariant in claudeintegration.cpp.
// See tests/features/mcp_projection/spec.md.

#include "mcpprojection.h"

#include <gtest/gtest.h>

#include <iterator>  // std::size — INV-10 derives its count

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

// The in-scope projection tools, named ONCE. INV-8 asserts each is a member
// of mcp::isFieldProjectionTool; INV-10 asserts each declares a `fields`
// schema property. Those two must agree, and sharing the list is what makes
// them agree by construction rather than by somebody remembering.
//
// ANTS-4624 — they did not agree, and the gap shipped. INV-10 asserted a
// HARDCODED call-site count of 14, so when ANTS-4523 added session_orient to
// the allowlist (15) the count still matched the 14 verbs that DO declare the
// property, INV-10 kept passing, and the one member without it went out. The
// dispatcher then required an array (`fv.isArray()`), the undeclared argument
// arrived as a string, and the projection was silently skipped on the largest
// response in the session. Deriving the count from this array is the whole
// fix: adding a tool to one list now fails the other until both are done.
//
// Do NOT replace std::size(...) with a literal. The literal is the defect.
constexpr const char *kFieldProjectionTools[] = {
    "roadmap_query", "changelog_query", "project_layout", "file_outline",
    "get_environment", "tab_list", "subsystem", "git_state", "read_log",
    "model_switch_stats", "read_region", "codebase_index", "docs_index",
    "co_change_family", "session_orient"};

// INV-8 — every in-scope tool is field-projectable, and the out-of-scope
// ones are not. ANTS-4624 widened the positive list from 13 to the full 15:
// changelog_query (ANTS-3533) and co_change_family (ANTS-3368) were on the
// allowlist and asserted by nothing, in a test calling itself Exact.
TEST(McpProjection, Inv8AllowlistExact) {
    for (const char *t : kFieldProjectionTools) {
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

// INV-10 — each in-scope tool declares a `fields` schema property. The count
// is DERIVED from kFieldProjectionTools above, so a tool added to the
// allowlist without a schema property fails here (ANTS-4624).
TEST(McpProjection, Inv10SchemaDeclaresFields) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray s = f.readAll();
    EXPECT_TRUE(s.contains("auto makeFieldsProp"))
        << "the shared `fields` schema fragment must be defined once";
    // The lambda definition reads `makeFieldsProp = [` so it is not counted
    // by the call-form needle.
    int count = 0;
    int idx = 0;
    const QByteArray needle = "makeFieldsProp();";
    while ((idx = s.indexOf(needle, idx)) != -1) { ++count; idx += needle.size(); }
    EXPECT_EQ(count, int(std::size(kFieldProjectionTools)))
        << "every tool in kFieldProjectionTools must declare a `fields` "
           "schema property; expected "
        << std::size(kFieldProjectionTools) << " makeFieldsProp() call sites, got "
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

// Below the leaner-byte threshold AND no etag → untouched, byte-for-byte.
TEST(McpReadHints, Ants2086SmallBodyNoEtagUntouched) {
    const QString body =
        QStringLiteral("{\"ok\":true,\"count\":3}");
    EXPECT_EQ(mcp::appendReadHints(QStringLiteral("roadmap_query"),
                                   QJsonObject{}, body, false),
              body);
}

// ANTS-2180 — a small (< 4 KiB) etag-bearing body STILL gets the
// etag-reuse nudge (a 304 next call saves the full body regardless of
// this slice's size — the read_region / file_outline re-read loop), but
// NOT the leaner nudge, which keeps its worthwhile-body gate.
TEST(McpReadHints, Ants2180SmallEtagBodyGetsReuseHintOnly) {
    const QString body =
        QStringLiteral("{\"ok\":true,\"etag\":\"abc123\"}");
    const QJsonObject o = parse(mcp::appendReadHints(
        QStringLiteral("file_outline"), QJsonObject{}, body,
        /*etagUnchanged=*/false));
    ASSERT_TRUE(o.contains("next_call_hint"));
    EXPECT_NE(o.value("next_call_hint").toString().indexOf("abc123"), -1);
    EXPECT_FALSE(o.contains("leaner_call_hint"))
        << "the byte-gated leaner nudge must not fire on a small body";
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

// workspace_search names its own cheaper knob. ANTS-3548 made the
// max_match_bytes clip default-ON, so the hint no longer nudges it —
// the remaining levers are a narrower search / a leaner row shape.
TEST(McpReadHints, Ants2086WorkspaceSearchLeanerHint) {
    const QJsonObject o = parse(mcp::appendReadHints(
        QStringLiteral("workspace_search"), QJsonObject{}, bigBodyNoEtag(),
        false));
    ASSERT_TRUE(o.contains("leaner_call_hint"));
    EXPECT_NE(o.value("leaner_call_hint").toString().indexOf("narrower lane"),
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
// ANTS-3550 — the advisory-hint "already-taught" latch. Once enabled, a
// (tool, hint-kind) pair emits its nudge once per process, then is
// suppressed. Module default OFF (existing McpReadHints tests above run
// with the latch off, so they see a hint on every call). The fixture's
// TearDown always restores the module default (OFF + empty set) so a
// failing assertion can't leak latch state into a sibling test.
// ───────────────────────────────────────────────────────────────────
class McpHintLatch : public ::testing::Test {
protected:
    void SetUp() override { mcp::resetHintLatch(); }
    void TearDown() override {
        mcp::setHintLatchEnabled(false);
        mcp::resetHintLatch();
    }
};

// Getter/setter round-trip; the module default is OFF (mirrors terseDefault).
TEST_F(McpHintLatch, Ants3550GetterSetterDefaultOff) {
    EXPECT_FALSE(mcp::hintLatchEnabled());  // OFF at rest (TearDown restores it)
    mcp::setHintLatchEnabled(true);
    EXPECT_TRUE(mcp::hintLatchEnabled());
    mcp::setHintLatchEnabled(false);
    EXPECT_FALSE(mcp::hintLatchEnabled());
}

// Enabled: the first emission of each kind passes, the identical repeat is
// dropped — and a body with both nudges suppressed returns unchanged.
TEST_F(McpHintLatch, Ants3550SuppressesRepeatWhenEnabled) {
    mcp::setHintLatchEnabled(true);
    const QJsonObject first = parse(mcp::appendReadHints(
        QStringLiteral("roadmap_query"), QJsonObject{}, bigBodyWithEtag(), false));
    ASSERT_TRUE(first.contains("next_call_hint"));
    ASSERT_TRUE(first.contains("leaner_call_hint"));
    const QJsonObject second = parse(mcp::appendReadHints(
        QStringLiteral("roadmap_query"), QJsonObject{}, bigBodyWithEtag(), false));
    EXPECT_FALSE(second.contains("next_call_hint"))
        << "the etag-reuse nudge must be taught once per process";
    EXPECT_FALSE(second.contains("leaner_call_hint"))
        << "the leaner nudge must be taught once per process";
}

// Keyed per (tool, kind): a DISTINCT verb's own nudge is never hidden by
// another verb having been taught — only exact (tool, kind) repeats drop.
TEST_F(McpHintLatch, Ants3550PerToolKeyingKeepsDistinctTip) {
    mcp::setHintLatchEnabled(true);
    // roadmap_query claims both its kinds.
    parse(mcp::appendReadHints(QStringLiteral("roadmap_query"), QJsonObject{},
                               bigBodyWithEtag(), false));
    // workspace_search's own (different) leaner tip still emits.
    const QJsonObject ws = parse(mcp::appendReadHints(
        QStringLiteral("workspace_search"), QJsonObject{}, bigBodyNoEtag(), false));
    ASSERT_TRUE(ws.contains("leaner_call_hint"));
    EXPECT_NE(ws.value("leaner_call_hint").toString().indexOf("narrower lane"),
              -1);
    // file_outline's etag-reuse nudge still emits (roadmap_query's claim is
    // its own key, not a global next_call_hint latch).
    const QString etagBody = QStringLiteral("{\"ok\":true,\"etag\":\"zzz\"}");
    const QJsonObject fo = parse(mcp::appendReadHints(
        QStringLiteral("file_outline"), QJsonObject{}, etagBody, false));
    EXPECT_TRUE(fo.contains("next_call_hint"));
}

// The two kinds latch independently: teaching next_call_hint for a tool does
// not consume that tool's leaner_call_hint slot.
TEST_F(McpHintLatch, Ants3550KindsLatchIndependently) {
    mcp::setHintLatchEnabled(true);
    // Small etag-only body: eligible for next_call_hint only (below the
    // leaner byte gate) → claims file_outline|next_call_hint.
    const QString small = QStringLiteral("{\"ok\":true,\"etag\":\"e1\"}");
    const QJsonObject a = parse(mcp::appendReadHints(
        QStringLiteral("file_outline"), QJsonObject{}, small, false));
    ASSERT_TRUE(a.contains("next_call_hint"));
    ASSERT_FALSE(a.contains("leaner_call_hint"));
    // A big file_outline body now: next_call_hint suppressed (already taught),
    // but leaner_call_hint is a fresh kind and still emits.
    const QJsonObject b = parse(mcp::appendReadHints(
        QStringLiteral("file_outline"), QJsonObject{}, bigBodyWithEtag(), false));
    EXPECT_FALSE(b.contains("next_call_hint")) << "next kind already taught";
    ASSERT_TRUE(b.contains("leaner_call_hint")) << "leaner kind is independent";
    EXPECT_NE(b.value("leaner_call_hint").toString().indexOf("filter="), -1);
}

// Disabled (module default): repeats are never suppressed — byte-for-byte the
// pre-ANTS-3550 behaviour.
TEST_F(McpHintLatch, Ants3550DisabledNeverSuppresses) {
    mcp::setHintLatchEnabled(false);
    const QJsonObject a = parse(mcp::appendReadHints(
        QStringLiteral("roadmap_query"), QJsonObject{}, bigBodyWithEtag(), false));
    const QJsonObject b = parse(mcp::appendReadHints(
        QStringLiteral("roadmap_query"), QJsonObject{}, bigBodyWithEtag(), false));
    EXPECT_TRUE(a.contains("next_call_hint"));
    EXPECT_TRUE(b.contains("next_call_hint")) << "latch off: no suppression";
    EXPECT_TRUE(a.contains("leaner_call_hint"));
    EXPECT_TRUE(b.contains("leaner_call_hint"));
}

// resetHintLatch() forgets the taught set, so the next call re-teaches.
TEST_F(McpHintLatch, Ants3550ResetReteaches) {
    mcp::setHintLatchEnabled(true);
    parse(mcp::appendReadHints(QStringLiteral("roadmap_query"), QJsonObject{},
                               bigBodyWithEtag(), false));
    const QJsonObject suppressed = parse(mcp::appendReadHints(
        QStringLiteral("roadmap_query"), QJsonObject{}, bigBodyWithEtag(), false));
    EXPECT_FALSE(suppressed.contains("next_call_hint"));
    mcp::resetHintLatch();
    const QJsonObject reTaught = parse(mcp::appendReadHints(
        QStringLiteral("roadmap_query"), QJsonObject{}, bigBodyWithEtag(), false));
    EXPECT_TRUE(reTaught.contains("next_call_hint")) << "reset must re-teach";
}

// Wiring: config exposes the off-switch key (default ON) and the app publishes
// it to the module flag at load / external reload.
TEST(McpHintLatchWiring, Ants3550ConfigAndAppPublish) {
    QFile cfg(QString::fromUtf8(SRC_CONFIG_CPP_PATH));
    ASSERT_TRUE(cfg.open(QIODevice::ReadOnly));
    const QByteArray c = cfg.readAll();
    EXPECT_TRUE(c.contains("value(\"claude.mcp_hint_latch\").toBool(true)"))
        << "config getter must expose claude.mcp_hint_latch defaulting true";
    QFile mw(QString::fromUtf8(SRC_MAINWINDOW_CPP));
    ASSERT_TRUE(mw.open(QIODevice::ReadOnly));
    const QByteArray m = mw.readAll();
    EXPECT_TRUE(m.contains("mcp::setHintLatchEnabled("))
        << "mainwindow must publish the latch flag";
    EXPECT_TRUE(m.contains("claudeMcpHintLatch()"))
        << "mainwindow must read the claude.mcp_hint_latch getter";
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
    // 12 compact schema props, one per in-scope projection tool
    // (ANTS-3533 added changelog_query → 12).
    int count = 0, idx = 0;
    const QByteArray needle = "makeCompactProp();";
    while ((idx = s.indexOf(needle, idx)) != -1) { ++count; idx += needle.size(); }
    EXPECT_EQ(count, 12) << "expected 12 makeCompactProp() call sites, got "
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

// ───────────────────────────────────────────────────────────────────
// ANTS-3532 — "auto-compact large replies by default" is DELIVERED by
// ANTS-2085's terse-default path, NOT by a separate config key (the
// roadmap item predated ANTS-2085 and its premise — "compact is opt-in
// per call" — was already stale). These guards lock in the three
// load-bearing facts that make the saving default-ON, so a future edit
// can't silently disable it:
//   (1) the config key defaults to true,
//   (2) startup publishes it into the dispatch-read global, and
//   (3) compaction is ordered BEFORE the offload/spill step (so a body
//       that compacts under the spill threshold skips the round-trip).
// ANTS-3532 closed as satisfied-by-ANTS-2085; see the ROADMAP note.
// ───────────────────────────────────────────────────────────────────
TEST(McpTerseDefault, Ants3532DefaultOnAndOrderedBeforeOffload) {
    // (1) claude.mcp_terse_responses defaults to true — the source of the
    // default-ON compaction. If this flips, every session loses the
    // no-opt-in saving with no other test catching it.
    QFile cfg(QString::fromUtf8(SRC_CONFIG_CPP_PATH));
    ASSERT_TRUE(cfg.open(QIODevice::ReadOnly));
    const QByteArray cfgSrc = cfg.readAll();
    EXPECT_TRUE(cfgSrc.contains(
        "m_data.value(\"claude.mcp_terse_responses\").toBool(true)"))
        << "claude.mcp_terse_responses must default to true — default-ON "
           "compaction (ANTS-3532's goal) depends on it";

    // (2) startup wires the config value into mcp::setTerseDefault so the
    // dispatch-read process-global reflects it.
    QFile mw(QString::fromUtf8(SRC_MAINWINDOW_CPP));
    ASSERT_TRUE(mw.open(QIODevice::ReadOnly));
    const QByteArray mwSrc = mw.readAll();
    EXPECT_TRUE(mwSrc.contains(
        "setTerseDefault(m_config.claudeMcpTerseResponses())"))
        << "startup must publish the terse-default config into the "
           "dispatch-read global";

    // (3) compaction runs BEFORE the offload/spill step: a body that
    // compacts under the spill threshold skips the read_spill round-trip
    // (ANTS-3532's ordering requirement — already satisfied).
    QFile ci(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(ci.open(QIODevice::ReadOnly));
    const QByteArray ciSrc = ci.readAll();
    const int comp = ciSrc.indexOf("mcp::compactEnvelope(");
    const int offl = ciSrc.indexOf("mcp::offloadBody(");
    ASSERT_GT(comp, 0) << "compactEnvelope dispatch call site not found";
    ASSERT_GT(offl, 0) << "offloadBody dispatch call site not found";
    EXPECT_LT(comp, offl)
        << "compaction must run before offload so a compacted body can "
           "skip the spill round-trip";
}

// ───────────────────────────────────────────────────────────────────
// ANTS-2090 — mcp::tabularize: pack homogeneous top-level arrays-of-objects
// into a columnar {__cols__, __rows__} form. See docs/specs/ANTS-2090.md.
// ───────────────────────────────────────────────────────────────────

namespace {

// Decode a tabularized field back to the array of objects (the consuming
// session's recipe): zip __cols__ with each __rows__ entry; a null cell
// means the key was absent on that element (missing-key ⟺ null — INV-3).
QJsonArray detabularize(const QJsonObject &tab) {
    const QJsonArray cols = tab.value("__cols__").toArray();
    const QJsonArray rows = tab.value("__rows__").toArray();
    QJsonArray out;
    for (const QJsonValue &rv : rows) {
        const QJsonArray row = rv.toArray();
        QJsonObject o;
        for (int i = 0; i < cols.size(); ++i) {
            const QJsonValue cell = row.at(i);
            if (!cell.isNull()) o.insert(cols.at(i).toString(), cell);
        }
        out.append(o);
    }
    return out;
}

// A roadmap-ish envelope of `n` bullet objects with longish, repeated keys
// (so the columnar header is amortised and the form is strictly smaller).
QString bulletBody(int n) {
    QJsonArray bullets;
    for (int i = 0; i < n; ++i) {
        QJsonObject b;
        b["headline_oneline"] = QStringLiteral("bullet number %1").arg(i);
        b["id"] = QStringLiteral("ANTS-%1").arg(2000 + i);
        b["section_slug"] = QStringLiteral("some-section-slug");
        b["status"] = QStringLiteral("📋");
        bullets.append(b);
    }
    QJsonObject env;
    env["ok"] = true;
    env["bullets"] = bullets;
    env["count"] = n;
    return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
}

}  // namespace

// INV-2/INV-3 — an eligible homogeneous array becomes columnar, and zipping
// __cols__ with __rows__ reconstructs the original array (length-preserving:
// __rows__.length == element count, and the sibling `count` stays consistent).
TEST(McpTabular, Ants2090TransformAndRoundTrip) {
    const QJsonObject before = parse(bulletBody(5));
    const QJsonObject o = parse(mcp::tabularize(bulletBody(5)));

    // The array field is now an object carrying the columnar discriminators.
    ASSERT_TRUE(o.value("bullets").isObject());
    const QJsonObject tab = o.value("bullets").toObject();
    ASSERT_TRUE(tab.contains("__cols__"));
    ASSERT_TRUE(tab.contains("__rows__"));
    EXPECT_EQ(tab.value("__rows__").toArray().size(), 5);
    EXPECT_EQ(o.value("count").toInt(), 5);
    EXPECT_EQ(o.value("count").toInt(), tab.value("__rows__").toArray().size());
    // Scalars untouched.
    EXPECT_TRUE(o.value("ok").toBool());

    // Round-trip equals the original parsed array, element order preserved.
    EXPECT_EQ(detabularize(tab), before.value("bullets").toArray());
}

// INV-2 — ineligible top-level arrays are left unchanged: empty, single
// element, a scalar array, and an array with a non-object element.
TEST(McpTabular, Ants2090EligibilitySkips) {
    for (const char *body : {
             "{\"ok\":true,\"a\":[]}",                       // empty
             "{\"ok\":true,\"a\":[{\"id\":\"x\",\"k\":1}]}", // single element
             "{\"ok\":true,\"a\":[1,2,3]}",                  // scalars
             "{\"ok\":true,\"a\":[{\"id\":1},[9,9]]}"}) {     // a non-object elem
        const QString s = QString::fromUtf8(body);
        EXPECT_EQ(mcp::tabularize(s), s) << "should be inert on: " << body;
    }
}

// INV-4 — never costs bytes: a 2-element array of disjoint short keys does
// not shrink in columnar form, so the original array is kept verbatim.
TEST(McpTabular, Ants2090NeverCostsBytes) {
    const QString s = QStringLiteral("{\"ok\":true,\"a\":[{\"x\":1},{\"y\":2}]}");
    EXPECT_EQ(mcp::tabularize(s), s);
}

// INV-5 — nested object/array cell values are carried verbatim into the row.
TEST(McpTabular, Ants2090NestedValuesPreserved) {
    QJsonArray rows;
    for (int i = 0; i < 4; ++i) {
        QJsonObject b;
        b["id"] = QStringLiteral("ANTS-%1").arg(i);
        b["lanes"] = QJsonArray{QStringLiteral("core"), QStringLiteral("vt")};
        b["headline_oneline"] = QStringLiteral("a fairly long headline here");
        rows.append(b);
    }
    QJsonObject env; env["ok"] = true; env["bullets"] = rows;
    const QString s =
        QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
    const QJsonObject o = parse(mcp::tabularize(s));
    ASSERT_TRUE(o.value("bullets").isObject());
    const QJsonArray back = detabularize(o.value("bullets").toObject());
    ASSERT_EQ(back.size(), 4);
    EXPECT_EQ(back.at(0).toObject().value("lanes").toArray().size(), 2);
    EXPECT_EQ(back, rows);  // full nested fidelity
}

// INV-6 — refusal (ok:false) and non-object bodies are returned unchanged.
TEST(McpTabular, Ants2090RefusalAndNonObjectFloor) {
    const QString refusal = QStringLiteral(
        "{\"ok\":false,\"code\":\"bad_args\","
        "\"items\":[{\"a\":1,\"b\":2},{\"a\":3,\"b\":4}]}");
    EXPECT_EQ(mcp::tabularize(refusal), refusal);
    const QString raw = QStringLiteral("not json at all");
    EXPECT_EQ(mcp::tabularize(raw), raw);
    const QString arr = QStringLiteral("[{\"a\":1},{\"a\":2}]");
    EXPECT_EQ(mcp::tabularize(arr), arr);  // top-level array is not an object
}

// INV-7 — determinism + lexicographic __cols__ independent of element order.
// The first element introduces keys out of alphabetical order, so a naive
// first-seen accumulator would differ from the sorted union.
TEST(McpTabular, Ants2090DeterministicLexicographicColumns) {
    // Long keys so the columnar form actually shrinks (and INV-4 lets it pass).
    const QString s = QStringLiteral(
        "{\"rows\":["
        "{\"zeta_field\":\"v1\",\"alpha_field\":\"v2\",\"middle_field\":\"v3\"},"
        "{\"zeta_field\":\"v4\",\"alpha_field\":\"v5\",\"middle_field\":\"v6\"},"
        "{\"zeta_field\":\"v7\",\"alpha_field\":\"v8\",\"middle_field\":\"v9\"}]}");
    const QString a = mcp::tabularize(s);
    const QString b = mcp::tabularize(s);
    EXPECT_EQ(a, b) << "identical input must yield byte-identical output";
    const QJsonObject tab = parse(a).value("rows").toObject();
    const QJsonArray cols = tab.value("__cols__").toArray();
    ASSERT_EQ(cols.size(), 3);
    EXPECT_EQ(cols.at(0).toString(), QStringLiteral("alpha_field"));
    EXPECT_EQ(cols.at(1).toString(), QStringLiteral("middle_field"));
    EXPECT_EQ(cols.at(2).toString(), QStringLiteral("zeta_field"));
}

// INV-3 — missing-key ⟺ explicit-null: an element missing a key and one
// carrying an explicit null decode identically. The union column gets a null
// cell for the missing key; both reconstruct to "key absent" on decode.
TEST(McpTabular, Ants2090MissingVsExplicitNull) {
    // Elements 2 (explicit null) and 3 (missing key) carry identical OTHER
    // values, so the collapse is observable as exact equality on decode.
    // Long keys + 3 elements so the sparse columnar form still shrinks.
    const QString s = QStringLiteral(
        "{\"rows\":["
        "{\"common_one\":\"aaaa\",\"common_two\":\"bbbb\",\"sometimes_here\":\"c\"},"
        "{\"common_one\":\"xxxx\",\"common_two\":\"yyyy\",\"sometimes_here\":null},"
        "{\"common_one\":\"xxxx\",\"common_two\":\"yyyy\"}]}");
    const QJsonObject o = parse(mcp::tabularize(s));
    ASSERT_TRUE(o.value("rows").isObject());
    const QJsonArray back = detabularize(o.value("rows").toObject());
    ASSERT_EQ(back.size(), 3);
    // Element 2 (explicit null) and element 3 (missing) both decode without
    // the `sometimes_here` key — the documented collapse — and, since their
    // other values match, decode to byte-identical objects.
    EXPECT_FALSE(back.at(1).toObject().contains("sometimes_here"));
    EXPECT_FALSE(back.at(2).toObject().contains("sometimes_here"));
    EXPECT_EQ(back.at(1).toObject(), back.at(2).toObject());
}

// INV-1/INV-8/INV-9 — dispatch ordering + the encoding guard. Source-scrape
// asserted BY SYMBOL (never line number): appendReadHints < tabularize <
// offloadBody, and the encoding:"tabular" guard precedes the tabularize call.
TEST(McpTabular, Ants2090DispatchOrderingAndGuard) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray s = f.readAll();

    const int hints = s.indexOf("mcp::appendReadHints(");
    const int tab   = s.indexOf("mcp::tabularize(");
    const int off   = s.indexOf("mcp::offloadBody(");
    ASSERT_GT(tab, 0) << "mcp::tabularize call site not found";
    ASSERT_GT(hints, 0) << "mcp::appendReadHints call site not found";
    ASSERT_GT(off, 0) << "mcp::offloadBody call site not found";
    EXPECT_LT(hints, tab) << "tabularize must run after appendReadHints";
    EXPECT_LT(tab, off)   << "tabularize must run before offloadBody";

    // INV-1 — gated on encoding:"tabular".
    const int guard = s.indexOf("argsObj.value(QStringLiteral(\"encoding\"))");
    ASSERT_GT(guard, 0) << "the encoding-arg guard must be present";
    EXPECT_LT(guard, tab) << "the encoding guard must precede the tabularize call";
    EXPECT_TRUE(s.contains("QStringLiteral(\"tabular\")"))
        << "the guard must compare against \"tabular\"";
}

// §2.4 — each of the 8 list-shaped read verbs declares the `encoding` prop.
TEST(McpTabular, Ants2090SchemaDeclaresEncoding) {
    QFile f(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray s = f.readAll();
    EXPECT_TRUE(s.contains("auto makeEncodingProp"))
        << "the shared `encoding` schema fragment must be defined once";
    // One call site per advertised verb: roadmap_query, find_sources,
    // workspace_search, file_outline, codebase_index, docs_index, find_caller,
    // and doc_citations (ANTS-3636 — citations[] / unparsed[] are both
    // list-shaped, so the columnar repack applies).
    int count = 0, idx = 0;
    const QByteArray needle = "makeEncodingProp();";
    while ((idx = s.indexOf(needle, idx)) != -1) { ++count; idx += needle.size(); }
    EXPECT_EQ(count, 8) << "expected 8 makeEncodingProp() call sites, got "
                        << count;
}

// ANTS-4382 — the leaner nudge stays quiet on a TARGETED fetch. On
// `{ids:[…], include_body:true}` it used to recommend headline_only /
// status / section_index: all three discard the bodies, which is the one
// thing the call explicitly opted into, and two cannot combine with `ids`
// at all — so it partly recommended a refusal. Cosmetic on its own; taken
// because a hint contradicting the call's stated intent trains a caller to
// stop reading hints, which costs more where they are right.
TEST(McpReadHints, Ants4382NoLeanerNudgeOnATargetedFetch) {
    QJsonObject ids;
    ids["ids"] = QJsonArray{QStringLiteral("ANTS-1"), QStringLiteral("ANTS-2")};
    ids["include_body"] = true;
    const QJsonObject o = parse(mcp::appendReadHints(
        QStringLiteral("roadmap_query"), ids, bigBodyWithEtag(), false));
    EXPECT_FALSE(o.contains("leaner_call_hint"))
        << "every option it names would drop the bodies the call asked for";
    // The etag nudge is unrelated and must survive — this narrows one hint,
    // it does not silence the verb.
    EXPECT_TRUE(o.contains("next_call_hint"));

    // include_body alone (no ids) is equally targeted.
    QJsonObject body;
    body["include_body"] = true;
    EXPECT_FALSE(parse(mcp::appendReadHints(QStringLiteral("roadmap_query"),
                                            body, bigBodyWithEtag(), false))
                     .contains("leaner_call_hint"));

    // Control: an UNTARGETED list query still gets the nudge, which is where
    // it is genuinely useful.
    EXPECT_TRUE(parse(mcp::appendReadHints(QStringLiteral("roadmap_query"),
                                           QJsonObject{}, bigBodyWithEtag(),
                                           false))
                    .contains("leaner_call_hint"));
}

// ANTS-4367 — a short query needs a narrowing knob.
//
// `query:"CI"` over 175 bullets returned 20 hits, truncated:true, and the
// first matched only because its body says "decision". "specific",
// "efficiency", "precision" and "facing" all do the same. The damage is
// REORDERING rather than padding: genuine hits are pushed off page one by
// substring noise, and truncated:true gives no clue the answer is in the
// tail — so a session trusting page one concludes the roadmap has no CI
// items. The documented recovery (raise `limit`, page with `offset`) grows
// the payload while the signal-to-noise stays identical.
TEST(McpProjection, Ants4367WholeWordAndRegexNarrowTheQuery) {
    const auto bullet = [](const char *headline, const char *body) {
        QJsonObject o;
        o[QStringLiteral("headline")] = QString::fromUtf8(headline);
        o[QStringLiteral("body")]     = QString::fromUtf8(body);
        return o;
    };
    const QJsonObject noise = bullet("A design decision about efficiency",
                                     "precision matters, facing specifics");
    const QJsonObject real  = bullet("CI is red on main",
                                     "ripgrep is installed by no job");
    const QJsonObject lower = bullet("ci runs on every push", "");

    // Default is unchanged — every existing caller sees the same answers.
    EXPECT_TRUE(mcp::bulletMatchesQuery(noise, QStringLiteral("CI")));
    EXPECT_TRUE(mcp::bulletMatchesQuery(real,  QStringLiteral("CI")));

    // whole_word — the substring noise goes, the genuine hits stay, and the
    // match stays case-insensitive like the default.
    EXPECT_FALSE(mcp::bulletMatchesQuery(noise, QStringLiteral("CI"),
                                         mcp::QueryMode::WholeWord))
        << "\"decision\" / \"efficiency\" / \"precision\" / \"facing\" must not "
           "match a whole-word CI";
    EXPECT_TRUE(mcp::bulletMatchesQuery(real, QStringLiteral("CI"),
                                        mcp::QueryMode::WholeWord));
    EXPECT_TRUE(mcp::bulletMatchesQuery(lower, QStringLiteral("CI"),
                                        mcp::QueryMode::WholeWord))
        << "whole_word narrows the boundary, not the casing";

    // A hyphenated occurrence IS a whole word — `-` is a boundary. Stated as a
    // test because it is the one case where the fix looks wrong at a glance.
    EXPECT_TRUE(mcp::bulletMatchesQuery(bullet("the CI-parity script", ""),
                                        QStringLiteral("CI"),
                                        mcp::QueryMode::WholeWord));

    // regex — the same knob workspace_search spells the same way.
    EXPECT_TRUE(mcp::bulletMatchesQuery(real, QStringLiteral("^CI is"),
                                        mcp::QueryMode::Regex));
    EXPECT_FALSE(mcp::bulletMatchesQuery(noise, QStringLiteral("^CI is"),
                                         mcp::QueryMode::Regex));

    // An invalid pattern must not silently match everything — that would turn
    // a typo into "the roadmap is entirely about X".
    EXPECT_FALSE(mcp::bulletMatchesQuery(real, QStringLiteral("CI("),
                                         mcp::QueryMode::Regex));
}
