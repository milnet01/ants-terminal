// Feature-conformance test for docs_index reading **Status:** with the shared
// header-field rule (ANTS-3786). Behavioural invariants drive DocsIndex::build()
// over a QTemporaryDir copy of fixtures/; INV-7 source-scrapes the adoption;
// INV-9 runs tools/spec-header-survey.py. See spec.md +
// docs/specs/ANTS-3786-docsindex-header-field.md.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "docsindex.h"
#include "specparse.h"

#include <string>

#include <gtest/gtest.h>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif
#ifndef SRC_DOCSINDEX_CPP_PATH
#error "SRC_DOCSINDEX_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

using namespace DocsIndex;

const char *kFixtures =
    ANTS_SOURCE_DIR "/tests/features/docsindex_header_field/fixtures";

// Copy one fixture subtree into `dst`. The invariants are asserted against a
// QTemporaryDir rather than the source tree so a walk can never write there.
void copyTree(const QString &src, const QString &dst) {
    QDir().mkpath(dst);
    QDirIterator it(src, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString from = it.next();
        const QString rel = QDir(src).relativeFilePath(from);
        const QString to = dst + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(to).absolutePath());
        ASSERT_TRUE(QFile::copy(from, to)) << to.toStdString();
    }
}

// `sub` is a fixture root ("corpus", "eof-flush", "exclusions"); returns the
// temp-dir path it was copied to. `dir` must outlive the returned path.
QString stage(QTemporaryDir &dir, const char *sub) {
    const QString dst = dir.path() + QStringLiteral("/root");
    copyTree(QString::fromUtf8(kFixtures) + QLatin1Char('/') +
                 QString::fromUtf8(sub),
             dst);
    return dst;
}

void writeFile(const QString &path, const QString &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content.toUtf8());
}

const DocEntry *findEntry(const Index &idx, const QString &rel) {
    for (const DocEntry &de : idx.docs)
        if (de.path == rel) return &de;
    return nullptr;
}

// The file's RAW lines — what headerField would see called directly, i.e.
// without scanDoc's over-long-line and fenced-block skips.
QStringList rawLines(const QString &absPath) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
}

const char *kWrappedWhole =
    "accepted 2026-08-02 — the value continues onto a second physical line, "
    "and onto a third, because the corpus hard-wraps at about eighty columns.";

}  // namespace

// INV-1 — a wrapped value yields the whole logical string, not its first
// physical line. The EXPECT_NE pins the pre-fix return value: statusRx captured
// exactly that prefix, so an implementation that regressed to it fails here
// with a named cause rather than a diff.
TEST(DocsIndexHeaderField, WrappedStatusIsJoinedWhole) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const Index idx = build(stage(dir, "corpus"), 1000);

    const DocEntry *e = findEntry(idx, QStringLiteral("docs/wrapped.md"));
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, QString::fromUtf8(kWrappedWhole));
    EXPECT_NE(e->status,
              QString::fromUtf8("accepted 2026-08-02 — the value continues "
                                "onto a second"))
        << "this is the pre-fix truncation at the first physical line";
}

// INV-2, equality half — every corpus fixture satisfies all four exclusion
// conditions (no fence, no over-long line, block under the cap, read not
// budget-truncated), so within those bounds docsindex and headerField are one
// implementation rather than two that happen to agree.
TEST(DocsIndexHeaderField, MatchesSharedRuleWithinBounds) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = stage(dir, "corpus");
    const Index idx = build(root, 1000);

    ASSERT_EQ(idx.docs.size(), 4);
    for (const DocEntry &e : idx.docs) {
        const auto want = SpecParse::headerField(
            rawLines(root + QLatin1Char('/') + e.path),
            QStringLiteral("Status"));
        EXPECT_EQ(e.status, want.value) << e.path.toStdString();
    }
}

// INV-2, difference half — outside those bounds the two inputs are deliberately
// different line lists, so the bound is pinned in BOTH directions. An
// unconditional equality would be false, not merely strict.
//
// Both fixtures place the excluded line BETWEEN the Status line and the prose
// after it. Because the skips are `continue`s the surviving lines close up, so
// that prose becomes adjacent to the field and is absorbed into its value —
// text the raw-line path never reaches. That second-order effect is the reason
// this half exists.
TEST(DocsIndexHeaderField, ExcludedLinesMakeTheTwoDiverge) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = stage(dir, "exclusions");
    const Index idx = build(root, 1000);

    struct Case { const char *path; const char *raw; };
    // fenced.md: the raw path treats the fence opener as a continuation and
    // stops at the blank line inside the fence. overlong.md: the raw path
    // terminates on the over-long line because it is itself a field marker.
    const Case cases[] = {
        {"docs/fenced.md", "draft ~~~"},
        {"docs/overlong.md", "draft"},
    };
    for (const Case &c : cases) {
        const DocEntry *e = findEntry(idx, QString::fromUtf8(c.path));
        ASSERT_NE(e, nullptr) << c.path;
        const auto raw = SpecParse::headerField(
            rawLines(root + QLatin1Char('/') + QString::fromUtf8(c.path)),
            QStringLiteral("Status"));
        EXPECT_EQ(raw.value, QString::fromUtf8(c.raw)) << c.path;
        EXPECT_EQ(e->status, QStringLiteral("draft absorbed")) << c.path;
        EXPECT_NE(e->status, raw.value)
            << c.path << ": the exclusion must be visible, not silently equal";
    }
}

// INV-3 — the search is bounded to the header block. The fixture's field is
// well-formed and unwrapped, so only the bound can reject it: INV-1's rule and
// the cap both accept it happily.
TEST(DocsIndexHeaderField, StatusBelowHeaderBlockIsNotRead) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const Index idx = build(stage(dir, "corpus"), 1000);

    const DocEntry *e = findEntry(idx, QStringLiteral("docs/below-block.md"));
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, QString())
        << "body prose below the first `## ` is not a document status";
    EXPECT_FALSE(e->headings.isEmpty()) << "the entry is still indexed";
}

// INV-4 — the header buffer is capped, and the cap is SILENT: no flag on the
// entry, exactly as ANTS-2139 INV-19 already specifies for headings and links.
TEST(DocsIndexHeaderField, HeaderBlockCapIsSilentAndBounded) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.path() + QStringLiteral("/capped.md"),
              QStringLiteral("# T\n"
                             "\n"
                             "filler\n"
                             "filler\n"
                             "**Status:** past the cap.\n"
                             "\n"
                             "## B\n"));

    Options opts;
    opts.maxHeaderBlockLines = 2;
    const Index idx = build(dir.path(), 1000, opts);

    const DocEntry *e = findEntry(idx, QStringLiteral("capped.md"));
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, QString()) << "the field sat past the cap";
    EXPECT_FALSE(e->headings.isEmpty())
        << "the entry is still emitted with its headings — the cap is silent";
    EXPECT_EQ(e->title, QStringLiteral("T"));

    // The same document under the default cap: the cap, not the fixture, is
    // what emptied the status above.
    const Index full = build(dir.path(), 1000);
    const DocEntry *f = findEntry(full, QStringLiteral("capped.md"));
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->status, QStringLiteral("past the cap."));
}

// INV-5 — ANTS-2139 INV-19 is unchanged: a read that ends on the byte budget
// leaves an empty status, never a partial field value. Only the budgetHit
// suppression can satisfy this — the field's OPENING line is inside the budget,
// so an unguarded EOF flush would return "one", a visibly different assertion.
TEST(DocsIndexHeaderField, BudgetTruncatedReadYieldsEmptyStatus) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // 4 + 1 + 16 = 21 bytes through the Status opening line; the next line
    // pushes the budget over and breaks the loop mid-header-block.
    writeFile(dir.path() + QStringLiteral("/straddle.md"),
              QStringLiteral("# T\n"
                             "\n"
                             "**Status:** one\n"
                             "two\n"
                             "\n"
                             "## B\n"));

    Options opts;
    opts.maxDocBytes = 21;
    const Index idx = build(dir.path(), 1000, opts);

    const DocEntry *e = findEntry(idx, QStringLiteral("straddle.md"));
    ASSERT_NE(e, nullptr) << "the entry is still emitted";
    EXPECT_EQ(e->status, QString());
    EXPECT_NE(e->status, QStringLiteral("one"))
        << "a truncated buffer must not be flushed as a complete header block";
}

// INV-6 — absence is never an error (ANTS-2139 INV-17's surviving half).
TEST(DocsIndexHeaderField, NoStatusLineIsNotAnError) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const Index idx = build(stage(dir, "corpus"), 1000);

    const DocEntry *e = findEntry(idx, QStringLiteral("docs/no-status.md"));
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status, QString());
    EXPECT_EQ(e->id, QStringLiteral("no-status"));
}

// INV-8 — the EOF exit. Only the flush can satisfy this fixture: the `^## `
// exit never fires and the block is far under the cap, so an implementation
// without it returns "" and loses a status the pre-fix code did report.
TEST(DocsIndexHeaderField, HeaderBlockIsFlushedAtEof) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const Index idx = build(stage(dir, "eof-flush"), 1000);

    const DocEntry *e = findEntry(idx, QStringLiteral("docs/no-heading.md"));
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->status,
              QStringLiteral("flushed at end of file, because this document "
                             "carries no level-two heading anywhere for the "
                             "block bound to fire on."));
}

// INV-7 — docsindex ADOPTS the shared rule rather than correcting its own copy.
// Both halves are load-bearing, and the absence half greps IDENTIFIERS:
// a positive assertion alone passes a build that kept a hand-rolled matcher,
// and an absence assertion aimed at the `Status:` literal would fail against a
// correct implementation that mentions it in a comment (ANTS-3785 INV-6's rule,
// applied to a third file for its reason).
TEST(DocsIndexHeaderField, DocsIndexAdoptsTheSharedRule) {
    const std::string src = ants_test::slurpFile(SRC_DOCSINDEX_CPP_PATH);
    ASSERT_FALSE(src.empty()) << SRC_DOCSINDEX_CPP_PATH;

    EXPECT_NE(src.find("headerField("), std::string::npos)
        << "docsindex.cpp must call the shared helper";
    EXPECT_NE(src.find("isHeaderBlockEnd("), std::string::npos)
        << "docsindex.cpp must share the block-end bound too";

    EXPECT_EQ(src.find("statusRx"), std::string::npos)
        << "docsindex.cpp must carry no field regex of its own";
    EXPECT_EQ(src.find("blockEndRx"), std::string::npos)
        << "an implementation that adopts headerField but re-writes the "
           "block-end bound is still a second copy of it";

    // headingRx is EXPLICITLY exempt: the heading harvest is docsindex's own
    // job, not the header-field rule. Asserting it survives keeps a later
    // over-eager sweep from deleting it in this invariant's name.
    EXPECT_NE(src.find("headingRx"), std::string::npos)
        << "the heading harvest is exempt from this invariant";
}

// INV-9 — the corpus figures are reproducible from the shipped tree. `other`
// is asserted at zero because § 1 rests its evidence on that bucket being
// empty; a tool that never reports it cannot reproduce the claim. No fixture
// can BE `other` by construction — asserting it is the point, not a gap.
TEST(DocsIndexHeaderField, SurveyToolReproducesTheClasses) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = stage(dir, "corpus");

    QProcess p;
    p.start(QStringLiteral("python3"),
            {QStringLiteral(ANTS_SOURCE_DIR "/tools/spec-header-survey.py"),
             QStringLiteral("--scope=docs-index"), root});
    if (!p.waitForStarted(5000)) GTEST_SKIP() << "python3 unavailable";
    ASSERT_TRUE(p.waitForFinished(30000));
    EXPECT_EQ(p.exitCode(), 0);

    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    for (const QString &want : {
             QStringLiteral("docs=4"),
             QStringLiteral("truncated_now_whole=1 (of which wrapped: 1)"),
             QStringLiteral("body_prose_now_empty=1 (of which wrapped: 1)"),
             QStringLiteral("unchanged_value=1 (of which wrapped: 0)"),
             QStringLiteral("both_empty=1 (of which wrapped: 0)"),
             QStringLiteral("other=0 (of which wrapped: 0)"),
         }) {
        EXPECT_TRUE(out.contains(want))
            << want.toStdString() << " missing from:\n" << out.toStdString();
    }
}
