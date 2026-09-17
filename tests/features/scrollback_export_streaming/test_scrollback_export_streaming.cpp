// ANTS-5078 — feature-conformance test; see spec.md. INV-1 to INV-8 drive
// ScrollbackExporter over a headless TerminalGrid. INV-9 and INV-10 are
// source scrapes, because the slices run on a live TerminalWidget.

#include "scrollbackexporter.h"
#include "terminalgrid.h"
#include "vtparser.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include <string>

namespace {

using Exporter = ScrollbackExporter;
using Step = ScrollbackExporter::Step;

void feed(TerminalGrid &grid, const QByteArray &bytes) {
    VtParser parser([&grid](const VtAction &act) { grid.processAction(act); });
    parser.feed(bytes.constData(), bytes.size());
}

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// Runs step(1) until the export leaves More.
Step drain(Exporter &ex) {
    Step s = Step::More;
    for (int guard = 0; s == Step::More && guard < 100000; ++guard)
        s = ex.step(1);
    return s;
}

// Text fixture: a line with trailing spaces, an empty line inside the text,
// and two empty screen rows at the end. Four lines are in scrollback.
void textFixture(TerminalGrid &grid) {
    feed(grid, "l1\r\n\r\nl3  \r\nl4\r\nl5\r\nl6\r\nl7\r\n\x1b[3;1H\x1b[J");
}
const QByteArray kTextExpected = "l1\n\nl3\nl4\nl5\nl6";

Exporter::Request textRequest(const QString &path) {
    Exporter::Request r;
    r.format = Exporter::Format::Text;
    r.path = path;
    return r;
}

}  // namespace

// INV-1
TEST(ScrollbackExportStreaming, Inv1TextMatchesExportAsText) {
    QTemporaryDir dir;
    const QString path = dir.filePath("out.txt");
    TerminalGrid grid(4, 10);
    textFixture(grid);
    ASSERT_EQ(grid.scrollbackSize(), 4) << "setup: fixture shape changed";

    Exporter ex(grid, textRequest(path));
    ASSERT_TRUE(ex.open()) << ex.error().toStdString();
    ASSERT_EQ(drain(ex), Step::Done) << ex.error().toStdString();
    EXPECT_EQ(readAll(path), kTextExpected)
        << "INV-1: text export differs from exportAsText";
}

// INV-2
TEST(ScrollbackExportStreaming, Inv2HtmlMatchesExportAsHtml) {
    QTemporaryDir dir;
    const QString path = dir.filePath("out.html");
    TerminalGrid grid(2, 6);
    feed(grid, "\x1b[1mB\x1b[0m\x1b[7mI\x1b[0m\x1b[41mR\x1b[0m\r\nok<&>\r\nz");
    ASSERT_EQ(grid.scrollbackSize(), 1) << "setup: fixture shape changed";

    Exporter::Request r;
    r.format = Exporter::Format::Html;
    r.path = path;
    r.defaultFg = grid.defaultFg();
    r.defaultBg = grid.defaultBg();
    r.fontPointSize = 11;
    Exporter ex(grid, r);
    ASSERT_TRUE(ex.open()) << ex.error().toStdString();
    ASSERT_EQ(drain(ex), Step::Done) << ex.error().toStdString();

    const QByteArray expected =
        "<!DOCTYPE html>\n"
        "<html><head><meta charset='utf-8'>\n"
        "<title>Ants Terminal Export</title>\n"
        "<style>\n"
        "body { background: #1e1e2e; color: #cdd6f4; font-family: 'JetBrains Mono', 'Fira Code', monospace; font-size: 11pt; white-space: pre; }\n"
        "span.bold { font-weight: bold; }\n"
        "span.italic { font-style: italic; }\n"
        "span.underline { text-decoration: underline; }\n"
        "span.strikethrough { text-decoration: line-through; }\n"
        "</style>\n"
        "</head><body>\n"
        "<span class='bold' style='color:#cdd6f4;'>B</span><span style='color:#1e1e2e;background:#cdd6f4;'>I</span><span style='color:#cdd6f4;background:#cd0000;'>R</span><span style='color:#cdd6f4;'>   </span>\n"
        "<span style='color:#cdd6f4;'>ok&lt;&amp;&gt; </span>\n"
        "<span style='color:#cdd6f4;'>z     </span>\n"
        "</body></html>\n";
    EXPECT_EQ(readAll(path), expected)
        << "INV-2: HTML export differs from exportAsHtml";
}

// INV-3
TEST(ScrollbackExportStreaming, Inv3CastMatchesExportBlockAsCast) {
    QTemporaryDir dir;
    const QString path = dir.filePath("out.cast");
    TerminalGrid grid(3, 12);
    feed(grid, "\x1b]133;A\x07$ \x1b]133;B\x07" "echo \"q\"\r\n\x1b]133;C\x07"
               "o\"1\r\no\\2\r\no3\xc3\xa9\r\no4\r\n\x1b]133;D;0\x07\x1b]133;A\x07$ ");
    auto &regions = grid.promptRegions();
    ASSERT_EQ(regions.size(), 2u) << "setup: fixture shape changed";
    // The output starts in scrollback and ends on the screen.
    ASSERT_LT(regions[0].outputStartLine, grid.scrollbackSize());
    ASSERT_GT(regions[1].startLine, grid.scrollbackSize());
    regions[0].commandStartMs = 1700000000000;
    regions[0].commandEndMs = 1700000001234;

    Exporter::Request r;
    r.format = Exporter::Format::Cast;
    r.path = path;
    r.blockId = regions[0].id;
    r.command = QStringLiteral("echo \"q\"");
    Exporter ex(grid, r);
    ASSERT_TRUE(ex.open()) << ex.error().toStdString();
    ASSERT_EQ(drain(ex), Step::Done) << ex.error().toStdString();

    const QByteArray expected =
        "{\"version\": 2, \"width\": 12, \"height\": 3, \"timestamp\": 1700000000, \"env\": {\"TERM\": \"xterm-256color\"}}\n"
        "[0.0, \"o\", \"echo \\\"q\\\"\\r\\n\"]\n"
        "[1.234, \"o\", \"o\\\"1\\no\\\\2\\no3\xc3\xa9\\no4\\n\"]\n";
    EXPECT_EQ(readAll(path), expected)
        << "INV-3: cast export differs from exportBlockAsCast";
}

// INV-4
TEST(ScrollbackExportStreaming, Inv4WritesAsItGoes) {
    QTemporaryDir dir;
    TerminalGrid grid(4, 10);
    textFixture(grid);
    Exporter ex(grid, textRequest(dir.filePath("out.txt")));
    ASSERT_TRUE(ex.open()) << ex.error().toStdString();

    int risesOnMore = 0;
    qint64 before = ex.bytesWritten();
    Step s = Step::More;
    while (s == Step::More) {
        s = ex.step(1);
        if (s == Step::More && ex.bytesWritten() > before) ++risesOnMore;
        before = ex.bytesWritten();
    }
    EXPECT_EQ(s, Step::Done) << ex.error().toStdString();
    EXPECT_GE(risesOnMore, 2)
        << "INV-4: bytes are not written while the export is still running";
}

// INV-5
TEST(ScrollbackExportStreaming, Inv5EvictedUnwrittenLineFails) {
    QTemporaryDir dir;
    const QString path = dir.filePath("out.txt");
    TerminalGrid grid(4, 10);
    grid.setMaxScrollback(1000);
    feed(grid, QByteArray("x\r\n").repeated(1100));
    ASSERT_EQ(grid.scrollbackSize(), 1000) << "setup: scrollback is not full";

    Exporter ex(grid, textRequest(path));
    ASSERT_TRUE(ex.open()) << ex.error().toStdString();
    ASSERT_EQ(ex.step(1), Step::More) << "setup: the export did not start";
    feed(grid, "\r\n\r\n");   // evicts line 1, which is not written yet
    EXPECT_EQ(drain(ex), Step::Failed)
        << "INV-5: an evicted, unwritten line did not fail the export";
    EXPECT_FALSE(ex.error().isEmpty()) << "INV-5: a failure left no error";
    EXPECT_FALSE(QFile::exists(path)) << "INV-5: a failed export left a file";
}

// INV-6
TEST(ScrollbackExportStreaming, Inv6OutputWithoutEvictionChangesNothing) {
    QTemporaryDir dir;
    const QString path = dir.filePath("out.txt");
    TerminalGrid grid(4, 10);
    grid.setMaxScrollback(100000);
    textFixture(grid);

    Exporter ex(grid, textRequest(path));
    ASSERT_TRUE(ex.open()) << ex.error().toStdString();
    ASSERT_EQ(ex.step(1), Step::More) << "setup: the export did not start";
    feed(grid, QByteArray("more\r\n").repeated(20));
    ASSERT_EQ(drain(ex), Step::Done) << ex.error().toStdString();
    EXPECT_EQ(readAll(path), kTextExpected)
        << "INV-6: output arriving mid-export changed the result";
}

// INV-7
TEST(ScrollbackExportStreaming, Inv7WidthChangeFails) {
    QTemporaryDir dir;
    const QString path = dir.filePath("out.txt");
    TerminalGrid grid(4, 10);
    textFixture(grid);

    Exporter ex(grid, textRequest(path));
    ASSERT_TRUE(ex.open()) << ex.error().toStdString();
    ASSERT_EQ(ex.step(1), Step::More) << "setup: the export did not start";
    grid.resize(4, 12);
    EXPECT_EQ(drain(ex), Step::Failed)
        << "INV-7: a width change did not fail the export";
    EXPECT_FALSE(QFile::exists(path)) << "INV-7: a failed export left a file";
}

// INV-8
TEST(ScrollbackExportStreaming, Inv8DestroyedExportLeavesNoFile) {
    QTemporaryDir dir;
    const QString path = dir.filePath("out.txt");
    TerminalGrid grid(4, 10);
    textFixture(grid);
    {
        Exporter ex(grid, textRequest(path));
        ASSERT_TRUE(ex.open()) << ex.error().toStdString();
        ASSERT_EQ(ex.step(1), Step::More) << "setup: the export did not start";
    }
    EXPECT_FALSE(QFile::exists(path))
        << "INV-8: an unfinished export left a file at the target path";
}

namespace {

const std::size_t npos = std::string::npos;

const char *const kRemoved[] = {"exportAsText", "exportAsHtml", "exportBlockAsCast"};

}  // namespace

// INV-9
TEST(ScrollbackExportStreaming, Inv9NoCallerBuildsAWholeExport) {
    const std::string widgetCpp = ants_test::slurpFile(SRC_TERMINALWIDGET_PATH);
    const std::string widgetH = ants_test::slurpFile(SRC_TERMINALWIDGET_H_PATH);
    const std::string mainWindow = ants_test::slurpMainWindow();
    ASSERT_FALSE(widgetCpp.empty() || widgetH.empty() || mainWindow.empty())
        << "setup: a scraped source was not found";
    for (const char *name : kRemoved) {
        EXPECT_EQ(widgetCpp.find(name), npos) << "INV-9: terminalwidget.cpp names " << name;
        EXPECT_EQ(widgetH.find(name), npos) << "INV-9: terminalwidget.h names " << name;
        EXPECT_EQ(mainWindow.find(name), npos) << "INV-9: MainWindow names " << name;
    }

    const std::string menu = ants_test::stripComments(ants_test::slurpFunctionBody(
        widgetCpp, "void TerminalWidget::contextMenuEvent("));
    ASSERT_FALSE(menu.empty()) << "setup: contextMenuEvent was not found";
    std::size_t calls = 0;
    for (std::size_t at = menu.find("startExport("); at != npos;
         at = menu.find("startExport(", at + 1))
        ++calls;
    EXPECT_GE(calls, 3u) << "INV-9: the three export actions do not all call startExport";

    const std::string settings = ants_test::stripComments(ants_test::slurpFunctionBody(
        mainWindow, "void MainWindow::setupSettingsMenu("));
    ASSERT_FALSE(settings.empty()) << "setup: setupSettingsMenu was not found";
    EXPECT_NE(settings.find("startExport("), npos)
        << "INV-9: MainWindow's export action does not call startExport";
}

// INV-10
TEST(ScrollbackExportStreaming, Inv10StartExportRunsInSlicesAndReports) {
    const std::string src = ants_test::slurpFile(SRC_TERMINALWIDGET_PATH);
    const std::string start = ants_test::stripComments(
        ants_test::slurpFunctionBody(src, "bool TerminalWidget::startExport("));
    const std::string slice = ants_test::stripComments(
        ants_test::slurpFunctionBody(src, "void TerminalWidget::runExportSlice("));
    const std::string finish = ants_test::stripComments(
        ants_test::slurpFunctionBody(src, "void TerminalWidget::finishExport("));
    const std::string resize = ants_test::stripComments(
        ants_test::slurpFunctionBody(src, "void TerminalWidget::recalcGridSize("));
    ASSERT_FALSE(start.empty()) << "INV-10: startExport was not found";
    ASSERT_FALSE(slice.empty()) << "INV-10: runExportSlice was not found";
    ASSERT_FALSE(finish.empty()) << "INV-10: finishExport was not found";
    ASSERT_FALSE(resize.empty()) << "setup: recalcGridSize was not found";

    EXPECT_NE(start.find("if (m_exporter)"), npos)
        << "INV-10: startExport does not refuse while an export is running";
    EXPECT_NE(start.find("emit captureFailed("), npos)
        << "INV-10: a refused or unopened export is not reported";
    EXPECT_NE(start.find("QTimer::singleShot("), npos)
        << "INV-10: startExport does not schedule its first slice";

    EXPECT_NE(slice.find("step(kExportLinesPerStep)"), npos)
        << "INV-10: the slice does not call step";
    EXPECT_NE(slice.find("kExportSliceMs"), npos)
        << "INV-10: the slice has no time budget";
    EXPECT_NE(slice.find("QTimer::singleShot("), npos)
        << "INV-10: the slice does not reschedule itself";
    EXPECT_NE(slice.find("Step::Failed"), npos)
        << "INV-10: the slice does not handle a failed step";
    EXPECT_NE(slice.find("finishExport("), npos)
        << "INV-10: the slice does not finish a stopped export";
    EXPECT_NE(finish.find("emit captureFailed("), npos)
        << "INV-10: a failed export is not reported";
    EXPECT_NE(finish.find("emit exportFinished("), npos)
        << "INV-10: the end of an export is not reported";

    EXPECT_NE(resize.find("finishExport(false"), npos)
        << "INV-10: recalcGridSize leaves an export running across a width change";
}
