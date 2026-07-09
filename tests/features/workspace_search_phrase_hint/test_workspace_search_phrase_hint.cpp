// Feature-conformance test for ANTS-2045 — workspace_search emits an
// advisory hint when a multi-word query returns zero matches. Source-scrape
// of cmdWorkspaceSearch: the envelope builder is GUI-bound (needs a live
// RemoteControl), so the contract is pinned structurally.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {
bool has(const std::string &h, const char *n) {
    return h.find(n) != std::string::npos;
}
}  // namespace

TEST(WorkspaceSearchPhraseHint, Inv1ZeroMatchPhraseHint) {
    const std::string src = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(src.empty());
    // The hint fires only on an empty result set with a whitespace query.
    EXPECT_TRUE(has(src, "matches.isEmpty() && pattern.trimmed().contains"))
        << "hint must gate on zero matches + a whitespace-bearing query";
    EXPECT_TRUE(has(src, "out[\"hint\"]"))
        << "the advisory hint must land on the envelope";
    // Both literal + regex phrasings present (isRegex branch).
    EXPECT_TRUE(has(src, "matched as one literal phrase"));
    EXPECT_TRUE(has(src, "matched as one regex pattern"));
}

// ANTS-2181 — a regex alternation carrying very short bare terms (e.g.
// "tan" in "constant") gets a complementary `regex_advisory`. The envelope
// builder is GUI-bound, so the contract is pinned structurally (mirrors the
// ANTS-2045 phrase-hint scrape above).
TEST(WorkspaceSearchPhraseHint, Ants2181RegexShortBareTermAdvisory) {
    const std::string src = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(src.empty());
    // The detector helper exists.
    EXPECT_TRUE(has(src, "rcShortBareAltTerms"))
        << "the short-bare-alternation-term detector must exist";
    // It gates on a top-level alternation and a purely [A-Za-z]{1,3} piece
    // (anchored / metacharacter-bearing pieces are exempt).
    EXPECT_TRUE(has(src, "^[A-Za-z]{1,3}$"))
        << "detector must match only short bare word terms";
    // The advisory is emitted under the isRegex branch on a distinct key.
    EXPECT_TRUE(has(src, "out[\"regex_advisory\"]"))
        << "the regex advisory must land on the envelope";
    EXPECT_TRUE(has(src, "anchor "))   // the \\b suggestion text
        << "the advisory must suggest anchoring with \\b";
}

// ANTS-3466 — companion to ANTS-2045 for the no-whitespace case: a
// regex:false pattern bearing regex metacharacters (e.g. `A|B|C`) that
// returns zero matches gets a "did you mean regex:true?" hint. Source-scrape
// (envelope builder is GUI-bound), mirroring the ANTS-2045 scrape above.
TEST(WorkspaceSearchPhraseHint, Ants3466MetacharRegexFalseHint) {
    const std::string src = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(src.empty());
    // The high-precision metachar detector helper exists.
    EXPECT_TRUE(has(src, "rcLooksLikeRegexButLiteral"))
        << "the metacharacter-intent detector must exist";
    // It fires only on the no-whitespace, regex:false, zero-match path (an
    // else-if off the ANTS-2045 whitespace branch so `hint` is never
    // double-set).
    EXPECT_TRUE(has(src, "rcLooksLikeRegexButLiteral(pattern))"))
        << "hint must gate on the metachar detector";
    EXPECT_TRUE(has(src, "matches.isEmpty() && !isRegex"))
        << "hint must gate on zero matches + regex:false";
    // The hint steers the caller to regex:true.
    EXPECT_TRUE(has(src, "did you mean regex:true?"))
        << "the advisory must suggest regex:true";
    // Detector is deliberately narrow — alternation is the canonical signal.
    EXPECT_TRUE(has(src, "pattern.contains(QChar('|'))"))
        << "alternation must be a detected metacharacter";
}
