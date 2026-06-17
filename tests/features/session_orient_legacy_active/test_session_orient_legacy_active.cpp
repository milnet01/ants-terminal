// Feature-conformance test for ANTS-2052 — session_orient recovers the
// active-bullet headlines on a fully id-less legacy roadmap. The orient
// bundle is GUI-bound (MainWindow + live RemoteControl), so the wiring is
// pinned by source-scrape of the active_bullets fallback.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {
bool has(const std::string &h, const char *n) {
    return h.find(n) != std::string::npos;
}
}  // namespace

TEST(SessionOrientLegacyActive, Inv1FallbackReissuesWithNarrators) {
    const std::string src = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(src.empty());
    // section_index's raw_active_count is captured for the orient bundle.
    EXPECT_TRUE(has(src, "legacyRawActive"))
        << "orient must capture the legacy raw active count";
    // active_bullets re-issues including narrator bullets on the legacy case.
    EXPECT_TRUE(has(src, "include_narrator_bullets"))
        << "the fallback must re-issue with narrator bullets";
    EXPECT_TRUE(has(src, "legacy_format_fallback"))
        << "the recovery must be flagged on the envelope";
    // Gated on zero active_bullets AND a non-zero legacy raw count.
    EXPECT_TRUE(has(src, "legacyRawActive > 0"));
}
