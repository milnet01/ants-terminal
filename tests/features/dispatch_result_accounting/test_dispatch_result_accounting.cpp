// Why this exists: recordDispatch derived success as equality with "ok",
// so an ETag 304 booked into the failed accumulators — the counters
// ANTS-1432 added to measure waste — while a handler returning its own
// ok:false never reached dispatchResult and booked as a success.
// See spec.md.
//
// Behaviour tests: both rules are exposed as statics so they can be
// checked without driving a dispatch.

#include "claudeintegration.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <gtest/gtest.h>

TEST(DispatchResultAccounting, Inv1OkIsSuccess) {
    EXPECT_TRUE(ClaudeIntegration::dispatchResultIsSuccess("ok"));
}

TEST(DispatchResultAccounting, Inv2EtagUnchangedIsSuccess) {
    EXPECT_TRUE(ClaudeIntegration::dispatchResultIsSuccess("etag_unchanged"))
        << "a 304 is the cheapest successful outcome the server has — the "
           "caller got what it asked for and the payload was skipped. "
           "Booking it failed puts the largest saving into the counters "
           "that exist to measure waste on failure";
}

TEST(DispatchResultAccounting, Inv3DispatcherRefusalsAreNotSuccess) {
    for (const char *r : {"rate_limited", "caller_cwd_required",
                          "tab_or_cwd_required", "tool_not_found",
                          "mcp_disabled", "dispatch_queue_full"}) {
        EXPECT_FALSE(ClaudeIntegration::dispatchResultIsSuccess(r))
            << "dispatcher refusal counted as success: " << r;
    }
}

TEST(DispatchResultAccounting, Inv4UnknownResultIsNotSuccess) {
    // The rule is a whitelist, so a result added later defaults to
    // failure rather than silently counting as success.
    EXPECT_FALSE(ClaudeIntegration::dispatchResultIsSuccess("something_new"));
    EXPECT_FALSE(ClaudeIntegration::dispatchResultIsSuccess(""));
}

TEST(DispatchResultAccounting, Inv5RefusalYieldsItsCode) {
    const QString body = QStringLiteral(
        R"({"ok":false,"code":"bad_args","error":"pattern is required"})");
    EXPECT_EQ(ClaudeIntegration::handlerRefusalCode(body),
              QStringLiteral("bad_args"));
}

TEST(DispatchResultAccounting, Inv6RefusalWithoutCodeStillRefuses) {
    const QString body = QStringLiteral(R"({"ok":false,"error":"nope"})");
    EXPECT_FALSE(ClaudeIntegration::handlerRefusalCode(body).isEmpty())
        << "an envelope that omitted `code` must not read as a success";
}

TEST(DispatchResultAccounting, Inv7SuccessYieldsNoRefusal) {
    EXPECT_TRUE(ClaudeIntegration::handlerRefusalCode(
        QStringLiteral(R"({"ok":true,"count":3})")).isEmpty());
    // No `ok` field at all: many payloads carry none, and absence is not
    // a refusal.
    EXPECT_TRUE(ClaudeIntegration::handlerRefusalCode(
        QStringLiteral(R"({"count":3})")).isEmpty());
}

TEST(DispatchResultAccounting, Inv8NonJsonYieldsNoRefusal) {
    EXPECT_TRUE(ClaudeIntegration::handlerRefusalCode(
        QStringLiteral("not json at all")).isEmpty());
    EXPECT_TRUE(ClaudeIntegration::handlerRefusalCode(
        QStringLiteral(R"(["an","array"])")).isEmpty());
    EXPECT_TRUE(ClaudeIntegration::handlerRefusalCode(QString()).isEmpty());
}

namespace {
QString padded(const QString &head) {
    return head + QString(qsizetype{64} * 1024, QLatin1Char('x')) +
           QStringLiteral(R"("})");
}
}  // namespace

TEST(DispatchResultAccounting, Inv9OversizedRefusalYieldsItsCode) {
    // ANTS-5090 — a refusal is a refusal whatever its size, or a large one
    // reaches the client without isError.
    EXPECT_EQ(ClaudeIntegration::handlerRefusalCode(padded(QStringLiteral(
                  R"({"ok":false,"code":"bad_args","pad":")"))),
              QStringLiteral("bad_args"));
    // Indented JSON spells the marker with a space.
    EXPECT_EQ(ClaudeIntegration::handlerRefusalCode(padded(QStringLiteral(
                  "{\n    \"code\": \"bad_args\",\n    \"ok\": false,\n"
                  "    \"pad\": \""))),
              QStringLiteral("bad_args"));
}

TEST(DispatchResultAccounting, Inv9OversizedSuccessYieldsNoRefusal) {
    EXPECT_TRUE(ClaudeIntegration::handlerRefusalCode(padded(QStringLiteral(
        R"({"ok":true,"pad":")"))).isEmpty());
    // A nested ok:false is an item's field, not the envelope's.
    EXPECT_TRUE(ClaudeIntegration::handlerRefusalCode(padded(QStringLiteral(
        R"({"ok":true,"items":[{"ok":false}],"pad":")"))).isEmpty());
}

TEST(DispatchResultAccounting, Inv10RefusalResultCarriesIsError) {
    const QString wrapped = QStringLiteral(R"({"ok":false,"code":"bad_args"})");
    const QJsonObject r =
        ClaudeIntegration::toolCallResult(wrapped, QStringLiteral("bad_args"));
    EXPECT_TRUE(r.value(QStringLiteral("isError")).toBool(false))
        << "MCP reports a tool execution error as isError:true "
           "(docs/standards/mcp-error-codes.md)";
    const QJsonArray content = r.value(QStringLiteral("content")).toArray();
    ASSERT_EQ(content.size(), 1);
    EXPECT_EQ(content.at(0).toObject().value(QStringLiteral("text")).toString(),
              wrapped) << "the body must travel unchanged";
}

TEST(DispatchResultAccounting, Inv11SuccessResultCarriesNoIsError) {
    const QJsonObject r = ClaudeIntegration::toolCallResult(
        QStringLiteral(R"({"ok":true})"), QString());
    EXPECT_FALSE(r.contains(QStringLiteral("isError")));
    EXPECT_EQ(r.value(QStringLiteral("content")).toArray().size(), 1);
}
