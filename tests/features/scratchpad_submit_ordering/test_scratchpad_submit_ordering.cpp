// Scratchpad submit-ordering — source-grep regression test for ANTS-4456.
// See spec.md for the contract and for why this is a source grep.
//
// sendScratchpad() used to write the trailing Enter as its own statement
// after calling pasteToTerminal(). That call returns immediately when the
// payload trips the confirmation dialog, so the Enter reached the shell
// first — on its own — and the scratchpad text followed with none. Cancel
// did not retract it. A newline is itself a risk reason, so this was the
// scratchpad's normal path.

#include <cstdio>
#include <regex>
#include <string>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_TERMINALWIDGET_PATH
#error "SRC_TERMINALWIDGET_PATH compile definition required"
#endif

// Balanced-brace extractor — same shape as paste_dialog_custom's.
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

TEST(ScratchpadSubmitOrdering, Main) {
    const std::string src = ants_test::slurpFile(SRC_TERMINALWIDGET_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    const std::string send =
        extractBlockAfter(src, "void TerminalWidget::sendScratchpad()");
    if (send.empty()) {
        fail("sendScratchpad() body not found — signature changed?");
        FAIL();
    }

    // INV-1: the caller no longer issues the Enter itself. It cannot
    // sequence it — the risky arm of pasteToTerminal is asynchronous.
    if (send.find(R"(ptyWrite("\r"))") != std::string::npos) {
        fail("INV-1: sendScratchpad still writes the Enter itself. On the "
             "confirmation-dialog arm pasteToTerminal returns before "
             "pasting, so this Enter reaches the shell alone and fires even "
             "when the user cancels");
    }

    // INV-2: it asks pasteToTerminal to submit instead.
    std::regex asksSubmit(R"(pasteToTerminal\s*\([\s\S]{0,90}?true\s*\))");
    if (!std::regex_search(send, asksSubmit)) {
        fail("INV-2: sendScratchpad does not pass the submit-after argument "
             "to pasteToTerminal — the scratchpad text would paste but never "
             "be submitted");
    }

    const std::string paste = extractBlockAfter(
        src, "TerminalWidget::pasteToTerminal(const QByteArray");
    if (paste.empty()) {
        fail("pasteToTerminal() body not found — signature changed?");
        FAIL();
    }

    // INV-3: the accept handler sends the Enter. Because this is the only
    // write, Cancel cannot submit anything. ANTS-5029: the dialog lives in
    // showSendConfirmation(); pasteToTerminal hands it the accept action.
    std::regex acceptSubmits(
        R"(showSendConfirmation\s*\([\s\S]{0,700}?submitAfter[\s\S]{0,200}?ptyWrite)");
    if (!std::regex_search(paste, acceptSubmits)) {
        fail("INV-3: the paste-confirmation accept handler does not send the "
             "Enter — a confirmed scratchpad send would paste without "
             "submitting");
    }

    // INV-4: the synchronous (no-risk) arm sends it too.
    std::regex syncSubmits(
        R"(reasons\.isEmpty\s*\(\)[\s\S]{0,220}?submitAfter[\s\S]{0,80}?ptyWrite)");
    if (!std::regex_search(paste, syncSubmits)) {
        fail("INV-4: the no-risk arm does not send the Enter — an unrisky "
             "scratchpad send would paste without submitting");
    }

    if (failures) FAIL();
}
