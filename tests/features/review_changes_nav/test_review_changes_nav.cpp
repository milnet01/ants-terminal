// Review Changes in-dialog navigation invariants. Source-grep harness.
// See spec.md for the contract (INV-1 through INV-9):
//   * clickable filenames jump to the file's diff (ANTS-1965 / ANTS-1966)
//   * a back-to-top button appears once the user scrolls (ANTS-1967)
//
// Source-grep style (same as test_review_changes_scroll_preserve.cpp) —
// the wiring is a pure-source contract; no MainWindow / display needed.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_DIFFVIEWER_CPP_PATH
#error "SRC_DIFFVIEWER_CPP_PATH compile definition required"
#endif


TEST(ReviewChangesNav, Main) {
    // The whole file is in scope — fileAnchorId() is a file-scope helper
    // outside diffviewer::show, and the back-to-top wiring + anchor
    // rendering all live inside the single show() function anyway.
    const std::string src = ants_test::slurpFile(SRC_DIFFVIEWER_CPP_PATH);

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };
    auto has = [&](const std::string &needle) {
        return src.find(needle) != std::string::npos;
    };
    auto hasRe = [&](const char *pattern) {
        return std::regex_search(src, std::regex(pattern));
    };

    // INV-1 — viewer is a QTextBrowser (anchor navigation + anchorClicked
    // require it; a bare QTextEdit cannot route internal-anchor clicks).
    if (!hasRe(R"(new\s+QTextBrowser\s*\()")) {
        fail("INV-1: viewer must be constructed as `new QTextBrowser(...)` "
             "— QTextEdit has no anchorClicked signal and cannot drive "
             "in-document #fragment navigation");
    }

    // INV-2 — internal links must not escape to an external handler.
    if (!hasRe(R"(setOpenLinks\s*\(\s*false\s*\))")) {
        fail("INV-2: viewer must call setOpenLinks(false) so a `#fragment` "
             "click stays in-document and is never routed to an external "
             "browser / file handler");
    }

    // INV-3 — anchorClicked is connected and the handler scrolls to the
    // clicked anchor. Both tokens must be present (the connect target and
    // the actual jump).
    if (!has("anchorClicked")) {
        fail("INV-3: viewer's anchorClicked signal must be connected — it "
             "is what fires when a file link is clicked");
    }
    if (!has("scrollToAnchor(")) {
        fail("INV-3: the anchorClicked handler must call scrollToAnchor(...) "
             "— the actual in-document jump to the file's hunk");
    }

    // INV-4 — a fileAnchorId(...) helper exists and is the single source of
    // both the href ids and the name= targets (so they always match).
    if (!hasRe(R"(fileAnchorId\s*\()")) {
        fail("INV-4: a `fileAnchorId(path)` helper must exist and be used "
             "to derive both link and target anchor ids identically");
    }

    // INV-5 — target anchors (`<a name=...>`) are emitted, built from
    // fileAnchorId. Accept single- or double-quoted HTML attr forms.
    if (!hasRe(R"(<a name=)")) {
        fail("INV-5: the render must emit `<a name=...>` target anchors at "
             "each file's diff hunk so links have somewhere to jump to");
    }

    // INV-6 — fragment links (`href='#...'`) are emitted for the Status /
    // diffstat file lists. The literal in source is an href to a '#' id.
    if (!hasRe(R"(href='#)") && !hasRe(R"(href=\\\"#)")) {
        fail("INV-6: the render must emit `href='#...'` fragment links "
             "wrapping the file paths in the Status list and the Diff "
             "--stat file list (ANTS-1965 / ANTS-1966)");
    }

    // INV-7 — a back-to-top button exists with the agreed object name.
    if (!has("reviewBackToTopBtn")) {
        fail("INV-7: a back-to-top QPushButton with objectName "
             "\"reviewBackToTopBtn\" must be created (ANTS-1967)");
    }

    // INV-8 — its visibility is driven by the viewer's vertical scroll bar.
    // valueChanged on the scroll bar is the show/hide trigger.
    if (!has("verticalScrollBar()")) {
        fail("INV-8: back-to-top visibility must be wired to the viewer's "
             "verticalScrollBar() position");
    }
    if (!has("valueChanged")) {
        fail("INV-8: the vertical scroll bar's valueChanged signal must be "
             "connected so the button shows on scroll and hides at the top");
    }

    // INV-9 — clicking back-to-top drives the scroll bar to the document
    // top. setValue(0) is the canonical form (scrollToAnchor is excluded
    // here because INV-3 already guarantees it appears for the file-jump
    // path, so allowing it would make this invariant always-true).
    if (!hasRe(R"(setValue\s*\(\s*0\s*\))")) {
        fail("INV-9: the back-to-top click handler must scroll to the top "
             "via verticalScrollBar()->setValue(0)");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see spec.md for context\n", failures);
        FAIL();
    }
    std::printf("OK: review-changes in-dialog navigation invariants present\n");
}
