// MD-1 to MD-3 of spec.md, checked on the dialog buildPromptBox builds
// (ANTS-5480). The rest of spec.md is test_verify_trust_gate.cpp, which
// lives in a bundle with no QApplication.

#include "verifytrustmodal.h"

#include <QByteArray>
#include <QCheckBox>
#include <QMessageBox>

#include <gtest/gtest.h>

TEST(VerifyTrustPrompt, ShowsGatesAndCheckedReprompt) {
    QMessageBox box;
    const VerifyTrust::PromptControls c = VerifyTrust::buildPromptBox(
        box, QStringLiteral("/tmp/proj"), QString(64, QLatin1Char('a')),
        QByteArrayLiteral(R"({"build":{"command":"true"},"tests":{"command":"true"}})"));

    // MD-1 — the prompt names the gates the config would run.
    EXPECT_TRUE(box.informativeText().contains(QStringLiteral("<b>Gates:</b> 2")))
        << "MD-1: " << box.informativeText().toStdString();

    // MD-2 — "Trust this repo" carries a re-prompt checkbox, on by default.
    ASSERT_NE(c.reprompt, nullptr) << "MD-2: no re-prompt checkbox";
    EXPECT_EQ(box.checkBox(), c.reprompt) << "MD-2: checkbox not on the box";
    EXPECT_TRUE(c.reprompt->isChecked()) << "MD-2: checkbox starts unchecked";
    EXPECT_NE(c.trustRepo, nullptr) << "MD-2: no Trust this repo button";
}

// MD-3 (ANTS-6633) — a character that prints nothing is shown as <U+XXXX>
// in the preview and the full config, so the command the user trusts reads
// the same as the bytes that run.
TEST(VerifyTrustPrompt, ShowsHiddenCharactersVisibly) {
    // A raw U+202E RIGHT-TO-LEFT OVERRIDE and U+200B ZERO WIDTH SPACE, and
    // an ESC the JSON writes as \u001b.
    const QByteArray config = QStringLiteral(
        "{\"build\":{\"command\":\"echo ok‮#​x\\u001b\"}}").toUtf8();
    QMessageBox box;
    VerifyTrust::buildPromptBox(box, QStringLiteral("/tmp/proj"),
                                QString(64, QLatin1Char('a')), config);

    for (const QString &text : {box.informativeText(), box.detailedText()}) {
        EXPECT_FALSE(text.contains(QChar(0x202E))) << "MD-3: raw bidi override: "
                                                   << text.toStdString();
        EXPECT_FALSE(text.contains(QChar(0x200B))) << "MD-3: raw zero-width space";
        EXPECT_TRUE(text.contains(QStringLiteral("U+202E"))) << "MD-3: " << text.toStdString();
        EXPECT_TRUE(text.contains(QStringLiteral("U+200B"))) << "MD-3: " << text.toStdString();
    }
    // The parsed first command holds the ESC itself.
    EXPECT_FALSE(box.informativeText().contains(QChar(0x1B))) << "MD-3: raw escape";
    EXPECT_TRUE(box.informativeText().contains(QStringLiteral("U+001B")))
        << "MD-3: " << box.informativeText().toStdString();
}
