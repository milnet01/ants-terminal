// ANTS-3660 — doc_dedup ENGINE conformance test (INV-1, INV-2, INV-3, INV-4,
// INV-6, INV-7, INV-9). Pure: document text in, pairs out, so every row drives
// DocDedup::Accumulator directly with no MainWindow and no filesystem.
//
// Several rows assert an ABSENCE of pairs and so pass trivially against an
// engine that pairs nothing (INV-2's fence row, INV-3's three exclusion arms).
// They are re-proven by mutation — see spec.md's table for what each mutation
// actually turned red, including the two that turned nothing red and forced a
// sharper fixture.
//
// Fixtures are BUILT, not written out: every threshold row here depends on an
// exact shingle count, and a hand-typed 100-word paragraph is one typo away
// from asserting a different number than its comment claims.

#include "docdedup.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

#include <gtest/gtest.h>

#if !defined(DOCS_DIR_PATH)
#error "doc_dedup test needs DOCS_DIR_PATH"
#endif

namespace {

// "pre0 pre1 … pre<n-1>" — n distinct tokens, so a k-word run yields exactly
// k-2 distinct 3-gram shingles and every Jaccard below is arithmetic.
QStringList tokens(const QString &prefix, int n) {
    QStringList out;
    for (int i = 0; i < n; ++i) out << prefix + QString::number(i);
    return out;
}

QString para(const QStringList &words) { return words.join(QLatin1Char(' ')); }

DocDedup::Result run(const QVector<QPair<QString, QString>> &docs,
                     const DocDedup::Options &opts) {
    DocDedup::Accumulator acc;
    for (const auto &d : docs) acc.add(d.second, d.first, opts);
    return acc.finish();
}

int pairsAt(const DocDedup::Result &r) { return r.pairs.size(); }

}  // namespace

// INV-1 — the threshold is asserted from BOTH sides, because either alone
// passes under both `>` and `>=` (the trap ANTS-3653 INV-40 records).
//
// A is 30 distinct tokens (28 shingles). B repeats A's first 18 (16 shingles
// fall wholly inside that run) then diverges: J = 16/(28+28-16) = 16/40 = 0.400
// EXACTLY. C repeats only A's first 17: J = 15/41 = 0.366.
TEST(DocDedup, Inv1ThresholdFromBothSides) {
    const QStringList a = tokens(QStringLiteral("aw"), 30);
    const QStringList b = a.mid(0, 18) + tokens(QStringLiteral("bw"), 12);
    const QStringList c = a.mid(0, 17) + tokens(QStringLiteral("cw"), 13);

    const QVector<QPair<QString, QString>> docs{
        {QStringLiteral("a.md"), para(a)},
        {QStringLiteral("b.md"), para(b) + QStringLiteral("\n\n") + para(c)},
    };

    // At the threshold: the 0.400 pair reports. A `>` implementation returns 0.
    DocDedup::Options at;
    at.minSimilarity = 0.40;
    const DocDedup::Result rAt = run(docs, at);
    ASSERT_EQ(pairsAt(rAt), 1) << "0.400 is AT the threshold and must report";
    EXPECT_DOUBLE_EQ(rAt.pairs.at(0).similarity, 0.4);
    EXPECT_EQ(rAt.pairs.at(0).a.file, QStringLiteral("a.md"));
    EXPECT_EQ(rAt.pairs.at(0).b.file, QStringLiteral("b.md"));

    // Just above it: the SAME pair must vanish. This is the arm that fails an
    // engine ignoring minSimilarity, or comparing with slack.
    DocDedup::Options above;
    above.minSimilarity = 0.41;
    EXPECT_EQ(pairsAt(run(docs, above)), 0);

    // Below the 0.366 pairs: all three appear. Without this row the fixture
    // never proves C is comparable at all, and "C did not report" would be
    // satisfied by C having been dropped for some unrelated reason.
    DocDedup::Options below;
    below.minSimilarity = 0.36;
    EXPECT_EQ(pairsAt(run(docs, below)), 3);
}

// INV-2 — fenced blocks never pair, and a masked fence line breaks a paragraph
// rather than joining it.
TEST(DocDedup, Inv2FenceAwareness) {
    // (a) the same code sample in two docs, nothing else shared. The sample is
    // deliberately WORDY (well over minWords once normalised) so that dropping
    // the fence mask makes it pair — a terse sample would be excluded by
    // minWords instead and the mutation would prove nothing.
    QString sample = QStringLiteral("```cpp\n");
    for (int i = 0; i < 12; ++i)
        sample += QStringLiteral("    const auto value%1 = compute total width "
                                 "for every column in this row;\n").arg(i);
    sample += QStringLiteral("```\n");

    const QVector<QPair<QString, QString>> shared{
        {QStringLiteral("a.md"), para(tokens(QStringLiteral("ap"), 40)) +
                                     QStringLiteral("\n\n") + sample},
        {QStringLiteral("b.md"), para(tokens(QStringLiteral("bp"), 40)) +
                                     QStringLiteral("\n\n") + sample},
    };
    const DocDedup::Result r = run(shared, {});
    EXPECT_EQ(pairsAt(r), 0) << "two identical code samples are not a "
                                "duplicated fact";
    EXPECT_EQ(r.passagesTotal, 2) << "one prose paragraph per doc; the fenced "
                                     "sample is not a passage at all";

    // (b) a fence BREAKS a run. The same two prose lines, once with a fence
    // between them and once without: two passages, then one.
    const QString line = para(tokens(QStringLiteral("pw"), 20));
    const QString fenced = line + QStringLiteral("\n```\nx = 1;\n```\n") + line;
    const QString joined = line + QStringLiteral("\n") + line;

    const DocDedup::Result rf =
        run({{QStringLiteral("f.md"), fenced}}, {});
    EXPECT_EQ(rf.passagesTotal, 2);
    EXPECT_EQ(pairsAt(rf), 1) << "and the two halves are identical, so they "
                                 "pair — which is what proves they are two";

    const DocDedup::Result rj =
        run({{QStringLiteral("j.md"), joined}}, {});
    EXPECT_EQ(rj.passagesTotal, 1);
    EXPECT_EQ(pairsAt(rj), 0);
}

// INV-3 — all three § 2.4 exclusions hold, and structural boilerplate is NOT
// excluded. The last arm is the regression guard for the deleted
// boilerplate filter: an implementation that reinstates one passes every other
// row in this file.
TEST(DocDedup, Inv3ExclusionsAndTheDeletedOne) {
    // (a) below minWords — an 8-word note repeated verbatim.
    const QString note = para(tokens(QStringLiteral("nw"), 8));
    EXPECT_EQ(pairsAt(run({{QStringLiteral("a.md"), note},
                           {QStringLiteral("b.md"), note}}, {})), 0);

    // (b) pointer lines. Long enough to clear minWords once normalised — a
    // short `see § 4` would be excluded by minWords and prove nothing about
    // the pointer rule. Three shapes: bare link, see-prefixed, list item.
    const QString link = QStringLiteral(
        "[the canonical statement of the passage segmentation rule and every "
        "constant it depends on](docs/specs/ANTS-3660.md#2-1-passages)");
    ASSERT_TRUE(DocDedup::isPointerLine(link));
    ASSERT_TRUE(DocDedup::isPointerLine(QStringLiteral("See ") + link + QStringLiteral(".")));
    ASSERT_TRUE(DocDedup::isPointerLine(QStringLiteral("- ") + link));
    EXPECT_EQ(pairsAt(run({{QStringLiteral("a.md"), link},
                           {QStringLiteral("b.md"), link}}, {})), 0);

    // ...and a sentence that merely CONTAINS a link is prose, not a pointer.
    // Without this the rule could be "any paragraph with a link in it" and
    // every row above would still pass.
    const QString prose = para(tokens(QStringLiteral("sw"), 25)) +
                          QStringLiteral(" ") + link;
    EXPECT_FALSE(DocDedup::isPointerLine(prose));
    EXPECT_EQ(pairsAt(run({{QStringLiteral("a.md"), prose},
                           {QStringLiteral("b.md"), prose}}, {})), 1);

    // (c) generated / templated artifacts, by path glob.
    const QString stanza = para(tokens(QStringLiteral("gw"), 40));
    DocDedup::Options ex;
    ex.excludedPathGlobs =
        QStringList{QStringLiteral("*AUTOMATED_AUDIT_REPORT*"),
                    QStringLiteral("*superpowers/*")};
    EXPECT_TRUE(DocDedup::isExcludedPath(
        QStringLiteral("docs/superpowers/plans/x.md"), ex));
    EXPECT_TRUE(DocDedup::isExcludedPath(
        QStringLiteral("AUTOMATED_AUDIT_REPORT_2026-07-01.md"), ex));
    EXPECT_FALSE(DocDedup::isExcludedPath(QStringLiteral("docs/specs/a.md"), ex));
    EXPECT_EQ(pairsAt(run({{QStringLiteral("docs/superpowers/plans/a.md"), stanza},
                           {QStringLiteral("docs/superpowers/plans/b.md"), stanza}},
                          ex)), 0);
    // The same two passages under ordinary paths DO pair — so the row above
    // measures the glob rather than the fixture being unpairable.
    EXPECT_EQ(pairsAt(run({{QStringLiteral("docs/specs/a.md"), stanza},
                           {QStringLiteral("docs/specs/b.md"), stanza}}, ex)), 1);

    // (d) THE DELETED EXCLUSION — two sibling specs sharing an identical
    // header block is a FINDING, not noise.
    //
    // The shared paragraph IS a spec header block — it opens `**Status:**` and
    // carries the whole `Kind` / `Source` / `Blocked by` stanza — and it is
    // long enough to clear minWords on its own. That is deliberate: an
    // implementation reinstating "a spec's header block" as an exclusion drops
    // this pair and still passes every other row in this file, so a fixture
    // whose header is under minWords (as an earlier draft's was) would let the
    // mutation survive.
    const QString header = QStringLiteral(
        "**Status:** accepted (2026-07-28). **Kind:** implement. "
        "**Source:** a user request from the same afternoon as its siblings. "
        "**Blocked by:** nothing at all. **Blocker for:** nothing either. "
        "**Composes with:** every sibling spec carrying this identical block.");
    ASSERT_GE(DocDedup::normalise(header).split(QLatin1Char(' ')).size(), 15)
        << "the header block must clear minWords on its own, or the "
           "boilerplate-filter mutation has nothing to delete";
    EXPECT_EQ(pairsAt(run({{QStringLiteral("docs/specs/ANTS-1.md"), header},
                           {QStringLiteral("docs/specs/ANTS-2.md"), header}},
                          {})), 1)
        << "repeated boilerplate across sibling specs is exactly what this "
           "corpus over-produces and exactly what the verb exists to report";
}

// INV-4 — each pair appears once, canonicalised so `a` precedes `b` in
// (file, line) order, and no paragraph pairs with itself.
//
// The documents are added OUT of alphabetical order. Index order then differs
// from (file, line) order, so an engine that just emits (lower index, higher
// index) fails — which is the only way this row is not free.
TEST(DocDedup, Inv4PairsCanonicalAndUnique) {
    const QString stanza = para(tokens(QStringLiteral("cw"), 40));
    const DocDedup::Result r = run({{QStringLiteral("c.md"), stanza},
                                    {QStringLiteral("a.md"), stanza},
                                    {QStringLiteral("b.md"), stanza}}, {});
    ASSERT_EQ(pairsAt(r), 3) << "three mutually similar passages, not six";

    QSet<QString> seen;
    for (const auto &p : r.pairs) {
        EXPECT_DOUBLE_EQ(p.similarity, 1.0);
        EXPECT_FALSE(p.a.file == p.b.file && p.a.line == p.b.line)
            << "no passage pairs with itself";
        const bool ordered = (p.a.file < p.b.file) ||
                             (p.a.file == p.b.file && p.a.line < p.b.line);
        EXPECT_TRUE(ordered) << "a must precede b: " << p.a.file.toStdString()
                             << " vs " << p.b.file.toStdString();
        seen.insert(p.a.file + QLatin1Char('|') + p.b.file);
    }
    EXPECT_EQ(seen.size(), 3) << "each unordered pair emitted once";
}

// INV-6 — maxPassages caps the paragraphs COMPARED, not those enumerated. A
// run that simply stopped walking could not report the first number.
TEST(DocDedup, Inv6PassageCapCountsAll) {
    QStringList paras;
    for (int i = 0; i < 30; ++i)
        paras << para(tokens(QStringLiteral("d%1w").arg(i), 20));

    DocDedup::Options opts;
    opts.maxPassages = 10;
    const DocDedup::Result r =
        run({{QStringLiteral("a.md"), paras.join(QStringLiteral("\n\n"))}}, opts);
    EXPECT_TRUE(r.truncated);
    EXPECT_EQ(r.passagesTotal, 30);
    EXPECT_EQ(r.passagesCompared, 10);

    // Uncapped, the same corpus reports 30/30 and is NOT truncated — so the
    // row above measures the cap and not the fixture.
    const DocDedup::Result full =
        run({{QStringLiteral("a.md"), paras.join(QStringLiteral("\n\n"))}}, {});
    EXPECT_FALSE(full.truncated);
    EXPECT_EQ(full.passagesTotal, 30);
    EXPECT_EQ(full.passagesCompared, 30);
}

// INV-7 — a stop-shingle is dropped from candidate GATHERING and still counted
// in every SCORE. The fixture sets maxPostings low enough to fire: at the
// shipped 400 nothing in the real corpus reaches it, so a test run against
// docs/ would assert nothing.
//
// Twelve paragraphs all opening "alpha beta gamma" (12 postings). p0 and p1
// additionally share a 20-token tail. Each is 24 words → 22 shingles; they
// share the common opener plus 18 tail shingles = 19, so the EXACT Jaccard is
// 19/(22+22-19) = 19/25 = 0.76. Score it without the pruned shingle and it is
// 18/25 = 0.72 — which is what makes this row an assertion rather than a hope.
TEST(DocDedup, Inv7StopShinglePrune) {
    const QStringList tail = tokens(QStringLiteral("t"), 20);
    QStringList paras;
    for (int i = 0; i < 12; ++i) {
        QStringList w{QStringLiteral("alpha"), QStringLiteral("beta"),
                      QStringLiteral("gamma"), QStringLiteral("u%1").arg(i)};
        w += (i < 2) ? tail : tokens(QStringLiteral("s%1_").arg(i), 20);
        paras << para(w);
    }
    const QVector<QPair<QString, QString>> docs{
        {QStringLiteral("a.md"), paras.join(QStringLiteral("\n\n"))}};

    DocDedup::Options pruned;
    pruned.maxPostings = 3;
    const DocDedup::Result rp = run(docs, pruned);
    ASSERT_EQ(pairsAt(rp), 1) << "the genuine pair survives the prune";
    EXPECT_DOUBLE_EQ(rp.pairs.at(0).similarity, 19.0 / 25.0)
        << "the pruned shingle is still counted in the score";
    EXPECT_EQ(rp.candidatePairsConsidered, 1)
        << "every other pair shares only the stop-shingle and is never scored";

    // Unpruned, the same corpus scores the same pair identically and considers
    // all 66. Same score, different work — which is the whole claim.
    DocDedup::Options open;
    open.maxPostings = 400;
    const DocDedup::Result ro = run(docs, open);
    ASSERT_EQ(pairsAt(ro), 1);
    EXPECT_DOUBLE_EQ(ro.pairs.at(0).similarity, 19.0 / 25.0);
    EXPECT_EQ(ro.candidatePairsConsidered, 12 * 11 / 2);
    EXPECT_LT(rp.candidatePairsConsidered, ro.candidatePairsConsidered);
}

// INV-9 — clusters are the CONNECTED components of pairs[], not the pairs
// themselves. A pairs with B and B with C, but A and C score 0.101 and never
// pair: one cluster of three, not two of two.
//
// B is 100 base tokens. A carries B's first 60, C its last 60, so A and C
// overlap in only 20 — the arithmetic that makes the transitive row possible
// at all (58/138 = 0.420 twice, 18/178 = 0.101 once).
TEST(DocDedup, Inv9ClustersAreConnectedComponents) {
    const QStringList base = tokens(QStringLiteral("b"), 100);
    const QString a = para(base.mid(0, 60) + tokens(QStringLiteral("ua"), 40));
    const QString b = para(base);
    const QString c = para(base.mid(40, 60) + tokens(QStringLiteral("uc"), 40));
    const QString d = para(tokens(QStringLiteral("dw"), 40));

    const DocDedup::Result r = run({{QStringLiteral("a.md"), a},
                                    {QStringLiteral("b.md"), b},
                                    {QStringLiteral("c.md"), c},
                                    {QStringLiteral("d.md"), d},
                                    {QStringLiteral("e.md"), d}}, {});

    // The premise, asserted rather than assumed: A-B and B-C pair, A-C does not.
    ASSERT_EQ(pairsAt(r), 3);
    bool sawAC = false;
    for (const auto &p : r.pairs)
        if (p.a.file == QStringLiteral("a.md") && p.b.file == QStringLiteral("c.md"))
            sawAC = true;
    ASSERT_FALSE(sawAC) << "A and C must NOT pair, or the row is not transitive";

    ASSERT_EQ(r.clusters.size(), 2);
    const DocDedup::Cluster &big =
        r.clusters.at(0).passages.size() >= r.clusters.at(1).passages.size()
            ? r.clusters.at(0) : r.clusters.at(1);
    const DocDedup::Cluster &small =
        r.clusters.at(0).passages.size() >= r.clusters.at(1).passages.size()
            ? r.clusters.at(1) : r.clusters.at(0);
    EXPECT_EQ(big.passages.size(), 3)
        << "an implementation grouping only directly-scored pairs returns two "
           "clusters of two here and passes every other row";
    EXPECT_NEAR(big.maxSimilarity, 58.0 / 138.0, 1e-9);
    EXPECT_EQ(small.passages.size(), 2);
    EXPECT_DOUBLE_EQ(small.maxSimilarity, 1.0);

    // Every reported passage in exactly one cluster.
    QSet<QString> members;
    int total = 0;
    for (const auto &cl : r.clusters)
        for (const auto &p : cl.passages) {
            members.insert(p.file + QLatin1Char(':') + QString::number(p.line));
            ++total;
        }
    EXPECT_EQ(members.size(), total) << "no passage appears in two clusters";
    EXPECT_EQ(total, 5);
}

// Spec § 2.3's acceptance criterion, measured against the real corpus. Not an
// invariant row and not run by ctest: it walks docs/, its numbers move whenever
// a document is edited, and its job is to be re-runnable by the next person to
// touch a constant rather than re-derived.
//
//   ./build/test_claude --gtest_also_run_disabled_tests
//                       --gtest_filter=DocDedup.DISABLED_CorpusCalibration
//
// Every constant in ANTS-3660 came from a run like this one.
TEST(DocDedup, DISABLED_CorpusCalibration) {
    DocDedup::Options opts;
    opts.excludedPathGlobs =
        QStringList{QStringLiteral("*AUTOMATED_AUDIT_REPORT*"),
                    QStringLiteral("*superpowers/*")};

    const QString root = QString::fromUtf8(DOCS_DIR_PATH);
    QDirIterator it(root, {QStringLiteral("*.md")}, QDir::Files,
                    QDirIterator::Subdirectories);
    QStringList files;
    while (it.hasNext()) files << it.next();
    files.sort();
    ASSERT_FALSE(files.isEmpty());

    int walked = 0, excluded = 0;
    const auto walk = [&](const DocDedup::Options &o, bool honourGlobs) {
        DocDedup::Accumulator acc;
        for (const QString &abs : files) {
            const QString rel =
                QStringLiteral("docs/") + QDir(root).relativeFilePath(abs);
            if (honourGlobs && DocDedup::isExcludedPath(rel, opts)) {
                ++excluded;
                continue;
            }
            QFile f(abs);
            if (!f.open(QIODevice::ReadOnly)) continue;
            acc.add(QString::fromUtf8(f.readAll()), rel, o);
            if (honourGlobs) ++walked;
        }
        return acc.finish();
    };
    const DocDedup::Result r = walk(opts, true);

    // § 2.4's exclusion claim, measured by the shipping engine rather than
    // asserted: the same walk with the globs off. An exclusion nobody can put a
    // number on is the kind this spec already deleted once.
    const DocDedup::Result all = walk({}, false);
    const auto exact = [](const DocDedup::Result &x) {
        int n = 0;
        for (const auto &p : x.pairs) if (p.similarity >= 0.9999) ++n;
        return n;
    };
    fprintf(stderr,
            "  exclusions: %d files / %d passages removed; report %d pairs / "
            "%d clusters -> %d / %d; EXACT duplicates %d -> %d\n",
            int(files.size()) - walked, all.passagesTotal - r.passagesTotal,
            int(all.pairs.size()), int(all.clusters.size()),
            int(r.pairs.size()), int(r.clusters.size()),
            exact(all), exact(r));

    // stderr rather than qInfo: this bundle suppresses qInfo, and a measurement
    // whose output is swallowed is a measurement nobody makes twice.
    fprintf(stderr,
            "doc_dedup corpus calibration: %d docs walked (%d excluded), "
            "%d passages, %d pairs, %d clusters, %d candidate comparisons\n",
            walked, excluded, r.passagesTotal, int(r.pairs.size()),
            int(r.clusters.size()), r.candidatePairsConsidered);
    fprintf(stderr,
            "  index: %d distinct shingles, %d postings, longest list %d "
            "(maxPostings %d)\n",
            r.distinctShingles, r.totalPostings, r.maxPostingLength,
            opts.maxPostings);

    // Pairs AND clusters per threshold edge: § 2.5's claim is precisely that
    // the ratio between them widens as the threshold tightens, and a pair count
    // alone cannot show that.
    for (double edge : {1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4}) {
        QHash<QString, QString> parent;
        const std::function<QString(QString)> rootOf = [&](QString x) -> QString {
            while (parent.value(x, x) != x) x = parent.value(x);
            return x;
        };
        int np = 0;
        QSet<QString> nodes;
        for (const auto &p : r.pairs) {
            if (p.similarity < edge) continue;
            ++np;
            const QString ka = p.a.file + QLatin1Char(':') + QString::number(p.a.line);
            const QString kb = p.b.file + QLatin1Char(':') + QString::number(p.b.line);
            nodes.insert(ka);
            nodes.insert(kb);
            const QString ra = rootOf(ka), rb = rootOf(kb);
            if (ra != rb) parent.insert(ra, rb);
        }
        QSet<QString> roots;
        for (const QString &n : std::as_const(nodes)) roots.insert(rootOf(n));
        fprintf(stderr, "  >=%.2f  pairs=%4d  clusters=%4d\n", edge, np,
                int(roots.size()));
    }

    // § 2.5's within/across split. Within-document pairs are legal (only a
    // self-pair is not, INV-4) and this is the number that says whether that
    // decision changed the output volume or merely the wording.
    int within = 0;
    for (const auto &p : r.pairs) if (p.a.file == p.b.file) ++within;
    fprintf(stderr, "  within-document=%d  across=%d\n", within,
            int(r.pairs.size()) - within);
    // Sample the band a threshold decision turns on. A rate nobody reads is
    // not a measurement — someone has to look at the rows.
    int shown = 0;
    for (const auto &p : r.pairs) {
        if (p.similarity >= 0.75 || p.similarity < 0.40) continue;
        if (shown++ % 40) continue;
        fprintf(stderr, "  [%.3f] %s:%d <-> %s:%d\n", p.similarity,
                qPrintable(p.a.file), p.a.line, qPrintable(p.b.file), p.b.line);
    }

    // § 2.3's acceptance criterion, checked rather than admired.
    EXPECT_LE(r.clusters.size(), 150)
        << "cluster count must stay triageable in a sitting";
    for (const auto &probe : {std::make_pair(0.583, "verb-contract minimum"),
                              std::make_pair(0.450, "build-cost stanza")}) {
        bool found = false;
        for (const auto &p : r.pairs)
            if (qAbs(p.similarity - probe.first) <= 0.005) found = true;
        EXPECT_TRUE(found) << "§ 1.1's " << probe.second
                           << " pair must still report at " << probe.first;
    }
}

// ANTS-3691 / INV-10 — deterministic emission order. ANTS-3663 INV-7 makes
// findings totally ordered with the producer's own emissionIndex as the
// tiebreak of last resort, and that tiebreak is only as well-defined as each
// producer's order. Three of the five producers state one; this engine's was
// real in code and stated nowhere, so the consumer's invariant rested on an
// unwritten guarantee.
//
// The order is strongest similarity first, then by location. Both halves are
// asserted: a corpus with two DIFFERENT similarities fixes the primary key,
// and a corpus with two EQUAL ones fixes the tiebreak — either alone passes
// against an implementation that has only the other.
TEST(DocDedup, Ants3691EmissionOrderIsStrongestThenLocation) {
    DocDedup::Options o;
    const QStringList base = tokens(QStringLiteral("w"), 30);

    // z.md/y.md are a NEAR-exact pair; b.md shares less with a.md. The two
    // similarities differ, so similarity alone decides the order.
    const QString a = para(base);
    const QString strong = para(base);                       // identical
    const QString weak   = para(base.mid(0, 18) + tokens(QStringLiteral("q"), 12));

    const auto r = run({{QStringLiteral("a.md"), a},
                        {QStringLiteral("b.md"), weak},
                        {QStringLiteral("y.md"), strong},
                        {QStringLiteral("z.md"), a}},
                       o);
    ASSERT_GE(pairsAt(r), 2) << "the fixture must produce pairs to order";
    ASSERT_EQ(r.findings.size(), r.pairs.size());

    for (int i = 1; i < r.findings.size(); ++i) {
        const auto &px = r.pairs.at(i - 1);
        const auto &py = r.pairs.at(i);
        EXPECT_GE(px.similarity, py.similarity)
            << "strongest first is the primary key";
        if (px.similarity == py.similarity) {
            // The tiebreak chain, in the order the engine applies it.
            const bool ordered =
                (px.a.file < py.a.file) ||
                (px.a.file == py.a.file && px.a.line < py.a.line) ||
                (px.a.file == py.a.file && px.a.line == py.a.line &&
                 (px.b.file < py.b.file ||
                  (px.b.file == py.b.file && px.b.line <= py.b.line)));
            EXPECT_TRUE(ordered)
                << "equal similarity falls back to location, a then b";
        }
        EXPECT_LT(r.findings.at(i - 1).emissionIndex,
                  r.findings.at(i).emissionIndex)
            << "emissionIndex is strictly ascending — the field ANTS-3663 "
               "INV-7 actually reads";
    }
}
