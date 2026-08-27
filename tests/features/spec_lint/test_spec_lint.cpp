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
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#if !defined(DOCS_STANDARDS_SPECS_MD_PATH) || !defined(DOCS_SPECS_DIR_PATH)
#error "spec_lint test needs DOCS_STANDARDS_SPECS_MD_PATH and DOCS_SPECS_DIR_PATH"
#endif
#if !defined(SRC_SPECLINT_CPP_PATH) || !defined(SRC_SPECLINT_H_PATH)
#error "spec_lint test needs SRC_SPECLINT_CPP_PATH and SRC_SPECLINT_H_PATH"
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

// ANTS-4127 — a one-invariant spec, so a fixture differs from its sibling only
// in the line under test. `status` empty writes NO Status line at all, which is
// the state 53 specs of this corpus are in.
QString specDoc(const QString &status, const QString &clause) {
    QString d = QStringLiteral("# ANTS-1 — a spec\n");
    if (!status.isEmpty())
        d += QStringLiteral("**Status:** ") + status + QLatin1Char('\n');
    d += QStringLiteral("\n## 3. Invariants\n\n- **INV-1** — a rule. *Test:* ") +
         clause + QLatin1Char('\n');
    return d;
}

SpecLint::Options withDirs(const QSet<QString> &existing,
                           const QSet<QString> &wired = {}) {
    SpecLint::Options o;
    o.existingTestDirs = existing;
    o.wiredTestDirs    = wired;
    return o;
}

const DocFinding::Finding *firstOfKind(const SpecLint::Result &r,
                                       const char *kind) {
    for (const auto &f : r.findings)
        if (f.kind == QString::fromUtf8(kind)) return &f;
    return nullptr;
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

    // (c) THE CORPUS'S ACTUAL STATE — a live assertion against the real file,
    //     not a fixture, so it fails the moment an implementation starts
    //     guessing the list from § 3's prose or § 7's skeleton instead of
    //     reading the marked block.
    //
    //     ANTS-4345 (2026-08-14) added that block, so this flipped from "the
    //     list is empty and the check skips" to naming the five entries. They
    //     are UNNUMBERED on purpose: this standard does not fix section
    //     numbers, and an exact-match list built from § 7's skeleton was
    //     measured firing on 188 of 243 specs, almost none of it real.
    const QString real = slurp(DOCS_STANDARDS_SPECS_MD_PATH);
    ASSERT_FALSE(real.isEmpty()) << "docs/standards/specs.md must be readable";
    const QStringList realReq = SpecLint::parseRequiredSections(real);
    EXPECT_EQ(realReq, (QStringList{QStringLiteral("## Problem"),
                                    QStringLiteral("## Surface"),
                                    QStringLiteral("## Invariants"),
                                    QStringLiteral("## Tests"),
                                    QStringLiteral("## Cold-eyes loop log")}))
        << "specs.md § 3's <!-- required-sections --> block is the list; it is "
           "a deliberate SUBSET of § 3 + § 4 (no Title/Header block — not `##` "
           "sections; no RAM / build cost — § 4 requires it conditionally and "
           "a flat list cannot carry a condition)";
    for (const QString &e : realReq)
        EXPECT_FALSE(e.contains(QRegularExpression(QStringLiteral(R"(^##\s+\d)"))))
            << "entries stay unnumbered: " << qPrintable(e);
}

// ANTS-4345 — a required entry written WITHOUT a number matches on the section
// NAME; a numbered one keeps the exact match. Both halves are load-bearing and
// they serve different standards: the global config repo fixes its numbering
// (1..12) and relies on the number being part of a section's identity, while
// this project's standard never fixes numbers — § 4's optional sections are
// inserted where they read best, which shifts every heading after them.
// Measured 2026-08-14 over all 243 specs in docs/specs/: an exact-match block
// built from § 7's skeleton fired on 188 of them, and `## 6. Tests` alone was
// absent-as-written in 174 — almost none of that missing structure, just a
// number the standard never mandated.
TEST(SpecLint, RequiredSectionMatchesByNameWhenUnnumbered) {
    const QString doc = QStringLiteral(
        "# ANTS-1 — t\n"
        "\n"
        "**Status:** spec draft (2026-08-14).\n"
        "\n"
        "## 1. Problem\n"
        "\n"
        "p\n"
        "\n"
        "## 4. Tests\n"
        "\n"
        "t\n"
        "\n"
        "## Cold-eyes loop log\n");

    // (a) unnumbered entries match whatever number the document carries —
    //     `## Tests` finds `## 4. Tests`, and an unnumbered heading still
    //     matches an unnumbered entry.
    const QString byName = QStringLiteral(
        "<!-- required-sections -->\n"
        "```\n"
        "## Problem\n"
        "## Tests\n"
        "## Cold-eyes loop log\n"
        "```\n");
    SpecLint::Options byNameOpts;
    byNameOpts.requiredSections = SpecLint::parseRequiredSections(byName);
    ASSERT_EQ(byNameOpts.requiredSections.size(), 3);
    const SpecLint::Result a =
        SpecLint::check(doc, QStringLiteral("s.md"), byNameOpts);
    EXPECT_TRUE(a.sectionsChecked);
    EXPECT_EQ(countKind(a, "missing_section"), 0)
        << "an unnumbered entry must not care which number the heading has";

    // (b) a NUMBERED entry still matches exactly, so a standard whose numbers
    //     are identity keeps the stricter check: `## 6. Tests` does not match
    //     this document's `## 4. Tests`.
    const QString byNumber = QStringLiteral(
        "<!-- required-sections -->\n"
        "```\n"
        "## 1. Problem\n"
        "## 6. Tests\n"
        "```\n");
    SpecLint::Options byNumberOpts;
    byNumberOpts.requiredSections = SpecLint::parseRequiredSections(byNumber);
    ASSERT_EQ(byNumberOpts.requiredSections.size(), 2);
    const SpecLint::Result b =
        SpecLint::check(doc, QStringLiteral("s.md"), byNumberOpts);
    EXPECT_TRUE(b.sectionsChecked);
    EXPECT_EQ(countKind(b, "missing_section"), 1)
        << "`## 1. Problem` matches; `## 6. Tests` must not match `## 4. Tests`";

    // (c) a genuinely absent section is still reported under name matching —
    //     the loosening must not swallow the finding the check exists for.
    const QString missing = QStringLiteral(
        "<!-- required-sections -->\n"
        "```\n"
        "## Surface\n"
        "```\n");
    SpecLint::Options missingOpts;
    missingOpts.requiredSections = SpecLint::parseRequiredSections(missing);
    const SpecLint::Result c =
        SpecLint::check(doc, QStringLiteral("s.md"), missingOpts);
    EXPECT_EQ(countKind(c, "missing_section"), 1)
        << "the document has no Surface section at any number";
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

// ANTS-4115 — the Invariants heading need not BEGIN with the word. OneUp writes
// `## 5. Correctness invariants` in all 13 of its specs; the anchored regex
// matched none of them, so the section was never located and every bullet under
// it was invisible. The tell in the reply was invariants:[] sitting beside
// possible_untabled_invariants:9 — the parser saying it could see them and would
// not return them. Measured against the live pre-fix verb on
// docs/specs/ONEUP-0032-i18n.md before this shipped.
TEST(SpecLint, Ants4115HeadingNeedNotBeginWithInvariants) {
    const QString doc = QStringLiteral(
        "# ONEUP-1 — a spec\n"
        "\n"
        "## 5. Correctness invariants\n"
        "\n"
        "- **INV-1** Nothing under `engine/` imports the GUI. *Test:* a grep → none.\n"
        "- **INV-2** This one carries no clause.\n"
        "\n"
        "## 6. Notes\n");

    const QJsonObject parsed = SpecParse::parseSpecBody(doc);
    const QJsonArray  invs   = parsed.value(QStringLiteral("invariants")).toArray();
    ASSERT_EQ(invs.size(), 2) << "the heading contains the word; that is enough";
    EXPECT_EQ(parsed.value(QStringLiteral("possible_untabled_invariants")).toInt(), 0)
        << "nothing is untabled once the section is found";

    // And spec_lint agrees about where the section is — the two must not read
    // different documents.
    const SpecLint::Result r = SpecLint::check(doc, QStringLiteral("o.md"), {});
    EXPECT_EQ(countKind(r, "invariant_no_test"), 1);
    for (const auto &f : r.findings)
        if (f.kind == QLatin1String("invariant_no_test"))
            EXPECT_TRUE(f.message.contains(QStringLiteral("INV-2")));

    // Strict-first is what keeps a document with BOTH shapes on its real
    // section: the prose heading comes first here and must lose.
    const QString both = QStringLiteral(
        "# ANTS-1 — both\n"
        "\n"
        "## 2. Why these invariants matter\n"
        "\n"
        "- **INV-9** an example quoted in prose.\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — the real one. *Test:* t → ok.\n");
    const QJsonArray picked = SpecParse::parseSpecBody(both)
                                  .value(QStringLiteral("invariants")).toArray();
    ASSERT_EQ(picked.size(), 1);
    EXPECT_EQ(picked.at(0).toObject().value(QStringLiteral("id")).toString(),
              QStringLiteral("INV-1"));
}

// ANTS-4107 — a sub-lettered id (INV-3b) is its own invariant. It used to match
// no anchor, so its body was absorbed into INV-3's, invariants_count undercounted
// and — the consequence that mattered — invariant_no_test could not see it, so a
// sub-lettered invariant with no test surface passed the lint silently. A
// /cold-eyes split into 3 and 3b is how one comes to exist, which put the blind
// spot exactly where a contract had just been divided.
TEST(SpecLint, Ants4107SubLetteredInvariantIds) {
    const QString doc = QStringLiteral(
        "# ANTS-1 — split\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-3** — the narrowed half. *Test:* t → ok.\n"
        "- **INV-3b** — the split-off half, with no clause at all.\n"
        "- **INV-4** — another. *Test:* t → ok.\n"
        "\n"
        "## 4. Notes\n");

    const QJsonObject parsed = SpecParse::parseSpecBody(doc);
    const QJsonArray  invs   = parsed.value(QStringLiteral("invariants")).toArray();
    ASSERT_EQ(invs.size(), 3);
    QStringList ids;
    for (const auto &v : invs) ids << v.toObject().value(QStringLiteral("id")).toString();
    EXPECT_EQ(ids, (QStringList{QStringLiteral("INV-3"), QStringLiteral("INV-3b"),
                                QStringLiteral("INV-4")}));
    EXPECT_FALSE(invs.at(0).toObject().value(QStringLiteral("body")).toString()
                     .contains(QStringLiteral("split-off half")))
        << "3b's body must not be swallowed into 3's";

    const SpecLint::Result r = SpecLint::check(doc, QStringLiteral("s.md"), {});
    EXPECT_EQ(countKind(r, "invariant_no_test"), 1);
    for (const auto &f : r.findings)
        if (f.kind == QLatin1String("invariant_no_test"))
            EXPECT_TRUE(f.message.contains(QStringLiteral("INV-3b"))) << f.message.toStdString();
    // A sub-letter occupies its parent's slot, so it can never open a gap.
    EXPECT_EQ(countKind(r, "invariant_id_gap"), 0);
}

// ANTS-4110 — on a project that numbers invariants once across the corpus, a
// number a SIBLING spec owns is not a gap. 26 such findings landed on one
// three-spec corpus, all false, in a bucket /cold-eyes records as pre-verified —
// and both repairs they invite are forbidden by the standard the check enforces:
// renumber (ids are permanent) or tombstone ids that were never in the file.
TEST(SpecLint, Ants4110SiblingNumbersAreNotGaps) {
    const QString doc = QStringLiteral(
        "# LOTTO-1 — a spec\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — a. *Test:* t → ok.\n"
        "- **INV-2** — b. *Test:* t → ok.\n"
        "- **INV-7** — g. *Test:* t → ok.\n"
        "\n"
        "## 4. Notes\n");

    // Without the set, the pre-ANTS-4110 behaviour exactly: 3, 4, 5, 6 are gaps.
    const SpecLint::Result plain = SpecLint::check(doc, QStringLiteral("l.md"), {});
    EXPECT_EQ(countKind(plain, "invariant_id_gap"), 4);
    EXPECT_EQ(plain.idGapsSuppressed, 0);

    // With the siblings' numbers injected, none of them is a gap — and the count
    // says so rather than leaving a short list to be read as a clean document.
    SpecLint::Options opts;
    opts.siblingInvNumbers = QSet<int>{3, 4, 5, 6};
    const SpecLint::Result r = SpecLint::check(doc, QStringLiteral("l.md"), opts);
    EXPECT_EQ(countKind(r, "invariant_id_gap"), 0);
    EXPECT_EQ(r.idGapsSuppressed, 4);

    // A number nobody owns is still a gap: the suppression is a lookup, not a
    // blanket off-switch.
    SpecLint::Options partial;
    partial.siblingInvNumbers = QSet<int>{3, 4};
    const SpecLint::Result p = SpecLint::check(doc, QStringLiteral("l.md"), partial);
    EXPECT_EQ(countKind(p, "invariant_id_gap"), 2);
    EXPECT_EQ(p.idGapsSuppressed, 2);

    // The corpus scan the verb layer feeds it: ownership, from either form.
    const QSet<int> owned = SpecLint::invariantNumbers(doc);
    EXPECT_EQ(owned, (QSet<int>{1, 2, 7}));
}

// ANTS-3784 — the DOCUMENT's own declaration of a deliberate floor. ANTS-4110
// above answers the same question corpus-wide and all-or-nothing: one number
// shared by two specs anywhere turns it off, so a corpus that numbers
// per-document except for one family cannot reach it. Measured 2026-08-15 on
// this project: ANTS-3782 reported 11 gaps (INV-15..25, owned by ANTS-3756) out
// of the corpus's 39, straight into review-contract's pre-verified bucket.
TEST(SpecLint, Ants3784DeclaredIdBaseSuppressesGapsBelowIt) {
    const QString body = QStringLiteral(
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-14** — inherited. *Test:* t → ok.\n"
        "- **INV-26** — new. *Test:* t → ok.\n"
        "- **INV-28** — new. *Test:* t → ok.\n"
        "\n"
        "## 4. Notes\n");

    // Undeclared: 15..25 and 27 are gaps — the state the item was filed against.
    const SpecLint::Result plain =
        SpecLint::check(QStringLiteral("# S — a spec\n") + body,
                        QStringLiteral("s.md"), {});
    EXPECT_EQ(countKind(plain, "invariant_id_gap"), 12);
    EXPECT_EQ(plain.idGapsSuppressed, 0);

    // Declared: everything below 26 is suppressed and COUNTED. INV-14 is the
    // document's own anchor below its own floor and is left alone; 27 is above
    // the floor and stays a gap, so the floor is not an off-switch.
    const SpecLint::Result r = SpecLint::check(
        QStringLiteral("# S — a spec\n<!-- invariant-id-base: 26 -->\n") + body,
        QStringLiteral("s.md"), {});
    EXPECT_EQ(countKind(r, "invariant_id_gap"), 1);
    EXPECT_EQ(r.idGapsSuppressed, 11);
    for (const auto &f : r.findings)
        if (f.kind == QLatin1String("invariant_id_gap"))
            EXPECT_TRUE(f.message.contains(QStringLiteral("INV-27")))
                << f.message.toStdString();

    // A document DOCUMENTING the syntax has not declared one: the line is read
    // outside fenced code only. Without this, this very test file's spec would
    // silence its own corpus.
    const SpecLint::Result fenced = SpecLint::check(
        QStringLiteral("# S — a spec\n\n```\n<!-- invariant-id-base: 26 -->\n```\n")
            + body,
        QStringLiteral("s.md"), {});
    EXPECT_EQ(countKind(fenced, "invariant_id_gap"), 12);
    EXPECT_EQ(fenced.idGapsSuppressed, 0);
}

// =====================================================================
// ANTS-4127 — test-surface resolution. A `*Test:*` clause is the claim that an
// invariant is locked by something real; nothing checked that the thing it names
// exists, and three specs in this corpus name test directories that never have.
// Every row below drives the engine by INJECTING the two filesystem sets, which
// is what keeping it filesystem-free buys.
// =====================================================================

// INV-1 — one resolution attempt per DISTINCT name in a clause, a wrapped clause
// harvested whole, and distinctness scoped per DOCUMENT.
//
// The assertion is `surfaces_resolved`, not a finding count: "attempts" is not an
// emitted quantity, and counting findings would pass against an engine that
// harvests once and errors twice.
TEST(SpecLint, Ants4127Inv1SurfaceHarvest) {
    const auto opts = withDirs({QStringLiteral("alpha"), QStringLiteral("beta")});

    // (a) two distinct present directories in ONE clause → two surfaces.
    const SpecLint::Result two = SpecLint::check(
        specDoc(QStringLiteral("spec draft."),
                QStringLiteral("`tests/features/alpha/` and "
                               "`tests/features/beta/` both cover it.")),
        QStringLiteral("a.md"), opts);
    EXPECT_EQ(two.surfacesResolved, 2)
        << "a harvest that stops at the first match leaves the second surface "
           "of every multi-surface clause unchecked";
    EXPECT_TRUE(two.findings.isEmpty());

    // (b) the SAME directory twice → one.
    const SpecLint::Result dup = SpecLint::check(
        specDoc(QStringLiteral("spec draft."),
                QStringLiteral("`tests/features/alpha/`, and again "
                               "`tests/features/alpha/`.")),
        QStringLiteral("b.md"), opts);
    EXPECT_EQ(dup.surfacesResolved, 1);

    // (c) the path on a CONTINUATION line, `*Test:*` ending its own. Invariant
    //     bullets hard-wrap, so this is the shape the corpus actually writes —
    //     and a line-scoped scan misses every one of them, the error that put
    //     this spec's own yield measurement at 40 instead of 137.
    const QString wrapped = QStringLiteral(
        "# ANTS-1 — a spec\n"
        "**Status:** spec draft.\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — a rule that needs a long sentence to state. *Test:*\n"
        "  `tests/features/alpha/` — a fixture asserting the thing.\n");
    const SpecLint::Result w = SpecLint::check(wrapped, QStringLiteral("c.md"), opts);
    EXPECT_EQ(w.surfacesResolved, 1) << "a wrapped clause is harvested whole";

    // (d) PER-DOCUMENT distinctness: two separate invariants, one directory.
    //     Without this an engine deduplicating per clause passes every other row
    //     here and still reports a different total for the same corpus.
    const QString twice = QStringLiteral(
        "# ANTS-1 — a spec\n"
        "**Status:** spec draft.\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — a rule. *Test:* `tests/features/alpha/` covers it.\n"
        "- **INV-2** — another rule. *Test:* `tests/features/alpha/` too.\n");
    const SpecLint::Result t = SpecLint::check(twice, QStringLiteral("d.md"), opts);
    EXPECT_EQ(t.surfacesResolved, 1)
        << "a directory two invariants of one spec both name is one surface";
}

// INV-2 — a `tests/features/<name>*` WILDCARD yields no attempt and no finding.
// A spec legitimately writes one when it means a family of tests, so reading
// `tests/features/audit_*` as a directory named `audit_` manufactures a finding
// against a correct spec — worse than not checking at all.
TEST(SpecLint, Ants4127Inv2WildcardIsNotADirectory) {
    const auto opts = withDirs({QStringLiteral("alpha")});
    const SpecLint::Result r = SpecLint::check(
        specDoc(QStringLiteral("shipped (2026-01-01)."),
                QStringLiteral("`tests/features/audit_*` and "
                               "`tests/features/remote_control_*` cover it.")),
        QStringLiteral("w.md"), opts);
    EXPECT_TRUE(r.findings.isEmpty()) << "a wildcard names no directory";
    EXPECT_EQ(r.surfacesResolved, 0);

    // And the pattern is not simply blind: a real name in the same clause still
    // resolves, so the row cannot pass against an extractor that harvests nothing.
    const SpecLint::Result mixed = SpecLint::check(
        specDoc(QStringLiteral("shipped (2026-01-01)."),
                QStringLiteral("`tests/features/audit_*` plus "
                               "`tests/features/alpha/`.")),
        QStringLiteral("m.md"), opts);
    EXPECT_TRUE(mixed.findings.isEmpty());
    EXPECT_EQ(mixed.surfacesResolved, 1);
}

// INV-3 — the spec's own Status picks the bucket, and only a confidently-shipped
// one yields a FINDING. The two documents differ in NOTHING else.
TEST(SpecLint, Ants4127Inv3StatusChoosesTheBucket) {
    const auto opts  = withDirs({QStringLiteral("something_else")});
    const QString cl = QStringLiteral("`tests/features/absent_one/` covers it.");

    const SpecLint::Result shipped = SpecLint::check(
        specDoc(QStringLiteral("shipped 2026-05-13 in 0.7.88."), cl),
        QStringLiteral("s.md"), opts);
    EXPECT_EQ(countKind(shipped, "test_surface_absent"), 1);
    EXPECT_EQ(countKind(shipped, "test_surface_unresolved"), 0);

    const SpecLint::Result draft = SpecLint::check(
        specDoc(QStringLiteral("spec draft."), cl), QStringLiteral("d.md"), opts);
    EXPECT_EQ(countKind(draft, "test_surface_unresolved"), 1)
        << "an absent surface in a spec nobody has implemented yet is a forward "
           "reference, and collapsing the two buckets files 20 findings against "
           "specs that are doing nothing wrong";
    EXPECT_EQ(countKind(draft, "test_surface_absent"), 0);

    // The per-finding detail § 2.3 puts on the wire.
    const DocFinding::Finding *f = firstOfKind(shipped, "test_surface_absent");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->extra.value(QStringLiteral("invariant")).toString(),
              QStringLiteral("INV-1"));
    EXPECT_EQ(f->extra.value(QStringLiteral("surface")).toString(),
              QStringLiteral("tests/features/absent_one"));
    EXPECT_EQ(f->extra.value(QStringLiteral("spec_status")).toString(),
              QStringLiteral("shipped"));
    EXPECT_GT(f->line, 0) << "the finding anchors on the INV-N bullet";
    EXPECT_FALSE(f->autoFixable);

    // `v1 shipped …` — the form ANTS-1111 actually carries, and the reason `v1`
    // is in the vocabulary at all.
    const SpecLint::Result v1 = SpecLint::check(
        specDoc(QStringLiteral("v1 shipped 2026-05-13 in 0.7.88."), cl),
        QStringLiteral("v.md"), opts);
    EXPECT_EQ(countKind(v1, "test_surface_absent"), 1);
}

// INV-4 — no Status line, or an unrecognised word, is a CANDIDATE and NEVER a
// FINDING. Two further arms pin § 2.3's normalisation, and each fails a
// different one of its three steps.
TEST(SpecLint, Ants4127Inv4UnparsedStatusNeverReachesFinding) {
    const auto opts  = withDirs({QStringLiteral("something_else")});
    const QString cl = QStringLiteral("`tests/features/absent_one/` covers it.");

    // No Status line at all — 53 specs of this corpus, so the common case.
    const SpecLint::Result none =
        SpecLint::check(specDoc(QString(), cl), QStringLiteral("n.md"), opts);
    EXPECT_EQ(countKind(none, "test_surface_unresolved"), 1);
    EXPECT_EQ(countKind(none, "test_surface_absent"), 0);

    // …and its `spec_status` is JSON null, not "" and not an absent key: a
    // consumer grouping by this field would silently drop a quarter of the
    // corpus if the key could vanish.
    const DocFinding::Finding *nf = firstOfKind(none, "test_surface_unresolved");
    ASSERT_NE(nf, nullptr);
    ASSERT_TRUE(nf->extra.contains(QStringLiteral("spec_status")));
    EXPECT_TRUE(nf->extra.value(QStringLiteral("spec_status")).isNull());

    const SpecLint::Result odd = SpecLint::check(
        specDoc(QStringLiteral("banana (2026-01-01)."), cl),
        QStringLiteral("o.md"), opts);
    EXPECT_EQ(countKind(odd, "test_surface_unresolved"), 1);
    EXPECT_EQ(countKind(odd, "test_surface_absent"), 0);

    // Normalisation step 1 — the value opens BOLD. An engine taking the literal
    // first word reads `**shipped**`, matches no row, and files a CANDIDATE.
    const SpecLint::Result bold = SpecLint::check(
        specDoc(QStringLiteral("**shipped** (2026-01-01)."), cl),
        QStringLiteral("b.md"), opts);
    EXPECT_EQ(countKind(bold, "test_surface_absent"), 1);

    // Normalisation step 3 — trailing punctuation on the word itself.
    const SpecLint::Result comma = SpecLint::check(
        specDoc(QStringLiteral("shipped, then amended."), cl),
        QStringLiteral("c.md"), opts);
    EXPECT_EQ(countKind(comma, "test_surface_absent"), 1);
}

// INV-5 — an abandoned spec is skipped BEFORE either check, so it yields nothing
// at all AND contributes nothing to the counter. Per-spec, not per-check: the
// wiring check is deliberately Status-proof, so a per-check skip would let it
// fire on work nobody intends to finish and report it as drift forever.
TEST(SpecLint, Ants4127Inv5AbandonedSpecsAreSkippedEntirely) {
    const auto opts = withDirs({QStringLiteral("present_one")},
                               {QStringLiteral("something_else")});

    const SpecLint::Result absent = SpecLint::check(
        specDoc(QStringLiteral("superseded (2026-07-28) — merged into ANTS-3663."),
                QStringLiteral("`tests/features/absent_one/` covers it.")),
        QStringLiteral("a.md"), opts);
    EXPECT_TRUE(absent.findings.isEmpty());
    EXPECT_EQ(absent.surfacesResolved, 0);

    // The present-but-UNWIRED arm is the one a per-check skip would miss: INV-6
    // fires whatever the live Status, and only the per-spec skip stops it here.
    const SpecLint::Result unwired = SpecLint::check(
        specDoc(QStringLiteral("superseded (2026-07-28) — merged into ANTS-3663."),
                QStringLiteral("`tests/features/present_one/` covers it.")),
        QStringLiteral("u.md"), opts);
    EXPECT_TRUE(unwired.findings.isEmpty());
    EXPECT_EQ(unwired.surfacesResolved, 0)
        << "a skipped spec resolves nothing, so a walk over one corpus reports "
           "one denominator rather than two";

    const SpecLint::Result considered = SpecLint::check(
        specDoc(QStringLiteral("**considered / shelved (2026-07-19, user decision)"),
                QStringLiteral("`tests/features/absent_one/` covers it.")),
        QStringLiteral("c.md"), opts);
    EXPECT_TRUE(considered.findings.isEmpty())
        << "`**considered` must normalise to `considered`, or a shelved spec "
           "falls to the catch-all and is reported forever";
    EXPECT_EQ(considered.surfacesResolved, 0);
}

// INV-6 — present on disk but in no bundle is a `test_surface_unwired` FINDING
// whatever the live Status. A test source on disk and in no bundle compiles
// nowhere and runs never, and no draft state makes that intended.
TEST(SpecLint, Ants4127Inv6UnwiredIsStatusProof) {
    const auto opts = withDirs({QStringLiteral("present_one")},
                               {QStringLiteral("something_else")});
    const QString cl = QStringLiteral("`tests/features/present_one/` covers it.");

    const SpecLint::Result draft = SpecLint::check(
        specDoc(QStringLiteral("spec draft."), cl), QStringLiteral("d.md"), opts);
    EXPECT_EQ(countKind(draft, "test_surface_unwired"), 1);
    EXPECT_EQ(countKind(draft, "test_surface_absent"), 0)
        << "the directory is right there — a reader told `absent` goes hunting "
           "for a missing one";
    EXPECT_EQ(draft.surfacesResolved, 1) << "it resolved; it just does not run";

    const SpecLint::Result shipped = SpecLint::check(
        specDoc(QStringLiteral("shipped (2026-01-01)."), cl),
        QStringLiteral("s.md"), opts);
    EXPECT_EQ(countKind(shipped, "test_surface_unwired"), 1);

    // A wired directory fires nothing, so the row cannot pass against an engine
    // that reports every resolved surface.
    const SpecLint::Result ok = SpecLint::check(
        specDoc(QStringLiteral("spec draft."), cl), QStringLiteral("k.md"),
        withDirs({QStringLiteral("present_one")}, {QStringLiteral("present_one")}));
    EXPECT_TRUE(ok.findings.isEmpty());
    EXPECT_EQ(ok.surfacesResolved, 1);
}

// INV-7 — `surfaces_resolved` counts distinct SURFACES that resolved, not the
// clauses carrying them, and is emitted even when zero. The multi-surface clause
// is what separates the two readings; a one-per-clause fixture cannot.
TEST(SpecLint, Ants4127Inv7ResolvedCountsSurfacesNotClauses) {
    const QString doc = QStringLiteral(
        "# ANTS-1 — a spec\n"
        "**Status:** shipped (2026-01-01).\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — a rule. *Test:* `tests/features/alpha/` and "
        "`tests/features/beta/`.\n"
        "- **INV-2** — another. *Test:* `tests/features/absent_one/` covers it.\n");
    const SpecLint::Result r = SpecLint::check(
        doc, QStringLiteral("r.md"),
        withDirs({QStringLiteral("alpha"), QStringLiteral("beta")}));
    EXPECT_EQ(r.surfacesResolved, 2) << "two clauses, three surfaces, two found";
    EXPECT_EQ(countKind(r, "test_surface_absent"), 1);

    // Zero is a REPORTED value: without it "checked everything, found nothing"
    // and "harvested nothing" are the same envelope.
    const SpecLint::Result clean = SpecLint::check(
        specDoc(QStringLiteral("shipped (2026-01-01)."),
                QStringLiteral("a prose fixture asserting the thing.")),
        QStringLiteral("p.md"), withDirs({QStringLiteral("alpha")}));
    EXPECT_EQ(clean.surfacesResolved, 0);
    EXPECT_TRUE(clean.surfacesChecked);
    EXPECT_TRUE(clean.findings.isEmpty()) << "a prose surface is invisible by design";
}

// INV-8 — the engine opens no file: both filesystem sets arrive through Options,
// and nothing is written or executed anywhere.
//
// COMMENTS ARE STRIPPED FIRST, and that is not hygiene. speclint.cpp cites
// `QProcess` in a comment as an example of a code span that is NOT a command, so
// the naive grep returns 1 against correct code — and the likely repair is
// weakening the pattern.
TEST(SpecLint, Ants4127Inv8EngineTouchesNoFilesystem) {
    for (const char *path : {SRC_SPECLINT_CPP_PATH, SRC_SPECLINT_H_PATH}) {
        const std::string src =
            ants_test::stripComments(ants_test::slurpFile(path));
        ASSERT_FALSE(src.empty()) << path;
        for (const char *banned :
             {"QProcess", "system(", "popen", "fork(", "QFile", "QDir"})
            EXPECT_EQ(src.find(banned), std::string::npos)
                << path << " must reach the filesystem through Options alone, "
                           "never directly: " << banned;
    }

    // The other arm cannot be static: "this run wrote nothing" is a claim about
    // something that ran. Digest the spec corpus either side of a real check.
    const auto digest = []() {
        QDir dir(QString::fromUtf8(DOCS_SPECS_DIR_PATH));
        QCryptographicHash h(QCryptographicHash::Sha256);
        const QStringList names = dir.entryList({QStringLiteral("*.md")},
                                                QDir::Files, QDir::Name);
        for (const QString &n : names) {
            const QFileInfo fi(dir.filePath(n));
            h.addData(n.toUtf8());
            h.addData(QByteArray::number(fi.size()));
            h.addData(fi.lastModified().toString(Qt::ISODateWithMs).toUtf8());
        }
        return h.result();
    };
    const QByteArray before = digest();
    ASSERT_FALSE(before.isEmpty());
    const QString real = slurp(DOCS_SPECS_DIR_PATH "/ANTS-4127-test-surface-resolution.md");
    ASSERT_FALSE(real.isEmpty()) << "the owning spec must be readable";
    SpecLint::check(real, QStringLiteral("docs/specs/ANTS-4127.md"),
                    withDirs({QStringLiteral("spec_lint")},
                             {QStringLiteral("spec_lint")}));
    EXPECT_EQ(digest(), before) << "a run must leave the corpus byte-identical";
}

// INV-9 — each injected set gates its own check, and EMPTY MEANS SKIP.
//
// (a) against (b) is the whole test: same document, same absent surface, and the
// only difference is whether the set was empty. Three arms all asserting zero
// would pass against an engine that never ran either check.
TEST(SpecLint, Ants4127Inv9EmptySetMeansSkipNotFail) {
    const QString doc = specDoc(QStringLiteral("shipped (2026-01-01)."),
                                QStringLiteral("`tests/features/absent_one/` covers it."));

    const SpecLint::Result a = SpecLint::check(doc, QStringLiteral("a.md"), {});
    EXPECT_TRUE(a.findings.isEmpty()) << "the verb layer supplied nothing";

    const SpecLint::Result b = SpecLint::check(
        doc, QStringLiteral("b.md"), withDirs({QStringLiteral("something_else")}));
    EXPECT_EQ(countKind(b, "test_surface_absent"), 1);

    const SpecLint::Result c = SpecLint::check(
        doc, QStringLiteral("c.md"), withDirs({QStringLiteral("absent_one")}));
    EXPECT_TRUE(c.findings.isEmpty())
        << "`not in the set` must never be read as `not on disk` — one failed "
           "CMakeLists.txt read would become a FINDING against every resolved "
           "surface in the corpus";
}

// INV-10 — `surfaces_checked` is false exactly when INV-9 skipped, and is
// emitted on every run.
//
// ARM (b) IS THE ONLY FALSIFIER of the three. An implementation reading
// `surfaces_checked = (surfaces_resolved > 0)` gives false/0 at (a) and true/1
// at (c) and passes both; so does one setting the flag false whenever ANY check
// skipped. Only flag-true-while-counter-zero separates the contract from either.
TEST(SpecLint, Ants4127Inv10CheckedIsNotInferredFromTheCounter) {
    const QString doc = specDoc(QStringLiteral("shipped (2026-01-01)."),
                                QStringLiteral("`tests/features/absent_one/` covers it."));

    const SpecLint::Result a = SpecLint::check(doc, QStringLiteral("a.md"), {});
    EXPECT_FALSE(a.surfacesChecked);
    EXPECT_EQ(a.surfacesResolved, 0);

    const SpecLint::Result b = SpecLint::check(
        doc, QStringLiteral("b.md"), withDirs({QStringLiteral("something_else")}));
    EXPECT_TRUE(b.surfacesChecked)
        << "check (1) ran — the surface was simply not there";
    EXPECT_EQ(b.surfacesResolved, 0);

    // Arm (c) is also § 2.6's middle row: `wiredTestDirs` is empty here, so the
    // wiring check skipped — and the flag is still true, because it reports
    // whether resolution happened at all, not whether every check fired.
    const SpecLint::Result c = SpecLint::check(
        doc, QStringLiteral("c.md"), withDirs({QStringLiteral("absent_one")}));
    EXPECT_TRUE(c.surfacesChecked);
    EXPECT_EQ(c.surfacesResolved, 1);
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

// ANTS-4351 — a tombstone is exempt however it is WRAPPED.
//
// The anchors are `^\*moved to (ID)\*` and `^\*withdrawn — (.+?)\*`, and `.`
// does not cross a newline — so a hard-wrapped tombstone was not exempt and
// came back as `invariant_no_test`, the same finding a genuinely untested
// invariant gets. The natural reading of that is "my vocabulary is wrong",
// not "my line wrapping is wrong", and one reporting project burned three
// attempts on it. This project then hit it independently the same day while
// withdrawing INV-8 of docs/specs/ANTS-3368-co-change-family.md.
//
// It bites hardest exactly where tombstones happen — hard-wrapped prose specs
// — and withdrawing-with-a-pointer is the only sanctioned way to retire an
// invariant here, so it is the common path rather than an edge.
TEST(SpecLint, Ants4351TombstoneIsExemptAcrossLineWraps) {
    // Same document twice; only the wrapping differs. Neither may be reported.
    const QString oneLine = QStringLiteral(
        "# ANTS-1 — wrapping\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** *withdrawn — superseded by the scan-pattern rule.*\n"
        "- **INV-2** *moved to ANTS-9999*\n");
    const QString wrapped = QStringLiteral(
        "# ANTS-1 — wrapping\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** *withdrawn — superseded by the scan-pattern rule, which\n"
        "  makes the narrower form unreachable and therefore untestable.*\n"
        "- **INV-2** *moved to\n"
        "  ANTS-9999*\n");

    EXPECT_EQ(countKind(SpecLint::check(oneLine, QStringLiteral("s.md"), {}),
                        "invariant_no_test"), 0)
        << "control: the single-line spelling was always exempt";
    EXPECT_EQ(countKind(SpecLint::check(wrapped, QStringLiteral("s.md"), {}),
                        "invariant_no_test"), 0)
        << "a hard-wrapped tombstone must be exempt too — a reader joins the "
           "body before reading it, and so must the check";
}

// ANTS-4351 — and the exemption must not have widened into "mentions the word
// somewhere". The tombstone forms are anchored at the START of the body on
// purpose (spec INV-3): a live invariant whose prose merely discusses
// withdrawal is still a live invariant, and still owes a test.
TEST(SpecLint, Ants4351JoiningDoesNotWidenTheExemption) {
    const QString discussesIt = QStringLiteral(
        "# ANTS-1 — not a tombstone\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** The parser rejects a clause that was *withdrawn — see\n"
        "  the note* in an earlier revision, and says so.\n");
    EXPECT_EQ(countKind(SpecLint::check(discussesIt, QStringLiteral("s.md"), {}),
                        "invariant_no_test"), 1)
        << "prose that merely CONTAINS the tombstone vocabulary is a live "
           "invariant with no test, and joining lines must not exempt it";
}

// ---------------------------------------------------------------------------
// ANTS-4738 — a required heading is matched verbatim, so a descriptive suffix
// reads as an absent section.
//
// It matters more than it looks: it makes the tool dictate house style. One
// reporting corpus uses a qualifier routinely because the section name alone is
// generic, and the standard they had just written had to add a rule forbidding
// the practice purely to satisfy the matcher. A check that changes the prose it
// checks has stopped being a check.
//
// Verbatim stays the DEFAULT. Loosening it silently would weaken every corpus
// that currently relies on the exact match.
TEST(SpecLint, Ants4738PrefixMatchIsOptInOnTheBlock) {
    const QString doc = QStringLiteral(
        "# ANTS-1 — a spec\n"
        "\n"
        "## 1. Problem\n"
        "\n"
        "text\n"
        "\n"
        "## 6. Tests (unit only)\n"
        "\n"
        "text\n");
    const QString block = QStringLiteral(
        "```\n"
        "## 1. Problem\n"
        "## 6. Tests\n"
        "```\n");

    // Default: the marker without the flag keeps the exact match, and the
    // qualified heading is still reported absent.
    bool prefix = true;  // seeded wrong, so an unwritten out-param is caught
    SpecLint::Options strict;
    strict.requiredSections = SpecLint::parseRequiredSections(
        QStringLiteral("<!-- required-sections -->\n") + block, &prefix);
    ASSERT_EQ(strict.requiredSections.size(), 2);
    EXPECT_FALSE(prefix) << "an unflagged marker must not enable prefix matching";
    strict.sectionsPrefixMatch = prefix;
    EXPECT_EQ(countKind(SpecLint::check(doc, QStringLiteral("s.md"), strict),
                        "missing_section"), 1);

    // Flagged: the same document is clean.
    SpecLint::Options loose;
    loose.requiredSections = SpecLint::parseRequiredSections(
        QStringLiteral("<!-- required-sections: prefix -->\n") + block, &prefix);
    ASSERT_EQ(loose.requiredSections.size(), 2)
        << "the flagged marker must still introduce its fenced block";
    EXPECT_TRUE(prefix);
    loose.sectionsPrefixMatch = prefix;
    EXPECT_EQ(countKind(SpecLint::check(doc, QStringLiteral("s.md"), loose),
                        "missing_section"), 0)
        << "`## 6. Tests (unit only)` carries the required number and name";

    // The loosening must not swallow the finding the check exists for. A
    // genuinely absent section is still reported under the flag...
    SpecLint::Options absent;
    absent.requiredSections = SpecLint::parseRequiredSections(
        QStringLiteral("<!-- required-sections: prefix -->\n```\n## 9. Surface\n```\n"),
        &prefix);
    absent.sectionsPrefixMatch = prefix;
    EXPECT_EQ(countKind(SpecLint::check(doc, QStringLiteral("s.md"), absent),
                        "missing_section"), 1);

    // ...and a prefix that ends mid-word is NOT a match. Without this arm the
    // flag would accept `## 6. Test` for `## 6. Tests`, which is a different
    // section with a similar name.
    const QString midWord = QStringLiteral("## 1. Problems\n\ntext\n");
    SpecLint::Options mw;
    mw.requiredSections = SpecLint::parseRequiredSections(
        QStringLiteral("<!-- required-sections: prefix -->\n```\n## 1. Problem\n```\n"),
        &prefix);
    mw.sectionsPrefixMatch = prefix;
    EXPECT_EQ(countKind(SpecLint::check(midWord, QStringLiteral("s.md"), mw),
                        "missing_section"), 1)
        << "`## 1. Problems` is not `## 1. Problem` plus a qualifier";
}

// ANTS-4739 — a document with no way to conform reports the same rows forever.
//
// Measured on the reporting corpus: every residual finding sat on a document
// that CANNOT conform — specs written before the project adopted its section
// run, plus build plans and a fix ledger that are not specs at all but live in
// specs_dir. The documents following the convention reported zero. So the
// residue is permanent, and a reader re-triages it on every run to find the few
// rows that are new. That is the failure the check exists to prevent: noise is
// where a real finding hides.
TEST(SpecLint, Ants4739DocumentCanExemptItselfFromRequiredSections) {
    const QString block = QStringLiteral(
        "<!-- required-sections -->\n"
        "```\n"
        "## 1. Problem\n"
        "## 2. Surface\n"
        "```\n");
    SpecLint::Options opts;
    opts.requiredSections = SpecLint::parseRequiredSections(block);
    ASSERT_EQ(opts.requiredSections.size(), 2);

    const QString body = QStringLiteral("# A build plan\n\n## Steps\n\ntext\n");

    // Control — without the marker the rows are reported, so the arms below
    // cannot pass because the check never fired.
    const SpecLint::Result plain =
        SpecLint::check(body, QStringLiteral("p.md"), opts);
    EXPECT_EQ(countKind(plain, "missing_section"), 2);
    EXPECT_FALSE(plain.sectionsExempt);

    const SpecLint::Result exempt = SpecLint::check(
        QStringLiteral("<!-- spec-lint: no-required-sections -->\n") + body,
        QStringLiteral("p.md"), opts);
    EXPECT_TRUE(exempt.sectionsExempt);
    EXPECT_EQ(countKind(exempt, "missing_section"), 0);

    // sections_checked stays TRUE: the list resolved and the check ran. An
    // exemption says this document opted out, never that nobody looked — which
    // is why the verb layer echoes the COUNT rather than leaving the envelope
    // to imply it.
    EXPECT_TRUE(exempt.sectionsChecked);

    // The marker is honoured only OUTSIDE fenced code, so a standard quoting it
    // as an example cannot exempt the document quoting it.
    const SpecLint::Result quoted = SpecLint::check(
        QStringLiteral("# Doc\n\n```\n<!-- spec-lint: no-required-sections -->\n```\n")
            + body,
        QStringLiteral("p.md"), opts);
    EXPECT_FALSE(quoted.sectionsExempt);
    EXPECT_EQ(countKind(quoted, "missing_section"), 2);
}
