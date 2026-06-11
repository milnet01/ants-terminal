// Feature-conformance test for spec.md —
//
// Source-grep test. The QTabBar::close-button stylesheet must
// carry an explicit data-URI SVG `image: url(...)` rule for both
// default and hover states, with the URL-encoded `%23` color
// pre-encoding spliced into the arg list (not the format string).

#include "../../_support/expect.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_MAINWINDOW_CPP_PATH
#  error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

// ANTS-1147 — the QTabBar::close-button stylesheet rules
// (default + hover, with the data-URI SVG and the %23 arg
// splices) moved into themedstylesheet::buildAppStylesheet.
// All assertions in this test now read the new TU.
#ifndef SRC_THEMEDSTYLESHEET_CPP_PATH
#  error "SRC_THEMEDSTYLESHEET_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {




bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

TEST(TabCloseButtonVisible, Main) {
    expect_reset();
    // ANTS-1147 — the close-button QSS rules moved into
    // themedstylesheet.cpp. Source path of the QSS body is now
    // there; mainwindow.cpp no longer carries the data-URI SVG.
    const std::string src = ants_test::slurpFile(SRC_THEMEDSTYLESHEET_CPP_PATH);

    // I1 — default-state rule has the data-URI image.
    expect(contains(src, "QTabBar::close-button {"),
           "I1/close-button-default-rule-present");
    {
        std::regex defaultRule(
            R"(QTabBar::close-button \{[^\}]*image:\s*url\(\\\"data:image/svg\+xml)");
        expect(std::regex_search(src, defaultRule),
               "I1/default-rule-has-data-uri-image");
    }
    // SVG body must draw two lines for the × — guards against an
    // empty-rect or single-line regression. ANTS-1321: SVG strokes
    // now use %9 (URL-encoded textSecondary) NOT %6 (CSS-context
    // textSecondary). The pre-1321 shape reused %6 in both contexts
    // which produced invalid CSS `color: %23RRGGBB;` rules.
    expect(contains(src,
               "<line x1='2' y1='2' x2='8' y2='8' stroke='%9'"),
           "I1/default-svg-line1-textSecondary-stroke");
    expect(contains(src,
               "<line x1='8' y1='2' x2='2' y2='8' stroke='%9'"),
           "I1/default-svg-line2-textSecondary-stroke");

    // I2 — hover rule exists with its own image and ansi-red bg.
    expect(contains(src, "QTabBar::close-button:hover {"),
           "I2/close-button-hover-rule-present");
    {
        std::regex hoverRule(
            R"(QTabBar::close-button:hover \{[^\}]*image:\s*url\(\\\"data:image/svg\+xml)");
        expect(std::regex_search(src, hoverRule),
               "I2/hover-rule-has-data-uri-image");
    }
    // ANTS-1321: hover stroke moved from %3 (CSS textPrimary) to %8
    // (URL-encoded textPrimary, SVG-only) to fix the parse-bug where
    // %3 ended up serving both contexts.
    expect(contains(src,
               "<line x1='2' y1='2' x2='8' y2='8' stroke='%8'"),
           "I2/hover-svg-line1-textPrimary-stroke");
    expect(contains(src, "background-color: %7;"),
           "I2/hover-keeps-ansi-red-bg");

    // I-regression-1321: explicitly forbid the old buggy shape. If
    // an SVG stroke reuses %3 or %6, the URL-encoded `%23` would be
    // spliced into the CSS-context arg position too, breaking every
    // `color: %3;` / `color: %6;` rule. "Could not parse application
    // stylesheet" was the user-visible signal pre-1321.
    {
        std::regex svgUsesCssTextPrimary(
            R"(stroke=\\?'%3\\?')");
        expect(!std::regex_search(src, svgUsesCssTextPrimary),
               "I-regression-1321/svg-must-not-use-css-percent-3");
        std::regex svgUsesCssTextSecondary(
            R"(stroke=\\?'%6\\?')");
        expect(!std::regex_search(src, svgUsesCssTextSecondary),
               "I-regression-1321/svg-must-not-use-css-percent-6");
    }

    // I3 — URL-encoded `%23` injected via the arg list, not the
    // format string. Two `QStringLiteral("%23") + ` splices: one
    // for textPrimary (used by hover %3 stroke), one for
    // textSecondary (used by default %6 stroke).
    {
        std::regex argSplice(
            R"(QStringLiteral\("%23"\)\s*\+\s*theme\.textPrimary\.name\(\)\.mid\(1\))");
        expect(std::regex_search(src, argSplice),
               "I3/textPrimary-arg-splice-with-encoded-hash");
    }
    {
        std::regex argSplice(
            R"(QStringLiteral\("%23"\)\s*\+\s*theme\.textSecondary\.name\(\)\.mid\(1\))");
        expect(std::regex_search(src, argSplice),
               "I3/textSecondary-arg-splice-with-encoded-hash");
    }

    // I4 — image rules in BOTH state rules. Count the `image: url(`
    // occurrences inside the close-button block; expect ≥ 2.
    int imageCount = 0;
    size_t pos = 0;
    while ((pos = src.find("image: url(\\\"data:image/svg+xml", pos))
           != std::string::npos) {
        ++imageCount;
        pos += 30;
    }
    expect(imageCount >= 2,
           "I4/two-image-rules-default-and-hover",
           "got " + std::to_string(imageCount));

    ASSERT_EQ(0, expect_finish());
    std::fprintf(stderr, "\nall invariants hold\n");
    return;
}
