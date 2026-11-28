// ANTS-6654 — terminal widget fixes from the 2026-11-06 reviews.
// Contract: tests/features/terminal_widget_review_fixes/spec.md.

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QMetaObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointF>
#include <QString>

#include <memory>
#include <string>
#include <vector>

#include "terminalgrid.h"
#include "terminalwidget.h"
#include "vtparser.h"
#include "vtstream.h"

#ifndef SRC_TERMINALWIDGET_PATH
#define SRC_TERMINALWIDGET_PATH ""
#endif

namespace {

constexpr int kCap = 1000;   // the smallest scrollback cap the grid accepts
constexpr int kRows = 4;

// Output as the widget receives it: one parsed batch through onVtBatch.
void feedBatch(TerminalWidget &w, const std::string &s) {
    auto batch = std::make_shared<VtBatch>();
    VtParser parser([&batch](const VtAction &a) { batch->actions.push_back(a); });
    parser.feed(s.data(), static_cast<int>(s.size()));
    batch->rawBytes = QByteArray(s.data(), static_cast<qsizetype>(s.size()));
    VtBatchPtr shipped = batch;
    ASSERT_TRUE(QMetaObject::invokeMethod(&w, "onVtBatch", Qt::DirectConnection,
                                          Q_ARG(VtBatchPtr, shipped)));
}

bool lineStartsWith(const TerminalGrid *g, int line, const std::string &text) {
    const auto &cells = g->scrollbackLine(line);
    if (cells.size() < text.size()) return false;
    for (std::size_t k = 0; k < text.size(); ++k)
        if (cells[k].codepoint != static_cast<unsigned char>(text[k])) return false;
    return true;
}

std::string tw() { return ants_test::stripComments(ants_test::slurpFile(SRC_TERMINALWIDGET_PATH)); }

std::string body(const std::string &src, const std::string &anchor) {
    return ants_test::slurpFunctionBody(src, anchor);
}

bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1
TEST(TerminalWidgetReviewFixes, EveryFailedCommandIsReported) {
    TerminalWidget w;
    w.grid()->resize(kRows, 40);
    int failed = 0;
    QObject::connect(&w, &TerminalWidget::commandFailed, &w,
                     [&failed](int code, const QString &) { if (code == 1) ++failed; });
    const std::string command =
        "\x1B]133;A\x07$ \x1B]133;B\x07false\r\n\x1B]133;C\x07\x1B]133;D;1\x07";
    feedBatch(w, command);
    feedBatch(w, command);
    EXPECT_EQ(failed, 2) << "a second failure with the same exit code was not reported";
}

// INV-2
TEST(TerminalWidgetReviewFixes, BookmarkMovesWithItsLine) {
    TerminalWidget w;
    w.grid()->resize(kRows, 40);
    w.setMaxScrollback(kCap);
    std::string fill;
    for (int i = 0; i < kCap + kRows; ++i) fill += "fill\r\n";
    feedBatch(w, fill);
    feedBatch(w, "MARK");
    ASSERT_EQ(w.grid()->scrollbackSize(), kCap);
    w.toggleBookmark();
    ASSERT_EQ(w.bookmarks().size(), 1u);

    std::string more;
    for (int i = 0; i < 10; ++i) more += "\r\nnext";
    feedBatch(w, more);
    ASSERT_EQ(w.bookmarks().size(), 1u);
    const int bm = w.bookmarks().front();
    ASSERT_GE(bm, 0);
    ASSERT_LT(bm, w.grid()->scrollbackSize());
    EXPECT_TRUE(lineStartsWith(w.grid(), bm, "MARK"))
        << "the bookmark stayed on its old index, which now holds other text";
}

// INV-3
TEST(TerminalWidgetReviewFixes, RichCopyKeepsCombiningMarks) {
    TerminalWidget w;
    VtParser parser([&w](const VtAction &a) { w.grid()->processAction(a); });
    w.resize(640, 240);
    w.grid()->resize(kRows, 20);
    const std::string text = "he\xCC\x81llo";   // e + U+0301
    parser.feed(text.data(), static_cast<int>(text.size()));
    const QPointF at(2, 2);   // the first cell of the top row
    QMouseEvent ev(QEvent::MouseButtonDblClick, at, w.mapToGlobal(at),
                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&w, &ev);
    ASSERT_TRUE(w.selectedText().contains(QString::fromUtf8("e\xCC\x81")));

    w.copySelectionRich();
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    ASSERT_NE(mime, nullptr);
    EXPECT_TRUE(mime->html().contains(QString::fromUtf8("e\xCC\x81")))
        << "the HTML half dropped the combining mark";
}

// INV-4
TEST(TerminalWidgetReviewFixes, SendPreviewWrapsLongLines) {
    const std::string b = body(tw(), "void TerminalWidget::showSendConfirmation(");
    ASSERT_FALSE(b.empty()) << "showSendConfirmation not found";
    EXPECT_FALSE(has(b, "ln.left(117)")) << "the preview still cuts long lines";
    EXPECT_TRUE(has(b, "new QPlainTextEdit("));
    EXPECT_TRUE(has(b, "setReadOnly(true)"));
    EXPECT_TRUE(has(b, "QPlainTextEdit::WidgetWidth"));
}

// INV-5
TEST(TerminalWidgetReviewFixes, EditorLaunchSuitsXdgOpenAndReportsFailure) {
    const std::string b = body(tw(), "void TerminalWidget::openFileAtPath(");
    ASSERT_FALSE(b.empty()) << "openFileAtPath not found";
    EXPECT_TRUE(has(b, "editorBase == \"xdg-open\""))
        << "xdg-open gets the `--` older xdg-utils reject";
    const std::size_t launch = b.find("if (!QProcess::startDetached(editor, args))");
    ASSERT_NE(launch, std::string::npos) << "a failed editor launch is silent";
    EXPECT_NE(b.find("emit captureFailed(", launch), std::string::npos);
}

// INV-6
TEST(TerminalWidgetReviewFixes, RecordingHeaderWriteIsChecked) {
    const std::string b = body(tw(), "void TerminalWidget::startRecording(");
    ASSERT_FALSE(b.empty()) << "startRecording not found";
    const std::size_t check = b.find("!= headerLine.size() || !m_recordFile->flush()");
    ASSERT_NE(check, std::string::npos) << "the header write is unchecked";
    const std::size_t report = b.find("emit captureFailed(", check);
    const std::size_t start = b.find("m_recording = true", check);
    ASSERT_NE(report, std::string::npos);
    ASSERT_NE(start, std::string::npos);
    EXPECT_LT(report, start);
}

// INV-7
TEST(TerminalWidgetReviewFixes, ClickToMoveCountsCharacters) {
    const std::string b = body(tw(), "void TerminalWidget::clickToMoveCursor(");
    ASSERT_FALSE(b.empty()) << "clickToMoveCursor not found";
    EXPECT_TRUE(has(b, ".isWideCont")) << "wide-character halves are counted as characters";
    EXPECT_FALSE(has(b, "int delta = col - cursorCol;"));
}

// INV-8
TEST(TerminalWidgetReviewFixes, PlainTextFileUrlAsksFirst) {
    const std::string b = body(tw(), "void TerminalWidget::openHyperlink(");
    ASSERT_FALSE(b.empty()) << "openHyperlink not found";
    const std::size_t local = b.find("!span.isOsc8 && QUrl(span.url).isLocalFile()");
    const std::size_t open = b.find("QDesktopServices::openUrl(");
    ASSERT_NE(local, std::string::npos) << "a file:// link opens with no confirmation";
    ASSERT_NE(open, std::string::npos);
    EXPECT_LT(local, open);
}

// INV-9
TEST(TerminalWidgetReviewFixes, ReloadKeepsPerStyleFonts) {
    const std::string b = body(ants_test::stripComments(ants_test::slurpMainWindow()),
                               "void MainWindow::reapplyConfig(");
    ASSERT_FALSE(b.empty()) << "reapplyConfig not found";
    ASSERT_TRUE(has(b, "applyConfigToTerminal("));
    EXPECT_FALSE(has(b, "setFontFamily("))
        << "a setFontFamily after applyConfigToTerminal resets the bold and italic families";
}
