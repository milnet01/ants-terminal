// Feature-conformance test for ANTS-1346 section-cache LRU.
// See tests/features/roadmap_section_cache_lru/spec.md.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1: cap constant declared in remotecontrol.h.
TEST(RoadmapSectionCacheLru, CapConstantDeclared) {
    const std::string h = slurp(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(h.empty());
    EXPECT_TRUE(contains(h,
        "static constexpr int    kRoadmapSectionCacheCap = 64"))
        << "kRoadmapSectionCacheCap = 64 declaration missing";
}

// INV-2: LRU member is a QList<QString>.
TEST(RoadmapSectionCacheLru, LruMemberDeclared) {
    const std::string h = slurp(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(h.empty());
    EXPECT_TRUE(contains(h, "QList<QString>"))
        << "QList<QString> not found in remotecontrol.h";
    EXPECT_TRUE(contains(h, "m_roadmapSectionLru"))
        << "m_roadmapSectionLru member missing";
}

// INV-4: mtime-stale wipe clears all three structures (index +
// cache + LRU) in one block.
TEST(RoadmapSectionCacheLru, StaleWipeClearsAllThree) {
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty());
    // Find the m_roadmapIndex.clear() site (anchor for the wipe
    // block) and check the next 200 chars contain all three clears.
    const auto idxPos = cpp.find("m_roadmapIndex.clear();");
    ASSERT_NE(idxPos, std::string::npos)
        << "m_roadmapIndex.clear() anchor missing";
    const std::string region = cpp.substr(idxPos, 200);
    EXPECT_TRUE(contains(region, "m_roadmapSectionCache.clear();"))
        << "section cache not cleared in same block as index";
    EXPECT_TRUE(contains(region, "m_roadmapSectionLru.clear();"))
        << "LRU not cleared in same block as index";
}

// Hit-path bump: the contains() branch must bump the slug to MRU.
TEST(RoadmapSectionCacheLru, HitPathBumpsToMru) {
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty());
    // Anchor: the existing `contains(sec->slug)` branch (single hit
    // in cmdRoadmapQuery).
    const auto hitPos = cpp.find(
        "m_roadmapSectionCache.contains(sec->slug)");
    ASSERT_NE(hitPos, std::string::npos);
    // Within ~300 chars after the hit anchor, expect the bump pair.
    const std::string region = cpp.substr(hitPos, 300);
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
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
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
