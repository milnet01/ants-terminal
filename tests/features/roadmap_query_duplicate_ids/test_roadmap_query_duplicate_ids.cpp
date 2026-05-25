// ANTS-1646 — feature-conformance test for roadmap_query's
// duplicate-ID envelope field + cache plumbing. Source-grep style
// matches sibling roadmap_query feature tests.

#include "../../_support/expect.h"
#include "roadmapindex.h"

#include <gtest/gtest.h>

#include <QString>

#include <cstdio>
#include <fstream>
#include <map>
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

int count(const std::string &hay, const std::string &needle) {
    int n = 0;
    std::string::size_type pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

}  // namespace

// INV-1 — helper exists and skips empty-id entries.
TEST(roadmap_query_duplicate_ids, Inv1HelperComputesOccurrences) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "rcComputeDuplicateIds"),
           "INV-1: rcComputeDuplicateIds helper present");
    expect(contains(cpp, "occurrences"),
           "INV-1: helper builds an `occurrences` array per id");
    expect(contains(cpp, "section_slug"),
           "INV-1: occurrences carry section_slug");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — recompute on every cache-fill path. Three call sites
// assign m_roadmapCacheBullets; each must call the detector.
TEST(roadmap_query_duplicate_ids, Inv2RecomputeOnCacheFill) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // The recompute callsite signature is the helper invocation
    // immediately under each `m_roadmapCacheBullets = arr;`
    // assignment. Count both occurrences across the file.
    const int callSites = count(cpp,
        "m_roadmapCacheDuplicateIds =\n"
        "                rcComputeDuplicateIds(m_roadmapCacheBullets);");
    const int callSitesAlt = count(cpp,
        "m_roadmapCacheDuplicateIds =\n"
        "                    rcComputeDuplicateIds(m_roadmapCacheBullets);");
    const int callSitesPlain = count(cpp,
        "m_roadmapCacheDuplicateIds =\n"
        "            rcComputeDuplicateIds(m_roadmapCacheBullets);");
    const int total = callSites + callSitesAlt + callSitesPlain;
    expect(total >= 3,
           ("INV-2: recompute call site expected at all three "
            "cache-fill paths; found " +
            std::to_string(total)).c_str());
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — cache wipe clears the duplicate-id cache alongside the
// other cache members.
TEST(roadmap_query_duplicate_ids, Inv3CacheWipeClearsDescriptors) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "m_roadmapCacheDuplicateIds = QJsonArray();"),
           "INV-3: stale-cache wipe also clears the duplicate-id "
           "cache");
    EXPECT_EQ(0, expect_failures());
}

// INV-4/5 — envelope emission gated on non-empty descriptor,
// reachable from all three response-assembly paths.
TEST(roadmap_query_duplicate_ids, Inv45EmissionGatedAndThreePaths) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // The emission idiom appears once per response shape (section_
    // index, section, full-file). Each writes the cached array under
    // the `duplicate_ids` key when non-empty.
    // Avoid the Qt `emit` macro (defined to nothing) — it would
    // strip the identifier.
    const int emitCount = count(cpp,
        "out[\"duplicate_ids\"] = m_roadmapCacheDuplicateIds;");
    expect(emitCount >= 3,
           ("INV-4/5: expected three emission sites; found " +
            std::to_string(emitCount)).c_str());
    expect(contains(cpp, "!m_roadmapCacheDuplicateIds.isEmpty()"),
           "INV-4: emission gated on non-empty descriptor (keeps "
           "back-compat envelope shape on a clean roadmap)");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1688 INV-7 — canonical-ID predicate behaviour. The detector
// must key only on the allocated `[A-Za-z][A-Za-z0-9_-]*-<digits>`
// shape; this is the unit reproduction of the cross-session bug where
// a 10-char content-hash nonce / Obsidian `^anchor` token (e.g.
// `35ra39wbn1`) was surfaced as a duplicate "ID" on a legacy roadmap.
TEST(roadmap_query_duplicate_ids, Inv7CanonicalIdPredicate) {
    expect_reset();
    // Accept: real allocated IDs across projects.
    expect(RoadmapIndex::isCanonicalId(QStringLiteral("ANTS-1688")),
           "INV-7: ANTS-1688 is canonical");
    expect(RoadmapIndex::isCanonicalId(QStringLiteral("VEST-0042")),
           "INV-7: multi-letter-prefix VEST-0042 is canonical");
    expect(RoadmapIndex::isCanonicalId(QStringLiteral("PROJ-1")),
           "INV-7: single-digit suffix is canonical");
    // Reject: the bug shapes + empties.
    expect(!RoadmapIndex::isCanonicalId(QStringLiteral("35ra39wbn1")),
           "INV-7: content-hash nonce is NOT a duplicate ID (the "
           "over-report bug)");
    expect(!RoadmapIndex::isCanonicalId(QString()),
           "INV-7: empty id is not canonical");
    expect(!RoadmapIndex::isCanonicalId(QStringLiteral("Sh4")),
           "INV-7: hyphen-less legacy bold-id is not canonical");
    expect(!RoadmapIndex::isCanonicalId(QStringLiteral("ANTS-")),
           "INV-7: prefix without digits is not canonical");
    expect(!RoadmapIndex::isCanonicalId(QStringLiteral("Phase 9F-1")),
           "INV-7: narrator headline with a space is not canonical");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1688 INV-8 — the detector is wired to the predicate + caps the
// per-ID occurrences tail so a legacy roadmap can't blow the response
// size budget.
TEST(roadmap_query_duplicate_ids, Inv8DetectorFiltersAndCaps) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "RoadmapIndex::isCanonicalId"),
           "INV-8: rcComputeDuplicateIds keys on the canonical-ID "
           "predicate so anchors/hashes don't masquerade as IDs");
    expect(contains(cpp, "truncated_count"),
           "INV-8: occurrences tail capped with a truncated_count "
           "field (payload-shrink for large legacy roadmaps)");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — MCP descriptor advertises the field.
TEST(roadmap_query_duplicate_ids, Inv6McpDescriptorMentionsField) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto pos = ci.find("\"roadmap_query\"");
    ASSERT_NE(pos, std::string::npos)
        << "INV-6 precondition: roadmap_query tool name missing "
           "from claudeintegration.cpp";
    // 9 KiB window: the description grew with the ANTS-1646
    // duplicate_ids paragraph. Past tests bumped from 6→7 KiB; this
    // budget keeps headroom for the next minor additive change
    // without bleeding into the following tool descriptor.
    const std::string region = ci.substr(pos, 9000);
    expect(contains(region, "duplicate_ids"),
           "INV-6: roadmap_query descriptor names duplicate_ids[] "
           "so the field is discoverable from tools/list");
    expect(contains(region, "ANTS-1646"),
           "INV-6: descriptor anchors the field to ANTS-1646 so a "
           "future audit can trace the rationale");
    EXPECT_EQ(0, expect_failures());
}

// Live-roadmap sanity — the project's own ROADMAP.md must carry
// exactly one bullet per [ANTS-NNNN] id. The historical ANTS-1118
// cross-cite was deduped on 2026-05-20 (the Tier-1 0.7.65 entry is
// now a non-ID cross-reference), so there are zero intentional
// duplicates today. This test fires the moment a NEW collision
// lands — the same hand-edit drift `tools/check-roadmap.sh` guards.
TEST(roadmap_query_duplicate_ids, LiveRoadmapNoDuplicateBullets) {
    expect_reset();
    const std::string md = slurp(ROADMAP_MD_PATH);
    // Hand-rolled scanner mirroring rcComputeDuplicateIds' predicate
    // (line begins with bullet status emoji + bracketed ANTS-id).
    // ASCII status check + UTF-8 emoji presence is plenty for
    // catching real duplicates; we don't need the full parser here.
    std::string line;
    std::stringstream in(md);
    int dupes = 0;
    std::string dupeNames;
    std::map<std::string, int> seen;
    while (std::getline(in, line)) {
        if (line.size() < 4 || line[0] != '-' || line[1] != ' ')
            continue;
        // Look for "[ANTS-" past the status emoji + space.
        const auto p = line.find("[ANTS-");
        if (p == std::string::npos) continue;
        const auto end = line.find(']', p);
        if (end == std::string::npos) continue;
        const std::string id = line.substr(p, end - p + 1);
        // Require the inside-bracket id to be ANTS-<digits>. Skips
        // the placeholder `[ANTS-NNNN]` literals used in roadmap
        // examples (the doc carries dozens) and the `[ANTS-1118+]`
        // shorthand that names a follow-up bundle.
        bool digitsOnly = id.size() > 7;  // "[ANTS-" + ≥1 digit + "]"
        for (std::size_t i = 6; digitsOnly && i + 1 < id.size(); ++i) {
            if (id[i] < '0' || id[i] > '9') digitsOnly = false;
        }
        if (!digitsOnly) continue;
        // Count second-or-later sightings as duplicates.
        if (++seen[id] == 2) {
            ++dupes;
            dupeNames += (dupeNames.empty() ? "" : ", ") + id;
        }
    }
    expect(dupes == 0,
           ("LiveRoadmap: duplicate bullet id(s) detected: " +
            (dupeNames.empty() ? std::string("(none)") : dupeNames) +
            " — each [ANTS-NNNN] must own exactly one bullet; "
            "renumber via roadmap_log op:append or convert the "
            "cross-cite to a non-ID reference").c_str());
    EXPECT_EQ(0, expect_failures());
}
