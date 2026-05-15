// Feature-conformance test for spec.md — ANTS-1326.
//
// Source-grep test: enforces that TerminalWidget's scroll-to-bottom
// chip carries its own ID-scoped stylesheet with explicit padding /
// min-width / max-width overrides, so the app-wide QPushButton
// cascade can't inflate the chip's painted size and clip it past
// the widget's right edge.

#include "../../_support/expect.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <gtest/gtest.h>

#ifndef SRC_TERMINALWIDGET_PATH
#  error "SRC_TERMINALWIDGET_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {



std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

TEST(ScrollToBottomChipSize, Main) {
    expect_reset();
    const std::string src = slurp(SRC_TERMINALWIDGET_PATH);

    // I-1: setObjectName binds the chip with the ID the stylesheet
    // selectors will rely on.
    expect(contains(src, "m_scrollToBottomBtn->setObjectName(\"scrollToBottomBtn\")"),
           "I-1/objectName-set");

    // I-2: ID-scoped selector. A bare `QPushButton {` inside the
    // chip's stylesheet would inherit from the app-wide cascade.
    expect(contains(src, "QPushButton#scrollToBottomBtn {"),
           "I-2/id-scoped-selector-present");
    expect(contains(src, "QPushButton#scrollToBottomBtn:hover {"),
           "I-2/id-scoped-hover-selector-present");

    // I-3: explicit padding: 0 override defeats the app rule's
    // `padding: 6px 14px;`.
    expect(contains(src, " padding: 0;"),
           "I-3/padding-zero-override-present");

    // I-3: explicit min-width: 32px override defeats the app rule's
    // `min-width: 60px;`.
    expect(contains(src, " min-width: 32px;"),
           "I-3/min-width-32-override-present");

    // I-4: max-width / min-height / max-height for full size pinning.
    expect(contains(src, " max-width: 32px;"),
           "I-4/max-width-32-override-present");
    expect(contains(src, " min-height: 32px;"),
           "I-4/min-height-32-override-present");
    expect(contains(src, " max-height: 32px;"),
           "I-4/max-height-32-override-present");

    EXPECT_EQ(0, expect_failures());
}
