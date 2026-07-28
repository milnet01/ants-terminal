// ANTS-3661 — doc_symbols ENGINE conformance test (INV-1/2/3/5/7). The verb
// hooks and the absence-asserting rows live in tests/features/doc_symbols_verb,
// matching how ANTS-3601 splits engine from verb.
//
// Resolution is exercised against a seeded temp source tree rather than mocked:
// the contract is "SymbolQuery's ladder, unchanged", and a mock would let the
// two drift without a test noticing.

#include "docsymbols.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>

#include <gtest/gtest.h>

#if !defined(ANTS_SOURCE_DIR) || !defined(SRC_MAINWINDOW_CPP_PATH)
#error "doc_symbols test needs the test_claude source-path compile defs"
#endif

namespace {

bool writeFile(const QString &path, const QString &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(content.toUtf8());
    return true;
}

// A tree where `bar`, `parseSpecBody` and `render_frame` resolve and nothing
// else does. SymbolQuery's C++ ladder wants a return-type-led form with a
// trailing `(`.
//
// `render_frame` is bare lowercase on purpose (ANTS-3692): shell, Python and
// Lua name their functions that way, so the ambiguous-shape class must contain
// a member that genuinely resolves or INV-9 could pass by dropping all of them.
bool seedTree(const QString &root) {
    return writeFile(root + "/src/real.cpp",
                     "void bar(int x) {\n    (void)x;\n}\n"
                     "QString parseSpecBody(const QString &s) {\n    return s;\n}\n"
                     "void render_frame(int n) {\n    (void)n;\n}\n");
}

// The calibration harness's stand-in for the live registry: the verb names are
// harvested from their registration sites. Schema argument names have no source
// yet (ANTS-3679), which is precisely what the calibration is meant to measure.
//
// The sites are in mainwindow.cpp, NOT claudeintegration.cpp — the first run of
// this harness scraped the latter, matched almost nothing, and reported verb
// names among its top unresolved population. That was a harness defect reading
// as a verb defect; the live verb reads the real registry.
QSet<QString> registeredVerbNamesFromSource() {
    QSet<QString> out;
    QFile f(QString::fromUtf8(SRC_MAINWINDOW_CPP_PATH));
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QString src = QString::fromUtf8(f.readAll());
    static const QRegularExpression re(QStringLiteral("registerToolProvider\\(\"([a-z0-9_]+)\""));
    auto it = re.globalMatch(src);
    while (it.hasNext()) out.insert(it.next().captured(1));
    out << QStringLiteral("get_session_info") << QStringLiteral("tool_info");
    return out;
}

int countWithResolution(const DocSymbols::ScanResult &r, DocSymbols::Resolution want) {
    int n = 0;
    for (const DocSymbols::Symbol &s : r.symbols)
        if (s.resolution == want) ++n;
    return n;
}

}  // namespace

// INV-1 — a span is a candidate only if it matches § 2.1's production
// whole-span, with all six exclusions applied.
TEST(DocSymbols, Inv1CandidateHarvest) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(seedTree(root));

    const QString doc = QStringLiteral(R"MD(# Title

Prose about `parseSpecBody` and `Foo::bar()` here.
A path `src/a.cpp`, a short word `ok`, a keyword `return`.
A verb `spec_query` and a schema argument `caller_cwd`.

```cpp
`Widget`
```

<!-- doc-examples: begin -->
`Ghost`
<!-- doc-examples: end -->
)MD");

    DocSymbols::Options opts;
    opts.rootCanonical = root;
    // Supplied by the fixture per § 2.4 — the engine takes the registry-sourced
    // set as data, so this test needs no live registry.
    opts.excludedNames = {QStringLiteral("spec_query"), QStringLiteral("caller_cwd")};

    const DocSymbols::ScanResult r = DocSymbols::scan(doc, QStringLiteral("docs/x.md"), opts);

    QStringList got;
    for (const DocSymbols::Symbol &s : r.symbols) got << s.symbol;
    EXPECT_EQ(got, (QStringList{QStringLiteral("parseSpecBody"), QStringLiteral("Foo::bar()")}))
        << "harvested: " << got.join(QStringLiteral(", ")).toStdString();
    EXPECT_EQ(r.total, 2);
}

// INV-2 — a ::-bearing candidate resolves on the text after its last `::`, and
// `symbol` echoes the span verbatim. Reporting the needle would make the
// finding un-greppable against the doc.
TEST(DocSymbols, Inv2NeedleAfterLastScope) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(seedTree(root));

    DocSymbols::Options opts;
    opts.rootCanonical = root;
    const DocSymbols::ScanResult r =
        DocSymbols::scan(QStringLiteral("See `Foo::bar` for detail.\n"),
                         QStringLiteral("docs/x.md"), opts);

    ASSERT_EQ(r.symbols.size(), 1);
    EXPECT_EQ(r.symbols.at(0).symbol, QStringLiteral("Foo::bar"));
    EXPECT_EQ(r.symbols.at(0).resolution, DocSymbols::Resolution::Resolved);
    EXPECT_FALSE(r.symbols.at(0).definitions.isEmpty());
}

// INV-3 — every candidate appears in symbols[] whether resolved or not, and
// only unresolved ones produce a DocFinding.
TEST(DocSymbols, Inv3EveryCandidateReportedOnlyUnresolvedFinds) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(seedTree(root));

    DocSymbols::Options opts;
    opts.rootCanonical = root;
    const DocSymbols::ScanResult r =
        DocSymbols::scan(QStringLiteral("`parseSpecBody` and `NeverDeclared`\n"),
                         QStringLiteral("docs/x.md"), opts);

    ASSERT_EQ(r.symbols.size(), 2);
    EXPECT_EQ(r.resolved, 1);
    EXPECT_EQ(r.unresolved, 1);
    EXPECT_EQ(r.notChecked, 0);
    ASSERT_EQ(r.findings.size(), 1);
    EXPECT_EQ(r.findings.at(0).verb, QStringLiteral("doc_symbols"));
    EXPECT_EQ(r.findings.at(0).kind, QStringLiteral("unresolved_symbol"));
    EXPECT_EQ(r.findings.at(0).file, QStringLiteral("docs/x.md"));
    // § 2.3: a reader decides rot vs forward reference, so nothing is repairable.
    EXPECT_FALSE(r.findings.at(0).autoFixable);
}

// INV-5 — symbols[] ascends by doc_line then doc_col, stable across runs.
TEST(DocSymbols, Inv5DocumentOrder) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    DocSymbols::Options opts;
    opts.rootCanonical = root;  // empty tree — order is what is under test
    const QString doc = QStringLiteral("`Alpha`\n`Bravo` then `Charlie`\n");

    for (int run = 0; run < 2; ++run) {
        const DocSymbols::ScanResult r =
            DocSymbols::scan(doc, QStringLiteral("docs/x.md"), opts);
        ASSERT_EQ(r.symbols.size(), 3) << "run " << run;
        EXPECT_EQ(r.symbols.at(0).symbol, QStringLiteral("Alpha"));
        EXPECT_EQ(r.symbols.at(1).symbol, QStringLiteral("Bravo"));
        EXPECT_EQ(r.symbols.at(2).symbol, QStringLiteral("Charlie"));
        for (int i = 1; i < r.symbols.size(); ++i) {
            const auto &prev = r.symbols.at(i - 1);
            const auto &cur  = r.symbols.at(i);
            EXPECT_TRUE(cur.docLine > prev.docLine
                        || (cur.docLine == prev.docLine && cur.docCol > prev.docCol));
        }
    }
}

// INV-7 — a needle the run never looked up is not_checked, never unresolved,
// and emits no finding. The uncapped half is what makes the capped half
// falsifiable: without it, an engine reporting everything not_checked passes.
TEST(DocSymbols, Inv7ElidedNeedleIsNotChecked) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(seedTree(root));  // neither candidate below is in it

    const QString doc = QStringLiteral("`NeverDeclared` and `AlsoMissing`\n");

    DocSymbols::Options capped;
    capped.rootCanonical    = root;
    capped.maxSymbolsPerRun = 1;
    const DocSymbols::ScanResult r1 = DocSymbols::scan(doc, QStringLiteral("docs/x.md"), capped);

    ASSERT_EQ(r1.symbols.size(), 2);
    EXPECT_EQ(countWithResolution(r1, DocSymbols::Resolution::Unresolved), 1);
    EXPECT_EQ(countWithResolution(r1, DocSymbols::Resolution::NotChecked), 1);
    EXPECT_EQ(r1.notChecked, 1);
    EXPECT_TRUE(r1.truncated);
    // The elided one must NOT be asserted non-existent.
    EXPECT_EQ(r1.findings.size(), 1);

    DocSymbols::Options uncapped;
    uncapped.rootCanonical = root;
    const DocSymbols::ScanResult r2 = DocSymbols::scan(doc, QStringLiteral("docs/x.md"), uncapped);

    ASSERT_EQ(r2.symbols.size(), 2);
    EXPECT_EQ(countWithResolution(r2, DocSymbols::Resolution::Unresolved), 2);
    EXPECT_EQ(r2.notChecked, 0);
    EXPECT_FALSE(r2.truncated);
    EXPECT_EQ(r2.findings.size(), 2);
}

// INV-9 (ANTS-3692) — an ambiguous-shape span (bare lowercase: no `::`, no
// `()`, no case boundary) is emitted ONLY when it resolves. Unresolved, it is
// dropped from symbols[] entirely rather than reported.
//
// Measured on this corpus: of 100 sampled bare-lowercase spans, 7 resolved and
// 60 did not, and every one of the 60 was a JSON response key, a config key or
// a refusal code — `counts`, `truncated`, `dry_run`. A lookup cannot tell those
// from rot, so the engine says nothing rather than guessing, which is the same
// report-never-judge constraint as § 2.3 applied one level earlier.
//
// The shape is NOT the discriminator — resolution is. Requiring a `::`/`()`/
// mixed-case shape was the first proposal and it was wrong: it would blind the
// verb to every shell, Python and Lua symbol, whose naming convention IS bare
// lowercase. `render_frame` below is the guard against that regression.
TEST(DocSymbols, Inv9AmbiguousShapeReportedOnlyWhenResolved) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(seedTree(root));

    DocSymbols::Options opts;
    opts.rootCanonical = root;
    const DocSymbols::ScanResult r = DocSymbols::scan(
        QStringLiteral("`render_frame` resolves, `check_stats` does not, "
                       "and `AlsoMissing` does not either.\n"),
        QStringLiteral("docs/x.md"), opts);

    QStringList got;
    for (const DocSymbols::Symbol &s : r.symbols) got << s.symbol;
    // `check_stats` is gone; `AlsoMissing` survives on shape, proving the drop
    // is scoped to ambiguous spans and is not a blanket unresolved suppression.
    EXPECT_EQ(got, (QStringList{QStringLiteral("render_frame"), QStringLiteral("AlsoMissing")}))
        << "harvested: " << got.join(QStringLiteral(", ")).toStdString();
    EXPECT_EQ(r.resolved, 1);
    EXPECT_EQ(r.unresolved, 1);
    EXPECT_EQ(r.total, 2);
    ASSERT_EQ(r.findings.size(), 1);
    EXPECT_TRUE(r.findings.at(0).message.contains(QStringLiteral("AlsoMissing")))
        << r.findings.at(0).message.toStdString();
}

// INV-10 (ANTS-3692) — two properties that only a capped run can show.
//
// (a) The budget goes to unambiguous needles FIRST. Without this, a doc whose
//     prose keys outnumber its real symbols 17:1 (measured on docs/*.md) spends
//     the whole allowance on names that will be dropped unreported, and the
//     symbols the check exists for come back not_checked.
// (b) `truncated` still fires for a needle nobody looked up even though that
//     needle never reaches symbols[]. Deriving it from notChecked > 0 — the
//     pre-ANTS-3692 form — would silently report a complete run here.
TEST(DocSymbols, Inv10AmbiguousNeedlesResolveLastAndTruncatedSurvivesTheDrop) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(seedTree(root));

    DocSymbols::Options capped;
    capped.rootCanonical    = root;
    capped.maxSymbolsPerRun = 1;
    // Ambiguous span FIRST in document order, so first-appearance ordering
    // alone would hand it the single unit of budget.
    const DocSymbols::ScanResult r = DocSymbols::scan(
        QStringLiteral("`check_stats` and `AlsoMissing`\n"),
        QStringLiteral("docs/x.md"), capped);

    ASSERT_EQ(r.symbols.size(), 1);
    EXPECT_EQ(r.symbols.at(0).symbol, QStringLiteral("AlsoMissing"));
    EXPECT_EQ(r.symbols.at(0).resolution, DocSymbols::Resolution::Unresolved);
    EXPECT_EQ(r.notChecked, 0);   // the elided needle is not in symbols[] …
    EXPECT_TRUE(r.truncated);     // … but the run still admits it was elided.
}

// ANTS-3661 § 2.1 / § 4 — CALIBRATION HARNESS, not a conformance row. It sweeps
// the live corpus and prints the measured split, because "looks dominated" is
// not a test. Two numeric gates it feeds:
//
//   § 2.1 — an exclusion is missing if any single population accounts for
//           >= 25% of unresolved occurrences.
//   § 4   — maxSymbolsPerRun (500) is wrong if the sweep's DISTINCT-needle
//           count exceeds 250, i.e. half the ceiling.
//
// DISABLED_ because it measures the repo rather than asserting a contract, and
// its numbers move as the corpus grows. Re-run when either gate is in question:
//
//   ./build/test_claude --gtest_also_run_disabled_tests
//       --gtest_filter=DocSymbols.DISABLED_CorpusCalibration
//
// PER-DOC is the number that matters, because a reviewer runs this verb over
// one spec. The corpus-wide total is reported beside it to show the gap.
TEST(DocSymbols, DISABLED_CorpusCalibration) {
    const QString root = QString::fromUtf8(ANTS_SOURCE_DIR);
    DocSymbols::Options opts;
    opts.rootCanonical = root;
    opts.excludedNames = registeredVerbNamesFromSource();

    QStringList docs;
    for (const QString &sub : {QStringLiteral("docs/specs"), QStringLiteral("docs/standards")}) {
        QDirIterator it(QDir(root).filePath(sub), {QStringLiteral("*.md")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) docs << it.next();
    }
    docs.sort();
    ASSERT_FALSE(docs.isEmpty()) << "no corpus under " << root.toStdString();

    // The engine's two reductions (spec § 2.2), applied here so this harness
    // can count NEEDLES — what the budget is actually spent on — and not just
    // spans. `Symbol::symbol` is the span VERBATIM, so `Foo::bar()`,
    // `Foo::bar` and `bar` are three spans for one needle, and quoting a span
    // count as a needle count overstates the population it is compared
    // against (ANTS-3721).
    auto needleOf = [](QString s) {
        if (s.endsWith(QLatin1String("()"))) s.chop(2);
        const int i = s.lastIndexOf(QLatin1String("::"));
        return i >= 0 ? s.mid(i + 2) : s;
    };

    int total = 0, resolved = 0, unresolved = 0, notChecked = 0;
    int walksSpent = 0;   // Σ needlesResolved — the only true walk count
    QSet<QString> distinctSpans, distinctNeedles;
    QMap<QString, int> unresolvedByNeedle;
    QList<int> perDocDistinct, perDocSpans;
    int budget = opts.maxSymbolsPerRun;

    for (const QString &abs : std::as_const(docs)) {
        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QString text = QString::fromUtf8(f.readAll());
        f.close();
        opts.maxSymbolsPerRun = budget;
        const DocSymbols::ScanResult r =
            DocSymbols::scan(text, QDir(root).relativeFilePath(abs), opts);
        budget -= r.needlesResolved;
        walksSpent += r.needlesResolved;
        total += r.total; resolved += r.resolved;
        unresolved += r.unresolved; notChecked += r.notChecked;
        QSet<QString> thisDocSpans, thisDocNeedles;
        for (const DocSymbols::Symbol &s : r.symbols) {
            distinctSpans.insert(s.symbol);
            thisDocSpans.insert(s.symbol);
            distinctNeedles.insert(needleOf(s.symbol));
            thisDocNeedles.insert(needleOf(s.symbol));
            if (s.resolution == DocSymbols::Resolution::Unresolved)
                unresolvedByNeedle[s.symbol] += 1;
        }
        perDocDistinct.append(thisDocNeedles.size());
        perDocSpans.append(thisDocSpans.size());
    }
    std::sort(perDocSpans.begin(), perDocSpans.end());
    std::sort(perDocDistinct.begin(), perDocDistinct.end());
    const int median = perDocDistinct.isEmpty() ? 0 : perDocDistinct.at(perDocDistinct.size() / 2);
    const int worst  = perDocDistinct.isEmpty() ? 0 : perDocDistinct.last();
    int overCap = 0;
    for (int n : std::as_const(perDocDistinct))
        if (n > 500) ++overCap;

    // § 2.1's gate is about POPULATIONS, not individual needles: no single name
    // will ever reach 25%, so eyeballing a top-N list cannot answer it. These
    // four buckets are the classes the first run made visible.
    QMap<QString, int> population;
    for (auto it = unresolvedByNeedle.cbegin(); it != unresolvedByNeedle.cend(); ++it) {
        const QString &s = it.key();
        QString bucket = QStringLiteral("other");
        if (s.startsWith(QLatin1String("Qt6::")))
            bucket = QStringLiteral("cmake_target");        // Qt6::Widgets — a link target
        else if (s.startsWith(QLatin1String("m_")))
            bucket = QStringLiteral("cpp_data_member");     // ANTS-3668 resolver gap
        else if (s.size() > 1 && s.at(0) == QLatin1Char('Q') && s.at(1).isUpper())
            bucket = QStringLiteral("qt_framework");        // declared outside the project tree
        population[bucket] += it.value();
    }

    QList<QPair<int, QString>> top;
    for (auto it = unresolvedByNeedle.cbegin(); it != unresolvedByNeedle.cend(); ++it)
        top.append({it.value(), it.key()});
    std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });

    std::cout << "\n=== doc_symbols corpus calibration ===\n"
              << "docs scanned      : " << docs.size() << "\n"
              << "occurrences       : " << total << "\n"
              << "  resolved        : " << resolved << "\n"
              << "  unresolved      : " << unresolved << "\n"
              << "  not_checked     : " << notChecked << "\n"
              << "needle walks spent: " << walksSpent
              << "   (Σ needlesResolved — the run-wide budget's debit; every\n"
              << "                       per-needle cost in § 4 divides by THIS,\n"
              << "                       never by the occurrence count above)\n"
              << "distinct spans    : " << distinctSpans.size() << "\n"
              << "distinct needles  : " << distinctNeedles.size()
              << "   corpus-wide (§ 4 gate: cap is wrong above 250)\n"
              << "per-doc needles   : median " << median << ", worst " << worst
              << ", docs over the 500 cap: " << overCap << "\n"
              << "per-doc spans     : median "
              << (perDocSpans.isEmpty() ? 0 : perDocSpans.at(perDocSpans.size() / 2))
              << ", worst " << (perDocSpans.isEmpty() ? 0 : perDocSpans.last())
              << "\n"
              << "unresolved populations (§ 2.1 gate: >= 25% means an exclusion "
              << "is missing):\n";
    for (auto it = population.cbegin(); it != population.cend(); ++it)
        std::cout << "  " << it.value() << "  ("
                  << (unresolved ? it.value() * 100 / unresolved : 0) << "%)  "
                  << it.key().toStdString() << "\n";
    std::cout << "top unresolved needles:\n";
    for (int i = 0; i < std::min<int>(25, top.size()); ++i)
        std::cout << "  " << top.at(i).first << "  " << top.at(i).second.toStdString() << "\n";
    std::cout << std::flush;
}
