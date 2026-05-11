// Feature-conformance test for tests/features/roadmap_density/spec.md.
//
// Locks ANTS-1238 — RoadmapDialog density toggle. Nine invariants
// across three layers:
//   - Renderer (INV-1 / INV-2 / INV-6 / INV-8): static
//     renderCardsHtml + tier substitution.
//   - Config (INV-3 / INV-4): persistence round-trip + fallback.
//   - Dialog (INV-5 / INV-7 / INV-9): combo a11y, state
//     preservation, persistence-failure path.
//
// Exit 0 = all invariants hold. Diagnostics on first failure
// name the INV ID + the specific failure mode.

#include "config.h"
#include "roadmapdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

#include <gtest/gtest.h>

namespace {

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

QString fixtureMarkdown() {
    return QStringLiteral(
        "# Sample Roadmap\n"
        "\n"
        "## 0.8.0\n"
        "\n"
        "- 🚧 [ANTS-9999] **Active card.**\n"
        "  Layman: in-progress item.\n"
        "  Kind: implement.\n"
        "\n"
        "- ✅ [ANTS-9998] **Shipped card.**\n"
        "  Layman: shipped item.\n"
        "  Kind: fix.\n");
}

RoadmapDialog::CardRenderOptions makeOpts(RoadmapDialog::Density d) {
    RoadmapDialog::CardRenderOptions opts;
    opts.density = d;
    opts.expandedSections.insert(QStringLiteral("0-8-0"));
    return opts;
}

QString renderAt(RoadmapDialog::Density d) {
    return RoadmapDialog::renderCardsHtml(
        fixtureMarkdown(),
        /*filter*/0x0F,
        /*currentBullets*/QStringList(),
        QStringLiteral("light"),
        RoadmapDialog::SortOrder::Document,
        QString(), {}, makeOpts(d));
}

QString stripStyleBlock(QString html) {
    static const QRegularExpression rx(
        QStringLiteral("<style>.*?</style>"),
        QRegularExpression::DotMatchesEverythingOption);
    html.replace(rx, QString());
    return html;
}

}  // namespace

static int runMain() {
    // ----- INV-1: default CardRenderOptions{} == explicit Cozy -----
    {
        RoadmapDialog::CardRenderOptions def;  // default-ctor
        def.expandedSections.insert(QStringLiteral("0-8-0"));
        RoadmapDialog::CardRenderOptions cozy;
        cozy.density = RoadmapDialog::Density::Cozy;
        cozy.expandedSections.insert(QStringLiteral("0-8-0"));
        const QString hDef = RoadmapDialog::renderCardsHtml(
            fixtureMarkdown(), 0x0F, QStringList(),
            QStringLiteral("light"),
            RoadmapDialog::SortOrder::Document,
            QString(), {}, def);
        const QString hCozy = RoadmapDialog::renderCardsHtml(
            fixtureMarkdown(), 0x0F, QStringList(),
            QStringLiteral("light"),
            RoadmapDialog::SortOrder::Document,
            QString(), {}, cozy);
        if (hDef != hCozy)
            return fail("INV-1",
                "Default CardRenderOptions{} render is not byte-equal "
                "to explicit Density::Cozy render — the default field "
                "value drifted away from Cozy.");
    }

    // ----- INV-2 + INV-8: three tiers pairwise distinct + sentinels -----
    {
        const QString hCompact     = renderAt(RoadmapDialog::Density::Compact);
        const QString hCozy        = renderAt(RoadmapDialog::Density::Cozy);
        const QString hComfortable = renderAt(RoadmapDialog::Density::Comfortable);

        // INV-2a: pairwise distinct.
        if (hCompact == hCozy)
            return fail("INV-2a",
                "Compact and Cozy renders are byte-equal — the tier "
                "substitution isn't reaching the CSS.");
        if (hCozy == hComfortable)
            return fail("INV-2a",
                "Cozy and Comfortable renders are byte-equal — "
                "Comfortable substitution isn't firing.");
        if (hCompact == hComfortable)
            return fail("INV-2a",
                "Compact and Comfortable renders are byte-equal — "
                "tier table degenerate.");

        // INV-2b: tier-unique sentinels.
        // Compact uses 9px for .rm-state-label + meta floor; no
        // other tier emits 9.
        if (!hCompact.contains(QStringLiteral("font-size:9px")))
            return fail("INV-2b",
                "Compact render missing `font-size:9px` sentinel — "
                "label/meta floor isn't being emitted.");
        if (hCozy.contains(QStringLiteral("font-size:9px")))
            return fail("INV-2b",
                "Cozy render contains `font-size:9px` — sentinel "
                "is not Compact-unique.");
        if (hComfortable.contains(QStringLiteral("font-size:9px")))
            return fail("INV-2b",
                "Comfortable render contains `font-size:9px` — "
                "sentinel is not Compact-unique.");
        // Cozy uses 16px for h1 only.
        if (!hCozy.contains(QStringLiteral("font-size:16px")))
            return fail("INV-2b",
                "Cozy render missing `font-size:16px` sentinel — "
                "h1 isn't being emitted at Cozy.");
        if (hCompact.contains(QStringLiteral("font-size:16px")))
            return fail("INV-2b",
                "Compact render contains `font-size:16px` — "
                "sentinel is not Cozy-unique.");
        if (hComfortable.contains(QStringLiteral("font-size:16px")))
            return fail("INV-2b",
                "Comfortable render contains `font-size:16px` — "
                "sentinel is not Cozy-unique.");
        // Comfortable uses 18px for h1.
        if (!hComfortable.contains(QStringLiteral("font-size:18px")))
            return fail("INV-2b",
                "Comfortable render missing `font-size:18px` "
                "sentinel — h1 isn't being emitted at Comfortable.");
        if (hCompact.contains(QStringLiteral("font-size:18px")))
            return fail("INV-2b",
                "Compact render contains `font-size:18px` — "
                "sentinel is not Comfortable-unique.");
        if (hCozy.contains(QStringLiteral("font-size:18px")))
            return fail("INV-2b",
                "Cozy render contains `font-size:18px` — sentinel "
                "is not Comfortable-unique.");

        // INV-8: 9px floor — no tier emits font-size below 9.
        static const QRegularExpression rxPx(
            QStringLiteral("font-size:([0-9]+)px"));
        auto checkFloor = [&](const QString &html,
                              const char *tier) -> int {
            auto it = rxPx.globalMatch(html);
            while (it.hasNext()) {
                const int px = it.next().captured(1).toInt();
                if (px < 9) {
                    std::fprintf(stderr,
                        "[INV-8] FAIL: %s render emitted "
                        "font-size:%dpx — below the 9px floor.\n",
                        tier, px);
                    return 1;
                }
            }
            return 0;
        };
        if (checkFloor(hCompact,     "Compact"))     return 1;
        if (checkFloor(hCozy,        "Cozy"))        return 1;
        if (checkFloor(hComfortable, "Comfortable")) return 1;
    }

    // ----- INV-6: content unaffected (strip <style>, compare rest) -----
    {
        const QString sCompact = stripStyleBlock(
            renderAt(RoadmapDialog::Density::Compact));
        const QString sCozy = stripStyleBlock(
            renderAt(RoadmapDialog::Density::Cozy));
        const QString sComfortable = stripStyleBlock(
            renderAt(RoadmapDialog::Density::Comfortable));
        if (sCompact != sCozy)
            return fail("INV-6",
                "Compact and Cozy renders differ outside the <style> "
                "block — density is leaking into card content.");
        if (sCozy != sComfortable)
            return fail("INV-6",
                "Cozy and Comfortable renders differ outside the "
                "<style> block — density is leaking into card "
                "content.");
    }

    // ----- INV-3 + INV-4: Config round-trip + fallback -----
    // Redirect XDG_CONFIG_HOME so Config writes into a temp dir.
    QTemporaryDir tempCfg;
    if (!tempCfg.isValid())
        return fail("INV-3", "could not create QTemporaryDir for Config");
    qputenv("XDG_CONFIG_HOME", tempCfg.path().toUtf8());
    // Reset Qt's cached config location.
    QStandardPaths::setTestModeEnabled(false);
    {
        Config cfg;
        // Default before any write should be "cozy".
        if (cfg.roadmapDensity() != QStringLiteral("cozy"))
            return fail("INV-4",
                "First-run Config::roadmapDensity() was not \"cozy\" — "
                "default-value path is wrong.");
        // Round-trip each known value.
        for (const QString &name : {QStringLiteral("compact"),
                                     QStringLiteral("cozy"),
                                     QStringLiteral("comfortable")}) {
            cfg.setRoadmapDensity(name);
            if (cfg.roadmapDensity() != name) {
                std::fprintf(stderr,
                    "[INV-3] FAIL: round-trip for \"%s\" returned "
                    "\"%s\".\n",
                    qUtf8Printable(name),
                    qUtf8Printable(cfg.roadmapDensity()));
                return 1;
            }
        }
        // Reset to "cozy" so the unknown-value tests below start
        // from a known baseline.
        cfg.setRoadmapDensity(QStringLiteral("cozy"));
        // Unknown values are silently dropped on setter — the
        // getter should keep returning "cozy".
        for (const QString &bad : {QStringLiteral(""),
                                    QStringLiteral("humungous"),
                                    QStringLiteral("💀"),
                                    QStringLiteral("COMPACT")}) {
            cfg.setRoadmapDensity(bad);
            if (cfg.roadmapDensity() != QStringLiteral("cozy")) {
                std::fprintf(stderr,
                    "[INV-4] FAIL: writing \"%s\" then reading "
                    "returned \"%s\" — expected \"cozy\".\n",
                    qUtf8Printable(bad),
                    qUtf8Printable(cfg.roadmapDensity()));
                return 1;
            }
        }
    }

    // ----- INV-7: density combo accessibleName -----
    // Construct a RoadmapDialog with the temp Config; verify combo
    // a11y. The dialog needs a ROADMAP.md on disk to start its
    // file watcher; point it at the fixture file.
    QTemporaryDir tempRoadmap;
    if (!tempRoadmap.isValid())
        return fail("INV-7", "could not create QTemporaryDir for fixture");
    const QString roadmapPath = tempRoadmap.path() + QStringLiteral("/ROADMAP.md");
    {
        QFile f(roadmapPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return fail("INV-7", "could not write fixture roadmap");
        f.write(fixtureMarkdown().toUtf8());
    }
    {
        Config cfg;
        RoadmapDialog dlg(roadmapPath, QStringLiteral("light"),
                          nullptr, &cfg);
        QComboBox *combo = dlg.findChild<QComboBox *>(
            QStringLiteral("roadmap-density-combo"));
        if (!combo)
            return fail("INV-7",
                "Density combo not found by objectName "
                "\"roadmap-density-combo\" — ctor wiring is broken.");
        if (combo->accessibleName() != QStringLiteral("Roadmap card density"))
            return fail("INV-7",
                "Density combo accessibleName drifted away from "
                "\"Roadmap card density\".");

        // INV-5: state preservation across density change. Seed
        // search text + flip a filter checkbox, then change combo,
        // assert state unchanged.
        QLineEdit *search = dlg.findChild<QLineEdit *>(
            QStringLiteral("roadmap-search-box"));
        if (!search)
            return fail("INV-5", "search box not found");
        search->setText(QStringLiteral("sample-density-probe"));

        QCheckBox *filterDone = dlg.findChild<QCheckBox *>(
            QStringLiteral("roadmap-filter-done"));
        if (!filterDone)
            return fail("INV-5", "filter-done checkbox not found");
        const bool doneBefore = filterDone->isChecked();
        filterDone->setChecked(!doneBefore);  // flip
        const bool doneSeed = filterDone->isChecked();

        // Trigger density change.
        const int origIdx = combo->currentIndex();
        const int newIdx = (origIdx == 0) ? 2 : 0;
        combo->setCurrentIndex(newIdx);

        if (search->text() != QStringLiteral("sample-density-probe"))
            return fail("INV-5",
                "search text was lost across a density change — "
                "rebuild() is clobbering the search box.");
        if (filterDone->isChecked() != doneSeed)
            return fail("INV-5",
                "filter-done checkbox state was lost across a "
                "density change — rebuild() is resetting filters.");
    }

    // ----- INV-9: persistence-failure path -----
    // Make the parent directory read-only (0500) — Config::save()
    // writes via tmpfile + rename, both of which require write
    // permission on the *directory*, not just the target file. A
    // chmod 0400 on the file alone is insufficient: rename(2)
    // respects directory perms, not file perms.
    {
        Config cfg;
        cfg.setRoadmapDensity(QStringLiteral("cozy"));  // last
                                                        // successful disk write
        const QString cfgDir = tempCfg.path()
            + QStringLiteral("/ants-terminal");
        const QString cfgPath = cfgDir
            + QStringLiteral("/config.json");
        if (!QFile::exists(cfgPath))
            return fail("INV-9",
                "expected config.json under XDG_CONFIG_HOME does not "
                "exist after a save() — Config wiring is broken.");
        // 0500 = read + execute owner, no write.
        if (!QFile::setPermissions(cfgDir,
                QFileDevice::ReadOwner | QFileDevice::ExeOwner))
            return fail("INV-9",
                "could not chmod parent dir to 0500 — test "
                "environment doesn't support permission changes.");
        // Setter must not throw; in-memory advances; disk doesn't.
        cfg.setRoadmapDensity(QStringLiteral("compact"));
        if (cfg.roadmapDensity() != QStringLiteral("compact")) {
            // Restore perms even on failure so cleanup works.
            QFile::setPermissions(cfgDir, QFileDevice::ReadOwner
                | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            return fail("INV-9",
                "in-memory roadmapDensity did not advance to "
                "\"compact\" despite the read-only parent dir — "
                "the live state should always reflect the setter.");
        }
        // Second half: a freshly-constructed Config reading the
        // same on-disk file returns the pre-chmod value (the last
        // successful write was "cozy").
        {
            Config cfg2;
            if (cfg2.roadmapDensity() != QStringLiteral("cozy")) {
                const QString got = cfg2.roadmapDensity();
                QFile::setPermissions(cfgDir, QFileDevice::ReadOwner
                    | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
                std::fprintf(stderr,
                    "[INV-9] FAIL: a fresh Config instance read "
                    "roadmapDensity = \"%s\"; expected \"cozy\" "
                    "(the last successful pre-chmod write). The "
                    "read-only-dir save() either silently wrote "
                    "anyway or corrupted the on-disk JSON.\n",
                    qUtf8Printable(got));
                return 1;
            }
        }
        // Restore write perms so subsequent test runs + the
        // QTemporaryDir destructor can clean up.
        QFile::setPermissions(cfgDir, QFileDevice::ReadOwner
            | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    std::fprintf(stderr,
        "OK — ANTS-1238 RoadmapDialog density-toggle INVs hold.\n");
    return 0;
}

TEST(RoadmapDensity, Main) {
    if (runMain() != 0) FAIL();
}
