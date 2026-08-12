// ANTS-4108 — spec_conformance VERB conformance test.
// See tests/features/spec_conformance_verb/spec.md. The engine lane
// (tests/features/spec_conformance/) owns SpecConformance::run; this lane owns
// only the MCP boundary.
//
// The handler needs a live MainWindow, so behavioural rows drive the pure
// helper and wiring rows source-scrape the registration sites — the same split
// tests/features/spec_lint_verb/ uses.

#include "remotecontrol.h"

#include <string>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#if !defined(SRC_MAINWINDOW_CPP_PATH) || !defined(ANTS_RC_SOURCES) || \
    !defined(SRC_CLAUDE_INTEGRATION_CPP_PATH)
#error "spec_conformance_verb test needs the test_claude source-path compile defs"
#endif

namespace {

// A minimal engine envelope in the § 2.3 shape — enough to tell a 304 from a
// full body without running the engine.
QJsonObject engineEnvelope() {
    QJsonObject finding;
    finding[QStringLiteral("kind")] = QStringLiteral("mismatch");
    finding[QStringLiteral("line")] = 88;
    QJsonObject timing;
    timing[QStringLiteral("kind")]   = QStringLiteral("timing");
    timing[QStringLiteral("micros")] = 77;

    QJsonObject env;
    env[QStringLiteral("ok")]           = true;
    env[QStringLiteral("path")]         = QStringLiteral("/abs/root/docs/specs/x.md");
    env[QStringLiteral("cases_run")]    = 1;
    env[QStringLiteral("findings")]     = QJsonArray{finding};
    env[QStringLiteral("candidates")]   = QJsonArray{};
    env[QStringLiteral("refusals")]     = QJsonArray{};
    env[QStringLiteral("truncated")]    = false;
    env[QStringLiteral("etag")]         = QStringLiteral("abc123");
    env[QStringLiteral("observations")] = QJsonArray{timing};
    return env;
}

QString rel() { return QStringLiteral("docs/specs/x.md"); }

std::string ciSource() {
    return ants_test::stripComments(
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH));
}

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-9's second half — the engine lane proves the etag is stable across runs;
// only the verb can prove a matching one short-circuits. The negative scrape is
// what keeps that true: `spec_conformance` must stay OUT of
// isEtagSupportedTool, because the central etag hashes the whole response text
// and observations[] carries a measured microsecond count per case. Wired
// centrally, the etag would differ on every run, the 304 would never fire, and
// the engine's stable etag would be overwritten by a timing-sensitive one.
TEST(spec_conformance_verb, Inv9EtagShortCircuitIsHandlerLocal) {
    const QJsonObject env = engineEnvelope();

    // (a) no etag_match → the full envelope, etag intact.
    const QJsonObject full =
        RemoteControl::specConformanceBuildResponse(env, rel(), QString());
    EXPECT_TRUE(full.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(full.value(QStringLiteral("etag")).toString().toStdString(), "abc123");
    EXPECT_EQ(full.value(QStringLiteral("findings")).toArray().size(), 1);
    EXPECT_FALSE(full.contains(QStringLiteral("unchanged")));

    // (b) a matching etag_match → {ok, unchanged, etag} and nothing else.
    const QJsonObject same = RemoteControl::specConformanceBuildResponse(
        env, rel(), QStringLiteral("abc123"));
    EXPECT_TRUE(same.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(same.value(QStringLiteral("unchanged")).toBool());
    EXPECT_EQ(same.value(QStringLiteral("etag")).toString().toStdString(), "abc123");
    EXPECT_FALSE(same.contains(QStringLiteral("findings")))
        << "a 304 carries no body";
    EXPECT_FALSE(same.contains(QStringLiteral("observations")));

    // (c) a stale etag_match → the full envelope, not a 304.
    const QJsonObject stale = RemoteControl::specConformanceBuildResponse(
        env, rel(), QStringLiteral("stale"));
    EXPECT_FALSE(stale.contains(QStringLiteral("unchanged")));
    EXPECT_EQ(stale.value(QStringLiteral("findings")).toArray().size(), 1);

    // (d) a refusal is never short-circuited. It carries no etag, and reporting
    // "unchanged" for a call that never ran hides the refusal from the caller.
    QJsonObject bad;
    bad[QStringLiteral("ok")]    = false;
    bad[QStringLiteral("code")]  = QStringLiteral("bad_args");
    bad[QStringLiteral("error")] = QStringLiteral("max_cases out of range");
    const QJsonObject refused = RemoteControl::specConformanceBuildResponse(
        bad, rel(), QStringLiteral("abc123"));
    EXPECT_FALSE(refused.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(refused.contains(QStringLiteral("unchanged")));
    EXPECT_EQ(refused.value(QStringLiteral("code")).toString().toStdString(),
              "bad_args");

    // The negative that makes (b) reachable at all.
    const std::string etagFn = ants_test::slurpFunctionBody(
        ciSource(), "ClaudeIntegration::isEtagSupportedTool");
    ASSERT_FALSE(etagFn.empty()) << "isEtagSupportedTool body not located";
    EXPECT_FALSE(has(etagFn, "spec_conformance"))
        << "the central etag hashes observations[]; wiring both mechanisms "
           "kills the 304 silently";
}

// The engine is handed an absolute path (it opens the file); the envelope
// crosses the wire, where an absolute path leaks the caller's home directory
// and differs per machine. § 2.3's example is project-relative.
TEST(spec_conformance_verb, PathIsRewrittenProjectRelative) {
    const QJsonObject out = RemoteControl::specConformanceBuildResponse(
        engineEnvelope(), rel(), QString());
    EXPECT_EQ(out.value(QStringLiteral("path")).toString().toStdString(),
              "docs/specs/x.md");
}

// The verb-contract minimum per docs/standards/mcp-tools.md: caller_cwd
// Required at BOTH declaration sites, `path` required and validated before the
// engine opens anything, max_cases forwarded unclamped, and a kindForName
// bucket so the verb is not tagged `[other]` in tools/list.
TEST(spec_conformance_verb, VerbContractMinimums) {
    const std::string mw =
        ants_test::stripComments(ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH));
    ASSERT_FALSE(mw.empty());
    const std::size_t reg = mw.find("registerToolProvider(\"spec_conformance\"");
    ASSERT_NE(reg, std::string::npos) << "verb is not registered";
    EXPECT_TRUE(has(mw.substr(reg, 160), "CallerCwdContract::Required"));

    const std::string ci = ciSource();
    ASSERT_FALSE(ci.empty());
    const std::size_t cc =
        ci.find("toolName == QStringLiteral(\"spec_conformance\")");
    ASSERT_NE(cc, std::string::npos) << "no explicit CallerCwdContract entry";
    EXPECT_TRUE(has(ci.substr(cc, 100), "C::Required"));
    EXPECT_TRUE(has(ci, "specConf[\"name\"] = \"spec_conformance\""))
        << "no tools/list schema";

    // `path` is required — this verb runs one document, so an absent path has
    // no sane default (spec_lint's tree walk is what makes its path optional).
    const std::size_t schema = ci.find("specConf[\"name\"] = \"spec_conformance\"");
    ASSERT_NE(schema, std::string::npos);
    const std::string block = ci.substr(schema, 4000);
    const std::size_t req = block.find("schema[\"required\"]");
    ASSERT_NE(req, std::string::npos);
    const std::string required = block.substr(req, 160);
    EXPECT_TRUE(has(required, "caller_cwd"));
    EXPECT_TRUE(has(required, "path"));
    EXPECT_TRUE(has(block, "max_cases"));
    EXPECT_TRUE(has(block, "etag_match"));

    // Every registered verb has a kindForName bucket (ANTS-1567); this one is
    // asserted here as well so the requirement is visible from its own lane.
    const std::size_t kind = ci.find("auto kindForName = [](const QString &name)");
    ASSERT_NE(kind, std::string::npos);
    EXPECT_TRUE(has(ants_test::slurpFunctionBody(ci, "auto kindForName ="),
                    "spec_conformance"));

    // The handler validates `path` before the engine opens anything, refuses an
    // absent one, and does NOT clamp max_cases — an out-of-range value refuses
    // the call (engine INV-7), and a qBound here would silently defeat that.
    const std::string handler = ants_test::slurpFunctionBody(
        ants_test::stripComments(ants_test::slurpRemoteControl()),
        "RemoteControl::cmdSpecConformance");
    ASSERT_FALSE(handler.empty()) << "handler not found in any RC source";
    EXPECT_TRUE(has(handler, "validatePath("));
    EXPECT_TRUE(has(handler, "check.err"));
    EXPECT_TRUE(has(handler, "path is required"));
    EXPECT_TRUE(has(handler, "max_cases"));
    EXPECT_FALSE(has(handler, "qBound"))
        << "max_cases must refuse out of range, not clamp";
}
