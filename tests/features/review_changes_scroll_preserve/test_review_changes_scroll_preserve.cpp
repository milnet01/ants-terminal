// Review Changes scroll-preservation invariants. Source-grep harness.
// See spec.md for the contract (INV-1 through INV-6).

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Extract the body of MainWindow::showDiffViewer so each grep below
// can be scoped to that function — the ctor and other methods also
// touch QScrollBar / setHtml elsewhere in mainwindow.cpp and would
// cause false positives. The function ends at the matching closing
// brace; we approximate by capturing from the signature up to the
// next top-level `void MainWindow::` definition.
static std::string showDiffViewerBody(const std::string &mw) {
    const std::string sig = "void MainWindow::showDiffViewer()";
    const auto start = mw.find(sig);
    if (start == std::string::npos) return {};
    // Heuristic end: next `void MainWindow::` after the signature.
    const auto next = mw.find("\nvoid MainWindow::", start + sig.size());
    return mw.substr(start, next == std::string::npos ? std::string::npos
                                                       : next - start);
}

int main() {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    const std::string body = showDiffViewerBody(mw);
    if (body.empty()) {
        std::fprintf(stderr, "FAIL: cannot locate MainWindow::showDiffViewer\n");
        return 1;
    }

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: lastHtml shared cache declared with std::make_shared<QString>.
    std::regex lastHtmlDecl(
        R"(auto\s+lastHtml\s*=\s*std::make_shared\s*<\s*QString\s*>\s*\(\s*\))");
    if (!std::regex_search(body, lastHtmlDecl)) {
        fail("INV-1: showDiffViewer must declare "
             "`auto lastHtml = std::make_shared<QString>()` — a static or "
             "per-call local would not survive across runProbes invocations");
    }

    // INV-2: lastHtml captured by BOTH runProbes and finalize lambdas.
    // Match the capture clauses by name. There are exactly two lambdas in
    // showDiffViewer that need this: the outer runProbes and the inner
    // finalize.
    std::regex runProbesCap(
        R"(auto\s+runProbes\s*=\s*\[[^\]]*\blastHtml\b[^\]]*\])");
    if (!std::regex_search(body, runProbesCap)) {
        fail("INV-2: runProbes lambda capture list must include lastHtml — "
             "without it, finalize sees an empty cache on every refresh and "
             "the skip-identical guard never fires");
    }
    std::regex finalizeCap(
        R"(auto\s+finalize\s*=\s*\[[^\]]*\blastHtml\b[^\]]*\])");
    if (!std::regex_search(body, finalizeCap)) {
        fail("INV-2: finalize lambda capture list must include lastHtml");
    }

    // INV-3: skip-identical guard with early return.
    std::regex skipGuard(
        R"(if\s*\(\s*\*lastHtml\s*==\s*html\s*\)\s*\{\s*return\s*;)");
    if (!std::regex_search(body, skipGuard)) {
        fail("INV-3: finalize must `if (*lastHtml == html) return;` — "
             "without the early-return setHtml runs on every probe completion "
             "and the scroll bar snaps to the top regardless of whether "
             "anything changed");
    }

    // INV-4: scroll-bar value captured BEFORE setHtml. Accept either
    // the direct form `viewerGuard->verticalScrollBar()->value()` or
    // a local-aliased form (`QScrollBar *vbar = ...; vbar->value()`).
    // Order check anchors on whichever is present.
    auto firstOf = [](const std::string &haystack,
                      std::initializer_list<const char *> needles)
        -> std::string::size_type {
        std::string::size_type best = std::string::npos;
        for (const char *n : needles) {
            const auto pos = haystack.find(n);
            if (pos != std::string::npos && (best == std::string::npos || pos < best))
                best = pos;
        }
        return best;
    };
    const auto vbarValuePos = firstOf(body,
        {"verticalScrollBar()->value()", "vbar->value()"});
    const auto setHtmlPos   = body.find("viewerGuard->setHtml(");
    if (vbarValuePos == std::string::npos) {
        fail("INV-4: finalize must read the vertical scroll-bar value "
             "(`verticalScrollBar()->value()` or via a `vbar->value()` alias) "
             "to capture the pre-setHtml scroll position");
    } else if (setHtmlPos == std::string::npos) {
        fail("INV-4: cannot locate `viewerGuard->setHtml(` anchor");
    } else if (vbarValuePos >= setHtmlPos) {
        fail("INV-4: vertical scroll-bar `value()` read must appear BEFORE "
             "`viewerGuard->setHtml(`. Capturing the value after setHtml "
             "would read the post-reset value (always 0) and the restore "
             "would be a no-op");
    }

    // INV-5: scroll-bar restore call AFTER setHtml, with clamp to maximum().
    // The restore call is `vbar->setValue(std::min(vPos, vbar->maximum()))`
    // (or equivalent). Match on the structural shape, not exact spacing.
    std::regex restoreCall(
        R"(vbar->setValue\s*\(\s*std::min\s*\(\s*vPos\s*,\s*vbar->maximum\s*\(\s*\)\s*\)\s*\))");
    if (!std::regex_search(body, restoreCall)) {
        fail("INV-5: finalize must restore the vertical scroll bar via "
             "`vbar->setValue(std::min(vPos, vbar->maximum()))` after "
             "setHtml. Without the std::min clamp, a longer-then-shorter "
             "content sequence over-scrolls past the new document end");
    }
    // Order: restore must come AFTER setHtml.
    const auto restorePos = [&]() -> std::string::size_type {
        std::smatch m;
        if (std::regex_search(body, m, restoreCall)) {
            return static_cast<std::string::size_type>(m.position(0));
        }
        return std::string::npos;
    }();
    if (restorePos != std::string::npos && setHtmlPos != std::string::npos
            && restorePos <= setHtmlPos) {
        fail("INV-5: scroll-bar restore must come AFTER setHtml — "
             "calling setValue before setHtml is a no-op (setHtml resets "
             "the bar to 0 after)");
    }

    // INV-6: first-render carve-out gated on `!isFirstRender`.
    if (body.find("isFirstRender") == std::string::npos) {
        fail("INV-6: finalize must declare an `isFirstRender` flag "
             "(sampled from `lastHtml->isEmpty()` before the cache update) "
             "and gate the scroll-restore on `!isFirstRender`. Without "
             "this, the very first render restores a scroll value from "
             "whatever leftover state the QTextEdit had");
    }
    std::regex isFirstRenderInit(
        R"(isFirstRender\s*=\s*lastHtml->isEmpty\s*\(\s*\))");
    if (!std::regex_search(body, isFirstRenderInit)) {
        fail("INV-6: `isFirstRender` must be initialized from "
             "`lastHtml->isEmpty()` BEFORE `*lastHtml = html` updates the "
             "cache, otherwise it's always false");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: review-changes scroll-preservation invariants present\n");
    return 0;
}
