// Why this exists: the shaped-run cache is keyed by (text, style variant)
// and not by font, so replacing the QFont behind a variant leaves cached
// layouts shaped with the old family. See spec.md.
//
// Source-grep: proving the visual difference needs a real paint device
// and two measurably different fonts on the host, which is the E2E
// harness's job, not this bundle's.

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_TERMINALWIDGET_PATH
#  error "SRC_TERMINALWIDGET_PATH compile definition required"
#endif

namespace {

std::string memberBody(const std::string &src, const char *qualName) {
    const std::string pat =
        std::string("void\\s+") + qualName + R"(\s*\([^)]*\)[^;{]*\{)";
    std::regex re(pat);
    std::smatch m;
    if (!std::regex_search(src, m, re)) return {};
    const size_t start = static_cast<size_t>(m.position(0)) + m.length(0);
    size_t i = start;
    int depth = 1;
    while (i < src.size() && depth > 0) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') --depth;
        ++i;
    }
    if (depth != 0) return {};
    return src.substr(start, i - start - 1);
}

bool clearsCache(const std::string &body) {
    return std::regex_search(
        body, std::regex(R"(m_shapedRunCache\s*\.\s*clear\s*\()"));
}

}  // namespace

TEST(StyledFontCacheInvalidation, Main) {
    const std::string src = ants_test::slurpFile(SRC_TERMINALWIDGET_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    struct Case { const char *fn; const char *inv; };
    const Case cases[] = {
        {"TerminalWidget::setBoldFontFamily",       "INV-1"},
        {"TerminalWidget::setItalicFontFamily",     "INV-2"},
        {"TerminalWidget::setBoldItalicFontFamily", "INV-3"},
    };

    for (const Case &c : cases) {
        const std::string body = memberBody(src, c.fn);
        if (body.empty()) {
            std::fprintf(stderr, "FAIL: precondition: %s body not found\n", c.fn);
            ++failures;
            continue;
        }
        if (!clearsCache(body)) {
            std::fprintf(stderr,
                "FAIL: %s: %s replaces the QFont behind a cached style "
                "variant without clearing m_shapedRunCache. The cache key "
                "is (text, variant) and carries no font, so every already-"
                "cached run of that style keeps drawing in the old family "
                "until a resize or a font-size change clears it.\n",
                c.inv, c.fn);
            ++failures;
        }
    }

    // INV-4 — the pre-existing clear must stay where it is.
    const std::string metrics = memberBody(src, "TerminalWidget::updateFontMetrics");
    if (metrics.empty()) {
        fail("precondition: TerminalWidget::updateFontMetrics body not found.");
    } else if (!clearsCache(metrics)) {
        fail("INV-4: updateFontMetrics no longer clears the shaped-run "
             "cache. The three per-style setters are an addition to that "
             "guarantee, not a replacement for it.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/styled_font_cache_invalidation/spec.md\n",
            failures);
    }
    ASSERT_EQ(0, failures);
}
