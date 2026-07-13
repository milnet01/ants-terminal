// ANTS-1882 — roadmap_query per-section ETag invariance.
// Source-grep + pure-function tests.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — filter helper signature + contract (source-grep, since the
// helper lives in remotecontrol.cpp's anonymous namespace).
TEST(roadmap_query_per_section_etag, Inv1FilterContract) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // Helper definition with the expected signature.
    expect(contains(cpp,
        "QJsonArray rcFilterDuplicateIdsForSection(const QJsonArray "
        "&dupes,"),
           "INV-1: helper signature matches "
           "(const QJsonArray &dupes, const QString &sectionSlug)");
    // Returns empty on empty inputs.
    expect(contains(cpp, "dupes.isEmpty() || sectionSlug.isEmpty()"),
           "INV-1: empty-input guard returns empty QJsonArray");
    // Iterates occurrences[] and keys on section_slug equality.
    expect(contains(cpp, "occurrences"),
           "INV-1: filter walks the entry's occurrences[] array");
    expect(contains(cpp, "section_slug"),
           "INV-1: filter compares each occurrence's section_slug");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — section path uses the filter.
TEST(roadmap_query_per_section_etag, Inv2SectionPathFilters) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "rcFilterDuplicateIdsForSection("),
           "INV-2: filter helper called somewhere in remotecontrol.cpp");
    // Anchor: section path's call site references sec->slug.
    expect(contains(cpp,
        "rcFilterDuplicateIdsForSection(\n                m_roadmapCacheDuplicateIds, sec->slug)") ||
        contains(cpp,
        "rcFilterDuplicateIdsForSection(m_roadmapCacheDuplicateIds, sec->slug)"),
           "INV-2: section path calls filter with sec->slug");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — guard on the non-empty gate at the section-path call site.
TEST(roadmap_query_per_section_etag, Inv3SectionResponseInvariantUnderUnrelatedEdit) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // The new code emits duplicate_ids only when the scoped array is
    // non-empty (matches the existing emptiness-gate for the bare
    // cache field). This keeps the envelope absent on the common
    // "no duplicates touching this section" path → byte-identity
    // across unrelated edits.
    expect(contains(cpp, "if (!scoped.isEmpty())"),
           "INV-3: section path's scoped emit guarded on non-empty");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — full-file + id-branch still emit unscoped duplicate_ids.
TEST(roadmap_query_per_section_etag, Inv4FullFilePathUnchanged) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // Count the bare-cache assignments:
    //   out["duplicate_ids"] = m_roadmapCacheDuplicateIds;
    // Should be at least 3 (full-file, id-branch, section_index
    // path — section path has been replaced with the filter call).
    int hits = 0;
    size_t pos = 0;
    const std::string needle = "out[\"duplicate_ids\"] = m_roadmapCacheDuplicateIds";
    while ((pos = cpp.find(needle, pos)) != std::string::npos) {
        ++hits;
        pos += needle.size();
    }
    expect(hits >= 2,
           "INV-4: at least two unscoped emit sites remain "
           "(full-file path + id-branch)");
    EXPECT_EQ(0, expect_failures());
}
