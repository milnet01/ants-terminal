// ANTS-1880 — spec_query + project_layout recognise docs/phases/.

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

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

// INV-1 — id validator accepts ANTS-NNNN AND phase_*.
TEST(spec_query_phases_layout, Inv1IdValidatorAcceptsBothShapes) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = slurp(SRC_PROJECTLAYOUTENGINE_CPP_PATH);
    expect(contains(cpp, "phasesDir") || contains(cpp, "phases_dir"),
           "INV-4: phases_dir field referenced in projectlayoutengine.cpp");
    expect(contains(cpp, "docs/phases"),
           "INV-4: docs/phases probe path present");
    EXPECT_EQ(0, expect_failures());
}
#endif

#ifdef SRC_PROJECTLAYOUTENGINE_H_PATH
// INV-5 — kProbeSetVersion bumped to 4.
TEST(spec_query_phases_layout, Inv5ProbeSetVersionBumped) {
    expect_reset();
    const std::string h = slurp(SRC_PROJECTLAYOUTENGINE_H_PATH);
    expect(contains(h, "kProbeSetVersion  = 4") ||
           contains(h, "kProbeSetVersion = 4"),
           "INV-5: kProbeSetVersion bumped to 4");
    EXPECT_EQ(0, expect_failures());
}
#endif

// INV-6 — cmdInvariantCheck scans both dirs, adds new fields.
TEST(spec_query_phases_layout, Inv6InvariantCheckScansBothDirs) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "phases_scanned"),
           "INV-6: phases_scanned field emitted");
    expect(contains(cpp, "total_scanned"),
           "INV-6: total_scanned field emitted");
    EXPECT_EQ(0, expect_failures());
}

TEST(spec_query_phases_layout, Inv6SpecsScannedRetainsSpecsOnlyCount) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // The specs_scanned field must continue to be assigned from the
    // specs-only file count (back-compat).
    expect(contains(cpp, "specs_scanned"),
           "INV-6: specs_scanned field retained");
    EXPECT_EQ(0, expect_failures());
}
