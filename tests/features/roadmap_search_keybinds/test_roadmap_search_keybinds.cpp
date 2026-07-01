// Feature-conformance test for tests/features/roadmap_search_keybinds/spec.md.
//
// Locks ANTS-1234 — Roadmap dialog `/`-focus + Esc-clear + body-only
// auto-expand on substring match. Drives the keyPressEvent extension
// + eventFilter override against a real RoadmapDialog, and calls the
// static `renderCardsHtml` directly for the auto-expand cases.
//
// Exit 0 = all invariants hold.

#include "roadmapdialog.h"

#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include <cstdio>

#include <gtest/gtest.h>

namespace {

int fail(const char *label, const char *why) {
    ADD_FAILURE_AT(__FILE__, __LINE__) << "[" << label << "] " << why;
    return 1;
}

// Minimal fixture — two cards, one whose body carries a token NOT in
// the headline/layman/id, so the auto-expand test can fire. ANTS-9999
// (chosen far from any real ANTS ID) is the id:NNNN target.
QString fixtureMarkdown() {
    return QStringLiteral(
        "# Sample Roadmap\n"
        "\n"
        "## 0.8.0\n"
        "\n"
        "- ✅ [ANTS-9998] **Shipped foo bar.**\n"
        "  Layman: short summary line.\n"
        "  The word baz lives only in this continuation prose,\n"
        "  not in the headline.\n"
        "  Kind: implement.\n"
        "\n"
        "- 🚧 [ANTS-9999] **Active card.**\n"
        "  Layman: in-progress item.\n"
        "  Kind: refactor.\n");
}

QString writeRoadmap(const QTemporaryDir &dir, const QString &markdown) {
    const QString path = dir.path() + QStringLiteral("/ROADMAP.md");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
    QTextStream out(&f);
    out << markdown;
    return path;
}

}  // namespace

static int runMain() {
    QTemporaryDir tmp;
    if (!tmp.isValid())
        fail("Setup", "QTemporaryDir construction failed");
    const QString roadmapPath = writeRoadmap(tmp, fixtureMarkdown());
    if (roadmapPath.isEmpty())
        fail("Setup", "could not write fixture ROADMAP.md");

    // -----------------------------------------------------------------
    // Live-widget assertions: construct one dialog, run all keybind
    // checks against it sequentially. Each block leaves the dialog in
    // a clean state (search box empty, no focus) for the next block.
    // -----------------------------------------------------------------
    RoadmapDialog dialog(roadmapPath, QStringLiteral("light"));
    dialog.show();
    QApplication::processEvents();

    auto *searchBox = dialog.findChild<QLineEdit *>(
        QStringLiteral("roadmap-search-box"));
    if (!searchBox)
        fail("Setup",
            "could not locate roadmap-search-box — has its "
            "objectName drifted?");

    // ANTS-1234-INV-1: pressing `/` on the dialog focuses the search
    // box; any pre-existing text is select-all'd so the next keystroke
    // replaces it.
    {
        searchBox->setText(QStringLiteral("preseed"));
        searchBox->clearFocus();
        dialog.setFocus();
        QApplication::processEvents();
        QTest::keyClick(&dialog, Qt::Key_Slash);
        QApplication::processEvents();
        if (!searchBox->hasFocus())
            fail("INV-1",
                "pressing `/` did NOT focus the search box — the "
                "keyPressEvent extension didn't fire or the gate "
                "swallowed it.");
        if (searchBox->selectedText() != QStringLiteral("preseed"))
            fail("INV-1",
                "selectAll() did not fire — next keystroke would "
                "append rather than replace.");
        // Cleanup
        searchBox->clear();
        searchBox->clearFocus();
        QApplication::processEvents();
    }

    // ANTS-1234-INV-2: with the search box already focused, `/` is
    // appended to the predicate verbatim — the override gate holds.
    {
        searchBox->setText(QString());
        searchBox->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
        QTest::keyClick(searchBox, Qt::Key_Slash);
        QApplication::processEvents();
        if (searchBox->text() != QStringLiteral("/"))
            fail("INV-2",
                "`/` typed into a focused search box was swallowed "
                "— the gate (`!m_searchBox->hasFocus()`) is broken.");
        // Cleanup
        searchBox->clear();
        searchBox->clearFocus();
        QApplication::processEvents();
    }

    // ANTS-1234-INV-3: Esc on focused search clears the predicate and
    // removes focus; the dialog stays open (eventFilter consumes the
    // event before QDialog::reject can fire).
    {
        searchBox->setText(QStringLiteral("some predicate"));
        searchBox->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
        QTest::keyClick(searchBox, Qt::Key_Escape);
        QApplication::processEvents();
        if (!searchBox->text().isEmpty())
            fail("INV-3",
                "Esc on focused search did NOT clear the predicate.");
        if (searchBox->hasFocus())
            fail("INV-3",
                "Esc on focused search did NOT remove focus.");
        if (!dialog.isVisible())
            fail("INV-3",
                "Esc on focused search closed the dialog — the "
                "eventFilter override is not consuming the event "
                "(QDialog::reject fired).");
    }

    // ANTS-1234-INV-4: Esc on the dialog (search box NOT focused)
    // closes the dialog. Tested via QDialog::reject by sending the
    // event after explicitly removing focus from any child. We run
    // this check LAST among the live-widget tests because it
    // tears the dialog down.
    {
        searchBox->clearFocus();
        dialog.setFocus();
        QApplication::processEvents();
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QApplication::processEvents();
        if (dialog.isVisible())
            fail("INV-4",
                "Esc on dialog (search box not focused) did NOT "
                "close the dialog — QDialog::reject path is broken.");
    }

    // -----------------------------------------------------------------
    // Renderer assertions: drive renderCardsHtml directly. The
    // renderer is static so no dialog instance is required.
    // -----------------------------------------------------------------
    const QString markdown = fixtureMarkdown();
    const unsigned filter = 0x0F;  // all four status categories on
    const QStringList currentBullets;
    const QString theme = QStringLiteral("light");

    // Pre-expand the "0.8.0" section so cards actually render. The
    // dialog auto-expands sections by user click; the renderer treats
    // empty expandedSections as "all collapsed". For these renderer
    // tests we need the cards visible to inspect their body
    // emission. Section slug derives from "0.8.0" → "0-8-0".
    auto makeOptsWithSectionOpen = []() {
        RoadmapDialog::CardRenderOptions opts;
        opts.expandedSections.insert(QStringLiteral("0-8-0"));
        return opts;
    };

    // ANTS-1234-INV-5 + INV-8: body-only match triggers render-time
    // auto-expand; opts.expandedItems remains unchanged after.
    {
        RoadmapDialog::CardRenderOptions opts = makeOptsWithSectionOpen();
        // opts.expandedItems is empty by default; no card is
        // user-expanded.
        const QString html = RoadmapDialog::renderCardsHtml(
            markdown, filter, currentBullets, theme,
            RoadmapDialog::SortOrder::Document,
            QStringLiteral("baz"),  // matches body of ANTS-9998 only
            {}, opts);
        if (!html.contains(QStringLiteral("<p class=\"rm-body-first\">")))
            fail("INV-5",
                "predicate 'baz' (body-only match) did NOT trigger "
                "auto-expand — the rm-body-first body emission "
                "is missing from the rendered HTML.");
        if (!opts.expandedItems.isEmpty())
            fail("INV-8",
                "renderCardsHtml mutated CardRenderOptions::expandedItems "
                "— auto-expand must be render-time only.");
    }

    // ANTS-1234-INV-6: a predicate that matches the headline / layman
    // does NOT auto-expand (the card stays in its user-driven state,
    // which is collapsed here).
    {
        RoadmapDialog::CardRenderOptions opts = makeOptsWithSectionOpen();
        const QString html = RoadmapDialog::renderCardsHtml(
            markdown, filter, currentBullets, theme,
            RoadmapDialog::SortOrder::Document,
            QStringLiteral("Shipped foo"),  // matches headline
            {}, opts);
        if (html.contains(QStringLiteral("<p class=\"rm-body-first\">")))
            fail("INV-6",
                "predicate matching the headline triggered "
                "auto-expand — body-only guard is broken.");
    }

    // ANTS-1234-INV-7 + INV-8: id:NNNN predicate auto-expands the
    // matched card, again without mutating expandedItems.
    {
        RoadmapDialog::CardRenderOptions opts = makeOptsWithSectionOpen();
        const QString html = RoadmapDialog::renderCardsHtml(
            markdown, filter, currentBullets, theme,
            RoadmapDialog::SortOrder::Document,
            QStringLiteral("id:9999"),
            {}, opts);
        if (!html.contains(QStringLiteral("<p class=\"rm-body-first\">")))
            fail("INV-7",
                "id:9999 predicate did NOT auto-expand the matched "
                "card — idJumpToThisCard path is broken.");
        if (!opts.expandedItems.isEmpty())
            fail("INV-8",
                "id:NNNN auto-expand mutated expandedItems — must "
                "be render-time only.");
    }

    std::fprintf(stderr,
        "OK — RoadmapDialog `/`-focus + Esc-clear + auto-expand INVs hold.\n");
    return 0;
}

TEST(RoadmapSearchKeybinds, Main) {
    ASSERT_EQ(0, runMain());
}
