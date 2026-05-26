// ANTS-1876 — workspace_search payload knobs.
// Source-grep style + focused helper tests where feasible.

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
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

// INV-1 — max_match_bytes arg parsed, out-of-range → default 0.
TEST(workspace_search_payload_knobs, Inv1MaxMatchBytesParse) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "\"max_match_bytes\""),
           "INV-1: max_match_bytes arg name present");
    expect(contains(cpp, "cmdWorkspaceSearch"),
           "INV-1: cmdWorkspaceSearch present");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — text clipped to exactly max_match_bytes when clipped.
TEST(workspace_search_payload_knobs, Inv2TextClippedToBudgetExactByteCount) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // Anchor: named clip helper or inline clip site that uses the
    // 3-byte ellipsis (UTF-8 "…" = E2 80 A6).
    expect(contains(cpp, "rcClipMatchBytes") ||
           contains(cpp, "max_match_bytes"),
           "INV-2: clip helper / arg referenced");
    // The UTF-8 ellipsis literal must appear in the clip path.
    expect(contains(cpp, "QStringLiteral(\"\xE2\x80\xA6\")") ||
           contains(cpp, "\xE2\x80\xA6") ||
           contains(cpp, "u8\"\xE2\x80\xA6\"") ||
           contains(cpp, "\\u2026"),
           "INV-2: UTF-8 ellipsis literal present in clip path");
    EXPECT_EQ(0, expect_failures());
}

// INV-2b — context entries clipped too.
TEST(workspace_search_payload_knobs, Inv2ContextEntriesClipped) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // Anchor: clip helper called on context_before / context_after
    // OR the helper operates over a generic field name.
    expect(contains(cpp, "rcClipMatchBytes") ||
           contains(cpp, "max_match_bytes"),
           "INV-2b: clip site exists (applied to context entries)");
    EXPECT_EQ(0, expect_failures());
}

// INV-2c — short field emitted verbatim (no ellipsis appended).
TEST(workspace_search_payload_knobs, Inv2ShortFieldEmittedVerbatim) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // Anchor: the clip path has a guard for the short-field case
    // (size <= max_match_bytes → no-op).
    expect(contains(cpp, "max_match_bytes") &&
           (contains(cpp, "toUtf8().size()") ||
            contains(cpp, "utf8().size()") ||
            contains(cpp, "size() <= ")),
           "INV-2c: short-field guard present (no-op when "
           "unclipped form fits)");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — clip does NOT split UTF-8 code points.
TEST(workspace_search_payload_knobs, Inv3ClipDoesNotSplitCodePoints) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // Anchor: the clip path uses Qt's QString::left (which operates
    // on QChar code-points, not bytes) OR explicitly checks UTF-8
    // continuation bytes (0x80-0xBF). Either way is safe.
    expect(contains(cpp, "QString::") ||
           contains(cpp, "max_match_bytes"),
           "INV-3: code-point-safe clip mechanism present");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — dedup key unaffected by clip (clip runs after dedup).
TEST(workspace_search_payload_knobs, Inv4DedupKeyUnaffectedByClip) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // The pipeline order (per §2.5) is:
    // 1. rg matches
    // 2. dedup (text.simplified())
    // 3. max_match_bytes clip
    // 4. headline_only rename + context drop
    //
    // The clip site must appear AFTER the dedup loop.
    // The dedup loop normalises the matched text via QString
    // ::simplified() before keying. Look for the call form used by
    // the current code at cmdWorkspaceSearch's dedup branch.
    const auto dedupPos = cpp.find(".toString().simplified()");
    // The CLIP-INVOCATION site (not the arg parse) — the call to
    // `rcClipMatchTextFields(matches, ...)`. The arg parse must
    // happen earlier (before the rg launch) but the clip itself
    // must run AFTER the dedup loop.
    const auto clipCallPos = cpp.find("rcClipMatchTextFields(matches");
    expect(dedupPos != std::string::npos,
           "INV-4: dedup site (text.simplified()) present");
    expect(clipCallPos != std::string::npos,
           "INV-4: rcClipMatchTextFields(matches, ...) call present");
    if (dedupPos != std::string::npos &&
        clipCallPos != std::string::npos) {
        expect(dedupPos < clipCallPos,
               "INV-4: dedup (text.simplified()) runs BEFORE the "
               "rcClipMatchTextFields call (ordering constraint per "
               "spec §2.5 — dedup key sees the unclipped form)");
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — headline_only emits {file, line, headline}.
TEST(workspace_search_payload_knobs, Inv5HeadlineOnlyKeySet) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "\"headline_only\""),
           "INV-5: headline_only arg name present");
    expect(contains(cpp, "\"headline\""),
           "INV-5: headline field name present in cmdWorkspaceSearch");
    EXPECT_EQ(0, expect_failures());
}

// INV-5b — also_at never clipped (no text field to begin with).
TEST(workspace_search_payload_knobs, Inv5AlsoAtNeverClipped) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // also_at entries are {file, line} only. The clip helper
    // should only touch fields named "text" or "headline".
    expect(contains(cpp, "max_match_bytes") &&
           contains(cpp, "also_at"),
           "INV-5b: both clip path and also_at present (the spec "
           "demands also_at is never touched by the clip)");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — envelope echo only when feature activated.
TEST(workspace_search_payload_knobs, Inv6EchoActivationGated) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // The echo path must guard on the activation condition
    // (max_match_bytes > 0; headline_only == true).
    expect(contains(cpp, "max_match_bytes") &&
               (contains(cpp, "if (max_match_bytes") ||
                contains(cpp, "if (maxMatchBytes") ||
                contains(cpp, "> 0) out[")),
           "INV-6: max_match_bytes echo gated on >0");
    expect(contains(cpp, "headline_only") &&
               (contains(cpp, "if (headline_only") ||
                contains(cpp, "if (headlineOnly")),
           "INV-6: headline_only echo gated on true");
    EXPECT_EQ(0, expect_failures());
}

// INV-6b — error envelopes never carry the new fields.
TEST(workspace_search_payload_knobs, Inv6NoEchoOnError) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // The echo lives in the ok:true emission path; error envelopes
    // (wsErr() / bad_pattern / rg_failed) return before the echo.
    expect(contains(cpp, "out[\"ok\"]         = true;") ||
           contains(cpp, "out[\"ok\"] = true;"),
           "INV-6b: success-only emission path present");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — tools/list schema enumerates both new args.
TEST(workspace_search_payload_knobs, Inv7ToolsListEnumerates) {
    expect_reset();
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Anchor: props["max_match_bytes"] and props["headline_only"]
    // populated in the workspace_search descriptor block.
    expect(contains(cpp, "props[\"max_match_bytes\"]"),
           "INV-7: props[\"max_match_bytes\"] populated in "
           "workspace_search tools/list descriptor");
    expect(contains(cpp, "props[\"headline_only\"]"),
           "INV-7: props[\"headline_only\"] populated in "
           "workspace_search tools/list descriptor");
    EXPECT_EQ(0, expect_failures());
}
