// Feature-conformance test for tests/features/roadmap_kind_facets/spec.md.
//
// Locks ANTS-1106 — Kind-faceted filter in the RoadmapDialog viewer.
// Drives RoadmapDialog::renderCardsHtml (every section expanded) with synthetic 3-bullet markdown
// across the four filter shapes (empty, single, multi, missing-Kind),
// plus source-greps roadmapdialog.{h,cpp} for the new parameter, the
// Kind-filter UI row, and the per-checkbox objectNames.
//
// Exit 0 = all invariants hold.

#include "roadmapdialog.h"

#include <QApplication>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <string>


#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include "../../_support/roadmap_cards.h"
namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    ADD_FAILURE_AT(__FILE__, __LINE__) << "[" << label << "] " << why;
    return 1;
}

// Synthetic 3-bullet markdown — one each of implement / fix / doc.
// Kind: lines mirror the convention used across the live ROADMAP:
// a continuation line whose first non-whitespace token is `Kind:`.
QString sampleMarkdown() {
    return QStringLiteral(
        "# Sample\n"
        "\n"
        "## Section\n"
        "\n"
        "- 📋 [ANTS-9001] **First implement bullet.**\n"
        "  Body line.\n"
        "  Kind: implement. Source: test.\n"
        "\n"
        "- 📋 [ANTS-9002] **Second fix bullet.**\n"
        "  Body line.\n"
        "  Kind: fix. Source: test.\n"
        "\n"
        "- 📋 [ANTS-9003] **Third doc bullet.**\n"
        "  Body line.\n"
        "  Kind: doc. Source: test.\n"
    );
}

constexpr unsigned kAllOn = 0x1F;  // ShowDone | Planned | InProgress | Considered | Current

}  // namespace

static int runMain() {
    // QApplication app(argc, argv);  // ANTS-1217: bundle_main creates the app

    // INV-1: renderCardsHtml takes the kindFilter parameter, as a
    // QSet<QString>. ANTS-1263 — anchored on the renderer's own
    // declaration; a bare "kindFilter" also matches m_kindFilter.
    {
        const std::string hdr = ants_test::slurpFile(ROADMAPDIALOG_H);
        if (hdr.empty()) fail("INV-1", "roadmapdialog.h not readable");
        const auto decl = hdr.find("static QString renderCardsHtml(");
        const auto end = hdr.find(");", decl);
        if (decl == std::string::npos || end == std::string::npos
            || !contains(hdr.substr(decl, end - decl),
                         "const QSet<QString> &kindFilter"))
            fail("INV-1",
                "renderCardsHtml must take const QSet<QString> &kindFilter");
    }

    // INV-2: empty filter — all three bullets pass.
    {
        const QString html = ants_test::cardsAllOpen(
            sampleMarkdown(), kAllOn, {},
            QStringLiteral("default"),
            RoadmapDialog::SortOrder::Document, QString(), {});
        if (!html.contains(QStringLiteral("First implement bullet")))
            fail("INV-2", "implement bullet missing under empty filter");
        if (!html.contains(QStringLiteral("Second fix bullet")))
            fail("INV-2", "fix bullet missing under empty filter");
        if (!html.contains(QStringLiteral("Third doc bullet")))
            fail("INV-2", "doc bullet missing under empty filter");
    }

    // INV-3: filter = {"implement"} — only the implement bullet survives.
    {
        const QSet<QString> kindFilter = {QStringLiteral("implement")};
        const QString html = ants_test::cardsAllOpen(
            sampleMarkdown(), kAllOn, {},
            QStringLiteral("default"),
            RoadmapDialog::SortOrder::Document, QString(), kindFilter);
        if (!html.contains(QStringLiteral("First implement bullet")))
            fail("INV-3", "implement bullet should survive filter");
        if (html.contains(QStringLiteral("Second fix bullet")))
            fail("INV-3", "fix bullet leaked through implement filter");
        if (html.contains(QStringLiteral("Third doc bullet")))
            fail("INV-3", "doc bullet leaked through implement filter");
    }

    // INV-4: filter = {"fix", "doc"} — fix + doc survive, implement dropped.
    {
        const QSet<QString> kindFilter = {
            QStringLiteral("fix"), QStringLiteral("doc")};
        const QString html = ants_test::cardsAllOpen(
            sampleMarkdown(), kAllOn, {},
            QStringLiteral("default"),
            RoadmapDialog::SortOrder::Document, QString(), kindFilter);
        if (html.contains(QStringLiteral("First implement bullet")))
            fail("INV-4", "implement bullet leaked through {fix,doc}");
        if (!html.contains(QStringLiteral("Second fix bullet")))
            fail("INV-4", "fix bullet should survive {fix,doc}");
        if (!html.contains(QStringLiteral("Third doc bullet")))
            fail("INV-4", "doc bullet should survive {fix,doc}");
    }

    // INV-5: bullet with no Kind: line is excluded under non-empty filter.
    {
        const QString markdownWithUnclassified = QStringLiteral(
            "# Sample\n"
            "\n"
            "## Section\n"
            "\n"
            "- 📋 [ANTS-9001] **Classified.**\n"
            "  Body line.\n"
            "  Kind: implement. Source: test.\n"
            "\n"
            "- 📋 [ANTS-9002] **Unclassified.**\n"
            "  Body without a Kind line.\n"
            "  Source: test.\n"
        );
        const QSet<QString> kindFilter = {QStringLiteral("implement")};
        const QString html = ants_test::cardsAllOpen(
            markdownWithUnclassified, kAllOn, {},
            QStringLiteral("default"),
            RoadmapDialog::SortOrder::Document, QString(), kindFilter);
        // ANTS-1263 — match the card rows: "Classified" is a substring
        // of "Unclassified".
        if (!html.contains(QStringLiteral("id=\"rm-ANTS-9001\"")))
            fail("INV-5", "classified bullet should survive");
        if (html.contains(QStringLiteral("id=\"rm-ANTS-9002\"")))
            fail("INV-5",
                "unclassified bullet should be excluded under non-empty filter");
    }

    // INV-6 + INV-7: the dialog adds a Kind-filter row with QLabel +
    // checkboxes; each checkbox carries a stable objectName.
    {
        const std::string src = ants_test::slurpFile(ROADMAPDIALOG_CPP);
        if (src.empty()) fail("INV-6", "roadmapdialog.cpp not readable");
        if (!contains(src, "Kind:"))
            fail("INV-6", "Kind: row label missing in dialog setup");
        if (!contains(src, "roadmap-filter-kind-implement"))
            fail("INV-7",
                "missing objectName roadmap-filter-kind-implement");
        if (!contains(src, "roadmap-filter-kind-fix"))
            fail("INV-7",
                "missing objectName roadmap-filter-kind-fix");
        if (!contains(src, "roadmap-filter-kind-doc"))
            fail("INV-7",
                "missing objectName roadmap-filter-kind-doc");
    }

    std::fprintf(stderr,
        "OK — roadmap Kind-facet filter INVs hold.\n");
    return 0;
}

TEST(RoadmapKindFacets, Main) {
    ASSERT_EQ(0, runMain());
}
