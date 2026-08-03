// ANTS-3662 — spec_lint ENGINE conformance test (INV-1, INV-2, INV-3, INV-5,
// INV-6). Pure: document text in, findings out, so every row here drives
// SpecLint::check directly with no MainWindow and no filesystem.
//
// Three rows assert an ABSENCE of findings and so pass trivially against an
// engine that finds nothing (INV-1's skip arms, INV-3's tombstone arm, INV-6).
// They are re-proven by mutation — see spec.md's table for what each mutation
// actually turned red.

#include "speclint.h"
#include "specparse.h"   // ANTS-3799 — the mixed-form test asserts on the parser

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#if !defined(DOCS_STANDARDS_SPECS_MD_PATH) || !defined(DOCS_SPECS_DIR_PATH)
#error "spec_lint test needs DOCS_STANDARDS_SPECS_MD_PATH and DOCS_SPECS_DIR_PATH"
#endif

namespace {

int countKind(const SpecLint::Result &r, const char *kind) {
    int n = 0;
    for (const auto &f : r.findings)
        if (f.kind == QString::fromUtf8(kind)) ++n;
    return n;
}

QString slurp(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-1 — the required-section list is READ, never assumed, and the check is
// skipped when the marked block is absent — including when the standard itself
// is present. Arm (c) is the one that matters: it is docs/standards/specs.md's
// state today, so an implementation that falls back to scraping § 3's prose or
// § 7's skeleton passes (a) and (b) and fails only here.
TEST(SpecLint, Inv1SectionListIsReadNotAssumed) {
    const QString doc = QStringLiteral(
        "# ANTS-1 — a spec\n"
        "\n"
        "## 1. Problem\n"
        "\n"
        "text\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — holds. *Test:* a unit test → passes.\n");

    // (a) a standard whose block names three headings → one missing_section
    //     (the doc has `## 1. Problem` and `## 3. Invariants`, not `## 2.…`).
    const QString standard = QStringLiteral(
        "# Spec format\n"
        "\n"
        "<!-- required-sections -->\n"
        "```\n"
        "## 1. Problem\n"
        "## 2. Surface\n"
        "## 3. Invariants\n"
        "```\n");
    const QStringList req = SpecLint::parseRequiredSections(standard);
    ASSERT_EQ(req.size(), 3) << "the marked fenced block is the list";

    SpecLint::Options withList;
    withList.requiredSections = req;
    const SpecLint::Result a = SpecLint::check(doc, QStringLiteral("s.md"), withList);
    EXPECT_TRUE(a.sectionsChecked);
    EXPECT_EQ(countKind(a, "missing_section"), 1);
    for (const auto &f : a.findings) {
        if (f.kind != QLatin1String("missing_section")) continue;
        EXPECT_EQ(f.line, 0) << "document scope, per ANTS-3664 § 2.1";
    }

    // (b) no standard at all → skipped.
    EXPECT_TRUE(SpecLint::parseRequiredSections(QString()).isEmpty());
    const SpecLint::Result b = SpecLint::check(doc, QStringLiteral("s.md"), {});
    EXPECT_FALSE(b.sectionsChecked);
    EXPECT_EQ(countKind(b, "missing_section"), 0);

    // (c) THE CORPUS'S ACTUAL STATE — the project's standard is present and
    //     carries no marked block, so the list is empty and the check skips.
    //     This is a live assertion against the real file, not a fixture: it
    //     fails the moment an implementation starts scraping prose instead.
    const QString real = slurp(DOCS_STANDARDS_SPECS_MD_PATH);
    ASSERT_FALSE(real.isEmpty()) << "docs/standards/specs.md must be readable";
    EXPECT_TRUE(SpecLint::parseRequiredSections(real).isEmpty())
        << "specs.md carries no <!-- required-sections --> block yet "
           "(ANTS-3662 § 7 files it); anything else here means the engine is "
           "guessing the list from § 3's prose or § 7's skeleton";
}

// INV-2 — every INV-N carrying no test-surface clause produces exactly one
// invariant_no_test, in BOTH forms, except a tombstone.
//
// Separate documents per form below, which is the clean way to isolate each.
// It used to be the ONLY way — parseSpecBody's bullet branch ran solely when
// the table branch matched nothing — and that limitation is what ANTS-3799
// fixed; MixedFormsInOneDocument at the end of this block is its regression.
TEST(SpecLint, Inv2EveryInvariantNeedsATestClause) {
    const QString bulletDoc = QStringLiteral(
        "# ANTS-1 — bullets\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — has one. *Test:* a unit test → passes.\n"
        "- **INV-2** — has none at all.\n"
        "- **INV-3** — *moved to ANTS-3669*\n"
        "\n"
        "## 4. Notes\n");
    const SpecLint::Result b = SpecLint::check(bulletDoc, QStringLiteral("b.md"), {});
    EXPECT_EQ(countKind(b, "invariant_no_test"), 1);
    for (const auto &f : b.findings)
        EXPECT_FALSE(f.message.contains(QStringLiteral("INV-3")))
            << "a moved invariant has nothing left to test";

    // Table form, three columns. INV-2's surface cell is BLANK — the parser
    // returns it as a present-but-whitespace `test_surface`, so keying on the
    // key's presence alone would pass this silently.
    const QString tableDoc = QStringLiteral(
        "# ANTS-2 — a table\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "| ID | Rule | Test |\n"
        "|---|---|---|\n"
        "| INV-1 | has one | a unit test |\n"
        "| INV-2 | has none |  |\n"
        "| INV-3 | *moved to ANTS-3669* |  |\n"
        "\n"
        "## 4. Notes\n");
    const SpecLint::Result t = SpecLint::check(tableDoc, QStringLiteral("t.md"), {});
    EXPECT_EQ(countKind(t, "invariant_no_test"), 1);
    EXPECT_EQ(countKind(t, "invariant_id_gap"), 0);
    for (const auto &f : t.findings)
        EXPECT_FALSE(f.message.contains(QStringLiteral("INV-3")));

    // Table form with NO test column at all — a malformed spec, and the one
    // shape parseSpecBody genuinely cannot see: its row regex needs three cells,
    // so a two-cell row matches nothing and is returned by nothing. Only the
    // anchor scan knows these invariants exist. Without it both rows are
    // invisible: no invariant_no_test, and INV-1/INV-2 would not even be in the
    // id sequence.
    const QString twoColDoc = QStringLiteral(
        "# ANTS-3 — no test column\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "| ID | Rule |\n"
        "|---|---|\n"
        "| INV-1 | a rule |\n"
        "| INV-2 | another |\n"
        "\n"
        "## 4. Notes\n");
    const SpecLint::Result n = SpecLint::check(twoColDoc, QStringLiteral("n.md"), {});
    EXPECT_EQ(countKind(n, "invariant_no_test"), 2);
    EXPECT_EQ(countKind(n, "invariant_id_gap"), 0);
    for (const auto &f : n.findings)
        EXPECT_GT(f.line, 0) << "the anchor scan is also where `line` comes from";

    EXPECT_EQ(b.findings.size() + t.findings.size() + n.findings.size(), 4);
}

// ANTS-3799 — bullet and table invariants COEXIST in one document, and the
// bullets keep their test surfaces.
//
// The shape finbreak hit: live invariants in bullet form, withdrawn ones
// collected in a `### Withdrawn invariants` summary table. The section runs to
// the next `## `, so that table sits inside it. Before the fix a single
// `| INV-N |` row put the parser in table mode for the whole section, every
// bullet lost its `*Test:*` clause, and each reported invariant_no_test — 11
// false findings on the reporting project's conforming spec, arriving at the
// top of a /cold-eyes run as already-logged fact. The tempting "fix" for an
// author is to add a second *Test:* line to invariants that already have one,
// which makes the document worse.
TEST(SpecLint, Ants3799MixedFormsInOneDocument) {
    const QString doc = QStringLiteral(
        "# ANTS-1 — mixed\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-5** — a live rule. *Test:* `tests/x.py` — asserts the thing.\n"
        "- **INV-7** — another live rule. *Test:* `tests/y.py` — asserts it.\n"
        "- **INV-8** — this one genuinely has none.\n"
        "\n"
        "### Withdrawn invariants\n"
        "\n"
        "| Id | Withdrawn | Now |\n"
        "|---|---|---|\n"
        "| INV-1 | 2026-07-28, loop 5 split | **FIBR-0193 INV-1** — moved |\n"
        "| INV-2 | 2026-07-28, loop 5 split | **FIBR-0193 INV-2** — moved |\n"
        "\n"
        "## 4. Notes\n");
    const SpecLint::Result r = SpecLint::check(doc, QStringLiteral("m.md"), {});

    // Exactly one: INV-8. The two bullets WITH clauses must not fire, and the
    // table rows carry a third cell so they have a surface of their own.
    EXPECT_EQ(countKind(r, "invariant_no_test"), 1)
        << "a `| INV-N |` row must not strip the test surface off bullet "
           "invariants elsewhere in the same section";
    for (const auto &f : r.findings) {
        if (f.kind != QLatin1String("invariant_no_test")) continue;
        EXPECT_TRUE(f.message.contains(QStringLiteral("INV-8"))) << f.message.toStdString();
    }

    // And the parser itself returns both forms, in document order — bullets
    // first here, because that is how the document reads.
    const QJsonObject parsed = SpecParse::parseSpecBody(doc);
    const QJsonArray invs = parsed.value(QStringLiteral("invariants")).toArray();
    ASSERT_EQ(invs.size(), 5) << "3 bullets + 2 table rows";
    QStringList ids;
    for (const auto &v : invs) ids << v.toObject().value(QStringLiteral("id")).toString();
    EXPECT_EQ(ids, (QStringList{QStringLiteral("INV-5"), QStringLiteral("INV-7"),
                                QStringLiteral("INV-8"), QStringLiteral("INV-1"),
                                QStringLiteral("INV-2")}));
    EXPECT_EQ(invs.at(0).toObject().value(QStringLiteral("test_surface")).toString(),
              QStringLiteral("`tests/x.py` — asserts the thing."))
        << "the bullet's clause survives the presence of a table in the section";
}

// INV-3 — an id gap fires only on a MISSING number, never on a tombstoned one.
// Both marker forms are in the fixture deliberately: `*moved to <ID>*` is
// observed a dozen times in ANTS-3636, while `*withdrawn — …*` had zero corpus
// occurrences when this shipped and would otherwise ship unexercised.
TEST(SpecLint, Inv3TombstoneIsNotAGap) {
    const QString doc = QStringLiteral(
        "# ANTS-1 — gaps\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — a. *Test:* t → ok.\n"
        "- **INV-2** — b. *Test:* t → ok.\n"
        "- **INV-3** — *moved to ANTS-3669*\n"
        "- **INV-4** — *withdrawn — superseded by INV-2*\n"
        "- **INV-6** — f. *Test:* t → ok.\n"
        "\n"
        "## 4. Notes\n");
    const SpecLint::Result r = SpecLint::check(doc, QStringLiteral("g.md"), {});
    EXPECT_EQ(countKind(r, "invariant_id_gap"), 1) << "5 alone is missing";
    EXPECT_EQ(countKind(r, "invariant_no_test"), 0);
    for (const auto &f : r.findings) {
        if (f.kind != QLatin1String("invariant_id_gap")) continue;
        EXPECT_TRUE(f.message.contains(QStringLiteral("INV-5")));
        // A missing invariant has no line of its own, so the finding anchors
        // on the bullet FOLLOWING the gap — INV-6, on line 9 (1-based).
        EXPECT_EQ(f.line, 9);
    }
}

// INV-5 — a loop-log row whose LAST cell is empty produces exactly one
// loop_row_no_outcome, in every table shape; a table whose first header cell is
// not `Loop` is not scanned.
TEST(SpecLint, Inv5LoopLogOutcomeCells) {
    const QString doc = QStringLiteral(
        "# ANTS-1 — logs\n"
        "\n"
        "## 2. Surface\n"
        "\n"
        "| Rule | Fires when |\n"
        "|---|---|\n"
        "| dead_anchor |  |\n"
        "\n"
        "## Cold-eyes loop log\n"
        "\n"
        "| Loop | Date | Lanes | C | H | M | L | I | Outcome |\n"
        "|---|---|---|---|---|---|---|---|---|\n"
        "| 1 | 2026-01-01 | a | 0 | 1 | 0 | 0 | 0 | 1 fixed |\n"
        "| 2 | 2026-01-02 | a | 0 | 0 | 1 | 0 | 0 |  |\n"
        "\n"
        "| Loop | C | H | M | L/I | Notes |\n"
        "|---|---|---|---|---|---|\n"
        "| 1 | 0 | 1 | 0 | 0 | all fixed |\n"
        "| 2 | 0 | 0 | 1 | 0 |  |\n");
    const SpecLint::Result r = SpecLint::check(doc, QStringLiteral("l.md"), {});
    EXPECT_EQ(countKind(r, "loop_row_no_outcome"), 2)
        << "one per loop table — the 6-column shape is what stops a "
           "nine-column assumption passing, and the `| Rule |` table is what "
           "stops the check firing on every table in the document";
    QSet<int> lineSet;
    for (const auto &f : r.findings)
        if (f.kind == QLatin1String("loop_row_no_outcome")) lineSet.insert(f.line);
    EXPECT_EQ(lineSet.size(), 2) << "each finding carries its own row's line";
    EXPECT_TRUE(lineSet.contains(14));
    EXPECT_TRUE(lineSet.contains(19));
}

// INV-6 — size is reported in the envelope and NEVER as a finding. A long spec
// is evidence about a run, not a defect in a document, and emitting it as a
// finding would put an unfixable item in a list whose consumer chain ends in a
// fixer (ANTS-3669).
TEST(SpecLint, Inv6SizeIsNotAFinding) {
    QString big = QStringLiteral("# ANTS-1 — big\n");
    for (int i = 0; i < 1998; ++i) big += QStringLiteral("filler\n");
    const SpecLint::Result r = SpecLint::check(big, QStringLiteral("big.md"), {});
    EXPECT_EQ(r.lineCount, 1999);
    for (const auto &f : r.findings)
        EXPECT_FALSE(f.kind.contains(QStringLiteral("size")) ||
                     f.kind.contains(QStringLiteral("line")) ||
                     f.kind.contains(QStringLiteral("long")))
            << "size must never be a finding kind: " << f.kind.toStdString();

    // Second arm — a spec with NO findings at all still reports its size. This
    // is what stops the row passing vacuously against an engine that emits
    // nothing: without it, `findings` is empty either way.
    const QString clean = QStringLiteral(
        "# ANTS-1 — clean\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — a. *Test:* a unit test → passes.\n");
    const SpecLint::Result c = SpecLint::check(clean, QStringLiteral("c.md"), {});
    EXPECT_TRUE(c.findings.isEmpty());
    EXPECT_EQ(c.lineCount, 5);
    EXPECT_FALSE(c.truncated);
}

// § 2.1's fire-rate measurement, on the ANTS-3661 precedent. `command_test_
// no_expectation` is a heuristic, and a candidate emitter that fires on most of
// the corpus is a rule that has been guessed rather than a corpus that is
// broken — a number is the only thing that tells the two apart.
//
// DISABLED because it is a measurement, not a contract: it walks the whole spec
// tree, its output is a report rather than an assertion, and its numbers move
// whenever a spec is edited. Re-runnable rather than one-off, so the next person
// to touch the heuristic can re-measure instead of re-deriving:
//
//   ./build/test_claude --gtest_also_run_disabled_tests
//                       --gtest_filter=SpecLint.DISABLED_CorpusCalibration
TEST(SpecLint, DISABLED_CorpusCalibration) {
    QDir dir(QString::fromUtf8(DOCS_SPECS_DIR_PATH));
    const QStringList files = dir.entryList({QStringLiteral("*.md")}, QDir::Files,
                                            QDir::Name);
    ASSERT_FALSE(files.isEmpty());

    QHash<QString, int> byKind;
    int docs = 0, invariants = 0, totalLines = 0, worstLines = 0;
    QString worstDoc;
    QHash<QString, QStringList> samples;
    for (const QString &name : files) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QString text = QString::fromUtf8(f.readAll());
        f.close();
        ++docs;

        const SpecLint::Result r = SpecLint::check(text, name, {});
        totalLines += r.lineCount;
        if (r.lineCount > worstLines) { worstLines = r.lineCount; worstDoc = name; }
        for (const auto &fnd : r.findings) {
            byKind[fnd.kind]++;
            // A per-kind sample, because a rate alone cannot tell a broken
            // corpus from a guessed rule — someone has to read the rows.
            if (samples[fnd.kind].size() < 8)
                samples[fnd.kind] << QStringLiteral("%1:%2 %3")
                                         .arg(name).arg(fnd.line).arg(fnd.message);
        }
        // Invariant population, for the fire rate's denominator.
        for (const QString &line : text.split(QLatin1Char('\n')))
            if (line.startsWith(QLatin1String("- **INV-")) ||
                line.startsWith(QLatin1String("| INV-")))
                ++invariants;
    }

    // stderr rather than qInfo: this bundle suppresses qInfo, and a measurement
    // whose output is swallowed is a measurement nobody makes twice.
    fprintf(stderr,
            "spec_lint corpus calibration: %d specs, %d invariant anchors, "
            "%d lines total, largest %s at %d\n",
            docs, invariants, totalLines, qPrintable(worstDoc), worstLines);
    for (auto it = byKind.constBegin(); it != byKind.constEnd(); ++it)
        fprintf(stderr, "  %-30s %5d  (%.1f%% of invariant anchors)\n",
                qPrintable(it.key()), it.value(),
                invariants ? 100.0 * it.value() / invariants : 0.0);
    for (auto it = samples.constBegin(); it != samples.constEnd(); ++it)
        for (const QString &s : it.value())
            fprintf(stderr, "  [%s] %s\n", qPrintable(it.key()), qPrintable(s));
    SUCCEED();
}
