// Feature-conformance test for tests/features/roadmap_viewer/spec.md.
//
// Asserts the contract for the status-bar Roadmap viewer:
//
//   INV-1 RoadmapDialog::renderHtml exists as a static method.
//   INV-2 Filter behaviour — toggling each emoji filter flips the
//         expected category and the Other category always renders.
//   INV-3 All four legend emojis (✅ 📋 🚧 💭) are recognised in the
//         renderer; all five filter bits are referenced.
//   INV-4 Current-work highlight produces the expected CSS marker
//         when a signal phrase matches a bullet.
//   INV-5 Empty signal set means no `border-left: 4px solid` marker.
//   INV-6 MainWindow::refreshRoadmapButton hides the button when no
//         ROADMAP.md is present.
//   INV-7 RoadmapDialog::rebuild reuses the scroll-preservation
//         pattern (`maximum`, `qMin`, capture-before-setHtml).
//   INV-8 Case-insensitive filename probe (Qt::CaseInsensitive).
//   INV-9 MainWindow constructs an m_roadmapBtn and wires its clicked
//         signal to a slot.
//   INV-10 refreshStatusBarForActiveTab calls refreshRoadmapButton.
//   INV-11 extractToc returns headings in document order with
//          matching level, text, and `roadmap-toc-N` anchor.
//   INV-12 renderHtml emits an `<a name="roadmap-toc-N">` anchor
//          before each heading element.
//   INV-13 Dialog wires a TOC list (m_toc / objectName "roadmap-toc")
//          into a QSplitter and connects activation to scrollToAnchor.
//   INV-14 Close button connected directly via
//          QAbstractButton::clicked (not only via rejected()).
//
// INV-1 / INV-2 / INV-4 / INV-5 / INV-11 / INV-12 also drive the
// renderer behaviourally (link the source file directly, call the
// helper, assert HTML / TOC entries).
//
// Exit 0 = all assertions hold.

#include "roadmapdialog.h"

#include <QCoreApplication>

#include <cstring>
#include <string>


#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    ADD_FAILURE_AT(__FILE__, __LINE__) << "[" << label << "] " << why;
    return 1;
}

bool qcontains(const QString &hay, const char *needle) {
    return hay.contains(QString::fromUtf8(needle));
}

}  // namespace

static int runMain() {
    // QCoreApplication app(argc, argv);  // ANTS-1217: bundle_main creates the app

    const std::string header = ants_test::slurpFile(ROADMAPDIALOG_H);
    const std::string source = ants_test::slurpFile(ROADMAPDIALOG_CPP);
    const std::string mwHeader = ants_test::slurpFile(MAINWINDOW_H);
    const std::string mwSource = ants_test::slurpFile(MAINWINDOW_CPP);

    if (header.empty()) fail("INV-1", "roadmapdialog.h not readable");
    if (source.empty()) fail("INV-1", "roadmapdialog.cpp not readable");
    if (mwHeader.empty()) fail("INV-1", "mainwindow.h not readable");
    if (mwSource.empty()) fail("INV-1", "mainwindow.cpp not readable");

    // INV-1: helper signature
    if (!contains(header, "static QString renderHtml"))
        fail("INV-1", "RoadmapDialog::renderHtml static method missing");

    // INV-3: all four legend emojis present in the source
    const char *emojis[] = {"✅", "\U0001F4CB", "\U0001F6A7", "\U0001F4AD"};
    const char *names[] = {"done", "planned", "in-progress", "considered"};
    for (size_t i = 0; i < sizeof(emojis) / sizeof(emojis[0]); ++i) {
        if (!contains(source, emojis[i]))
            fail("INV-3", names[i]);
    }
    // All five filter bits
    const char *bits[] = {
        "ShowDone", "ShowPlanned", "ShowInProgress",
        "ShowConsidered", "ShowCurrent",
    };
    for (const char *b : bits) {
        if (!contains(header, b)) fail("INV-3", b);
        if (!contains(source, b)) fail("INV-3", b);
    }

    // INV-2: filter behaviour. Synthesize a 5-bullet input and assert
    // the renderer drops the right category each time.
    const QString sample = QStringLiteral(
        "## Section\n"
        "\n"
        "- ✅ **Done thing.** Body.\n"
        "- 📋 **Planned thing.** Body.\n"
        "- 🚧 **In-progress thing.** Body.\n"
        "- 💭 **Considered thing.** Body.\n"
        "- Plain narrative bullet.\n");

    auto haveBullet = [&](unsigned filterBits, const char *needle) {
        QString out = RoadmapDialog::renderHtml(sample, filterBits, {}, QStringLiteral("default"));
        return qcontains(out, needle);
    };

    // All filters on — every bullet renders.
    const unsigned allOn =
        RoadmapDialog::ShowDone | RoadmapDialog::ShowPlanned |
        RoadmapDialog::ShowInProgress | RoadmapDialog::ShowConsidered |
        RoadmapDialog::ShowCurrent;
    if (!haveBullet(allOn, "Done thing.")) fail("INV-2", "all-on missing Done");
    if (!haveBullet(allOn, "Planned thing.")) fail("INV-2", "all-on missing Planned");
    if (!haveBullet(allOn, "In-progress thing.")) fail("INV-2", "all-on missing InProgress");
    if (!haveBullet(allOn, "Considered thing.")) fail("INV-2", "all-on missing Considered");
    if (!haveBullet(allOn, "Plain narrative")) fail("INV-2", "all-on missing Other");

    // Done off only — Done dropped, others kept.
    const unsigned noDone = allOn & ~RoadmapDialog::ShowDone;
    if (haveBullet(noDone, "Done thing.")) fail("INV-2", "ShowDone off didn't drop Done");
    if (!haveBullet(noDone, "Planned thing.")) fail("INV-2", "ShowDone off dropped Planned");
    if (!haveBullet(noDone, "Plain narrative")) fail("INV-2", "ShowDone off dropped Other");

    // Planned off only.
    const unsigned noPlanned = allOn & ~RoadmapDialog::ShowPlanned;
    if (haveBullet(noPlanned, "Planned thing.")) fail("INV-2", "ShowPlanned off didn't drop Planned");
    if (!haveBullet(noPlanned, "Done thing.")) fail("INV-2", "ShowPlanned off dropped Done");

    // In-progress off only.
    const unsigned noInProg = allOn & ~RoadmapDialog::ShowInProgress;
    if (haveBullet(noInProg, "In-progress thing.")) fail("INV-2", "ShowInProgress off didn't drop InProgress");
    if (!haveBullet(noInProg, "Done thing.")) fail("INV-2", "ShowInProgress off dropped Done");

    // Considered off only.
    const unsigned noConsidered = allOn & ~RoadmapDialog::ShowConsidered;
    if (haveBullet(noConsidered, "Considered thing.")) fail("INV-2", "ShowConsidered off didn't drop Considered");
    if (!haveBullet(noConsidered, "Done thing.")) fail("INV-2", "ShowConsidered off dropped Done");

    // Everything off — Other category bullet still renders.
    if (!haveBullet(0u, "Plain narrative")) fail("INV-2", "all-off dropped Other");
    if (haveBullet(0u, "Done thing.")) fail("INV-2", "all-off kept Done");

    // INV-4: current-work highlight. With an explicit signal phrase
    // matching the Done bullet, the rendered output contains the
    // border-left CSS marker.
    QStringList sigs;
    sigs << QStringLiteral("done thing");
    QString hl = RoadmapDialog::renderHtml(sample, allOn, sigs, QStringLiteral("default"));
    if (!qcontains(hl, "border-left:4px solid"))
        fail("INV-4", "border-left CSS marker missing for matched signal");

    // INV-5: empty signal set produces no border-left marker.
    QString plain = RoadmapDialog::renderHtml(sample, allOn, {}, QStringLiteral("default"));
    // The .cur class definition is in <style>; the *applied* class
    // shows up only when a <li> uses class="cur". Search for the
    // class-applied form to keep the assertion specific to the
    // applied case.
    if (qcontains(plain, "class=\"cur\""))
        fail("INV-5", "class=\"cur\" applied with no signal phrases");

    // INV-6: refreshRoadmapButton hides the button on absence.
    if (!contains(mwSource, "void MainWindow::refreshRoadmapButton"))
        fail("INV-6", "refreshRoadmapButton method missing");
    // The hide() call must be reachable from the absent-roadmap branch
    // — easiest source-grep is for the post-loop "found.isEmpty()"
    // hide() pattern.
    if (!contains(mwSource, "found.isEmpty()"))
        fail("INV-6", "absent-ROADMAP found.isEmpty() guard missing");
    if (!contains(mwSource, "m_roadmapBtn->hide()"))
        fail("INV-6", "m_roadmapBtn->hide() missing");

    // INV-7: scroll-preservation triple.
    if (!contains(source, "maximum"))
        fail("INV-7", "rebuild path doesn't reference vbar maximum");
    if (!contains(source, "qMin"))
        fail("INV-7", "rebuild path doesn't clamp via qMin");
    if (!contains(source, "vbar->value()"))
        fail("INV-7", "rebuild path doesn't capture vbar value before setHtml");

    // INV-8: case-insensitive filename probe.
    if (!contains(mwSource, "Qt::CaseInsensitive"))
        fail("INV-8", "Qt::CaseInsensitive not used in mainwindow probes");

    // INV-9: button construction + click wiring.
    if (!contains(mwSource, "m_roadmapBtn = new QPushButton"))
        fail("INV-9", "m_roadmapBtn not constructed");
    if (!contains(mwSource, "MainWindow::showRoadmapDialog"))
        fail("INV-9", "showRoadmapDialog slot missing");
    if (!contains(mwSource, "connect(m_roadmapBtn"))
        fail("INV-9", "m_roadmapBtn click signal not connected");

    // INV-10: refreshStatusBarForActiveTab calls refreshRoadmapButton.
    // Locate the function start and assert the call appears within it.
    const size_t fnStart = mwSource.find("void MainWindow::refreshStatusBarForActiveTab");
    if (fnStart == std::string::npos)
        fail("INV-10", "refreshStatusBarForActiveTab not found");
    const size_t fnEnd = mwSource.find("\nvoid MainWindow::", fnStart + 1);
    const std::string fnBody = mwSource.substr(
        fnStart, fnEnd == std::string::npos ? std::string::npos : fnEnd - fnStart);
    if (!contains(fnBody, "refreshRoadmapButton"))
        fail("INV-10", "refreshRoadmapButton not called from refreshStatusBarForActiveTab");

    // INV-11: extractToc returns headings with matching level/text/anchor.
    {
        const QString tocSample = QStringLiteral(
            "# Top heading\n"
            "Some prose.\n"
            "\n"
            "## Section A\n"
            "- bullet\n"
            "\n"
            "### Sub of A\n"
            "\n"
            "## Section B\n"
            "#### Deep one\n");
        const QVector<RoadmapDialog::TocEntry> toc =
            RoadmapDialog::extractToc(tocSample);
        if (toc.size() != 5)
            fail("INV-11", "expected 5 TOC entries");
        const int levels[] = {1, 2, 3, 2, 4};
        const char *texts[] = {
            "Top heading", "Section A", "Sub of A", "Section B", "Deep one"
        };
        for (int i = 0; i < 5; ++i) {
            if (toc[i].level != levels[i])
                fail("INV-11", "level mismatch");
            if (toc[i].text != QString::fromUtf8(texts[i]))
                fail("INV-11", "text mismatch");
            const QString want =
                QStringLiteral("roadmap-toc-%1").arg(i);
            if (toc[i].anchor != want)
                fail("INV-11", "anchor mismatch");
        }
    }

    // INV-12: renderHtml emits anchors before headings. The sample
    // has a single `## Section`, which renders as
    // `<a name="roadmap-toc-0"></a><h2>Section</h2>`.
    {
        QString h = RoadmapDialog::renderHtml(
            sample, allOn, {}, QStringLiteral("default"));
        if (!qcontains(h, "<a name=\"roadmap-toc-0\"></a><h2>"))
            fail("INV-12", "anchor before heading missing");
    }

    // INV-13: TOC list widget wired into the dialog with scrollToAnchor.
    if (!contains(source, "roadmap-toc"))
        fail("INV-13", "roadmap-toc objectName missing");
    if (!contains(source, "QSplitter"))
        fail("INV-13", "QSplitter not used in dialog body");
    if (!contains(source, "scrollToAnchor"))
        fail("INV-13", "scrollToAnchor not invoked from TOC handler");

    // INV-14: Close button is a plain QPushButton wired to close.
    // 0.7.50 retired QDialogButtonBox::Close — its role-dispatch path
    // dropped clicks on KWin/Wayland (per QTBUG-79126) even with the
    // direct clicked-on-button workaround that 0.7.43 added. Plain
    // QPushButton + clicked → QDialog::close mirrors the proven
    // bg-tasks pattern.
    // Match the constructor call shape so explanatory comments
    // mentioning the type don't false-positive the grep.
    if (contains(source, "new QDialogButtonBox"))
        fail("INV-14",
                    "QDialogButtonBox constructor still present — 0.7.50 "
                    "reverted to plain QPushButton because role-dispatch "
                    "dropped clicks on KWin/Wayland (QTBUG-79126)");
    if (!contains(source, "roadmap-close-button"))
        fail("INV-14", "roadmap-close-button objectName missing");
    if (!contains(source, "&QPushButton::clicked"))
        fail("INV-14",
                    "QPushButton::clicked connect missing — Close button "
                    "must wire clicked() directly to QDialog::close");
    if (!contains(source, "&QDialog::close"))
        fail("INV-14",
                    "QDialog::close target missing — Close button must "
                    "be wired to close the dialog");

    std::puts("OK roadmap_viewer: 14/14 invariants");
    return 0;
}

TEST(RoadmapViewer, Main) {
    ASSERT_EQ(0, runMain());
}
