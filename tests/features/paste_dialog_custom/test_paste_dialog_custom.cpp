// Paste-confirmation dialog — async (Review-Changes-shape) source-grep
// regression test. See spec.md for the full contract.

#include <cstdio>
#include <regex>
#include <string>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_TERMINALWIDGET_PATH
#error "SRC_TERMINALWIDGET_PATH compile definition required"
#endif


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

TEST(PasteDialogCustom, Main) {
    const std::string src = ants_test::slurpFile(SRC_TERMINALWIDGET_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    const std::string body = extractBlockAfter(
        src, "TerminalWidget::pasteToTerminal(const QByteArray");
    if (body.empty()) {
        fail("pasteToTerminal() body not found — signature changed?");
        FAIL();
    }
    // ANTS-5029 — the dialog itself moved into showSendConfirmation(), which
    // an unsigned re-run shares. INV-1 to INV-7 read that helper's body;
    // INV-4 and INV-8 still read pasteToTerminal's.
    const std::string dlgBody = extractBlockAfter(
        src, "TerminalWidget::showSendConfirmation(const QString");
    if (dlgBody.empty()) {
        fail("showSendConfirmation() body not found — signature changed?");
        FAIL();
    }

    // INV-1: a QDialog is heap-allocated with `new`, not a stack QDialog.
    // Async lifetime requires heap + WA_DeleteOnClose — a stack dialog
    // would destruct when the function returns.
    std::regex dialogCtor(R"(new\s+QDialog\s*\(\s*this\s*\))");
    if (!std::regex_search(dlgBody, dialogCtor)) {
        fail("INV-1: showSendConfirmation must construct `new QDialog(this)` on "
             "the heap — a stack QDialog destructs when the function returns, "
             "orphaning the user's pending decision");
    }

    // INV-2: WA_DeleteOnClose — required for heap-allocated async dialog.
    if (dlgBody.find("WA_DeleteOnClose") == std::string::npos) {
        fail("INV-2: Qt::WA_DeleteOnClose must be set on the heap-allocated dialog "
             "or it will leak on every risky paste");
    }

    // INV-3: two explicit QPushButtons (Cancel + accept) with setDefault
    // and setAutoDefault wired for safety (Cancel is default on Enter).
    std::regex cancelDefault(R"(cancelBtn->setDefault\s*\(\s*true\s*\))");
    std::regex acceptNotDefault(R"(acceptBtn->setAutoDefault\s*\(\s*false\s*\))");
    if (!std::regex_search(dlgBody, cancelDefault)) {
        fail("INV-3: Cancel button must be setDefault(true) — Enter must not "
             "dangerously accept");
    }
    if (!std::regex_search(dlgBody, acceptNotDefault)) {
        fail("INV-3: the accept button must be setAutoDefault(false) — otherwise "
             "Enter can dangerously accept via auto-default propagation");
    }

    // INV-4: the accept button's clicked() runs the caller's action and then
    // closes the dialog, and pasteToTerminal's action calls performPaste().
    // This is the async paste trigger — there is no blocking exec() or event
    // loop that waits for a bool.
    std::regex acceptClickedLambda(
        R"(connect\s*\(\s*acceptBtn\s*,\s*&QPushButton::clicked[\s\S]{0,200}?onAccept\s*\()");
    if (!std::regex_search(dlgBody, acceptClickedLambda)) {
        fail("INV-4: the accept button's clicked() must run the onAccept action");
    }
    std::regex pasteAction(
        R"(showSendConfirmation\s*\([\s\S]{0,600}?performPaste\s*\()");
    if (!std::regex_search(body, pasteAction)) {
        fail("INV-4: pasteToTerminal's accept action must call performPaste() — "
             "this is the only path by which a confirmed paste reaches the PTY");
    }

    // INV-5: show() + raise() + activateWindow() to land the dialog on
    // top of the frameless main window and claim input focus. Operating
    // on the heap pointer `dlg`, not a stack variable.
    std::regex showRaiseActivate(
        R"(dlg->show\s*\(\s*\)[\s\S]{0,80}?dlg->raise\s*\(\s*\)[\s\S]{0,80}?dlg->activateWindow\s*\(\s*\))");
    if (!std::regex_search(dlgBody, showRaiseActivate)) {
        fail("INV-5: showSendConfirmation must call dlg->show() + dlg->raise() + "
             "dlg->activateWindow() in order, matching the Review-Changes pattern");
    }

    // INV-6: explicit setFocus on the Cancel button after activation.
    std::regex setFocusCall(R"(cancelBtn->setFocus\s*\()");
    if (!std::regex_search(dlgBody, setFocusCall)) {
        fail("INV-6: explicit cancelBtn->setFocus(...) missing after activation");
    }

    // INV-7 (negative): no QDialog::exec() and no QEventLoop on either
    // body. Either one re-opens the ApplicationModal-vs-frameless-parent
    // click-swallow regression.
    std::regex dlgExec(R"(\bdlg->exec\s*\()");
    if (std::regex_search(body, dlgExec) || std::regex_search(dlgBody, dlgExec)) {
        fail("INV-7 (neg): the paste path still calls dlg->exec() — drops "
             "back into the blocking-modal path that swallowed button clicks");
    }
    // Match `QEventLoop <ident>` declarations, not the word in comments.
    std::regex qeventLoopDecl(R"(\bQEventLoop\s+\w+\s*[;{(])");
    if (std::regex_search(body, qeventLoopDecl)
        || std::regex_search(dlgBody, qeventLoopDecl)) {
        fail("INV-7 (neg): the paste path must not instantiate a QEventLoop — "
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
        FAIL();
    }
    std::printf("OK: paste-dialog async-pattern invariants present\n");
    return;
}
