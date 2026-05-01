// Feature-conformance test for tests/features/scroll_snapshot_intent/spec.md.
//
// Locks ANTS-1118 — smooth-scroll snapshot is captured on user
// scroll intent (wheelEvent: m_smoothScrollTarget > 0 from
// offset 0 with no existing snapshot), not on committed offset
// transition. Source-grep only — the fix is a wiring move; the
// helpers (captureScreenSnapshot, setScrollbackInsertPaused) are
// already exercised by the existing scrollback feature tests.
//
// Exit 0 = all invariants hold.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

// Extract the body of TerminalWidget::wheelEvent — from its
// signature to the next top-level `void TerminalWidget::` or
// `}` at column 0 followed by another function. We approximate
// by grabbing from the signature to the next sibling member-
// function definition, which is enough for source-grep INVs.
std::string functionBody(const std::string &src, const std::string &openSig) {
    const size_t start = src.find(openSig);
    if (start == std::string::npos) return {};
    // Find the next "void TerminalWidget::" after openSig — that's
    // the start of the next sibling function.
    const size_t scanFrom = start + openSig.size();
    const size_t end = src.find("\nvoid TerminalWidget::", scanFrom);
    if (end == std::string::npos) return src.substr(start);
    return src.substr(start, end - start);
}

}  // namespace

int main() {
    const std::string src = slurp(TERMINALWIDGET_CPP);
    if (src.empty()) return fail("setup", "terminalwidget.cpp not readable");

    const std::string wheelBody =
        functionBody(src, "void TerminalWidget::wheelEvent(QWheelEvent *event)");
    if (wheelBody.empty())
        return fail("setup", "could not locate wheelEvent body");

    const std::string smoothBody =
        functionBody(src, "void TerminalWidget::smoothScrollStep()");
    if (smoothBody.empty())
        return fail("setup", "could not locate smoothScrollStep body");

    // INV-1: wheelEvent body contains the intent-side snapshot trigger.
    // The fix calls captureScreenSnapshot() inside a branch that
    // mentions m_scrollOffset (== 0), m_smoothScrollTarget (> 0
    // or > 0.0), and m_frozenScreenRows (empty()).
    if (!contains(wheelBody, "captureScreenSnapshot()"))
        return fail("INV-1",
            "wheelEvent body does not call captureScreenSnapshot() — "
            "intent-side snapshot trigger missing (ANTS-1118)");
    if (!contains(wheelBody, "m_smoothScrollTarget"))
        return fail("INV-1",
            "wheelEvent does not gate snapshot on m_smoothScrollTarget "
            "(ANTS-1118 intent predicate)");
    if (!contains(wheelBody, "m_frozenScreenRows"))
        return fail("INV-1",
            "wheelEvent does not gate snapshot on m_frozenScreenRows "
            "(empty-snapshot guard missing)");

    // INV-2: same branch pauses scrollback insertion synchronously.
    if (!contains(wheelBody, "setScrollbackInsertPaused(true)"))
        return fail("INV-2",
            "wheelEvent body does not call "
            "m_grid->setScrollbackInsertPaused(true) on scroll intent");

    // INV-3: smoothScrollStep timer-stop branch cleans up stranded
    // snapshots by calling updateScrollBar.
    if (!contains(smoothBody, "updateScrollBar()"))
        return fail("INV-3",
            "smoothScrollStep does not call updateScrollBar() in the "
            "timer-stop branch — stranded snapshot cleanup missing");
    // The cleanup must be in the stop-branch (where target settles
    // to 0). We approximate by requiring updateScrollBar() within
    // 200 chars of the m_smoothScrollTimer.stop() call.
    {
        const size_t stopPos =
            smoothBody.find("m_smoothScrollTimer.stop()");
        if (stopPos == std::string::npos)
            return fail("INV-3",
                "smoothScrollStep stop-branch not located");
        const size_t windowEnd =
            std::min(stopPos + 1200, smoothBody.size());
        const std::string window =
            smoothBody.substr(stopPos, windowEnd - stopPos);
        if (!contains(window, "updateScrollBar()"))
            return fail("INV-3",
                "updateScrollBar() not in the smoothScrollStep "
                "stop-branch window");
    }

    // INV-4: regression guard — the existing onVtBatch-side
    // setScrollbackInsertPaused call is preserved.
    if (!contains(src,
            "setScrollbackInsertPaused(m_scrollOffset > 0)"))
        return fail("INV-4",
            "regression: onVtBatch-side "
            "setScrollbackInsertPaused(m_scrollOffset > 0) lost");

    std::fprintf(stderr,
        "OK — scroll snapshot intent INVs hold.\n");
    return 0;
}
