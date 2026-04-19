// Focus-redirect menu/popup guard — source-grep regression test.
// See spec.md for the full contract.
//
// Why source-grep: the behavior is driven by a QApplication::focusChanged
// handler that cannot be exercised headlessly without a full QWidget event
// loop + simulated cursor positions. The invariants the spec cares about
// are "this early-return guard MUST exist", which a regex check on
// mainwindow.cpp locks in cheaply.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_PATH
#error "SRC_MAINWINDOW_PATH compile definition required"
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

// Balanced-brace scanner — see command_mark_gutter/test_command_mark_gutter.cpp
// for the rationale. std::regex's backtracking executor blows the stack on
// mainwindow.cpp (~4000+ lines), so we don't use multiline regex to extract
// function bodies. Linear scan, constant stack.
static std::string extractBlockAfter(const std::string &src, const std::string &needle) {
    auto needlePos = src.find(needle);
    if (needlePos == std::string::npos) return {};
    auto bracePos = src.find('{', needlePos);
    if (bracePos == std::string::npos) return {};
    int depth = 1;
    size_t i = bracePos + 1;
    for (; i < src.size() && depth > 0; ++i) {
        char c = src[i];
        if (c == '{') ++depth;
        else if (c == '}') --depth;
    }
    if (depth != 0) return {};
    return src.substr(bracePos, i - bracePos);
}

int main() {
    const std::string mw = slurp(SRC_MAINWINDOW_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // Extract the outer focusChanged lambda body. The signature is the
    // connect() call, and extractBlockAfter scans forward to the first
    // '{' (the lambda body's opening brace) and counts balanced braces
    // until the matching close — which is the lambda's closing brace,
    // just before `});` terminates the connect() call. The returned
    // block therefore contains the entire lambda body INCLUDING the
    // nested QTimer::singleShot lambda.
    const std::string outerBlock = extractBlockAfter(
        mw, "connect(qApp, &QApplication::focusChanged");
    if (outerBlock.empty()) {
        fail("mainwindow.cpp missing focusChanged connect() — the redirect handler "
             "has been removed or renamed; adjust this test if the refactor was intentional");
        return failures ? 1 : 0;
    }

    // INV-1: queue-time activePopupWidget guard. Present in the outer
    // block but BEFORE the nested QTimer::singleShot block.
    auto singleShotPos = outerBlock.find("QTimer::singleShot");
    if (singleShotPos == std::string::npos) {
        fail("focusChanged lambda missing QTimer::singleShot fire-time block");
        return failures ? 1 : 0;
    }
    const std::string queueTimePart = outerBlock.substr(0, singleShotPos);
    const std::string fireTimePart  = outerBlock.substr(singleShotPos);

    if (queueTimePart.find("QApplication::activePopupWidget()") == std::string::npos) {
        fail("INV-1: queue-time focusChanged handler must early-return on "
             "QApplication::activePopupWidget() — removing this re-opens the "
             "menubar hover-flash reported 2026-04-19");
    }

    // INV-2: queue-time menu-ancestor walk under the cursor. We look for
    // the combination: widgetAt(QCursor::pos()) + parentWidget() loop +
    // QMenu/QMenuBar inherits check.
    std::regex menuHoverGuard(
        R"(widgetAt\s*\(\s*QCursor::pos\s*\(\s*\)\s*\)[\s\S]{0,400}?inherits\s*\(\s*\"QMenu(Bar)?\")");
    if (!std::regex_search(queueTimePart, menuHoverGuard)) {
        fail("INV-2: queue-time focusChanged handler must early-return when the "
             "widget under QCursor::pos() has a QMenu/QMenuBar ancestor");
    }

    // INV-3 + INV-4: same guards repeated at fire time inside the
    // QTimer::singleShot lambda body.
    if (fireTimePart.find("QApplication::activePopupWidget()") == std::string::npos) {
        fail("INV-3: fire-time QTimer::singleShot block must re-check "
             "activePopupWidget() — a menu may open between queue and fire");
    }
    if (!std::regex_search(fireTimePart, menuHoverGuard)) {
        fail("INV-4: fire-time QTimer::singleShot block must re-check the "
             "QMenu/QMenuBar-ancestor-under-cursor guard");
    }

    // INV-6: queue-time visible-dialog walk. The queue-time body must
    // walk QApplication::topLevelWidgets() and early-return on any
    // visible QDialog — covers the QMessageBox::exec() modality-
    // handshake window where activeModalWidget() can briefly be null.
    std::regex visibleDialogWalk(
        R"(topLevelWidgets\s*\(\s*\)[\s\S]{0,400}?isVisible[\s\S]{0,400}?inherits\s*\(\s*\"QDialog\")");
    if (!std::regex_search(queueTimePart, visibleDialogWalk)) {
        fail("INV-6: queue-time focusChanged handler must walk topLevelWidgets() "
             "and early-return on any visible QDialog — without this, the paste-"
             "confirmation dialog's Paste button swallows mouse clicks (2026-04-19)");
    }

    // INV-5 — existing guards preserved. Each of these has its own reason-
    // in-comments history; if someone deletes them, the audit/test layer
    // must alert.
    if (outerBlock.find("QApplication::activeModalWidget()") == std::string::npos)
        fail("INV-5: activeModalWidget guard removed — modal-dialog focus will be hijacked");
    if (outerBlock.find("qobject_cast<QAbstractButton") == std::string::npos)
        fail("INV-5: QAbstractButton guard removed — re-opens the 2026-04-18 "
             "Review-Changes click-swallow regression");
    if (outerBlock.find("QApplication::mouseButtons()") == std::string::npos)
        fail("INV-5: mouseButtons-down guard removed — re-opens button-click race");
    if (outerBlock.find("inherits(\"QDialog\")") == std::string::npos)
        fail("INV-5: parent-chain QDialog skip removed — dialog internal focus will be hijacked");
    if (outerBlock.find("CommandPalette") == std::string::npos)
        fail("INV-5: parent-chain CommandPalette skip removed");

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: all focus-redirect menu/popup guards present\n");
    return 0;
}
