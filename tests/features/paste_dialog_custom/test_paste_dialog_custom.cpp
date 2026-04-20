// Paste-confirmation dialog — async (Review-Changes-shape) source-grep
// regression test. See spec.md for the full contract.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_TERMINALWIDGET_PATH
#error "SRC_TERMINALWIDGET_PATH compile definition required"
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

// Balanced-brace extractor — see focus_redirect_menu_guard for rationale.
static std::string extractBlockAfter(const std::string &src,
                                     const std::string &needle) {
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
    const std::string src = slurp(SRC_TERMINALWIDGET_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    const std::string body = extractBlockAfter(
        src, "TerminalWidget::pasteToTerminal(const QByteArray");
    if (body.empty()) {
        fail("pasteToTerminal() body not found — signature changed?");
        return 1;
    }

    // INV-1: a QDialog is heap-allocated with `new`, not a stack QDialog.
    // Async lifetime requires heap + WA_DeleteOnClose — a stack dialog
    // would destruct when pasteToTerminal() returns.
    std::regex dialogCtor(R"(new\s+QDialog\s*\(\s*this\s*\))");
    if (!std::regex_search(body, dialogCtor)) {
        fail("INV-1: pasteToTerminal must construct `new QDialog(this)` on the heap "
             "— a stack QDialog destructs when the function returns, orphaning the "
             "user's pending paste decision");
    }

    // INV-2: WA_DeleteOnClose — required for heap-allocated async dialog.
    if (body.find("WA_DeleteOnClose") == std::string::npos) {
        fail("INV-2: Qt::WA_DeleteOnClose must be set on the heap-allocated dialog "
             "or it will leak on every risky paste");
    }

    // INV-3: two explicit QPushButtons (Cancel + Paste) with setDefault
    // and setAutoDefault wired for safety (Cancel is default on Enter).
    std::regex cancelDefault(R"(cancelBtn->setDefault\s*\(\s*true\s*\))");
    std::regex pasteNotDefault(R"(pasteBtn->setAutoDefault\s*\(\s*false\s*\))");
    if (!std::regex_search(body, cancelDefault)) {
        fail("INV-3: Cancel button must be setDefault(true) — Enter must not "
             "dangerously accept");
    }
    if (!std::regex_search(body, pasteNotDefault)) {
        fail("INV-3: Paste button must be setAutoDefault(false) — otherwise "
             "Enter can dangerously accept via auto-default propagation");
    }

    // INV-4: `accepted` signal wired to a lambda that calls performPaste()
    // and then closes the dialog. This is the async paste trigger —
    // there is no blocking exec() or event loop that waits for a bool.
    std::regex pasteClickedLambda(
        R"(connect\s*\(\s*pasteBtn\s*,\s*&QPushButton::clicked[\s\S]{0,400}?performPaste\s*\()");
    if (!std::regex_search(body, pasteClickedLambda)) {
        fail("INV-4: Paste button clicked() must be wired to a lambda that "
             "calls performPaste() — this is the only path by which a "
             "confirmed paste actually reaches the PTY");
    }

    // INV-5: show() + raise() + activateWindow() to land the dialog on
    // top of the frameless main window and claim input focus. Operating
    // on the heap pointer `dlg`, not a stack variable.
    std::regex showRaiseActivate(
        R"(dlg->show\s*\(\s*\)[\s\S]{0,80}?dlg->raise\s*\(\s*\)[\s\S]{0,80}?dlg->activateWindow\s*\(\s*\))");
    if (!std::regex_search(body, showRaiseActivate)) {
        fail("INV-5: pasteToTerminal must call dlg->show() + dlg->raise() + "
             "dlg->activateWindow() in order, matching the Review-Changes pattern");
    }

    // INV-6: explicit setFocus on the Cancel button after activation.
    std::regex setFocusCall(R"(cancelBtn->setFocus\s*\()");
    if (!std::regex_search(body, setFocusCall)) {
        fail("INV-6: explicit cancelBtn->setFocus(...) missing after activation");
    }

    // INV-7 (negative): no QDialog::exec() and no QEventLoop on the
    // async paste path. Either one re-opens the ApplicationModal-vs-
    // frameless-parent click-swallow regression.
    std::regex dlgExec(R"(\bdlg->exec\s*\()");
    if (std::regex_search(body, dlgExec)) {
        fail("INV-7 (neg): pasteToTerminal still calls dlg->exec() — drops "
             "back into the blocking-modal path that swallowed button clicks");
    }
    // Match `QEventLoop <ident>` declarations, not the word in comments.
    std::regex qeventLoopDecl(R"(\bQEventLoop\s+\w+\s*[;{(])");
    if (std::regex_search(body, qeventLoopDecl)) {
        fail("INV-7 (neg): pasteToTerminal must not instantiate a QEventLoop — "
             "the 0.7.4 mid-session attempt had the same click-swallow regression "
             "as QDialog::exec(); the Review-Changes pattern is fully async");
    }

    // INV-8: QPointer guard around `this` inside the Paste-clicked
    // lambda. Without it, closing the tab (and destroying this
    // TerminalWidget) between show() and the user clicking Paste
    // dereferences a dangling pointer.
    std::regex qpointerGuard(R"(QPointer<TerminalWidget>)");
    if (!std::regex_search(body, qpointerGuard)) {
        fail("INV-8: QPointer<TerminalWidget> self-guard missing — closing "
             "the tab while the paste dialog is up would use-after-free");
    }

    // INV-9: performPaste() exists as a separate helper and does the
    // actual PTY write + bracketed-paste wrapping.
    std::regex performPasteDef(
        R"(void\s+TerminalWidget::performPaste\s*\(\s*const\s+QByteArray)");
    if (!std::regex_search(src, performPasteDef)) {
        fail("INV-9: performPaste(const QByteArray&) helper missing");
    }

    // INV-10: confirmDangerousPaste (old synchronous bool API) is gone.
    if (src.find("bool TerminalWidget::confirmDangerousPaste") != std::string::npos) {
        fail("INV-10 (neg): confirmDangerousPaste() is the old synchronous API — "
             "it must be gone from the .cpp to prevent accidental re-use");
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: paste-dialog async-pattern invariants present\n");
    return 0;
}
