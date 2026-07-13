// ANTS-1437 — feature-conformance test for the section_index mode
// added to roadmap_query. Source-scrape style, matching the sibling
// ANTS-1287 / ANTS-1398 tests.

#include "../../_support/expect.h"
#include "roadmapindex.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// ANTS-2036 — slice one RemoteControl::<fn> body out of remotecontrol.cpp
// so a count-based invariant can't be perturbed by an identical idiom in
// an unrelated function elsewhere in the TU (the global-count brittleness
// that ANTS-2031's helper tripped). Bounds from the function's signature
// to the next `\nQJsonDocument RemoteControl::` definition.
std::string functionSlice(const std::string &cpp,
                          const std::string &startSig) {
    const auto s = cpp.find(startSig);
    if (s == std::string::npos) return {};
    const auto e =
        cpp.find("\nQJsonDocument RemoteControl::", s + startSig.size());
    return cpp.substr(s, e == std::string::npos ? std::string::npos : e - s);
}

int countOccurrences(const std::string &hay, const std::string &needle) {
    int n = 0;
    std::string::size_type pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

}  // namespace

// INV-1 — default mode is unchanged; the `mode` arg is opt-in.
TEST(roadmap_query_section_index, Inv1ModeOptIn) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1437-INV-3"),
           "INV-3: bad_mode_combo guard anchor present");
    expect(contains(cpp, "bad_mode_combo"),
           "INV-3: bad_mode_combo error code present");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — every indexed section emitted, including empties.
TEST(roadmap_query_section_index, Inv4EmitEverySection) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "INV-4 — emit EVERY indexed section"),
           "INV-4: emission anchor present (loops m_roadmapIndex, "
           "not bullets tally)");
    expect(contains(cpp, "for (const auto &sec : std::as_const(m_roadmapIndex))"),
           "INV-4: walks the full index (catches empty sections)");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 (ANTS-1848) — status filter shapes section_index emission.
// Superseded the original "status filter is a no-op for section_index"
// rule (ANTS-1437 INV-7). Under status:active/shipped the emission loop
// drops sections whose matching id-only tally is 0; status:all keeps
// every section. Source-scrape style, matching the sibling emission INVs.
TEST(roadmap_query_section_index, Inv7Ants1848StatusFiltersEmission) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1848 — status filter drops zero-id-count"),
           "ANTS-1848: filter guard anchor present in the section_index "
           "emission loop");
    // The guard must key on the *_id_only tally (activeWithId /
    // shippedWithId), not the emoji-only count, so the kept set matches
    // the default bullets[] predicate. ANTS-2052 routed the drop through
    // a useRawPredicate ternary so the legacy fallback can switch to the
    // raw emoji count; the default (id-only) arm is still the predicate
    // ANTS-1848 specifies.
    expect(contains(cpp,
               "useRawPredicate ? t.active  == 0 : t.activeWithId  == 0"),
           "ANTS-1848/2052: active filter drops activeWithId==0 by default");
    expect(contains(cpp,
               "useRawPredicate ? t.shipped == 0 : t.shippedWithId == 0"),
           "ANTS-1848/2052: shipped filter drops shippedWithId==0 by default");
    // INV-4 must still hold for the default: the loop still walks the
    // full index so status:all emits every section.
    expect(contains(cpp, "INV-4 — emit EVERY indexed section"),
           "ANTS-1848: status:all still emits every section (INV-4 intact)");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — counts use the same emoji predicates as bullet-mode filter.
TEST(roadmap_query_section_index, Inv5CountsUseSamePredicate) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "INV-6 rollup"),
           "INV-6: rollup-exclude anchor present in tally loop");
    expect(contains(cpp, "if (id.isEmpty() && hl.isEmpty()) continue"),
           "INV-6: tally loop skips bullets with empty id+headline");
    EXPECT_EQ(0, expect_failures());
}

// INV-8 — unrecognised_format gate also fires in section_index mode.
TEST(roadmap_query_section_index, Inv8UnrecognisedFormatGate) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
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
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // ANTS-2036 — count within cmdRoadmapQuery's body only. The pre-fix
    // global count broke when ANTS-2031 added an unrelated helper using
    // the same `for (const auto &b : bullets)` idiom elsewhere in the TU;
    // bounding to the function makes the invariant robust to that.
    const std::string fn = functionSlice(
        cpp, "QJsonDocument RemoteControl::cmdRoadmapQuery(");
    expect(!fn.empty(),
           "INV-7: cmdRoadmapQuery body must be locatable");
    // Count the cache-fill loops (the canonical bullet-iteration
    // shape used by all four sites: bullet-mode pre-fill,
    // section_index lazy-fill, section-mode emission, and full-file
    // lazy-fill).
    const int loopCount =
        countOccurrences(fn, "for (const auto &b : bullets)");
    expect(loopCount == 4,
           "INV-7: cmdRoadmapQuery has exactly 4 cache-fill loops "
           "(bullet-mode pre-fill, section_index lazy-fill, section-"
           "mode emission, full-file lazy-fill). If this changes, "
           "audit each new loop for section_slug emission.");

    // Count any `section_slug` emission. Every loop must set it —
    // either `b.sectionSlug` (full-file paths) or `sec->slug` (the
    // section-mode emission, ANTS-1287-INV-7). The exact RHS doesn't
    // matter; what matters is the field is populated.
    const int slugCount = countOccurrences(fn, "o[\"section_slug\"] =");
    expect(slugCount == loopCount,
           "INV-7: every cache-fill loop must emit section_slug "
           "(ANTS-1442). Missing it on any path makes the "
           "section_index tally roll up zero for every section.");

    // The fix anchor must be present.
    expect(contains(cpp, "ANTS-1442"),
           "INV-7: ANTS-1442 anchor present in cmdRoadmapQuery");
    EXPECT_EQ(0, expect_failures());
}

// INV-10 — parent sections roll up descendant counts (ANTS-1442
// root cause). Source-scrape that the helper call is wired in.
TEST(roadmap_query_section_index, Inv10RollupCountsWired) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "RoadmapIndex::rollupCounts("),
           "INV-10: section_index branch calls "
           "RoadmapIndex::rollupCounts to fold descendant tallies "
           "into their parent section");
    expect(contains(cpp, "ANTS-1442 — INV-10"),
           "INV-10: anchor present alongside the rollupCounts call");
    EXPECT_EQ(0, expect_failures());
}

// INV-10 — functional. Build a hand-crafted index that mirrors the
// ROADMAP's nesting shape (one level-2 with two level-3 children)
// and verify rollupCounts sums child tallies into the parent.
//
// Pre-fix, the tally returned for the level-2 slug would equal the
// direct tally (which is zero, since no bullets live directly under
// the level-2 heading). Post-fix, it equals the sum of the two
// children plus the (zero) self.
TEST(roadmap_query_section_index, Inv10FunctionalLevel2Rollup) {
    QVector<RoadmapIndex::Section> index;
    // Mirrors `buildIndex` output: level-2 spans lines [0, 100),
    // child A spans [10, 50), child B spans [50, 100).
    {
        RoadmapIndex::Section parent;
        parent.slug        = QStringLiteral("release-072");
        parent.headingText = QStringLiteral("0.7.92 — release");
        parent.level       = 2;
        parent.lineStart   = 0;
        parent.lineEnd     = 100;
        index.append(parent);
    }
    {
        RoadmapIndex::Section a;
        a.slug        = QStringLiteral("childA");
        a.headingText = QStringLiteral("Bundle A");
        a.level       = 3;
        a.lineStart   = 10;
        a.lineEnd     = 50;
        index.append(a);
    }
    {
        RoadmapIndex::Section b;
        b.slug        = QStringLiteral("childB");
        b.headingText = QStringLiteral("Bundle B");
        b.level       = 3;
        b.lineStart   = 50;
        b.lineEnd     = 100;
        index.append(b);
    }

    QHash<QString, RoadmapIndex::SectionCounts> direct;
    direct[QStringLiteral("childA")] = {3, 2, 5};   // 3 active, 2 shipped
    direct[QStringLiteral("childB")] = {1, 4, 5};   // 1 active, 4 shipped
    // Note: no entry for the parent slug — it has no direct bullets.

    const auto rolled = RoadmapIndex::rollupCounts(index, direct);

    const auto pa = rolled.value(QStringLiteral("release-072"));
    EXPECT_EQ(pa.active,  4);   // 0 + 3 + 1
    EXPECT_EQ(pa.shipped, 6);   // 0 + 2 + 4
    EXPECT_EQ(pa.total,   10);  // 0 + 5 + 5

    // Children unchanged.
    const auto ca = rolled.value(QStringLiteral("childA"));
    EXPECT_EQ(ca.active,  3);
    EXPECT_EQ(ca.shipped, 2);
    EXPECT_EQ(ca.total,   5);

    const auto cb = rolled.value(QStringLiteral("childB"));
    EXPECT_EQ(cb.active,  1);
    EXPECT_EQ(cb.shipped, 4);
    EXPECT_EQ(cb.total,   5);
}

// INV-10 — functional, leaf-only roadmap (no level-2 / level-3 mix).
// Confirms the rollup is a no-op when sections are siblings, not
// nested. Guards against an over-eager containment check that would
// double-count flat layouts.
TEST(roadmap_query_section_index, Inv10FunctionalFlatNoRollup) {
    QVector<RoadmapIndex::Section> index;
    {
        RoadmapIndex::Section a;
        a.slug        = QStringLiteral("a");
        a.headingText = QStringLiteral("A");
        a.level       = 3;
        a.lineStart   = 0;
        a.lineEnd     = 50;
        index.append(a);
    }
    {
        RoadmapIndex::Section b;
        b.slug        = QStringLiteral("b");
        b.headingText = QStringLiteral("B");
        b.level       = 3;
        b.lineStart   = 50;
        b.lineEnd     = 100;
        index.append(b);
    }

    QHash<QString, RoadmapIndex::SectionCounts> direct;
    direct[QStringLiteral("a")] = {2, 1, 3};
    direct[QStringLiteral("b")] = {0, 5, 5};

    const auto rolled = RoadmapIndex::rollupCounts(index, direct);
    const auto ra = rolled.value(QStringLiteral("a"));
    const auto rb = rolled.value(QStringLiteral("b"));
    EXPECT_EQ(ra.active,  2);
    EXPECT_EQ(ra.total,   3);
    EXPECT_EQ(rb.shipped, 5);
    EXPECT_EQ(rb.total,   5);
}

// ANTS-1622 INV-11 — section_index tally maintains the *_id_only
// parallels alongside the headline counts; rollup propagates them
// to ancestor sections too. The bug was that callers reading
// `active_count: 11` from section_index couldn't predict that the
// default bullets[] predicate would return zero (legacy roadmaps
// without [PROJ-NNNN] ids on every bullet); the parallel counts
// make the disagreement visible without a second query.
TEST(roadmap_query_section_index, Inv11IdOnlyParallelsRollUp) {
    QVector<RoadmapIndex::Section> index;
    {
        RoadmapIndex::Section parent;
        parent.slug        = QStringLiteral("tier-3");
        parent.headingText = QStringLiteral("Tier 3");
        parent.level       = 2;
        parent.lineStart   = 0;
        parent.lineEnd     = 200;
        index.append(parent);
    }
    {
        RoadmapIndex::Section child;
        child.slug        = QStringLiteral("tier-3-tasks");
        child.headingText = QStringLiteral("Tasks");
        child.level       = 3;
        child.lineStart   = 10;
        child.lineEnd     = 100;
        index.append(child);
    }

    // Direct tally: child has 11 active total, 0 of which carry IDs
    // (legacy GFM-task-list shape — exactly the cross-session-reported
    // failure case).
    QHash<QString, RoadmapIndex::SectionCounts> direct;
    RoadmapIndex::SectionCounts c;
    c.active = 11; c.shipped = 0; c.total = 11;
    c.activeWithId = 0; c.shippedWithId = 0; c.totalWithId = 0;
    direct[QStringLiteral("tier-3-tasks")] = c;

    const auto rolled = RoadmapIndex::rollupCounts(index, direct);
    const auto parent = rolled.value(QStringLiteral("tier-3"));
    EXPECT_EQ(parent.active,        11);
    EXPECT_EQ(parent.activeWithId,  0)
        << "ANTS-1622: parent rollup must surface the ID-only "
           "shortfall so callers see the legacy-format disagreement";
    EXPECT_EQ(parent.total,         11);
    EXPECT_EQ(parent.totalWithId,   0);
}

// ANTS-1622 INV-12 — emission side: section_index response shape
// carries the parallel counts and the legacy-format hint surface.
// Source-scrape style (matches the sibling INVs).
TEST(roadmap_query_section_index, Inv12EmitsIdOnlyAndLegacyHint) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "active_count_id_only"),
           "INV-12: active_count_id_only emitted on each section");
    expect(contains(cpp, "shipped_count_id_only"),
           "INV-12: shipped_count_id_only emitted on each section");
    expect(contains(cpp, "total_count_id_only"),
           "INV-12: total_count_id_only emitted on each section");
    expect(contains(cpp, "legacy_format_sections"),
           "INV-12: top-level legacy_format_sections list emitted");
    expect(contains(cpp, "legacy_format_hint"),
           "INV-12: top-level legacy_format_hint string emitted");
    expect(contains(cpp, "ANTS-1622"),
           "INV-12: anchor comment present");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1714b INV-13 — per-section legacy_format flag mirrors the
// top-level legacy_format_sections[] array so a caller reading a
// section object knows it is ID-less without grepping the slug against
// the top-level list. Set under the same self.total>0 &&
// self.totalWithId==0 predicate that feeds the array.
TEST(roadmap_query_section_index, Inv13PerSectionLegacyFlag) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "obj[\"legacy_format\"] = true;"),
           "INV-13: per-section legacy_format flag set on the section "
           "object");
    expect(contains(cpp, "ANTS-1714b"),
           "INV-13: anchor comment present");
    EXPECT_EQ(0, expect_failures());
}

// Dispatch — mainwindow's roadmap_query provider must forward the
// `mode` arg (and `include_section_headers`, caught during 1437
// live-test) to cmdRoadmapQuery. ANTS-3422 retired the hand-maintained
// per-arg forward allowlist (which repeatedly dropped new args at the
// MCP boundary) in favour of a verbatim `rcDelegate` forward that passes
// the whole args object through — so `mode` (and every arg) reaches the
// handler by construction. This scrape now asserts the verbatim forward
// rather than a per-arg line; a regression back to a selective forward
// would remove the rcDelegate registration and fail here.
TEST(roadmap_query_section_index, DispatchForwardsModeArg) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(cpp, "rcDelegate(&RemoteControl::cmdRoadmapQuery)"),
           "dispatch: roadmap_query registered via the verbatim rcDelegate "
           "forward, so mode + include_section_headers reach the handler");
    expect(contains(cpp, "ANTS-3422"),
           "dispatch: ANTS-3422 verbatim-forward anchor present");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1586 — `include_body` must reach cmdRoadmapQuery or callers
// passing include_body:true never see the `body` field. Under ANTS-3422
// the whole args object is forwarded verbatim via rcDelegate, so the arg
// arrives by construction — assert the verbatim forward is in place.
TEST(roadmap_query_section_index, DispatchForwardsIncludeBody) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(cpp, "rcDelegate(&RemoteControl::cmdRoadmapQuery)"),
           "dispatch: roadmap_query registered via the verbatim rcDelegate "
           "forward, so include_body reaches the handler");
    expect(contains(cpp, "ANTS-3422"),
           "dispatch: ANTS-3422 verbatim-forward anchor present");
    EXPECT_EQ(0, expect_failures());
}

// INV-15 (ANTS-2052) — legacy-roadmap fallback for the lean status
// filter. On a fully ID-less roadmap the ANTS-1848 id-only predicate
// drops every section under status:active/shipped, so section_index
// (and the session_orient bundle that embeds it) returned an empty
// sections[] with no hint — reading as "no active work" on a non-empty
// roadmap (RetroArch / Music Production cross-session reports). The
// branch now falls back to the raw emoji predicate when the id-only
// predicate keeps zero sections AND the raw count is > 0. Source-scrape
// style, matching the sibling emission INVs (the emission lives in the
// private cmdRoadmapQuery; functional coverage is at the rollupCounts
// level in the INV-10/INV-11 tests above).
TEST(roadmap_query_section_index, Inv15Ants2052LegacyFallback) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string fn = functionSlice(
        cpp, "QJsonDocument RemoteControl::cmdRoadmapQuery(");
    expect(!fn.empty(),
           "INV-15: cmdRoadmapQuery body must be locatable");
    expect(contains(fn, "ANTS-2052"),
           "INV-15: ANTS-2052 anchor present in the section_index branch");
    // The fallback decision: zero id-only survivors AND a non-zero raw
    // emoji total for the filter.
    expect(contains(fn, "useRawPredicate = (idOnlySurvivors == 0 && rawTotal > 0)"),
           "INV-15: fallback fires only when the id-only predicate keeps "
           "nothing and the raw emoji count is non-zero");
    // The raw totals are summed from the un-rolled `direct` map so rollup
    // can't double-count parent+child.
    expect(contains(fn, "rawActiveTotal  += it.value().active") &&
               contains(fn, "rawShippedTotal += it.value().shipped"),
           "INV-15: raw totals summed from the direct (un-rolled) map");
    // The emission drop predicate switches on useRawPredicate so an
    // id-less roadmap lists sections by their emoji count.
    expect(contains(fn, "useRawPredicate ? t.active  == 0 : t.activeWithId  == 0") &&
               contains(fn, "useRawPredicate ? t.shipped == 0 : t.shippedWithId == 0"),
           "INV-15: drop predicate honours the raw fallback");
    // When the fallback fires, the envelope flags it + exposes raw counts.
    expect(contains(fn, "out[\"legacy_format\"]     = true;") &&
               contains(fn, "out[\"raw_active_count\"]  = rawActiveTotal;") &&
               contains(fn, "out[\"raw_shipped_count\"] = rawShippedTotal;"),
           "INV-15: legacy_format + raw_active_count + raw_shipped_count "
           "emitted on the fallback path");
    EXPECT_EQ(0, expect_failures());
}

// INV-14 (ANTS-1729) — section_index paginates / auto-truncates the
// sections[] array. The old offset/limit refusal is gone, the branch
// routes the sections array through PaginationEngine::pageBullets, and
// the envelope carries the pagination fields when they apply.
TEST(roadmap_query_section_index, Inv14SectionIndexPaginates) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1729"),
           "INV-14: ANTS-1729 anchor present in the section_index branch");
    expect(!contains(cpp, "section_index mode does not accept offset/limit"),
           "INV-14: the old offset/limit refusal is removed");
    expect(contains(cpp, "PaginationEngine::pageBullets(sections"),
           "INV-14: sections[] array is sliced through the pagination "
           "helper");
    expect(contains(cpp, "if (secPage.truncated) out[\"next_offset\"]"),
           "INV-14: next_offset emitted when the section index is "
           "truncated");
    EXPECT_EQ(0, expect_failures());
}
