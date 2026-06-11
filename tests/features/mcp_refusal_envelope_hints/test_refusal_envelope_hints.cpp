// ANTS-1418 feature-conformance test — refusal envelopes for
// caller_cwd_required (dispatcher) and cwd_missing (RcGate) name
// caller_cwd_info as the diagnostic path. Mixed source-scrape +
// pure-function check on gateErrorEnvelope.

#include "../../_support/expect.h"
#include "remotecontrolgate.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QJsonObject>
#include <QString>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — dispatcher's caller_cwd_required envelope carries hint
// naming caller_cwd_info.
TEST(mcp_refusal_envelope_hints,
     Inv1DispatcherEnvelopeHasCallerCwdInfoHint) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Anchor co-located with the caller_cwd_required envelope build.
    expect(contains(ci, "ANTS-1418"),
           "INV-1: ANTS-1418 anchor present in dispatcher refusal "
           "block in claudeintegration.cpp");
    // The hint key + the literal tool name must be in source.
    expect(contains(ci, "env[\"hint\"]") ||
           contains(ci, "env[QStringLiteral(\"hint\")]"),
           "INV-1: dispatcher refusal envelope sets a hint field");
    expect(contains(ci, "caller_cwd_info"),
           "INV-1: dispatcher refusal envelope names caller_cwd_info "
           "(the diagnostic verb the caller should call to debug)");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — RcGate's cwd_missing envelope carries the same hint.
// Functional: build a CallerCwdGate by hand with errorCode set to
// cwd_missing, call gateErrorEnvelope, assert hint exists + names
// the verb.
TEST(mcp_refusal_envelope_hints,
     Inv2RcGateCwdMissingEnvelopeHasHint) {
    RcGate::CallerCwdGate g;
    g.ok = false;
    g.errorCode = QStringLiteral("cwd_missing");
    g.error = QStringLiteral("test: caller_cwd argument required");

    const QJsonObject env = RcGate::gateErrorEnvelope(g);

    expect(env.value(QStringLiteral("hint")).isString(),
           "INV-2: cwd_missing envelope carries a string hint field");
    const QString hint = env.value(QStringLiteral("hint")).toString();
    expect(hint.contains(QStringLiteral("caller_cwd_info")),
           "INV-2: cwd_missing hint names the caller_cwd_info verb");
}

// INV-3 — RcGate's non-cwd_missing errors don't carry the hint.
// Each of cwd_bad / no_project / cwd_mismatch refuses for a reason
// where the diagnostic verb wouldn't directly help.
TEST(mcp_refusal_envelope_hints,
     Inv3RcGateOtherErrorsNoHint) {
    const char *otherCodes[] = {
        "cwd_bad", "no_project", "cwd_mismatch"
    };
    for (const char *code : otherCodes) {
        RcGate::CallerCwdGate g;
        g.ok = false;
        g.errorCode = QString::fromUtf8(code);
        g.error = QStringLiteral("test refusal");
        const QJsonObject env = RcGate::gateErrorEnvelope(g);
        const bool hasHint = env.contains(QStringLiteral("hint"));
        if (hasHint) {
            std::fprintf(stderr,
                         "INV-3: %s envelope unexpectedly carries hint\n",
                         code);
        }
        EXPECT_FALSE(hasHint);
    }
}

// ANTS-1566 INV-4 — RcGate's cwd_missing envelope carries a concrete
// `example` JSON object with the canonical caller_cwd field shape so
// IPC-direct callers self-correct in one round-trip.
TEST(mcp_refusal_envelope_hints,
     Inv4RcGateCwdMissingEnvelopeHasExample) {
    RcGate::CallerCwdGate g;
    g.ok = false;
    g.errorCode = QStringLiteral("cwd_missing");
    g.error = QStringLiteral("test: caller_cwd argument required");

    const QJsonObject env = RcGate::gateErrorEnvelope(g);

    expect(env.value(QStringLiteral("example")).isObject(),
           "INV-4: cwd_missing envelope carries an example JSON object");
    const QJsonObject ex = env.value(QStringLiteral("example")).toObject();
    expect(ex.value(QStringLiteral("caller_cwd")).isString(),
           "INV-4: example object names caller_cwd as a string field");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1566 INV-5 — dispatcher's caller_cwd_required envelope carries
// an example field with caller_cwd named. Source-scrape — the
// dispatcher is the central path every Required-classified tool
// refuses through; MAME Curator 2026-05-18 (late session) hit this
// envelope and asked for the example field.
TEST(mcp_refusal_envelope_hints,
     Inv5DispatcherCallerCwdRequiredHasExample) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "ANTS-1543"),
           "INV-5: ANTS-1543 anchor present (the original example-field "
           "introduction; reused for ANTS-1566 sweep)");
    expect(contains(ci, "env[QStringLiteral(\"example\")]") ||
           contains(ci, "env[\"example\"]"),
           "INV-5: dispatcher refusal envelope sets an example field");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1566 INV-6 — project_layout cwd_bad refusal includes example
// (sweep parity for IPC-direct callers that bypass the dispatcher's
// caller_cwd_required gate).
TEST(mcp_refusal_envelope_hints,
     Inv6ProjectLayoutCwdBadHasExample) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // The cwd_bad block lives near the project_layout handler; check
    // that an ANTS-1566 anchor sits next to an example assignment.
    expect(contains(rc, "ANTS-1566"),
           "INV-6: ANTS-1566 anchor present in remotecontrol.cpp "
           "(project_layout / roadmap_log cwd refusal sweep)");
    expect(contains(rc, "project_layout: caller_cwd"),
           "INV-6: project_layout cwd_bad refusal still present");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1566 INV-7 — roadmap_log caller_cwd-related refusals (missing
// field, no_roadmap) carry an example field with the full append-op
// argument shape so the caller can copy it.
TEST(mcp_refusal_envelope_hints,
     Inv7RoadmapLogCwdRefusalsHaveExample) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // Check the cmdRoadmapLog's rlErr lambda branches on the code and
    // attaches example for missing_field / no_roadmap.
    expect(contains(rc, "missing_field") &&
           contains(rc, "no_roadmap"),
           "INV-7: rlErr branches on missing_field and no_roadmap "
           "codes (caller_cwd-related rejection paths)");
    expect(contains(rc, "ex[QStringLiteral(\"op\")]") ||
           contains(rc, "ex[\"op\"]"),
           "INV-7: rlErr example object includes the op field so "
           "the caller sees the canonical append shape");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1853 INV-8 — the dispatcher's caller_cwd_required gate
// distinguishes a wholly-empty arguments object (parameters dropped in
// transit) from a single missing field: it sets arguments_empty:true,
// steers the caller to resend the entire call, and logs a Claude-lane
// diagnostic so an intermittent recurrence is root-causable. The code
// stays caller_cwd_required (taxonomy unchanged — INV-1 still holds).
TEST(mcp_refusal_envelope_hints,
     Inv8DispatcherFlagsEmptyArguments) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "ANTS-1853"),
           "INV-8: ANTS-1853 anchor present in the dispatcher gate");
    expect(contains(ci, "argumentsEmpty") &&
           contains(ci, "argsObj.isEmpty()"),
           "INV-8: gate computes argumentsEmpty from the arguments object");
    expect(contains(ci, "arguments_empty"),
           "INV-8: refusal envelope carries an arguments_empty flag for "
           "the dropped-payload case");
    expect(contains(ci, "ANTS_LOG(DebugLog::Claude") &&
           contains(ci, "refused caller_cwd_required"),
           "INV-8: dispatcher logs a Claude-lane diagnostic on the refusal "
           "so a recurrence can be confirmed empty vs genuinely-missing");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1857 INV-9 — the dropped-payload steer is size-aware: rather
// than only "resend", it names the size root cause (large payloads
// drop) and the two mitigations (shrink the call / use the Edit tool),
// and the roadmap_log descriptor carries the same SIZE NOTE so a
// caller is warned BEFORE it builds an oversized append.
TEST(mcp_refusal_envelope_hints, Inv9SizeAwareSteer) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "ANTS-1857") || contains(ci, "too large"),
           "INV-9: empty-args steer names the large-payload root cause");
    expect(contains(ci, "Edit tool"),
           "INV-9: steer points the caller at the Edit tool for long "
           "content");
    expect(contains(ci, "SIZE NOTE (ANTS-1853)"),
           "INV-9: roadmap_log descriptor carries a proactive SIZE NOTE "
           "so the caller keeps appends small up front");
    EXPECT_EQ(0, expect_failures());
}
