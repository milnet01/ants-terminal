// ANTS-1437 — feature-conformance test for the section_index mode
// added to roadmap_query. Source-scrape style, matching the sibling
// ANTS-1287 / ANTS-1398 tests.

#include "../../_support/expect.h"

#include <gtest/gtest.h>

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

// INV-1 — default mode is unchanged; the `mode` arg is opt-in.
TEST(roadmap_query_section_index, Inv1ModeOptIn) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1437-INV-1"),
           "INV-1: mode arg parse anchor present");
    expect(contains(cpp, "hasModeArg"),
           "INV-1: hasModeArg flag gates the mode echo so default "
           "envelope shape is preserved");
    expect(contains(cpp, "if (hasModeArg) out[\"mode\"]"),
           "INV-1: mode echo gated by hasModeArg in bullet-mode "
           "return paths");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — unknown mode rejected with bad_mode + verbatim hygiene.
TEST(roadmap_query_section_index, Inv2UnknownModeRejected) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "bad_mode"),
           "INV-2: bad_mode error code present");
    expect(contains(cpp, "unknown mode:"),
           "INV-2: unknown-mode error message present");
    // Verbatim hygiene reuses the 64-byte + control-char-? pattern
    // from bad_status (the next-door predicate); the bad_mode block
    // does the same scrub before echoing the user's input.
    expect(contains(cpp, "if (verbatim.size() > 64) verbatim.truncate(64)"),
           "INV-2: 64-byte verbatim cap present (shared with "
           "bad_status / bad_section)");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — mode:"section_index" + section= is bad_mode_combo.
TEST(roadmap_query_section_index, Inv3SectionPlusModeRejected) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1437-INV-3"),
           "INV-3: bad_mode_combo guard anchor present");
    expect(contains(cpp, "bad_mode_combo"),
           "INV-3: bad_mode_combo error code present");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — every indexed section emitted, including empties.
TEST(roadmap_query_section_index, Inv4EmitEverySection) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "INV-4 — emit EVERY indexed section"),
           "INV-4: emission anchor present (loops m_roadmapIndex, "
           "not bullets tally)");
    expect(contains(cpp, "for (const auto &sec : std::as_const(m_roadmapIndex))"),
           "INV-4: walks the full index (catches empty sections)");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — counts use the same emoji predicates as bullet-mode filter.
TEST(roadmap_query_section_index, Inv5CountsUseSamePredicate) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // The tally block uses the same plannedEmoji/progressEmoji/
    // doneEmoji constants as the bullet-mode filter. UTF-8 bytes:
    //   \xF0\x9F\x93\x8B = 📋 (planned)
    //   \xF0\x9F\x9A\xA7 = 🚧 (in-progress)
    //   \xE2\x9C\x85     = ✅ (shipped)
    expect(contains(cpp, "\\xF0\\x9F\\x93\\x8B"),
           "INV-5: plannedEmoji (📋) constant present");
    expect(contains(cpp, "\\xF0\\x9F\\x9A\\xA7"),
           "INV-5: progressEmoji (🚧) constant present");
    expect(contains(cpp, "\\xE2\\x9C\\x85"),
           "INV-5: doneEmoji (✅) constant present");
    expect(contains(cpp, "t.active++") && contains(cpp, "t.shipped++"),
           "INV-5: tally increments per status");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — rollup bullets excluded from counts.
TEST(roadmap_query_section_index, Inv6RollupExcluded) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "INV-6 rollup"),
           "INV-6: rollup-exclude anchor present in tally loop");
    expect(contains(cpp, "if (id.isEmpty() && hl.isEmpty()) continue"),
           "INV-6: tally loop skips bullets with empty id+headline");
    EXPECT_EQ(0, expect_failures());
}

// INV-8 — unrecognised_format gate also fires in section_index mode.
TEST(roadmap_query_section_index, Inv8UnrecognisedFormatGate) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1437-INV-8"),
           "INV-8: unrecognised_format gate anchor present in "
           "section_index branch");
    // The gate reuses the same kRoadmapMinParseableSize predicate
    // as bullet mode.
    expect(contains(cpp, "kRoadmapMinParseableSize"),
           "INV-8: gate predicate present");
    EXPECT_EQ(0, expect_failures());
}

// Schema — `mode` property advertised on the roadmap_query tool.
TEST(roadmap_query_section_index, SchemaModePropAdvertised) {
    expect_reset();
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "ANTS-1437 — mode arg"),
           "schema: mode property anchor present in claudeintegration");
    expect(contains(cpp, "\"section_index\""),
           "schema: section_index value in mode enum");
    expect(contains(cpp, "props[\"mode\"] = modeProp"),
           "schema: mode property registered on roadmap_query tool");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — section_slug populated on EVERY cache-fill path so the
// count tally doesn't roll up zero when an earlier bullet-mode call
// has already populated the cache. ANTS-1442 — fixed-up after live
// test showed every section reporting 0/0/0 even when populated.
//
// The pattern: each cache-fill site is a
// `for (const auto &b : bullets)` loop that builds a QJsonObject.
// Section_slug must be set inside every such loop. A future engineer
// adding a 4th cache-fill site needs to add section_slug too, or
// callers downstream of section_index will silently read "" for
// every bullet.
TEST(roadmap_query_section_index, Inv7SectionSlugOnEveryCacheFill) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // Count the cache-fill loops (the canonical bullet-iteration
    // shape used by all four sites: bullet-mode pre-fill,
    // section_index lazy-fill, section-mode emission, and full-file
    // lazy-fill).
    size_t loopCount = 0;
    size_t pos = 0;
    const std::string loopNeedle = "for (const auto &b : bullets)";
    while ((pos = cpp.find(loopNeedle, pos)) != std::string::npos) {
        ++loopCount;
        pos += loopNeedle.size();
    }
    expect(loopCount == 4,
           "INV-7: cmdRoadmapQuery has exactly 4 cache-fill loops "
           "(bullet-mode pre-fill, section_index lazy-fill, section-"
           "mode emission, full-file lazy-fill). If this changes, "
           "audit each new loop for section_slug emission.");

    // Count any `section_slug` emission. Every loop must set it —
    // either `b.sectionSlug` (full-file paths) or `sec->slug` (the
    // section-mode emission, ANTS-1287-INV-7). The exact RHS doesn't
    // matter; what matters is the field is populated.
    size_t slugCount = 0;
    pos = 0;
    const std::string slugNeedle = "o[\"section_slug\"] =";
    while ((pos = cpp.find(slugNeedle, pos)) != std::string::npos) {
        ++slugCount;
        pos += slugNeedle.size();
    }
    expect(slugCount == loopCount,
           "INV-7: every cache-fill loop must emit section_slug "
           "(ANTS-1442). Missing it on any path makes the "
           "section_index tally roll up zero for every section.");

    // The fix anchor must be present.
    expect(contains(cpp, "ANTS-1442"),
           "INV-7: ANTS-1442 anchor present in cmdRoadmapQuery");
    EXPECT_EQ(0, expect_failures());
}

// Dispatch — mainwindow's MCP→cmdRoadmapQuery lambda forwards the
// `mode` arg (and `include_section_headers`, caught during 1437
// live-test). Without this, the schema advertises args the
// dispatcher silently drops at the MCP boundary. Regression
// test for the bug found while live-testing ANTS-1437.
TEST(roadmap_query_section_index, DispatchForwardsModeArg) {
    expect_reset();
    const std::string cpp = slurp(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(cpp, "ANTS-1437 — forward `mode`"),
           "dispatch: mainwindow lambda forwards mode arg "
           "(anchor present)");
    expect(contains(cpp, "args.value(\"mode\")"),
           "dispatch: lambda reads mode from args");
    expect(contains(cpp, "req[\"mode\"]"),
           "dispatch: lambda writes mode into the cmdRoadmapQuery req");
    // ANTS-1398 forward-fix companion — caught while landing 1437.
    expect(contains(cpp, "ANTS-1398 forward-fix"),
           "dispatch: include_section_headers forward-fix anchor present");
    expect(contains(cpp, "args.value(\"include_section_headers\")"),
           "dispatch: lambda reads include_section_headers from args");
    EXPECT_EQ(0, expect_failures());
}
