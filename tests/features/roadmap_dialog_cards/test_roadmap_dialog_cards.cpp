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

// ANTS-1239 — duplicate h3 fixture. Three "Performance" h3s under
// three different version h2s, each with one bullet. parseBullets
// MUST produce three distinct sectionSlugs (performance,
// performance-2, performance-3); renderCardsHtml MUST emit the
// matching `ants://expand-section/<slug>` URLs so each Performance
// section toggles independently.
QString fixtureDuplicateHeadings() {
    return QStringLiteral(
        "# Sample Roadmap\n"
        "\n"
        "## 0.7.0 — release\n"
        "\n"
        "### Performance\n"
        "\n"
        "- ✅ [ANTS-7001] **First perf bullet.** Body.\n"
        "  Kind: implement.\n"
        "\n"
        "## 0.8.0 — release\n"
        "\n"
        "### Performance\n"
        "\n"
        "- ✅ [ANTS-7002] **Second perf bullet.** Body.\n"
        "  Kind: implement.\n"
        "\n"
        "## Beyond 1.0\n"
        "\n"
        "### Performance\n"
        "\n"
        "- ✅ [ANTS-7003] **Third perf bullet.** Body.\n"
        "  Kind: implement.\n");
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

    // ANTS-1239 — unique slugs for duplicate headings. parseBullets
    // and renderCardsHtml must walk in lockstep, producing the same
    // sequence performance / performance-2 / performance-3 so the
    // bySection[slug] keys match the slugs emitted into the click
    // URLs.
    {
        const auto bullets = RD::parseBullets(fixtureDuplicateHeadings());
        const RD::BulletRecord *b1 = nullptr;
        const RD::BulletRecord *b2 = nullptr;
        const RD::BulletRecord *b3 = nullptr;
        for (const auto &b : bullets) {
            if (b.id == QStringLiteral("ANTS-7001")) b1 = &b;
            else if (b.id == QStringLiteral("ANTS-7002")) b2 = &b;
            else if (b.id == QStringLiteral("ANTS-7003")) b3 = &b;
        }
        if (!b1 || !b2 || !b3)
            return fail("UniqueSlugs", "missing one of ANTS-7001/02/03");
        if (b1->sectionSlug != QStringLiteral("performance")) {
            std::fprintf(stderr, "got slug1='%s'\n",
                         b1->sectionSlug.toUtf8().constData());
            return fail("UniqueSlugs",
                "first Performance h3 slug != 'performance'");
        }
        if (b2->sectionSlug != QStringLiteral("performance-2")) {
            std::fprintf(stderr, "got slug2='%s'\n",
                         b2->sectionSlug.toUtf8().constData());
            return fail("UniqueSlugs",
                "second Performance h3 slug != 'performance-2'");
        }
        if (b3->sectionSlug != QStringLiteral("performance-3")) {
            std::fprintf(stderr, "got slug3='%s'\n",
                         b3->sectionSlug.toUtf8().constData());
            return fail("UniqueSlugs",
                "third Performance h3 slug != 'performance-3'");
        }
    }
    {
        RD::CardRenderOptions opts;
        opts.activePreset = RD::Preset::Full;
        // Expand only the *second* Performance section. With unique
        // slugs, only ANTS-7002 should render as a card; the first
        // and third must stay collapsed.
        opts.expandedSections.insert(QStringLiteral("performance-2"));
        const QString html = RD::renderCardsHtml(
            fixtureDuplicateHeadings(), kAllOn, {}, QStringLiteral("light"),
            RD::SortOrder::Document, QString(), {}, opts);
        const std::string h = html.toStdString();
        if (!contains(h, "ants://expand-section/performance\""))
            return fail("UniqueSlugs",
                "missing expand URL for performance (1st)");
        if (!contains(h, "ants://collapse-section/performance-2\""))
            return fail("UniqueSlugs",
                "missing collapse URL for performance-2 (the open one)");
        if (!contains(h, "ants://expand-section/performance-3\""))
            return fail("UniqueSlugs",
                "missing expand URL for performance-3 (3rd)");
        if (!contains(h, "id=\"rm-ANTS-7002\""))
            return fail("UniqueSlugs",
                "ANTS-7002 card not rendered when its slug expanded");
        if (contains(h, "id=\"rm-ANTS-7001\""))
            return fail("UniqueSlugs",
                "ANTS-7001 (1st Perf) leaked into expanded view");
        if (contains(h, "id=\"rm-ANTS-7003\""))
            return fail("UniqueSlugs",
                "ANTS-7003 (3rd Perf) leaked into expanded view");
    }

    // ANTS-1239 — section-header <a> tags carry an explicit color.
    // Source-grep guards against a regression to `color:inherit`
    // which Qt's text engine doesn't honor for <a> (it falls back
    // to QPalette::Link), leaving the section titles unreadable on
    // dark themes.
    {
        const std::string src = slurp(ROADMAPDIALOG_CPP);
        if (src.empty())
            return fail("LinkColor", "roadmapdialog.cpp not readable");
        if (!contains(src, ".rm-section-title{color:%2;"))
            return fail("LinkColor",
                ".rm-section-title must set explicit color (not inherit)");
        if (!contains(src, ".rm-section-toggle{color:%2;"))
            return fail("LinkColor",
                ".rm-section-toggle must set explicit color");
        if (!contains(src, "QPalette::Link"))
            return fail("LinkColor",
                "viewer palette must set QPalette::Link from theme");
    }

    // ANTS-1240 — INV-15: dialog/viewer/TOC palette themed from
    // bgPrimary/textPrimary. Source-grep guards against a regression
    // that drops the theme application (e.g. leaving only Link/
    // LinkVisited as in the 0.7.83 ship state). We look for the
    // four load-bearing palette roles on the three widgets.
    {
        const std::string src = slurp(ROADMAPDIALOG_CPP);
        if (src.empty())
            return fail("DialogPalette", "roadmapdialog.cpp not readable");
        // Dialog gets Window + Base from theme.
        if (!contains(src, "dp.setColor(QPalette::Window, th.bgPrimary)"))
            return fail("DialogPalette",
                "dialog must set QPalette::Window from theme bgPrimary");
        if (!contains(src, "dp.setColor(QPalette::Base, th.bgPrimary)"))
            return fail("DialogPalette",
                "dialog must set QPalette::Base from theme bgPrimary");
        // Viewer gets Base + Text from theme.
        if (!contains(src, "vp.setColor(QPalette::Base, th.bgPrimary)"))
            return fail("DialogPalette",
                "m_viewer must set QPalette::Base from theme bgPrimary");
        if (!contains(src, "vp.setColor(QPalette::Text, th.textPrimary)"))
            return fail("DialogPalette",
                "m_viewer must set QPalette::Text from theme textPrimary");
        // TOC gets Base + Highlight from theme.
        if (!contains(src, "tp.setColor(QPalette::Base, th.bgPrimary)"))
            return fail("DialogPalette",
                "m_toc must set QPalette::Base from theme bgPrimary");
        if (!contains(src, "tp.setColor(QPalette::Highlight, th.accent)"))
            return fail("DialogPalette",
                "m_toc must set QPalette::Highlight from theme accent");
    }

    // ANTS-1240 — INV-16: expanded body is emitted as direct <p>
    // children of the card (no `<div class="rm-body">` wrapper), so
    // Qt's text engine doesn't paint a nested block frame with
    // QPalette::Base over the card's bgSecondary background. Verify
    // both via render output (no rm-body div, but the new classes
    // appear) and via source-grep (the CSS rules exist).
    {
        RD::CardRenderOptions opts;
        opts.activePreset = RD::Preset::Full;
        opts.expandedSections.insert(QStringLiteral("features"));
        opts.expandedItems.insert(QStringLiteral("ANTS-9001"));
        const QString html = RD::renderCardsHtml(
            fixtureMarkdown(), kAllOn, {}, QStringLiteral("light"),
            RD::SortOrder::Document, QString(), {}, opts);
        const std::string h = html.toStdString();
        // No nested wrapper div.
        if (contains(h, "<div class=\"rm-body\">"))
            return fail("BodyFrame",
                "expanded card still wraps body in <div class=\"rm-body\">");
        // First body line carries rm-body-first class.
        if (!contains(h, "class=\"rm-body-first\""))
            return fail("BodyFrame",
                "first body <p> missing rm-body-first class");
    }
    {
        const std::string src = slurp(ROADMAPDIALOG_CPP);
        if (src.empty())
            return fail("BodyFrame", "roadmapdialog.cpp not readable");
        if (!contains(src, ".rm-body-first{padding-top:4px;"))
            return fail("BodyFrame",
                ".rm-body-first CSS rule missing in renderer");
        if (!contains(src, ".rm-body-line{padding-left:20px;"))
            return fail("BodyFrame",
                ".rm-body-line CSS rule missing in renderer");
        // The old wrapper rule MUST NOT regress back in.
        if (contains(src, ".rm-body{padding-top"))
            return fail("BodyFrame",
                "old .rm-body{...} wrapper rule reappeared — Qt paints a "
                "nested block frame over the card bg");
    }

    // ANTS-1241 — INV-17: card hashed-id + shipped date are inline
    // spans on the summary row (before the rm-toggle anchor), NOT
    // wrapped in `<div class="rm-meta">`. Same Qt nested-block
    // QPalette::Base rationale as INV-16; the old meta div painted
    // a darker band under the ID on dark themes. Also verifies the
    // CSS font-size bump (10 px → 12 px) and that the rm-id span
    // appears in the HTML stream BEFORE the rm-toggle anchor so it
    // visually lands at the end of the summary line rather than
    // after the toggle.
    {
        RD::CardRenderOptions opts;
        opts.activePreset = RD::Preset::Full;
        // Section must be expanded for the card bullets to render.
        opts.expandedSections.insert(QStringLiteral("features"));
        // ANTS-9001 is ✅ in the fixture; feed a shippedDate so the
        // rm-date span emission path is also exercised.
        opts.shippedDates.insert(QStringLiteral("ANTS-9001"),
                                 QStringLiteral("2026-05-11"));
        const QString html = RD::renderCardsHtml(
            fixtureMarkdown(), kAllOn, {}, QStringLiteral("light"),
            RD::SortOrder::Document, QString(), {}, opts);
        const std::string h = html.toStdString();
        // No nested meta div.
        if (contains(h, "<div class=\"rm-meta\">"))
            return fail("IdInline",
                "card still wraps id in <div class=\"rm-meta\">");
        // Inline rm-id span with #NNNN MUST be present for ANTS-9001.
        if (!contains(h, "<span class=\"rm-id\">#9001</span>"))
            return fail("IdInline",
                "expected inline <span class=\"rm-id\">#9001</span> not found");
        // rm-id span MUST appear before rm-toggle anchor in the
        // stream (positioning it at the end of the summary line,
        // ahead of the toggle).
        const auto idPos = h.find("<span class=\"rm-id\">#9001</span>");
        const auto togglePos = h.find("ants://expand/ANTS-9001");
        if (idPos == std::string::npos || togglePos == std::string::npos ||
            idPos >= togglePos)
            return fail("IdInline",
                "rm-id span MUST precede the rm-toggle anchor for ANTS-9001");
        // Shipped date renders inline as well (ANTS-9001 is ✅ and
        // shippedDates was populated above).
        if (!contains(h, "<span class=\"rm-date\">· 2026-05-11</span>"))
            return fail("IdInline",
                "expected inline <span class=\"rm-date\"> for shipped card");
    }
    {
        const std::string src = slurp(ROADMAPDIALOG_CPP);
        if (src.empty())
            return fail("IdInline", "roadmapdialog.cpp not readable");
        if (!contains(src, ".rm-id{font-family:monospace;font-size:12px;"))
            return fail("IdInline",
                ".rm-id CSS rule must declare font-size:12px (was 10px on "
                "the old meta row)");
        // Old meta wrapper rule MUST NOT regress.
        if (contains(src, ".rm-meta{font-size:10px"))
            return fail("IdInline",
                "old .rm-meta{font-size:10px;...} rule reappeared — Qt "
                "paints a nested block frame over the card bg");
        // The old `<div class="rm-meta">` emit site MUST NOT regress
        // back into the renderer source either.
        if (contains(src, "<div class=\\\"rm-meta\\\">"))
            return fail("IdInline",
                "renderer still emits <div class=\"rm-meta\"> wrapper");
    }

    std::fprintf(stderr, "OK — RoadmapDialog v2 card INVs hold.\n");
    return 0;
}

TEST(RoadmapDialogCards, Main) {
    if (runMain(0, nullptr) != 0) FAIL();
}
