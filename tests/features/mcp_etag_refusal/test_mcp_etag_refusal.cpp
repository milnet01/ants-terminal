// ANTS-4446 — the dispatcher-level ETag short-circuit must never speak for a
// refusal. Behavioural coverage of ClaudeIntegration::applyEtagPattern.
// See tests/features/mcp_etag_refusal/spec.md.

#include "claudeintegration.h"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace {

// An etag-supported verb, so the pattern is actually exercised; a tool
// outside isEtagSupportedTool returns early and would pass every case here
// for the wrong reason.
QString tool() { return QStringLiteral("roadmap_query"); }

QString refusal() {
    return QStringLiteral(
        R"({"ok":false,"code":"bad_args","error":"caller_cwd is required"})");
}

QString success() {
    return QStringLiteral(R"({"ok":true,"bullets":[],"count":0})");
}

QJsonObject parse(const QString &s) {
    return QJsonDocument::fromJson(s.toUtf8()).object();
}

QJsonObject withEtagMatch(const QString &etag) {
    QJsonObject a;
    a[QStringLiteral("etag_match")] = etag;
    return a;
}

}  // namespace

// INV-1 — a refusal is returned verbatim and carries no etag. This is the
// root: the caller can only replay an etag it was given, so withholding it
// here is what makes INV-2 unreachable in practice rather than merely
// guarded.
TEST(McpEtagRefusal, Inv1RefusalGetsNoEtag) {
    bool unchanged = true;  // seeded wrong on purpose
    const QJsonObject o =
        parse(ClaudeIntegration::applyEtagPattern(tool(), QJsonObject{},
                                                  refusal(), &unchanged));
    EXPECT_FALSE(o.contains("etag"))
        << "a refusal envelope must not be etag-eligible";
    EXPECT_FALSE(unchanged);
    // Returned verbatim: the refusal floor survives intact.
    EXPECT_FALSE(o.value("ok").toBool());
    EXPECT_EQ(o.value("code").toString(), QStringLiteral("bad_args"));
    EXPECT_EQ(o.value("error").toString(),
              QStringLiteral("caller_cwd is required"));
}

// INV-2 — the short-circuit never fires on a refusal, even when the caller's
// etag_match equals that refusal's own hash. Before ANTS-4446 the 304 arm ran
// before the parse, so this returned {ok:true, unchanged:true} for a call
// that had just been refused.
TEST(McpEtagRefusal, Inv2RefusalIsNeverShortCircuited) {
    const QString etag = ClaudeIntegration::etagFor(refusal());
    ASSERT_FALSE(etag.isEmpty());

    bool unchanged = true;
    const QJsonObject o = parse(ClaudeIntegration::applyEtagPattern(
        tool(), withEtagMatch(etag), refusal(), &unchanged));

    EXPECT_FALSE(unchanged) << "a refusal must not report a 304";
    EXPECT_FALSE(o.value("ok").toBool())
        << "the caller must still see ok:false";
    EXPECT_FALSE(o.contains("unchanged"));
    EXPECT_EQ(o.value("code").toString(), QStringLiteral("bad_args"));
}

// INV-3 — the guard against fixing INV-1/2 by disabling the pattern. A
// success still gets its etag, and a matching etag_match still short-circuits
// to the documented {ok:true, unchanged:true, etag} shape.
TEST(McpEtagRefusal, Inv3SuccessStillShortCircuits) {
    bool unchanged = true;
    const QJsonObject first = parse(ClaudeIntegration::applyEtagPattern(
        tool(), QJsonObject{}, success(), &unchanged));
    ASSERT_TRUE(first.contains("etag")) << "a success must stay etag-eligible";
    EXPECT_FALSE(unchanged);

    const QString etag = first.value("etag").toString();
    bool unchanged2 = false;
    const QJsonObject second = parse(ClaudeIntegration::applyEtagPattern(
        tool(), withEtagMatch(etag), success(), &unchanged2));

    EXPECT_TRUE(unchanged2);
    EXPECT_TRUE(second.value("ok").toBool());
    EXPECT_TRUE(second.value("unchanged").toBool());
    EXPECT_EQ(second.value("etag").toString(), etag);
}

// INV-4 — an envelope with no `ok` key is a success, not a refusal. Pinned so
// this guard and mcp::projectFields' refusal floor keep the same definition;
// if they diverge, one of them is narrowing an error envelope the other is
// protecting.
TEST(McpEtagRefusal, Inv4NoOkKeyIsNotARefusal) {
    bool unchanged = true;
    const QJsonObject o = parse(ClaudeIntegration::applyEtagPattern(
        tool(), QJsonObject{}, QStringLiteral(R"({"bullets":[]})"),
        &unchanged));
    EXPECT_TRUE(o.contains("etag"))
        << "absent ok must not be read as a refusal";
    EXPECT_FALSE(unchanged);
}
