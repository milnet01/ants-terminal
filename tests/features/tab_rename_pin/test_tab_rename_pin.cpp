// Right-click "Rename Tab…" — source-grep regression test. See spec.md.
//
// Asserts the rename lambda routes through setTabTitleForRemote() so the
// manual name populates m_tabTitlePins and survives the shell's OSC 0/2
// writes + the 2 s updateTabTitles refresh. Before the fix, the handler
// wrote directly to setTabText and the name was clobbered within seconds
// on any tab whose shell emits title updates (notably Claude Code).

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_CPP
#error "SRC_MAINWINDOW_CPP compile definition required"
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

// Extract the body enclosed by `{ ... }` starting at `openBracePos`
// (which must point at the `{`). Tracks brace nesting, ignores braces
// inside "string literals" and // line comments.
static std::string extractBracedBody(const std::string &src, size_t openBracePos) {
    if (openBracePos >= src.size() || src[openBracePos] != '{') return {};
    int depth = 0;
    bool inStr = false;
    bool inLineComment = false;
    char strQuote = 0;
    for (size_t i = openBracePos; i < src.size(); ++i) {
        char c = src[i];
        char next = (i + 1 < src.size()) ? src[i + 1] : '\0';
        if (inLineComment) {
            if (c == '\n') inLineComment = false;
            continue;
        }
        if (inStr) {
            if (c == '\\' && i + 1 < src.size()) { ++i; continue; }
            if (c == strQuote) inStr = false;
            continue;
        }
        if (c == '/' && next == '/') { inLineComment = true; continue; }
        if (c == '"' || c == '\'') { inStr = true; strQuote = c; continue; }
        if (c == '{') { ++depth; continue; }
        if (c == '}') {
            --depth;
            if (depth == 0) {
                return src.substr(openBracePos, i - openBracePos + 1);
            }
        }
    }
    return {};
}

int main() {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP);

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-4 (structural): the rename action exists and opens a QLineEdit
    // on editingFinished. Stated first so later invariants can anchor to
    // the same handler.
    const std::string renameActionMarker = "\"Rename Tab...\"";
    size_t renameMarkerPos = mw.find(renameActionMarker);
    if (renameMarkerPos == std::string::npos) {
        fail("INV-4a: context-menu action \"Rename Tab...\" missing — if it "
             "moved to a different wording, update this test + spec together");
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }

    // Locate the editingFinished connect within the following ~3 KiB.
    // That region covers the whole rename-action body in the current source.
    const size_t windowLen = 3000;
    const size_t windowEnd = std::min(mw.size(), renameMarkerPos + windowLen);
    const std::string window = mw.substr(renameMarkerPos, windowEnd - renameMarkerPos);

    size_t editingFinishedPos = window.find("editingFinished");
    if (editingFinishedPos == std::string::npos) {
        fail("INV-4b: rename handler must connect a QLineEdit's editingFinished "
             "signal — structural guard against a refactor that drops the "
             "inline-edit UX");
    }

    // Locate the editingFinished lambda body. Pattern: `editingFinished,
    // this, [...](...) { ... }`. We scan forward from the signal name to
    // the first `{` that isn't part of a capture list `[...]`.
    std::string lambdaBody;
    if (editingFinishedPos != std::string::npos) {
        // Advance to the next `[` (capture list), then scan for the `{`
        // that opens the lambda body (after the parameter list and
        // optional trailing return type).
        size_t p = renameMarkerPos + editingFinishedPos;
        size_t capStart = mw.find('[', p);
        if (capStart == std::string::npos) {
            fail("INV-4c: editingFinished connect must supply a capturing lambda");
        } else {
            // Find the `{` after the `)` that closes the lambda's parameter
            // list. `]` closes the capture, `(` opens params, `)` closes
            // params, `{` opens body. Scan linearly; bail if we overrun.
            size_t closeCap = mw.find(']', capStart);
            size_t openParams = (closeCap != std::string::npos) ? mw.find('(', closeCap) : std::string::npos;
            size_t closeParams = (openParams != std::string::npos) ? mw.find(')', openParams) : std::string::npos;
            size_t openBody = (closeParams != std::string::npos) ? mw.find('{', closeParams) : std::string::npos;
            if (openBody == std::string::npos || openBody > renameMarkerPos + windowLen) {
                fail("INV-4d: could not locate the editingFinished lambda body "
                     "within the expected window — source shape has changed, "
                     "update the parser");
            } else {
                lambdaBody = extractBracedBody(mw, openBody);
                if (lambdaBody.empty()) {
                    fail("INV-4e: editingFinished lambda body brace matching "
                         "failed — source is malformed or this parser needs fixing");
                }
            }
        }
    }

    if (!lambdaBody.empty()) {
        // INV-1: lambda routes through setTabTitleForRemote.
        if (lambdaBody.find("setTabTitleForRemote(") == std::string::npos) {
            fail("INV-1: rename handler lambda must call setTabTitleForRemote(…) "
                 "so the manual name populates m_tabTitlePins. A bare "
                 "m_tabWidget->setTabText(…) bypasses the pin map — the shell's "
                 "next OSC 0/2 (every ~3 s under Claude Code) wipes the name");
        }

        // INV-1 (negative): lambda must NOT write directly to setTabText.
        if (lambdaBody.find("m_tabWidget->setTabText(") != std::string::npos) {
            fail("INV-1b: rename handler lambda must NOT call "
                 "m_tabWidget->setTabText(…) directly — that path skips the "
                 "pin-map population and regresses the fix");
        }

        // INV-2: the setTabTitleForRemote call is NOT wrapped in a
        // `!newName.isEmpty()` guard. Empty string carries the clear-pin
        // semantics — guarding it out removes the user's only in-UI
        // un-rename path.
        //
        // We check by pairing: if "setTabTitleForRemote(" appears AND
        // "!newName.isEmpty()" also appears inside the same lambda body,
        // the guard is likely wrapping the call. This is a heuristic —
        // a reviewer adding an unrelated empty-check for a different
        // field would false-positive, but no such field exists today
        // and the spec locks this shape.
        bool hasCall   = lambdaBody.find("setTabTitleForRemote(") != std::string::npos;
        bool hasGuard  = lambdaBody.find("!newName.isEmpty()") != std::string::npos ||
                         lambdaBody.find("! newName.isEmpty()") != std::string::npos ||
                         lambdaBody.find("newName.isEmpty() == false") != std::string::npos;
        if (hasCall && hasGuard) {
            fail("INV-2: rename handler must NOT guard setTabTitleForRemote() "
                 "behind a !newName.isEmpty() check — empty string is the "
                 "clear-pin signal and is the only in-UI way to un-rename a tab");
        }
    }

    // INV-3a: the titleChanged lambda's pin guard still exists. This is
    // the consumer-side half of the contract — if this dies, the fix
    // becomes a no-op because the shell's OSC 0/2 will stomp pinned
    // labels unconditionally.
    std::regex titleChangedPinGuard(
        R"(titleChanged[\s\S]{0,400}?m_tabTitlePins\.contains\s*\()");
    if (!std::regex_search(mw, titleChangedPinGuard)) {
        fail("INV-3a: TerminalWidget::titleChanged handler must consult "
             "m_tabTitlePins.contains(...) before writing the tab label — "
             "consumer-side half of the contract; without it pins are wiped "
             "on every OSC 0/2");
    }

    // INV-3b: updateTabTitles also consults the pin map.
    size_t uttPos = mw.find("void MainWindow::updateTabTitles");
    if (uttPos == std::string::npos) {
        fail("INV-3b: MainWindow::updateTabTitles definition missing");
    } else {
        // 2000 bytes covers the whole function body today.
        std::string body = mw.substr(uttPos, 2000);
        if (body.find("m_tabTitlePins.contains") == std::string::npos) {
            fail("INV-3c: updateTabTitles must skip pinned tabs via "
                 "m_tabTitlePins.contains(...) — without it the 2 s refresh "
                 "tick wipes pins between OSC 0/2 writes");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: tab_rename_pin invariants present\n");
    return 0;
}
