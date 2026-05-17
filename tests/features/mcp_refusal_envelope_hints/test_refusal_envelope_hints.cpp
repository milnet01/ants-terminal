// ANTS-1418 feature-conformance test — refusal envelopes for
// caller_cwd_required (dispatcher) and cwd_missing (RcGate) name
// caller_cwd_info as the diagnostic path. Mixed source-scrape +
// pure-function check on gateErrorEnvelope.

#include "../../_support/expect.h"
#include "remotecontrolgate.h"

#include <gtest/gtest.h>

#include <QJsonObject>
#include <QString>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "setup-fail: cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — dispatcher's caller_cwd_required envelope carries hint
// naming caller_cwd_info.
TEST(mcp_refusal_envelope_hints,
     Inv1DispatcherEnvelopeHasCallerCwdInfoHint) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
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
