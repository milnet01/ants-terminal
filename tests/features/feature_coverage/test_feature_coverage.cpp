// Feature-conformance test for spec.md — locks the regex-based parsers
// and fuzzy matcher in featurecoverage.cpp. Headless: pure text → list,
// no QApplication needed.

#include "featurecoverage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <functional>


#include <gtest/gtest.h>
using FeatureCoverage::SpecToken;
using FeatureCoverage::ChangelogBullet;

namespace {
int failures = 0;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);   \
        ++failures;                                                          \
    }                                                                        \
} while (0)

void expectTokens(const char *label, const QList<SpecToken> &got,
                  const QStringList &expected) {
    QStringList gotStrs;
    for (const auto &t : got) gotStrs << t.token;
    if (gotStrs == expected) return;
    std::fprintf(stderr, "FAIL %s\n  expected: [%s]\n  actual:   [%s]\n",
                 label,
                 qPrintable(expected.join(", ")),
                 qPrintable(gotStrs.join(", ")));
    ++failures;
}

// ---------------------------------------------------------------------------
// Lane 1 — extraction (invariants 1-6)
// ---------------------------------------------------------------------------

void testExtractEmpty() {
    expectTokens("extract.empty",
                 FeatureCoverage::extractSpecTokens(""), {});
}

void testExtractIdentifierShapes() {
    const QString md = R"(The `RemoteControl::dispatch` routes `"launch"` to
`cmd_launch` via `new-tab` and `helper.func`.)";
    const QList<SpecToken> got = FeatureCoverage::extractSpecTokens(md);
    // `"launch"` is NOT expected — quotes disqualify it. The other four
    // should appear in encounter order.
    expectTokens("extract.shapes", got, {
        "RemoteControl::dispatch",
        "cmd_launch",
        "new-tab",
        "helper.func",
    });
}

void testExtractShortTokensDropped() {
    const QString md = "Short: `id`, `ok`, `\\n`, `X`, `ab` all dropped; "
                       "but `valid` kept.";
    expectTokens("extract.shortDropped",
                 FeatureCoverage::extractSpecTokens(md), {"valid"});
}

void testExtractStopwordsDropped() {
    const QString md = "Code like `nullptr`, `QString`, `class`, `void` "
                       "is dropped; `MyClass` is kept.";
    expectTokens("extract.stopwords",
                 FeatureCoverage::extractSpecTokens(md), {"MyClass"});
}

void testExtractDedup() {
    const QString md = "First line mentions `someFunc`.\n"
                       "Second line has prose.\n"
                       "Third line mentions `someFunc` again.\n";
    const QList<SpecToken> got = FeatureCoverage::extractSpecTokens(md);
    CHECK(got.size() == 1, "extract.dedup size");
    if (got.size() == 1) {
        CHECK(got[0].token == "someFunc", "extract.dedup token");
        // First-occurrence wins: line 1 in 1-based counting.
        CHECK(got[0].line == 1, "extract.dedup line is first occurrence");
    }
}

void testExtractLineNumbering() {
    const QString md = "line1\nline2\nHere `myToken` appears.\n";
    const QList<SpecToken> got = FeatureCoverage::extractSpecTokens(md);
    CHECK(got.size() == 1, "extract.lineNumbering size");
    if (got.size() == 1) {
        CHECK(got[0].line == 3, "extract.lineNumbering 1-based line 3");
    }
}

// ---------------------------------------------------------------------------
// Lane 1 — drift (invariants 7-8)
// ---------------------------------------------------------------------------

void testDriftPredicateFilter() {
    const QString md = "Refers to `foo_func` and `bar_func`.\n";
    auto predicate = [](const QString &t) { return t == "foo_func"; };
    const QList<SpecToken> drift =
        FeatureCoverage::findDriftTokens(md, predicate);
    CHECK(drift.size() == 1, "drift.predicate size");
    if (drift.size() == 1) {
        CHECK(drift[0].token == "bar_func", "drift.predicate token");
    }
}

void testDriftAllFound() {
    const QString md = "All exist: `alpha_one`, `beta_two`, `gamma_three`.\n";
    auto predicate = [](const QString &) { return true; };
    const QList<SpecToken> drift =
        FeatureCoverage::findDriftTokens(md, predicate);
    CHECK(drift.isEmpty(), "drift.allFound empty");
}

// ---------------------------------------------------------------------------
// Lane 2 — CHANGELOG extraction (invariants 9-13)
// ---------------------------------------------------------------------------

void testChangelogNoHeader() {
    const QString md = "Just prose. No `## ` headers.\n- Not a bullet here.\n";
    const QList<ChangelogBullet> b =
        FeatureCoverage::extractTopVersionBullets(md);
    CHECK(b.isEmpty(), "changelog.noHeader empty");
}

void testChangelogTopSectionOnly() {
    const QString md = R"(# Changelog

## [0.7.0] - 2026-04-21

### Added

- First release-note bullet.

## [0.6.0] - 2026-04-01

### Added

- Old bullet should NOT appear.
)";
    const QList<ChangelogBullet> b =
        FeatureCoverage::extractTopVersionBullets(md);
    CHECK(b.size() == 1, "changelog.topOnly size");
    if (b.size() == 1) {
        CHECK(b[0].text == "First release-note bullet.",
              "changelog.topOnly text");
    }
}

void testChangelogSectionTagging() {
    const QString md = R"(## [Unreleased]

### Added

- Added bullet.

### Fixed

- Fixed bullet.
)";
    const QList<ChangelogBullet> b =
        FeatureCoverage::extractTopVersionBullets(md);
    CHECK(b.size() == 2, "changelog.tagging size");
    if (b.size() == 2) {
        CHECK(b[0].section == "Added", "changelog.tagging [0] section");
        CHECK(b[1].section == "Fixed", "changelog.tagging [1] section");
    }
}

void testChangelogSkipUnreleased() {
    // ANTS-2007 — skipUnreleased anchors on the first RELEASED version so the
    // coverage check doesn't flag in-progress (unreleased) items.
    const QString md = R"(## [Unreleased]

### Added

- WIP bullet that should be skipped.

## [0.2] - 2026-06-05

### Added

- Released bullet.
)";
    const QList<ChangelogBullet> def =
        FeatureCoverage::extractTopVersionBullets(md);
    CHECK(def.size() == 1, "changelog.skipUnreleased default size");
    if (def.size() == 1)
        CHECK(def[0].text == "WIP bullet that should be skipped.",
              "changelog.skipUnreleased default text");

    const QList<ChangelogBullet> rel =
        FeatureCoverage::extractTopVersionBullets(md, /*skipUnreleased=*/true);
    CHECK(rel.size() == 1, "changelog.skipUnreleased skipped size");
    if (rel.size() == 1)
        CHECK(rel[0].text == "Released bullet.",
              "changelog.skipUnreleased skipped text");

    // Unreleased-only changelog under skip → no bullets (nothing released).
    const QString onlyUnreleased = "## [Unreleased]\n\n### Added\n\n- X.\n";
    const QList<ChangelogBullet> none =
        FeatureCoverage::extractTopVersionBullets(onlyUnreleased,
                                                  /*skipUnreleased=*/true);
    CHECK(none.isEmpty(), "changelog.skipUnreleased only-unreleased empty");
}

void testChangelogLeadingDashStripped() {
    const QString md = "## [0.1]\n\n### Added\n\n- Foo bar.\n";
    const QList<ChangelogBullet> b =
        FeatureCoverage::extractTopVersionBullets(md);
    CHECK(b.size() == 1, "changelog.dashStripped size");
    if (b.size() == 1) {
        CHECK(b[0].text == "Foo bar.", "changelog.dashStripped text");
    }
}

void testChangelogLineNumbering() {
    // Line 1: "## [0.1]"
    // Line 2: ""
    // Line 3: "### Added"
    // Line 4: ""
    // Line 5: "- Bullet."
    const QString md = "## [0.1]\n\n### Added\n\n- Bullet.\n";
    const QList<ChangelogBullet> b =
        FeatureCoverage::extractTopVersionBullets(md);
    CHECK(b.size() == 1, "changelog.lineNumber size");
    if (b.size() == 1) {
        CHECK(b[0].line == 5, "changelog.lineNumber line==5");
    }
}

// ---------------------------------------------------------------------------
// Lane 2 — fuzzy match (invariants 14-17)
// ---------------------------------------------------------------------------

void testMatchBacktickTokenWins() {
    const QString bullet = "Remote-control — `launch` command. Seventh and "
                           "final command in the protocol surface.";
    const QStringList titles = {
        "Remote-control `launch` — convenience wrapper for new-tab",
        "Something totally unrelated about OSC sequences",
    };
    CHECK(FeatureCoverage::bulletMatchesAnyTitle(bullet, titles),
          "match.backtickWins");
}

void testMatchSignificantWordFallback() {
    // Bullet with NO backticks — fuzzy-word fallback. Significant words
    // in the bullet: "main-thread", "stall", "detector", "debug".
    // Title also has "main-thread" + "stall" — 2+ overlap.
    const QString bullet = "Main-thread stall detector (Debug Mode → Perf "
                           "category). A 200ms threshold flags hitches.";
    const QStringList titles = {
        "Main-thread stall probe for the Perf category",
    };
    CHECK(FeatureCoverage::bulletMatchesAnyTitle(bullet, titles),
          "match.significantWords");
}

void testMatchNoMatch() {
    const QString bullet = "Totally unrelated feature `zzz_alpha_widget`.";
    const QStringList titles = {
        "Remote-control `launch` — convenience wrapper",
        "OSC 133 Last Completed Command Actions",
    };
    CHECK(!FeatureCoverage::bulletMatchesAnyTitle(bullet, titles),
          "match.noMatch");
}

void testMatchEmptyTitleList() {
    CHECK(!FeatureCoverage::bulletMatchesAnyTitle("Whatever `foo`.", {}),
          "match.emptyTitles");
}

// ---------------------------------------------------------------------------
// On-disk runners (invariants 18-22)
// ---------------------------------------------------------------------------

bool writeFile(const QString &path, const QString &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(content.toUtf8());
    return true;
}

// INV-18 / INV-19 — a spec citing its own sibling test file is not drift,
// but a symbol that exists nowhere still is. Models the ANTS-4098 report:
// a Python corpus where no build manifest names the test files, so a cited
// test filename appears in no file's *contents* anywhere in the tree.
void testSpecDriftCitedFilenameResolves() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "specDrift.tmpdir");
    if (!tmp.isValid()) return;
    const QString root = tmp.path();

    // A src/ tree must exist or the lane self-disables and returns "".
    writeFile(root + "/src/app.py", "def render():\n    return 1\n");
    writeFile(root + "/tests/features/dashboard/test_min_size.py",
              "def test_min_size():\n    assert True\n");
    writeFile(root + "/tests/features/dashboard/spec.md",
              "# Dashboard\n\nCovered by `test_min_size.py`.\n"
              "Renamed away: `ghost_symbol_xyz`.\n");

    const QString out = FeatureCoverage::runSpecDriftCheck(root);
    CHECK(!out.contains(QStringLiteral("test_min_size.py")),
          "specDrift.citedFilenameNotDrift (INV-18)");
    CHECK(out.contains(QStringLiteral("ghost_symbol_xyz")),
          "specDrift.missingSymbolStillDrift (INV-19)");
}

// INV-20 — trailing parenthesised id only; that is where changelog_log
// writes it.
void testChangelogEntryIdExtraction() {
    CHECK(FeatureCoverage::extractChangelogEntryId(
              "**Overspend nudges land before payday** (FIBR-0042)")
              == QStringLiteral("FIBR-0042"),
          "entryId.trailing (INV-20)");
    CHECK(FeatureCoverage::extractChangelogEntryId(
              "**Overspend nudges land before payday**").isEmpty(),
          "entryId.absent (INV-20)");
    CHECK(FeatureCoverage::extractChangelogEntryId(
              "Mentions (FIBR-0042) mid-sentence and then keeps going.")
              .isEmpty(),
          "entryId.midSentenceIgnored (INV-20)");
}

// INV-21 / INV-22 — an id-keyed project: bullet prose deliberately shares
// no significant word and no backtick token with the spec's H1 title, so
// the only available join key is the ticket id. Models ANTS-4099.
void testChangelogCoverageByEntryId() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "changelogCoverage.tmpdir");
    if (!tmp.isValid()) return;
    const QString root = tmp.path();

    writeFile(root + "/CHANGELOG.md",
              "# Changelog\n\n"
              "## [1.2.0] - 2026-08-01\n"
              "### Added\n"
              "- **Overspend nudges land before payday** (FIBR-0042)\n"
              "- **Receipts export as one bundle** (FIBR-9999)\n");
    writeFile(root + "/tests/features/spending_alerts/spec.md",
              "# Alert threshold evaluation pipeline\n\n"
              "Locks the behaviour shipped as FIBR-0042.\n");

    const QString out = FeatureCoverage::runChangelogCoverageCheck(root);
    CHECK(!out.contains(QStringLiteral("FIBR-0042")),
          "changelogCoverage.idCovered (INV-21)");
    CHECK(out.contains(QStringLiteral("FIBR-9999")),
          "changelogCoverage.idUncoveredStillReported (INV-22)");
}

} // namespace

static int runMain() {
    // Reset counter so `--gtest_repeat` and re-entry from a single
    // binary don't accumulate failures across runs (the variable is
    // file-scope, not function-local, by intent — many helpers read it).
    failures = 0;
    testExtractEmpty();
    testExtractIdentifierShapes();
    testExtractShortTokensDropped();
    testExtractStopwordsDropped();
    testExtractDedup();
    testExtractLineNumbering();

    testDriftPredicateFilter();
    testDriftAllFound();

    testChangelogNoHeader();
    testChangelogTopSectionOnly();
    testChangelogSectionTagging();
    testChangelogSkipUnreleased();
    testChangelogLeadingDashStripped();
    testChangelogLineNumbering();

    testMatchBacktickTokenWins();
    testMatchSignificantWordFallback();
    testMatchNoMatch();
    testMatchEmptyTitleList();

    testSpecDriftCitedFilenameResolves();
    testChangelogEntryIdExtraction();
    testChangelogCoverageByEntryId();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d test(s) failed.\n", failures);
        return 1;
    }
    std::fprintf(stderr, "All feature-coverage tests passed.\n");
    return 0;
}

TEST(FeatureCoverage, Main) {
    ASSERT_EQ(0, runMain());
}
