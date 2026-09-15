// Every key reaches the shell the same way — see spec.md. ANTS-5077.
// Source-scrape of src/terminalwidget.cpp: TerminalWidget is a
// QOpenGLWidget with a live PTY and keyPressEvent is protected.

#include <QFile>
#include <QString>
#include <QTextStream>

#include <gtest/gtest.h>

#ifndef SRC_TERMINALWIDGET_PATH
#define SRC_TERMINALWIDGET_PATH ""
#endif

namespace {

QString readSource() {
    const QString path = QStringLiteral(SRC_TERMINALWIDGET_PATH);
    if (path.isEmpty()) {
        ADD_FAILURE() << "SRC_TERMINALWIDGET_PATH is empty — run from the CMake build";
        return {};
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ADD_FAILURE() << "cannot open " << qUtf8Printable(path);
        return {};
    }
    QTextStream in(&f);
    return in.readAll();
}

// The text of a top-level function: from its signature to the first line
// that is a lone closing brace at column 0.
QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return {};
    const int end = src.indexOf(QStringLiteral("\n}\n"), start);
    if (end < 0) return {};
    return src.mid(start, end - start);
}

}  // namespace

// INV-1
TEST(TerminalwidgetInputContracts, KeyPressSendsOnlyThroughSendKeyData) {
    const QString src = readSource();
    if (src.isEmpty()) return;
    const QString body =
        functionBody(src, QStringLiteral("void TerminalWidget::keyPressEvent("));
    ASSERT_FALSE(body.isEmpty()) << "keyPressEvent not found";
    EXPECT_FALSE(body.contains(QStringLiteral("ptyWrite(")))
        << "keyPressEvent writes to the PTY directly, skipping the selection "
           "clear and the broadcast";
    EXPECT_TRUE(body.contains(QStringLiteral("sendKeyData(")));
}

// INV-2
TEST(TerminalwidgetInputContracts, SendKeyDataClearsWritesAndBroadcasts) {
    const QString src = readSource();
    if (src.isEmpty()) return;
    const QString body =
        functionBody(src, QStringLiteral("void TerminalWidget::sendKeyData("));
    ASSERT_FALSE(body.isEmpty()) << "sendKeyData not found";
    EXPECT_TRUE(body.contains(QStringLiteral("clearSelection()")));
    EXPECT_TRUE(body.contains(QStringLiteral("ptyWrite(data)")));
    EXPECT_TRUE(body.contains(QStringLiteral("m_broadcastCallback(this, event)")));
}

// INV-3
TEST(TerminalwidgetInputContracts, FailedStartClearsTheStream) {
    const QString src = readSource();
    if (src.isEmpty()) return;
    const QString body =
        functionBody(src, QStringLiteral("bool TerminalWidget::startShell("));
    ASSERT_FALSE(body.isEmpty()) << "startShell not found";
    const int failPos = body.indexOf(QStringLiteral("if (!ok) {"));
    ASSERT_GE(failPos, 0) << "startShell's failure branch not found";
    const int returnPos = body.indexOf(QStringLiteral("return false;"), failPos);
    ASSERT_GT(returnPos, failPos);
    EXPECT_TRUE(body.mid(failPos, returnPos - failPos)
                    .contains(QStringLiteral("m_vtStream = nullptr")))
        << "a failed start leaves m_vtStream set, so hasPty() stays true";
}
