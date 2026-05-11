// Feature-conformance test for tests/features/roadmap_dialog_cards/spec.md.
//
// Locks ANTS-1154 — v2 card-style RoadmapDialog renderer. Drives the
// pure-static helpers (parseBullets, renderCardsHtml, parseShippedDates)
// against synthetic markdown/changelog fixtures, plus source-greps
// roadmapdialog.cpp for the load-bearing INV anchors.
//
// Exit 0 = all invariants hold.

#include "roadmapdialog.h"

#include <QSet>
#include <QString>
#include <QTemporaryFile>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

// Mixed fixture — ✅, 🚧, 📋 bullets; one bullet has Layman: line,
// one bullet has no Kind: line, plus a non-status narration bullet
// and a prose-intro paragraph in one section. Exercises INV-3/4/5/11/12.
QString fixtureMarkdown() {
    return QStringLiteral(
        "# Sample Roadmap\n"
        "\n"
        "## 0.8.0 — feature delivery\n"
        "\n"
        "Prose intro paragraph describing the release. This should NOT\n"
        "appear under the History preset.\n"
        "\n"
        "### Features\n"
        "\n"
        "- ✅ [ANTS-9001] **Shipped widget.** Body text.\n"
        "  Layman: A new widget that users have asked for.\n"
        "  Kind: implement.\n"
        "\n"
        "- 🚧 [ANTS-9002] **In-progress refactor.** Body text.\n"
        "  Kind: refactor.\n"
        "\n"
        "- 📋 [ANTS-9003] **Planned no-kind bullet.** Body text.\n"
        "\n"
        "## 0.9.0 — far-future\n"
        "\n"
        "### Considered\n"
        "\n"
        "- 💭 [ANTS-9004] **Idea bullet.** Body.\n"
        "  Kind: research.\n"
        "\n"
        "## empty-section-only\n"
        "\n"
        "### no-bullets-here\n"
        "\n"
        "Just prose. No actionable bullets. Should fully suppress\n"
        "under non-Full presets.\n");
}

QString fixtureChangelog() {
    return QStringLiteral(
        "# Changelog\n"
        "\n"
        "## [Unreleased]\n"
        "\n"
        "- Work in progress on ANTS-9999.\n"
        "\n"
        "## [0.8.0] — 2026-05-11\n"
        "\n"
        "### Added\n"
        "\n"
        "- ANTS-9001 — Shipped widget.\n"
        "\n"
        "## [0.7.82] — 2026-05-10\n"
        "\n"
        "- Earlier work: ANTS-9000.\n");
}

constexpr unsigned kAllOn = 0x1F;  // Done | Planned | InProgress | Considered | Current

}  // namespace

static int runMain(int argc, char **argv) {
    (void)argc; (void)argv;
    using RD = RoadmapDialog;

    // INV-4 / parser: parseBullets exposes layman.
    {
        const auto bullets = RD::parseBullets(fixtureMarkdown());
        const RD::BulletRecord *with = nullptr;
        const RD::BulletRecord *without = nullptr;
        for (const auto &b : bullets) {
            if (b.id == QStringLiteral("ANTS-9001")) with = &b;
            else if (b.id == QStringLiteral("ANTS-9002")) without = &b;
        }
        if (!with) return fail("INV-4", "ANTS-9001 not parsed");
        if (with->layman.isEmpty())
            return fail("INV-4", "layman empty when Layman: line present");
        if (with->layman != QStringLiteral(
                "A new widget that users have asked for")) {
            std::fprintf(stderr, "got layman='%s'\n",
                         with->layman.toUtf8().constData());
            return fail("INV-4", "layman text doesn't match");
        }
        if (!without) return fail("INV-4", "ANTS-9002 not parsed");
        if (!without->layman.isEmpty())
            return fail("INV-4", "layman set when no Layman: line");
    }

    // INV-5 / parser: sectionSlug populated.
    {
        const auto bullets = RD::parseBullets(fixtureMarkdown());
        for (const auto &b : bullets) {
            if (b.id == QStringLiteral("ANTS-9001")) {
                if (b.sectionSlug != QStringLiteral("features")) {
                    std::fprintf(stderr, "got slug='%s'\n",
                                 b.sectionSlug.toUtf8().constData());
                    return fail("INV-5",
                        "ANTS-9001 sectionSlug != 'features'");
                }
            }
            if (b.id == QStringLiteral("ANTS-9004")) {
                if (b.sectionSlug != QStringLiteral("considered"))
                    return fail("INV-5",
                        "ANTS-9004 sectionSlug != 'considered'");
            }
        }
    }

    // INV-1 / INV-2 / renderer: render Full preset, check HTML shape.
    {
        RD::CardRenderOptions opts;
        opts.activePreset = RD::Preset::Full;
        opts.expandedSections.insert(QStringLiteral("features"));
        const QString html = RD::renderCardsHtml(
            fixtureMarkdown(), kAllOn, {}, QStringLiteral("light"),
            RD::SortOrder::Document, QString(), {}, opts);
        const std::string h = html.toStdString();
        if (!contains(h, "<div class=\"rm-card"))
            return fail("INV-1", "no rm-card div emitted");
        if (!contains(h, "id=\"rm-ANTS-9001\""))
            return fail("INV-1",
                "expected card id=\"rm-ANTS-9001\" not found");
        if (!contains(h, "class=\"rm-state\""))
            return fail("INV-2", "rm-state span missing");
        if (!contains(h, "class=\"rm-summary\""))
            return fail("INV-2", "rm-summary span missing");
        if (!contains(h, "class=\"rm-toggle\""))
            return fail("INV-2", "rm-toggle link missing");
        if (!contains(h, "ants://expand/ANTS-9001"))
            return fail("INV-2", "ants://expand URL missing");
    }

    // INV-3 / renderer: layman beats headline.
    {
        RD::CardRenderOptions opts;
        opts.activePreset = RD::Preset::Full;
        opts.expandedSections.insert(QStringLiteral("features"));
        const QString html = RD::renderCardsHtml(
            fixtureMarkdown(), kAllOn, {}, QStringLiteral("light"),
            RD::SortOrder::Document, QString(), {}, opts);
        const std::string h = html.toStdString();
        // The bullet with Layman: should show "A new widget...".
        if (!contains(h, "A new widget that users have asked for"))
            return fail("INV-3", "layman text not in render output");
        // ANTS-9002 has no Layman, so the headline appears (minus
        // any leading ANTS-NNNN — token; the test headline has no
        // such prefix so the raw text appears).
        if (!contains(h, "In-progress refactor"))
            return fail("INV-3", "headline fallback missing");
    }

    // INV-11 / renderer: History preset strips prose.
    {
        RD::CardRenderOptions opts;
        opts.activePreset = RD::Preset::History;
        const QString html = RD::renderCardsHtml(
            fixtureMarkdown(),
            /*filter=*/0x01,  // ShowDone only
            {}, QStringLiteral("light"),
            RD::SortOrder::Document, QString(), {}, opts);
        const std::string h = html.toStdString();
        if (contains(h, "Prose intro paragraph"))
            return fail("INV-11",
                "prose narration rendered under History preset");
        if (contains(h, "Just prose. No actionable bullets"))
            return fail("INV-11",
                "non-status section prose rendered under History");
    }

    // INV-12 / renderer: empty sections suppressed on non-Full.
    {
        RD::CardRenderOptions opts;
        opts.activePreset = RD::Preset::History;
        const QString html = RD::renderCardsHtml(
            fixtureMarkdown(),
            /*filter=*/0x01,  // ShowDone only
            {}, QStringLiteral("light"),
            RD::SortOrder::Document, QString(), {}, opts);
        const std::string h = html.toStdString();
        // "empty-section-only" has no ✅ bullets — its header MUST
        // be suppressed.
        if (contains(h, "empty-section-only"))
            return fail("INV-12",
                "empty section header rendered under History");
        // 0.9.0 has only 💭, no ✅ — its header MUST also be
        // suppressed when filter is ShowDone only.
        if (contains(h, "0.9.0 — far-future"))
            return fail("INV-12",
                "0.9.0 section header rendered when no ✅ inside");
    }

    // INV-1 / INV-4 source-grep — anchor comments live next to the
    // load-bearing strings.
    {
        const std::string src = slurp(ROADMAPDIALOG_CPP);
        if (src.empty())
            return fail("INV-1", "roadmapdialog.cpp not readable");
        if (!contains(src, "// ANTS-1154-INV-1"))
            return fail("INV-1", "// ANTS-1154-INV-1 anchor missing");
        if (!contains(src, "// ANTS-1154-INV-4"))
            return fail("INV-4", "// ANTS-1154-INV-4 anchor missing");
        // Anchor proximity — INV-1 should be within ~200 chars of
        // the `<div class=\"rm-card` literal in the source file. The
        // slurped file contains the escaped form (backslash + quote
        // pair) because the renderer uses `QStringLiteral("<div
        // class=\"rm-card...\"")`.
        const auto anchorPos = src.find("// ANTS-1154-INV-1");
        const auto literalPos = src.find("class=\\\"rm-card");
        if (anchorPos == std::string::npos ||
                literalPos == std::string::npos)
            return fail("INV-1", "could not locate both literals");
        const size_t gap =
            anchorPos < literalPos
                ? literalPos - anchorPos
                : anchorPos - literalPos;
        if (gap > 200)
            return fail("INV-1",
                "anchor far from load-bearing rm-card emission");
    }

    // ShippedDates — parseShippedDates maps ✅ IDs to release dates.
    {
        QTemporaryFile tmp;
        if (!tmp.open()) return fail("ShippedDates", "tmpfile failed");
        tmp.write(fixtureChangelog().toUtf8());
        tmp.flush();
        const auto dates = RD::parseShippedDates(tmp.fileName());
        if (dates.value(QStringLiteral("ANTS-9001"))
                != QStringLiteral("2026-05-11"))
            return fail("ShippedDates",
                "ANTS-9001 should map to 2026-05-11");
        if (dates.value(QStringLiteral("ANTS-9000"))
                != QStringLiteral("2026-05-10"))
            return fail("ShippedDates",
                "ANTS-9000 should map to 2026-05-10");
        // [Unreleased] section has no date — IDs in it must NOT map.
        if (dates.contains(QStringLiteral("ANTS-9999")))
            return fail("ShippedDates",
                "ANTS-9999 from [Unreleased] should not appear");
    }

    std::fprintf(stderr, "OK — RoadmapDialog v2 card INVs hold.\n");
    return 0;
}

TEST(RoadmapDialogCards, Main) {
    if (runMain(0, nullptr) != 0) FAIL();
}
