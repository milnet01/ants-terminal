// Feature-conformance test for ANTS-1346 section-cache LRU.
// See tests/features/roadmap_section_cache_lru/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1: cap constant declared in remotecontrol.h.
TEST(RoadmapSectionCacheLru, CapConstantDeclared) {
    const std::string h = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(h.empty());
    EXPECT_TRUE(contains(h,
        "static constexpr int    kRoadmapSectionCacheCap = 64"))
        << "kRoadmapSectionCacheCap = 64 declaration missing";
}

// INV-2: LRU member is a QList<QString>.
TEST(RoadmapSectionCacheLru, LruMemberDeclared) {
    const std::string h = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(h.empty());
    EXPECT_TRUE(contains(h, "QList<QString>"))
        << "QList<QString> not found in remotecontrol.h";
    EXPECT_TRUE(contains(h, "m_roadmapSectionLru"))
        << "m_roadmapSectionLru member missing";
}

// INV-4: mtime-stale wipe clears the sibling caches in one block.
// ANTS-1907 widened the wipe block to also clear
// m_roadmapSectionEtags (the per-section ETag cache lives in
// lockstep with the bullet cache); window widened to 400 chars to
// cover the new clear-line without making the locality check
// less meaningful (the original 200 covered three clears; we now
// have four siblings sitting together).
TEST(RoadmapSectionCacheLru, StaleWipeClearsAllThree) {
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty());
    const auto idxPos = cpp.find("m_roadmapIndex.clear();");
    ASSERT_NE(idxPos, std::string::npos)
        << "m_roadmapIndex.clear() anchor missing";
    const std::string region = cpp.substr(idxPos, 400);
    EXPECT_TRUE(contains(region, "m_roadmapSectionCache.clear();"))
        << "section cache not cleared in same block as index";
    EXPECT_TRUE(contains(region, "m_roadmapSectionLru.clear();"))
        << "LRU not cleared in same block as index";
    // ANTS-1907 — etag cache must also be cleared in the same block
    // so a content change doesn't leak a stale per-section etag.
    EXPECT_TRUE(contains(region, "m_roadmapSectionEtags.clear();"))
        << "section-etag cache not cleared in same block as index";
}

// Hit-path bump: the contains() branch must bump the slug to MRU.
// ANTS-1907 widened the hit-path with a `sectionEtag = …` lookup
// between the contains() check and the MRU bump; window widened
// to 500 chars to accommodate it.
TEST(RoadmapSectionCacheLru, HitPathBumpsToMru) {
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty());
    const auto hitPos = cpp.find(
        "m_roadmapSectionCache.contains(sec->slug)");
    ASSERT_NE(hitPos, std::string::npos);
    const std::string region = cpp.substr(hitPos, 500);
    EXPECT_TRUE(contains(region,
        "m_roadmapSectionLru.removeOne(sec->slug)"))
        << "hit-path missing removeOne bump";
    EXPECT_TRUE(contains(region,
        "m_roadmapSectionLru.prepend(sec->slug)"))
        << "hit-path missing prepend bump";
}

// Insert-path eviction: after the insert, push slug to MRU and
// evict tail while over cap.
TEST(RoadmapSectionCacheLru, InsertPathEvictsTail) {
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty());
    const auto insPos = cpp.find(
        "m_roadmapSectionCache.insert(sec->slug, sectionBullets)");
    ASSERT_NE(insPos, std::string::npos);
    const std::string region = cpp.substr(insPos, 600);
    EXPECT_TRUE(contains(region,
        "m_roadmapSectionLru.size() > kRoadmapSectionCacheCap"))
        << "cap-overflow check missing";
    EXPECT_TRUE(contains(region,
        "m_roadmapSectionLru.takeLast()"))
        << "tail-eviction takeLast missing";
    EXPECT_TRUE(contains(region,
        "m_roadmapSectionCache.remove("))
        << "evicted slug not removed from cache";
}
