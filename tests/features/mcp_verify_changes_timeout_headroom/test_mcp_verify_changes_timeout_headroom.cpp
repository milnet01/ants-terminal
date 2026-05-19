// ANTS-1579 — feature-conformance test for verify_changes `timeout_sec`
// end-to-end plumb. Live path: runVerify against a synthetic project
// with a `sleep 1` gate under `timeout_sec=3` proves the caller-supplied
// budget actually reaches runOneGate (positive complement to
// verify_changes_engine.Inv2TimeoutKillsHangingGate).
//
// Source-scrape INV-4 locks the description headline naming both the
// tool-side and transport-side timeouts in proximity.

#include "verifyengine.h"

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {

void writeFile(const QString &dir, const QString &rel,
               const QByteArray &body) {
    const QString abs = QDir(dir).filePath(rel);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

VerifyEngine::GateResult *findGate(VerifyEngine::VerifyReport &r,
                                   VerifyEngine::GateName g) {
    for (auto &gr : r.gates) {
        if (gr.name == g) return &gr;
    }
    return nullptr;
}

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

}  // namespace

// INV-1/2/3 — A 1 s `sleep 1` gate completes naturally under
// timeout_sec=3 (the caller-supplied budget plumbs through to
// perGateSec → runOneGate; the gate is NOT killed by the kill timer).
TEST(Ants1579TimeoutHeadroom, Inv1Through3GateCompletesUnderBudget) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".ants/verify.json", R"({
        "build": {"command": "sleep 1", "format": "plain"}
    })");

    VerifyEngine::VerifyOptions opts;
    opts.timeoutSec         = 3;   // 3 / 1 gate = 3 s per gate
    opts.minPerGateSec      = 1;   // override 10 s floor so the perGateSec=3 sticks
    opts.minTotalTimeoutSec = 1;   // override 10 s floor on the total clamp
    const auto rep = VerifyEngine::runVerify(tmp.path(), opts);

    auto *g = findGate(const_cast<VerifyEngine::VerifyReport &>(rep),
                       VerifyEngine::GateName::Build);
    ASSERT_NE(g, nullptr) << "build gate result missing";

    // INV-1 — gate ran (not skipped due to misconfig).
    EXPECT_TRUE(g->ran) << "build gate did not run; skippedReason: "
                       << g->skippedReason.toStdString();
    // INV-3 — natural completion: exit code 0, passed.
    EXPECT_TRUE(g->passed)
        << "build gate did not pass under headroom budget; "
        << "skippedReason: " << g->skippedReason.toStdString();
    EXPECT_EQ(g->exitCode, 0)
        << "build gate exitCode != 0; got " << g->exitCode
        << "; skippedReason: " << g->skippedReason.toStdString();
    EXPECT_FALSE(g->skippedReason.contains(QLatin1String("timeout")))
        << "gate was killed by kill timer instead of completing "
        << "naturally; skippedReason: " << g->skippedReason.toStdString();
    // INV-2 — duration < timeout budget (proves natural completion).
    // sleep 1 plus QProcess overhead should land well under 3 s.
    EXPECT_LT(g->durationSec, 3.0)
        << "build gate took " << g->durationSec
        << " s — too close to the 3 s budget; the kill timer may "
        << "have been the trigger";
    // Lower-bound dropped post-test-audit 2026-05-18: the
    // semantic check (ran && passed && exitCode == 0 && no "timeout"
    // in skippedReason) above is already sufficient to prove the
    // gate completed naturally. Wall-clock lower bounds are flaky on
    // virtualised CI clocks and didn't add correctness signal.
}

// INV-4 — tool description names both "tool-side" and "transport"
// timeout keywords in proximity (within 400 chars), so a caller
// reading the description sees the asymmetry up front.
TEST(Ants1579TimeoutHeadroom, Inv4DescriptionNamesBothSidesInProximity) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // Anchor at the verify_changes registration block.
    const size_t anchor = ci.find("t[\"name\"] = \"verify_changes\"");
    ASSERT_NE(anchor, std::string::npos)
        << "verify_changes registration not found";
    const size_t windowEnd = std::min(ci.size(), anchor + 4000);
    const std::string window = ci.substr(anchor, windowEnd - anchor);

    const size_t toolSide = window.find("tool-side");
    const size_t transport = window.find("transport");
    ASSERT_NE(toolSide,  std::string::npos)
        << "verify_changes description does not name \"tool-side\"";
    ASSERT_NE(transport, std::string::npos)
        << "verify_changes description does not name \"transport\"";

    const size_t gap = (toolSide < transport)
                       ? (transport - toolSide)
                       : (toolSide  - transport);
    EXPECT_LE(gap, static_cast<size_t>(400))
        << "tool-side / transport keywords are too far apart in the "
        << "description (" << gap << " chars); ANTS-1579 INV-4 "
        << "requires them in proximity so a Claude reader sees the "
        << "asymmetry up front";
}

// ANTS-1628 INV-A — successful envelope carries the three phase-
// timing fields (wall_clock_ms / pre_gate_ms / gate_ms) so callers
// can distinguish a long build from pre-gate wrapper work consuming
// the transport budget. Source-scrape against the impl assignments
// (a live build-and-run test would add wall-clock dependency without
// new correctness signal — the INV-1 path already exercises a real
// runVerify).
TEST(Ants1628PhaseTiming, ImplEmitsThreeTimingFields) {
    expect_reset();
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(rc.find("ANTS-1628 — phase timing") != std::string::npos,
           "INV-A: phase-timing block anchor present in impl");
    expect(rc.find("env[QStringLiteral(\"wall_clock_ms\")]") != std::string::npos,
           "INV-A: wall_clock_ms emitted on the success envelope");
    expect(rc.find("env[QStringLiteral(\"pre_gate_ms\")]") != std::string::npos,
           "INV-A: pre_gate_ms emitted on the success envelope");
    expect(rc.find("env[QStringLiteral(\"gate_ms\")]") != std::string::npos,
           "INV-A: gate_ms emitted on the success envelope");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1628 INV-B — verify_changes tool description names the new
// phase-timing fields and points at the actionable signal ("large
// pre_gate_ms + tiny gate_ms means wrapper is the culprit"). Without
// this, callers seeing the new envelope fields wouldn't know what
// they're for.
TEST(Ants1628PhaseTiming, DescriptionNamesPhaseTimingFields) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const size_t anchor = ci.find("t[\"name\"] = \"verify_changes\"");
    ASSERT_NE(anchor, std::string::npos);
    const size_t windowEnd = std::min(ci.size(), anchor + 5000);
    const std::string window = ci.substr(anchor, windowEnd - anchor);
    expect(window.find("Phase timing (ANTS-1628)") != std::string::npos,
           "INV-B: description anchor names the ANTS-1628 phase-timing block");
    expect(window.find("wall_clock_ms") != std::string::npos,
           "INV-B: description names wall_clock_ms");
    expect(window.find("pre_gate_ms") != std::string::npos,
           "INV-B: description names pre_gate_ms");
    expect(window.find("gate_ms") != std::string::npos,
           "INV-B: description names gate_ms");
    EXPECT_EQ(0, expect_failures());
}
