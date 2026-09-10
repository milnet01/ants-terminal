// ANTS-5010 — plaintext-prompt warning: AiDialog chat surface (INV-1) +
// AuditDialog triage call-site source-grep (INV-2). See
// docs/specs/ANTS-5010-plaintext-prompt-warning.md § 3 INV-2 / INV-4 for
// the full contract; this directory's own spec.md explains why these two
// surfaces live here rather than extending an existing paired test file.
//
// Stub source-first: LlmClient::plaintextPromptWarning currently always
// returns an empty QString (src/llmclient.cpp), so both tests below are
// expected to fail RED — INV-1 because no "System" warning message is ever
// appended, INV-2 because none of the three AuditDialog call sites yet
// calls the predicate at all.
//
// INV-1 AiDialog::sendRequest warns once per new endpoint, still sends.
// INV-2 AuditDialog::onBatchTriageClicked / onDebtTriageClicked /
//       requestAiTriage each call LlmClient::plaintextPromptWarning.

#include "aidialog.h"
#include "llmclient.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QTest>
#include <QTextEdit>

#ifndef SRC_AUDITDIALOG_CPP_PATH
#error "SRC_AUDITDIALOG_CPP_PATH compile definition required"
#endif

namespace {

// Neither the input field, the chat history, nor the Send button has a
// dedicated accessor (same situation review_dialog_base's
// findDispatchButton documents for its own Dispatch button) — locate by
// type / label via findChild, exactly as a screen-reader or a real click
// would.
QPushButton *findButtonByText(QWidget *w, const QString &text) {
    for (QPushButton *b : w->findChildren<QPushButton *>()) {
        if (b->text() == text) return b;
    }
    return nullptr;
}

QString expectedWarning(const QString &host) {
    return QStringLiteral(
        "Not encrypted: this request goes to %1 over plain http, so anyone "
        "on the network path can read it. Use https:// to protect it.")
        .arg(host);
}

}  // namespace

// ANTS-5010 § 3 INV-2 — AiDialog::sendRequest with a keyless remote
// plain-http endpoint appends one "System" chat message carrying
// LlmClient::plaintextPromptWarning's text, and the request is still sent
// (no "Refused" message, Send disabled as the in-flight indicator). A
// second send to the SAME endpoint adds no second copy. An https or
// loopback endpoint adds none.
TEST(PlaintextPromptWarning, INV1_AiDialogWarnsOncePerEndpointStillSends) {
    const QString host = QStringLiteral("192.0.2.1");   // TEST-NET-1 (RFC 5737)
    const QString remoteEndpoint =
        QStringLiteral("http://%1/v1/chat/completions").arg(host);
    const QString warning = expectedWarning(host);

    // --- Case A: keyless remote plain-http — warns, still sends. ---
    {
        AiDialog dlg;
        dlg.setConfig(remoteEndpoint, QString(), QStringLiteral("gpt-4"), 50);

        auto *input = dlg.findChild<QLineEdit *>();
        auto *chat  = dlg.findChild<QTextEdit *>();
        QPushButton *sendBtn = findButtonByText(&dlg, QStringLiteral("Send"));
        ASSERT_NE(input, nullptr);
        ASSERT_NE(chat, nullptr);
        ASSERT_NE(sendBtn, nullptr);
        ASSERT_TRUE(sendBtn->isEnabled()) << "test bug: Send must start enabled";

        input->setText(QStringLiteral("first message"));
        sendBtn->click();

        const QString afterFirst = chat->toPlainText();
        EXPECT_TRUE(afterFirst.contains(warning))
            << "INV-1: a keyless remote plain-http send must append the "
               "warning text verbatim; chat history was:\n"
            << afterFirst.toStdString();
        EXPECT_FALSE(afterFirst.contains(QStringLiteral("Refused"), Qt::CaseInsensitive))
            << "INV-1: the request must still be SENT, not refused, "
               "despite the warning (ANTS-5010 — warn and still send)";
        // Send goes disabled on sendRequest's first line, before it decides
        // to send or refuse, so it proves nothing. The dialog's own client,
        // holding a reply in flight, does.
        auto *client = dlg.findChild<LlmClient *>();
        ASSERT_NE(client, nullptr) << "test bug: AiDialog owns no LlmClient child";
        EXPECT_TRUE(client->busy())
            << "INV-1: the request must still be SENT after the warning: the "
               "dialog's LlmClient must hold a reply in flight";

        // Second send to the SAME endpoint: the button is disabled, but a
        // real Enter keypress in the input field is not gated on it —
        // returnPressed() is wired to onSend() unconditionally. Drive it
        // via QTest::keyClick (the codebase's established key-injection
        // idiom, e.g. tests/features/roadmap_search_keybinds), the same
        // route a real Enter keypress takes.
        input->setText(QStringLiteral("second message, same endpoint"));
        QTest::keyClick(input, Qt::Key_Return);

        const QString afterSecond = chat->toPlainText();
        const auto warningCount = afterSecond.count(warning, Qt::CaseSensitive);
        EXPECT_EQ(warningCount, 1)
            << "INV-1: a second send to the SAME endpoint must add no "
               "second copy of the warning; chat history was:\n"
            << afterSecond.toStdString();
    }

    // --- Case B: https — no warning at all. ---
    {
        AiDialog dlg;
        dlg.setConfig(QStringLiteral("https://%1/v1/chat/completions").arg(host),
                      QString(), QStringLiteral("gpt-4"), 50);
        auto *input = dlg.findChild<QLineEdit *>();
        auto *chat  = dlg.findChild<QTextEdit *>();
        QPushButton *sendBtn = findButtonByText(&dlg, QStringLiteral("Send"));
        ASSERT_NE(input, nullptr);
        ASSERT_NE(chat, nullptr);
        ASSERT_NE(sendBtn, nullptr);

        input->setText(QStringLiteral("https message"));
        sendBtn->click();

        EXPECT_FALSE(chat->toPlainText().contains(QStringLiteral("Not encrypted")))
            << "INV-1: https must never show the plaintext warning; chat "
               "history was:\n" << chat->toPlainText().toStdString();
    }

    // --- Case C: loopback plain-http — no warning at all. Port nothing
    // listens on, per the owning spec's § 6 test-endpoint convention. ---
    {
        AiDialog dlg;
        dlg.setConfig(QStringLiteral("http://127.0.0.1:9/v1/chat/completions"),
                      QString(), QStringLiteral("gpt-4"), 50);
        auto *input = dlg.findChild<QLineEdit *>();
        auto *chat  = dlg.findChild<QTextEdit *>();
        QPushButton *sendBtn = findButtonByText(&dlg, QStringLiteral("Send"));
        ASSERT_NE(input, nullptr);
        ASSERT_NE(chat, nullptr);
        ASSERT_NE(sendBtn, nullptr);

        input->setText(QStringLiteral("loopback message"));
        sendBtn->click();

        EXPECT_FALSE(chat->toPlainText().contains(QStringLiteral("Not encrypted")))
            << "INV-1: a loopback endpoint must never show the plaintext "
               "warning; chat history was:\n"
            << chat->toPlainText().toStdString();
    }
}

// ANTS-5010 § 3 INV-4 — the bodies of AuditDialog::onBatchTriageClicked,
// AuditDialog::onDebtTriageClicked, and AuditDialog::requestAiTriage each
// call LlmClient::plaintextPromptWarning. Source-grep only: none of the
// three is reachable offscreen without a live AI server or a modal
// QMessageBox::question click (docs/specs/ANTS-5010-plaintext-prompt-warning.md
// § 7's own "Partial" note for this invariant).
TEST(PlaintextPromptWarning, INV2_AuditDialogCallSitesCallPredicate) {
    const std::string src = ants_test::slurpFile(SRC_AUDITDIALOG_CPP_PATH);
    ASSERT_FALSE(src.empty()) << "could not read " << SRC_AUDITDIALOG_CPP_PATH;

    const std::string needle = "LlmClient::plaintextPromptWarning(";

    struct Site {
        const char *label;
        std::string signatureAnchor;
    };
    const Site sites[] = {
        { "AuditDialog::onBatchTriageClicked",
          "void AuditDialog::onBatchTriageClicked()" },
        { "AuditDialog::onDebtTriageClicked",
          "void AuditDialog::onDebtTriageClicked()" },
        { "AuditDialog::requestAiTriage",
          "void AuditDialog::requestAiTriage(const QString &dedupKey)" },
    };

    for (const Site &site : sites) {
        const std::string body =
            ants_test::slurpFunctionBody(src, site.signatureAnchor);
        ASSERT_FALSE(body.empty())
            << "INV-2: could not locate the body of " << site.label
            << " (signature anchor changed?)";
        EXPECT_NE(body.find(needle), std::string::npos)
            << "INV-2: " << site.label
            << " must call LlmClient::plaintextPromptWarning and surface "
               "its result in the text it shows the user (ANTS-5010 § 2.4)";
    }
}
