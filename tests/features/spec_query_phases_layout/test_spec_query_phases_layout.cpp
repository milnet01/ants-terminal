// ANTS-1880 — spec_query + project_layout recognise docs/phases/.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — id validator accepts ANTS-NNNN AND phase_*.
TEST(spec_query_phases_layout, Inv1IdValidatorAcceptsBothShapes) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "isValidSpecId"),
           "INV-1: isValidSpecId helper present");
    expect(contains(cpp, "ANTS-"),
           "INV-1: ANTS- regex literal present");
    expect(contains(cpp, "phase_"),
           "INV-1: phase_ regex literal present");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — per-id-shape routing.
TEST(spec_query_phases_layout, Inv2NoCrossDirFallback) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // The resolver dispatches: ANTS- prefix → docs/specs/;
    // phase_ prefix → docs/phases/. Source-grep for the routing.
    expect(contains(cpp, "docs/specs/"),
           "INV-2: docs/specs/ path present");
    expect(contains(cpp, "docs/phases/"),
           "INV-2: docs/phases/ path present");
    expect(contains(cpp, "startsWith(QStringLiteral(\"phase_\"))") ||
           contains(cpp, "startsWith(QLatin1String(\"phase_\"))") ||
           contains(cpp, "id.startsWith(\"phase_\")"),
           "INV-2: phase_ prefix check routes to docs/phases/");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — source field echoed.
TEST(spec_query_phases_layout, Inv3SourceFieldEchoed) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "result[\"source\"]") ||
           contains(cpp, "result[QStringLiteral(\"source\")]"),
           "INV-3: response carries `source` field");
    expect(contains(cpp, "\"phases\""),
           "INV-3: \"phases\" literal value present");
    expect(contains(cpp, "\"specs\""),
           "INV-3: \"specs\" literal value present");
    EXPECT_EQ(0, expect_failures());
}

#ifdef SRC_PROJECTLAYOUTENGINE_CPP_PATH
// INV-4 — LayoutEnvelope phasesDir field + phases_dir JSON key.
TEST(spec_query_phases_layout, Inv4LayoutEnvelopeCarriesPhasesDir) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_PROJECTLAYOUTENGINE_CPP_PATH);
    expect(contains(cpp, "phasesDir") || contains(cpp, "phases_dir"),
           "INV-4: phases_dir field referenced in projectlayoutengine.cpp");
    expect(contains(cpp, "docs/phases"),
           "INV-4: docs/phases probe path present");
    EXPECT_EQ(0, expect_failures());
}
#endif

#ifdef SRC_PROJECTLAYOUTENGINE_H_PATH
// INV-5 — kProbeSetVersion bumped to at least 4 (ANTS-1880 raised it
// from 3→4; ANTS-1903 raised it to 5 after adding the body-scan
// fallback and the sniffer-trace echo). The contract here is "the
// counter advanced after the spec-shape probe widening" — any later
// bump satisfies it.
TEST(spec_query_phases_layout, Inv5ProbeSetVersionBumped) {
    expect_reset();
    const std::string h = ants_test::slurpFile(SRC_PROJECTLAYOUTENGINE_H_PATH);
    // ANTS-4439 — this was a whitelist of the literals 4, 5 and 6 while calling
    // itself "≥4", so bumping a constant whose entire purpose is to be bumped
    // reddened it, at a distance, in an unrelated suite. It had already been
    // extended twice for that reason. Parse the value instead: the assertion the
    // name states is the one it now makes, and no future bump touches this file.
    int version = -1;
    const std::string decl = "constexpr int kProbeSetVersion";
    const size_t at = h.find(decl);
    if (at != std::string::npos) {
        const size_t eq = h.find('=', at + decl.size());
        if (eq != std::string::npos) {
            size_t i = eq + 1;
            while (i < h.size() && (h[i] == ' ' || h[i] == '\t')) ++i;
            int acc = 0;
            bool any = false;
            while (i < h.size() && h[i] >= '0' && h[i] <= '9') {
                acc = acc * 10 + (h[i] - '0');
                any = true;
                ++i;
            }
            if (any) version = acc;
        }
    }
    expect(version >= 4, "INV-5: kProbeSetVersion bumped (≥4)",
           std::string("parsed ") + std::to_string(version));
    EXPECT_EQ(0, expect_failures());
}
#endif

// INV-6 — cmdInvariantCheck scans both dirs, adds new fields.
TEST(spec_query_phases_layout, Inv6InvariantCheckScansBothDirs) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "phases_scanned"),
           "INV-6: phases_scanned field emitted");
    expect(contains(cpp, "total_scanned"),
           "INV-6: total_scanned field emitted");
    EXPECT_EQ(0, expect_failures());
}

TEST(spec_query_phases_layout, Inv6SpecsScannedRetainsSpecsOnlyCount) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    // The specs_scanned field must continue to be assigned from the
    // specs-only file count (back-compat).
    expect(contains(cpp, "specs_scanned"),
           "INV-6: specs_scanned field retained");
    EXPECT_EQ(0, expect_failures());
}
