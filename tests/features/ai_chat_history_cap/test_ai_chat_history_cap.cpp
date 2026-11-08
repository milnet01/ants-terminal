// Feature-conformance test for tests/features/ai_chat_history_cap/spec.md.
//
// Locks ANTS-5200: the AI Assistant's chat view drops its oldest messages
// past AiDialog::kMaxHistoryChars, so a long session does not hold every
// message until the app quits.

#include "aidialog.h"

#include <QTextBlock>
#include <QTextDocument>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {

// One block per message, as appendMessage's <p> produces.
void fill(QTextDocument &doc, int blocks, int charsPerBlock) {
    QString text;
    for (int i = 0; i < blocks; ++i) {
        if (i) text += QLatin1Char('\n');
        text += QStringLiteral("m%1:").arg(i, 4, 10, QLatin1Char('0'));
        text += QString(charsPerBlock, QLatin1Char('x'));
    }
    doc.setPlainText(text);
}

}  // namespace

TEST(AiChatHistoryCap, Inv1OverTheCapIsTrimmedToIt) {
    QTextDocument doc;
    fill(doc, 100, 100);
    ASSERT_GT(doc.characterCount(), 2000);
    AiDialog::trimHistory(&doc, 2000);
    EXPECT_LE(doc.characterCount(), 2000);
}

TEST(AiChatHistoryCap, Inv2NewestSurvivesOldestGoes) {
    QTextDocument doc;
    fill(doc, 100, 100);
    AiDialog::trimHistory(&doc, 2000);
    const QString text = doc.toPlainText();
    EXPECT_TRUE(text.endsWith(QStringLiteral("m0099:") + QString(100, QLatin1Char('x'))));
    EXPECT_FALSE(text.contains(QStringLiteral("m0000:")));
}

TEST(AiChatHistoryCap, Inv3UnderTheCapIsUnchanged) {
    QTextDocument doc;
    fill(doc, 5, 10);
    const QString before = doc.toPlainText();
    AiDialog::trimHistory(&doc, doc.characterCount());
    EXPECT_EQ(doc.toPlainText(), before);
}

TEST(AiChatHistoryCap, Inv4AppendMessageAppliesTheCap) {
    const std::string src = ants_test::slurpFile(SRC_AIDIALOG_CPP);
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("trimHistory(m_chatHistory->document(), kMaxHistoryChars)"),
              std::string::npos);
}
