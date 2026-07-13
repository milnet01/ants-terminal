// Feature-conformance test for spec.md — ANTS-1330.
//
// Source-grep test: the two floating prompt-jump chips (previous/next OSC
// 133 prompt) are structural additions to TerminalWidget. MainWindow is too
// heavy to instantiate here, so we assert the chips' shape in
// src/terminalwidget.cpp via the shared srcgrep.h helpers. Every grep is
// region-scoped — a function body, or one chip's construction block — so the
// literals this feature reuses from existing code (navigatePrompt in the
// keyboard handler, width()-52 in the scroll-to-bottom updater) can't
// false-pass a whole-file match.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <QString>

#include <cstddef>
#include <cstdio>
#include <string>
#include <gtest/gtest.h>

#ifndef SRC_TERMINALWIDGET_PATH
#  error "SRC_TERMINALWIDGET_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

using ants_test::countOccurrences;
using ants_test::slurpFile;
using ants_test::slurpFunctionBody;
using ants_test::squashWhitespace;

bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// One chip's construction block: from `<member> = new QPushButton` up to the
// next `= new QPushButton` in the file (or EOF). Sound, not always tight —
// each chip's asserted literals are unique to its own wiring, so a window
// that bleeds into a later unrelated construction can't false-pass.
std::string blockAround(const std::string &src, const std::string &member) {
    const std::string anchor = member + " = new QPushButton";
    const std::size_t start = src.find(anchor);
    if (start == std::string::npos) return {};
    const std::size_t next =
        src.find("= new QPushButton", start + anchor.size());
    return src.substr(start, next == std::string::npos
                                 ? std::string::npos
                                 : next - start);
}

// The body of one `QPushButton#<id> { ... }` selector within the styler:
// from the header to its first closing `}`.
std::string selectorBlock(const std::string &styler, const std::string &id) {
    const std::string header = "QPushButton#" + id + " {";
    const std::size_t start = styler.find(header);
    if (start == std::string::npos) return {};
    const std::size_t end = styler.find('}', start);
    return styler.substr(start, end == std::string::npos
                                    ? std::string::npos
                                    : end - start + 1);
}

}  // namespace

TEST(PromptJumpChip, Main) {
    expect_reset();
    const std::string src = slurpFile(SRC_TERMINALWIDGET_PATH);
    ASSERT_FALSE(src.empty()) << "cannot read terminalwidget.cpp";

    const std::string prevBlk = blockAround(src, "m_promptPrevBtn");
    const std::string nextBlk = blockAround(src, "m_promptNextBtn");
    expect(!prevBlk.empty(), "block/prev-located");
    expect(!nextBlk.empty(), "block/next-located");

    // INV-6 — each chip constructed exactly once.
    expect(countOccurrences(src, "m_promptPrevBtn = new QPushButton") == 1,
           "INV-6/prev-constructed-once");
    expect(countOccurrences(src, "m_promptNextBtn = new QPushButton") == 1,
           "INV-6/next-constructed-once");

    // INV-1a — objectNames set (anchors for the ID-scoped stylesheet).
    expect(has(src, "setObjectName(\"promptPrevBtn\")"),
           "INV-1/prev-objectName");
    expect(has(src, "setObjectName(\"promptNextBtn\")"),
           "INV-1/next-objectName");

    // INV-1b — each chip's ID-scoped selector block carries the structural
    // reset (padding:0 / min-width / max-width) INSIDE the block, not just
    // the header. An empty block would else pass, since the reset literals
    // also exist for the scroll-to-bottom chip.
    const std::string styler =
        slurpFunctionBody(src, "TerminalWidget::styleScrollToBottomButton");
    expect(!styler.empty(), "INV-1/styler-body-located");
    for (const char *id : {"promptPrevBtn", "promptNextBtn"}) {
        const std::string blk = squashWhitespace(selectorBlock(styler, id));
        const std::string tag = std::string("INV-1/reset-in-") + id;
        expect(has(blk, "padding: 0;") && has(blk, "min-width: 32px;") &&
                   has(blk, "max-width: 32px;"),
               tag.c_str(),
               std::string("selector block for #") + id +
                   " must carry the padding/min/max reset (ANTS-1326)");
    }

    // INV-2 — click reuses navigatePrompt; block-scoped so it isn't the
    // keyboard handler's identical literal at :1875/:1879.
    expect(has(prevBlk, "navigatePrompt(-1)"), "INV-2/prev-navigates-back");
    expect(has(nextBlk, "navigatePrompt(1)"), "INV-2/next-navigates-forward");

    // INV-3 — visibility gated on prompts existing; both chips hidden in the
    // not-shown path.
    const std::string updater =
        slurpFunctionBody(src, "TerminalWidget::updateScrollToBottomButton");
    expect(!updater.empty(), "INV-3/updater-body-located");
    expect(has(updater, "promptRegions().empty()"), "INV-3/prompt-gate");
    expect(has(updater, "m_promptPrevBtn->hide()") &&
               has(updater, "m_promptNextBtn->hide()"),
           "INV-3/both-hide-in-updater");

    // INV-4 — reuse-based positioning relative to the shared local x/y (so
    // the search-bar shift baked into y is inherited). Whitespace-normalised
    // so a reformat of the move() call can't false-fail.
    const std::string sqUpdater = squashWhitespace(updater);
    expect(has(sqUpdater, "move(x, y - 40)"), "INV-4/next-stride");
    expect(has(sqUpdater, "move(x, y - 80)"), "INV-4/prev-stride");

    // INV-5 — hidden at construction.
    expect(has(prevBlk, "->hide()"), "INV-5/prev-hidden-at-construction");
    expect(has(nextBlk, "->hide()"), "INV-5/next-hidden-at-construction");

    // INV-7 — glyph (literal UTF-8, see spec.md) + tooltip text.
    expect(has(prevBlk, "↑") && has(prevBlk, "setToolTip(") &&
               has(prevBlk, "previous prompt"),
           "INV-7/prev-glyph-and-tooltip");
    expect(has(nextBlk, "↓") && has(nextBlk, "setToolTip(") &&
               has(nextBlk, "next prompt"),
           "INV-7/next-glyph-and-tooltip");

    ASSERT_EQ(0, expect_finish());
    std::fprintf(stderr, "\nall prompt-jump-chip invariants hold\n");
}
