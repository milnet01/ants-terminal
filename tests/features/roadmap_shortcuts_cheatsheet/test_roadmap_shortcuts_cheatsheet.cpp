// Feature-conformance test for tests/features/roadmap_shortcuts_cheatsheet/spec.md.
//
// Locks ANTS-1236 — Roadmap dialog keyboard-shortcut cheatsheet.
// Drives the file-scope `kRoadmapShortcuts` data table + the
// `?`-toggle keyPressEvent wired into RoadmapDialog, plus source-greps
// roadmapdialog.cpp / roadmapshortcutsdialog.cpp for the load-bearing
// INV anchors.
//
// Exit 0 = all invariants hold.

#include "roadmapdialog.h"
#include "roadmapshortcutsdialog.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QVector>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    ADD_FAILURE_AT(__FILE__, __LINE__) << "[" << label << "] " << why;
    return 1;
}

// Minimal roadmap markdown fixture — one of every status emoji so the
// dialog has something to render (rebuild() is happy), but nothing
// here exercises the cheatsheet itself. The cheatsheet is driven by
// `?` key events independent of the document content.
QString fixtureMarkdown() {
    return QStringLiteral(
        "# Sample Roadmap\n"
        "\n"
        "## 0.8.0\n"
        "\n"
        "- ✅ [ANTS-9001] **Shipped.** Body.\n"
        "  Kind: implement.\n"
        "\n"
        "- 🚧 [ANTS-9002] **In progress.** Body.\n"
        "  Kind: refactor.\n");
}

// Write `markdown` to `<dir>/ROADMAP.md` and return the absolute path.
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
    // INV-1 / INV-2 / INV-8: data table has exactly 10 rows in the
    // canonical order documented in the spec (ANTS-1234 added `/`
    // after `?` on 2026-05-11).
    {
        const auto rows = roadmapShortcutRows();
        if (rows.size() != 10) {
            std::fprintf(stderr, "got rows.size() = %d\n", int(rows.size()));
            fail("INV-1/INV-8",
                "roadmapShortcutRows() must return exactly 10 rows; a "
                "shortcut addition without bumping the test is a build "
                "failure (the source-of-truth contract).");
        }
        const QStringList expectedKeys = {
            QStringLiteral("?"),
            QStringLiteral("/"),
            QStringLiteral("Esc"),
            QStringLiteral("F5"),
            QStringLiteral("Ctrl+C"),
            QStringLiteral("Ctrl+A"),
            QStringLiteral("↑ ↓"),
            QStringLiteral("PgUp PgDn"),
            QStringLiteral("Home End"),
            QStringLiteral("Tab Shift+Tab"),
        };
        for (int i = 0; i < expectedKeys.size(); ++i) {
            if (rows[i].first != expectedKeys[i]) {
                std::fprintf(stderr,
                    "row %d: got keys='%s', want '%s'\n",
                    i, rows[i].first.toUtf8().constData(),
                    expectedKeys[i].toUtf8().constData());
                fail("INV-2",
                    "key column mismatch — order must match the "
                    "kRoadmapShortcuts declaration.");
            }
            if (rows[i].second.isEmpty()) {
                fail("INV-2",
                    "action column must be non-empty (tr() of "
                    "QT_TR_NOOP body); empty hints the qualified "
                    "RoadmapDialog::tr() call is wired wrong.");
            }
        }
    }

    // INV-2 source-grep: kRoadmapShortcuts table + static_assert.
    {
        const std::string src = ants_test::slurpFile(ROADMAPDIALOG_CPP);
        if (src.empty())
            fail("Source-grep", "roadmapdialog.cpp not readable");
        if (!contains(src, "constexpr ShortcutRow kRoadmapShortcuts[]"))
            fail("INV-1",
                "kRoadmapShortcuts file-scope table missing");
        if (!contains(src, "static_assert(std::size(kRoadmapShortcuts) == 10"))
            fail("INV-1",
                "static_assert on kRoadmapShortcuts count missing — a "
                "new shortcut could land silently without updating "
                "the cheatsheet test.");
        // Literal-key prefixes — match the byte sequences the spec
        // pins. Unicode arrows (`↑ ↓`) are matched by their UTF-8
        // byte sequence, which is what C++ source files store.
        const char *keyPrefixes[] = {
            "{\"?\"",
            "{\"Esc\"",
            "{\"F5\"",
            "{\"Ctrl+C\"",
            "{\"Ctrl+A\"",
            "{\"↑ ↓\"",
            "{\"PgUp PgDn\"",
            "{\"Home End\"",
            "{\"Tab Shift+Tab\"",
        };
        for (const char *prefix : keyPrefixes) {
            if (!contains(src, prefix)) {
                std::fprintf(stderr,
                    "missing prefix in roadmapdialog.cpp: %s\n", prefix);
                fail("INV-2",
                    "expected literal key prefix not found");
            }
        }
        // keyPressEvent override on RoadmapDialog — verifies §3.d.
        if (!contains(src, "void RoadmapDialog::keyPressEvent("))
            fail("INV-3",
                "RoadmapDialog::keyPressEvent override missing — "
                "without it `?` would never reach the cheatsheet.");
        if (!contains(src, "event->text() == QLatin1String(\"?\")"))
            fail("INV-3",
                "layout-robust `event->text() == \"?\"` match missing — "
                "the spec rejects `Key_Question` because AltGr/dead-key "
                "layouts won't hit it.");
    }

    // INV-3 source-grep: header declares the override.
    {
        const std::string srch = ants_test::slurpFile(ROADMAPSHORTCUTSDIALOG_H);
        if (srch.empty())
            fail("Source-grep",
                "roadmapshortcutsdialog.h not readable");
        if (!contains(srch, "void keyPressEvent(QKeyEvent *event) override"))
            fail("INV-3",
                "RoadmapShortcutsDialog::keyPressEvent override missing "
                "— `?` on the cheatsheet itself must toggle it closed.");
        const std::string srcc = ants_test::slurpFile(ROADMAPSHORTCUTSDIALOG_CPP);
        if (srcc.empty())
            fail("Source-grep",
                "roadmapshortcutsdialog.cpp not readable");
        if (!contains(srcc, "event->text() == QLatin1String(\"?\")"))
            fail("INV-3",
                "RoadmapShortcutsDialog must also match by text() so "
                "the toggle is layout-robust on AltGr/dead-key paths.");
    }

    // -----------------------------------------------------------------
    // Live-widget assertions: construct a RoadmapDialog with a tmpdir
    // fixture, drive `?` via QTest::keyClick.
    // -----------------------------------------------------------------
    QTemporaryDir tmp;
    if (!tmp.isValid())
        fail("Setup", "QTemporaryDir construction failed");
    const QString roadmapPath = writeRoadmap(tmp, fixtureMarkdown());
    if (roadmapPath.isEmpty())
        fail("Setup", "could not write fixture ROADMAP.md");

    RoadmapDialog dialog(roadmapPath, QStringLiteral("light"));
    dialog.show();
    QApplication::processEvents();

    // INV-3 + INV-7: pressing `?` opens the cheatsheet; the window
    // title surfaces for screen readers.
    {
        QTest::keyClick(&dialog, Qt::Key_Question);
        QApplication::processEvents();
        auto *cheat = dialog.findChild<RoadmapShortcutsDialog *>();
        if (!cheat)
            fail("INV-3",
                "pressing `?` on RoadmapDialog did NOT create a "
                "RoadmapShortcutsDialog child.");
        if (!cheat->isVisible())
            fail("INV-3",
                "cheatsheet was created but is not visible — "
                "show()/raise()/activateWindow() chain misfired.");
        const QString expectedTitle = RoadmapShortcutsDialog::tr(
            "Roadmap Keyboard Shortcuts");
        if (cheat->windowTitle() != expectedTitle) {
            std::fprintf(stderr,
                "got title='%s', want '%s'\n",
                cheat->windowTitle().toUtf8().constData(),
                expectedTitle.toUtf8().constData());
            fail("INV-7",
                "window title must be tr(\"Roadmap Keyboard "
                "Shortcuts\") so screen readers announce it.");
        }
    }

    // INV-5: cheatsheet QTableWidget shape — 2 columns, 10 rows, row 0
    // contains `?` + the localised "Show this cheatsheet" string.
    {
        auto *cheat = dialog.findChild<RoadmapShortcutsDialog *>();
        if (!cheat) fail("INV-5", "cheatsheet child missing");
        auto *table = cheat->findChild<QTableWidget *>();
        if (!table)
            fail("INV-5",
                "cheatsheet must host a QTableWidget (not a "
                "QTextBrowser) — native widgets carry their own a11y "
                "per ANTS-1235.");
        if (table->columnCount() != 2)
            fail("INV-5",
                "table must have exactly 2 columns (Shortcut, Action)");
        const int expectedRows = roadmapShortcutRows().size();
        if (table->rowCount() != expectedRows) {
            std::fprintf(stderr,
                "got rowCount=%d, want %d\n",
                table->rowCount(), expectedRows);
            fail("INV-5",
                "table row count must equal the data table size — "
                "drift means the cheatsheet UI silently dropped a "
                "shortcut.");
        }
        auto *cellKeys   = table->item(0, 0);
        auto *cellAction = table->item(0, 1);
        if (!cellKeys || !cellAction)
            fail("INV-5", "row 0 has null cells");
        if (cellKeys->text() != QStringLiteral("?"))
            fail("INV-5",
                "row 0 column 0 must be `?` (the cheatsheet trigger "
                "documents itself).");
        const QString expectedAction = RoadmapDialog::tr(
            "Show this cheatsheet");
        if (cellAction->text() != expectedAction) {
            std::fprintf(stderr,
                "got action='%s', want '%s'\n",
                cellAction->text().toUtf8().constData(),
                expectedAction.toUtf8().constData());
            fail("INV-5",
                "action column must resolve through "
                "RoadmapDialog::tr() — strings live in that "
                "translation context (QT_TR_NOOP extraction in "
                "roadmapdialog.cpp).");
        }
    }

    // INV-3: pressing `?` on the cheatsheet closes it.
    {
        auto *cheat = dialog.findChild<RoadmapShortcutsDialog *>();
        if (!cheat) fail("INV-3", "cheatsheet child missing");
        QTest::keyClick(cheat, Qt::Key_Question);
        QApplication::processEvents();
        if (cheat->isVisible())
            fail("INV-3",
                "pressing `?` on the cheatsheet did NOT close it — "
                "the toggle contract is broken.");
    }

    // INV-6: re-opening reuses the same instance (QPointer identity).
    {
        auto *firstInstance = dialog.findChild<RoadmapShortcutsDialog *>();
        if (!firstInstance)
            fail("INV-6", "no cheatsheet instance after close");
        QTest::keyClick(&dialog, Qt::Key_Question);
        QApplication::processEvents();
        auto *secondInstance = dialog.findChild<RoadmapShortcutsDialog *>();
        if (secondInstance != firstInstance)
            fail("INV-6",
                "second open created a new instance — the "
                "QPointer-cached reuse contract is broken (lazy "
                "construction guard misfired).");
        if (!secondInstance->isVisible())
            fail("INV-6",
                "reused cheatsheet not visible on re-open.");
        // Close again so the search-box test below starts clean.
        secondInstance->close();
        QApplication::processEvents();
    }

    // INV-4: with the search box focused, `?` does NOT open the
    // cheatsheet — the key reaches the QLineEdit verbatim.
    {
        auto *searchBox = dialog.findChild<QLineEdit *>(
            QStringLiteral("roadmap-search-box"));
        if (!searchBox)
            fail("INV-4",
                "could not locate roadmap-search-box — has its "
                "objectName drifted?");
        const QString before = searchBox->text();
        searchBox->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
        // Key event goes to the focused widget — the QLineEdit.
        QTest::keyClick(searchBox, Qt::Key_Question);
        QApplication::processEvents();
        auto *cheat = dialog.findChild<RoadmapShortcutsDialog *>();
        if (cheat && cheat->isVisible())
            fail("INV-4",
                "pressing `?` while the search box was focused opened "
                "the cheatsheet — the focus guard misfired and the "
                "user can no longer type `?` into the substring "
                "filter.");
        if (!searchBox->text().contains(QLatin1Char('?')))
            fail("INV-4",
                "QLineEdit did NOT receive `?` as a text character "
                "— the focus path is broken even though the guard "
                "held.");
        // Sanity: text grew by exactly one `?`.
        if (searchBox->text() != before + QLatin1Char('?')) {
            std::fprintf(stderr,
                "before='%s', after='%s'\n",
                before.toUtf8().constData(),
                searchBox->text().toUtf8().constData());
            fail("INV-4",
                "QLineEdit text did not append exactly one `?` — "
                "something else swallowed the key.");
        }
    }

    std::fprintf(stderr, "OK — RoadmapDialog cheatsheet INVs hold.\n");
    return 0;
}

TEST(RoadmapShortcutsCheatsheet, Main) {
    ASSERT_EQ(0, runMain());
}
