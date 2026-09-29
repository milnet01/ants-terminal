// MD-1 / MD-2 of spec.md, checked on the dialog buildPromptBox builds
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
