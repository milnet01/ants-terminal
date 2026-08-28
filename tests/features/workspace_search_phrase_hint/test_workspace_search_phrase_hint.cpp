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
    const std::string src = ants_test::slurpRemoteControl();
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
    const std::string src = ants_test::slurpRemoteControl();
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
    const std::string src = ants_test::slurpRemoteControl();
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

// ANTS-4420 — a pattern carrying HTML entities where the caller meant the
// literal characters returns zero matches and, before this, no hint at all:
// neither the ANTS-2045 branch (the pattern has no whitespace) nor ANTS-3466's
// (it was regex:true) could fire. Reported by a Charls_Site session:
// `&lt;h[123][^&gt;]*&gt;` found nothing and read as "this file has no
// headings", while the literal `<h[123][ >]` found 11 in the same file.
//
// Source-scraped for the reason the three tests above are: the envelope
// builder needs a live RemoteControl. So this pins the WIRING — that the
// detector exists, that it gates the hint, that it runs first, and that the
// hint names the literal characters. It does NOT exercise the matcher against
// a real pattern; rcContainsHtmlEntity lives in remotecontrol_internal.h,
// which no test includes, and making it test-facing is a wider change than
// this item earns.
TEST(WorkspaceSearchPhraseHint, Ants4420HtmlEntityHint) {
    const std::string src = ants_test::slurpRemoteControl();
    ASSERT_FALSE(src.empty());
    EXPECT_TRUE(has(src, "rcContainsHtmlEntity"))
        << "the HTML-entity detector must exist";
    EXPECT_TRUE(has(src, "matches.isEmpty() && rcContainsHtmlEntity(pattern)"))
        << "the hint must gate on zero matches + an entity-bearing pattern";
    // Ordered FIRST: an entity-bearing pattern that ALSO has whitespace must
    // be diagnosed as an entity problem, not sent to fix its spacing. The
    // ANTS-2045 branch is therefore an else-if off this one.
    EXPECT_TRUE(has(src, "else if (matches.isEmpty() && pattern.trimmed()"))
        << "the phrase branch must chain off the entity branch, so the more "
           "specific diagnosis wins and `hint` is still set at most once";
    EXPECT_TRUE(has(src, "did you \"\n            \"mean the literal characters")
                || has(src, "mean the literal characters"))
        << "the advisory must point at the literal characters";
    // Narrow by construction: `&` + a name + `;`. A bare `&` is ubiquitous in
    // real code (`a && b`) and carries no terminator, so it must not qualify.
    EXPECT_TRUE(has(src, "&(lt|gt|amp|quot|apos|nbsp|#[0-9]+|#x[0-9A-Fa-f]+);"))
        << "the detector must require a terminated entity, not a bare &";
}

// ANTS-4753 — a `glob` naming a dot-directory returns zero matches and, before
// this, no reason at all: `include_hidden` defaults to false, ripgrep prunes
// the hidden directory during the walk, and the caller cannot tell an absent
// string from a path that was never read. Hit in-session: a glob over
// `.github/workflows/` came back empty and the conclusion drawn from it — that
// the release workflow had lost a step — was wrong.
//
// Measured on rg 15.2.0 before implementing, because the reported fix was half
// wrong: a glob naming a hidden DIRECTORY finds nothing without --hidden, but
// an explicitly-named `lane` under one IS descended, and a hidden FILE named
// by a glob is whitelisted through. So the advisory fires on `glob` alone —
// a lane-triggered warning would be a false alarm.
//
// Source-scraped for the reason the four tests above are: the envelope builder
// needs a live RemoteControl. This pins the WIRING — the detector exists, it
// gates on zero results, it reaches every return path that can emit a zero,
// and the hint names the argument that lifts it.
TEST(WorkspaceSearchPhraseHint, Ants4753HiddenGlobHint) {
    const std::string src = ants_test::slurpRemoteControl();
    ASSERT_FALSE(src.empty());
    EXPECT_TRUE(has(src, "rcGlobNamesHiddenPath"))
        << "the hidden-path glob detector must exist";
    // Gated on include_hidden being off — with it on, nothing was skipped.
    EXPECT_TRUE(has(src, "!include_hidden && rcGlobNamesHiddenPath(glob)"))
        << "the advisory must gate on include_hidden:false + a dotted glob";
    EXPECT_TRUE(has(src, "out[\"hidden_paths_skipped\"] = true"))
        << "the flag must land on the envelope";
    // Ordered FIRST in the zero-match chain: when the walk never reached the
    // path, no hint about the PATTERN can be the right diagnosis.
    EXPECT_TRUE(has(src, "if (matches.isEmpty() && hiddenGlobSkipped)"))
        << "the hidden-path branch must lead the zero-match hint chain";
    EXPECT_TRUE(has(src, "else if (matches.isEmpty() && rcContainsHtmlEntity(pattern))"))
        << "the entity branch must chain off it, so `hint` is set at most once";
    // The three rows-eliminated modes return their own envelopes and their own
    // zeros; an existence check via count_only is the most dangerous of them.
    EXPECT_TRUE(has(src, "seenMatchEvents == 0 && hiddenGlobSkipped"))
        << "count_only's zero must carry the advisory too";
    EXPECT_TRUE(has(src, "filesWithMatches == 0 && hiddenGlobSkipped"))
        << "files_only's zero must carry the advisory too";
    EXPECT_TRUE(has(src, "distinctOrder.isEmpty() && hiddenGlobSkipped"))
        << "matches_only's zero must carry the advisory too";
    // The hint names the argument that lifts the filter.
    EXPECT_TRUE(has(src, "include_hidden:true"))
        << "the advisory must name include_hidden:true as the fix";
    // A lane is descended even when hidden, so it must not trigger this.
    EXPECT_FALSE(has(src, "rcGlobNamesHiddenPath(laneRaw)"))
        << "a lane naming a hidden directory is searched anyway — no advisory";
}
