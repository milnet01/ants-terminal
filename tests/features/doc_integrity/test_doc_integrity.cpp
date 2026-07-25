// ANTS-3601 — DocIntegrity engine conformance test. Headless: builds fixture
// doc trees in a temp dir and drives DocIntegrity::check directly (Qt6::Core
// only, no widgets). See spec.md for the invariant map.

#include "docintegrity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using DocIntegrity::Finding;
using DocIntegrity::Kind;

namespace {

bool writeFile(const QString &path, const QString &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(content.toUtf8());
    return true;
}

int countKind(const QList<Finding> &fs, Kind k) {
    int n = 0;
    for (const Finding &f : fs)
        if (f.kind == k) ++n;
    return n;
}

// Does any finding of kind `k` mention `needle` in its message?
bool hasMention(const QList<Finding> &fs, Kind k, const QString &needle) {
    for (const Finding &f : fs)
        if (f.kind == k && f.message.contains(needle)) return true;
    return false;
}

// Canonical root path for a temp dir (resolves /tmp symlinks etc.).
QString canon(const QTemporaryDir &d) {
    return QFileInfo(d.path()).canonicalFilePath();
}

}  // namespace

// ---- gfmSlug case table (INV-13) -------------------------------------------
TEST(DocIntegritySlug, GithubSluggerCases) {
    using DocIntegrity::gfmSlug;
    // Live mcp-feedback-files.md anchors:
    EXPECT_EQ(gfmSlug("Contributor don'ts"), QStringLiteral("contributor-donts"));
    EXPECT_EQ(gfmSlug("Maintainer compaction (v2 \xE2\x80\x94 `compact_resolved`) "
                      "\xE2\x80\x94 ANTS-3443"),
              QStringLiteral("maintainer-compaction-v2--compact_resolved--ants-3443"));
    // Emoji → leading dash (constructed, pinned to github-slugger):
    EXPECT_EQ(gfmSlug(QString::fromUtf8("\xF0\x9F\x93\x8B Roadmap")),
              QStringLiteral("-roadmap"));
    // Trailing ATX hashes are stripped by extractHeadings, not gfmSlug; here we
    // confirm underscores are kept and case is lowered.
    EXPECT_EQ(gfmSlug("compact_resolved"), QStringLiteral("compact_resolved"));
    // Duplicate suffix (2nd identical base → -1).
    QHash<QString, int> seen;
    EXPECT_EQ(gfmSlug("Setup", seen), QStringLiteral("setup"));
    EXPECT_EQ(gfmSlug("Setup", seen), QStringLiteral("setup-1"));
}

// INV-1 — in-doc dead anchor. INV-3 (fence) — a fenced heading/anchor is inert.
TEST(DocIntegrity, DeadAnchorAndFence) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# Real Heading\n"
                          "See [x](#missing) and [y](#real-heading).\n"
                          "\n"
                          "```\n"
                          "# Fake\n"
                          "[z](#real)\n"       // fenced link — inert
                          "```\n"
                          "Also [w](#fake) below.\n"));  // #fake heading is fenced → dead
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::DeadAnchor), 2);       // #missing, #fake
    EXPECT_TRUE(hasMention(fs, Kind::DeadAnchor, "missing"));
    EXPECT_TRUE(hasMention(fs, Kind::DeadAnchor, "fake"));
    EXPECT_FALSE(hasMention(fs, Kind::DeadAnchor, "real"));  // #real-heading valid; #real fenced
}

// INV-2 — duplicate-disambiguated slug resolves; the next suffix does not.
TEST(DocIntegrity, DuplicateHeadingSlugs) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "## Setup\n## Setup\n"
                          "[a](#setup-1)\n"));  // 2nd occurrence → valid
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/a.md"}), Kind::DeadAnchor), 0);

    ASSERT_TRUE(writeFile(root + "/docs/b.md",
                          "## Setup\n## Setup\n"
                          "[a](#setup-2)\n"));  // no third Setup → dead
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/b.md"}), Kind::DeadAnchor), 1);
}

// INV-4 — broken vs existing relative link (any extension). INV-5 — external
// and pure-anchor links are never broken.
TEST(DocIntegrity, BrokenLinks) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/src/real.cpp", "int x;\n"));
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# H\n"
                          "[a](./gone.md)\n"
                          "[b](../src/real.cpp)\n"
                          "[c](https://example.com/x)\n"
                          "[d](#h)\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::BrokenLink), 1);
    EXPECT_TRUE(hasMention(fs, Kind::BrokenLink, "gone.md"));
}

// INV-5b (ANTS-3623) — a link inside an INLINE code span is prose about a
// link, not a link. Fenced blocks were already skipped; inline spans were
// not, and that was the dominant false-positive source: 21 of 22
// broken_link findings over this project's own doc tree were back-ticked
// examples, 12 of them in doc_integrity's own spec, which must contain
// `[text](target)` samples to describe what the checker does.
TEST(DocIntegrity, InlineCodeSpansNotHarvested) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# H\n"
                          "Prose about `[text](gone-a.md)` as an example.\n"
                          "Double ticks ``[t](gone-b.md)`` too.\n"
                          "A placeholder `[text](url)` and `[x](int idx)`.\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::BrokenLink), 0)
        << "back-ticked example links must not be harvested";
}

// INV-5c (ANTS-3623) — masking must not swallow a REAL link whose *text* is
// code. `[`a/b.md`](a/b.md)` is the house style for citing a file, and the
// brackets and target sit outside the span, so it must still be checked.
// This is the case that makes "delete the span" wrong and "blank it" right.
TEST(DocIntegrity, RealLinkWithCodeTextStillChecked) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/here.md", "# ok\n"));
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# H\n"
                          "See [`here.md`](here.md) and [`gone.md`](gone.md).\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::BrokenLink), 1)
        << "a real link with code-formatted text must still be checked";
    EXPECT_TRUE(hasMention(fs, Kind::BrokenLink, "gone.md"));
}

// INV-6 — a root-escaping link (relative escape AND absolute target) is
// skipped, never probed, never treated as an in-root file.
TEST(DocIntegrity, RootEscapeSkipped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# H\n"
                          "[x](../../outside.md)\n"
                          "[y](/etc/passwd)\n"
                          "[z](/docs/foo.md)\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::BrokenLink), 0);
    EXPECT_EQ(countKind(fs, Kind::DeadAnchor), 0);
}

// INV-7 — missing H2 section flags TocGap; a covered TOC yields none.
TEST(DocIntegrity, TocCoverage) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    // TOC missing the "Beta" section.
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "## Contents\n"
                          "\n"
                          "- [Alpha](#alpha)\n"
                          "\n"
                          "## Alpha\ntext\n"
                          "## Beta\ntext\n"));
    auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::TocGap), 1);
    EXPECT_TRUE(hasMention(fs, Kind::TocGap, "Beta"));

    // Add the Beta entry → covered.
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "## Contents\n"
                          "\n"
                          "- [Alpha](#alpha)\n"
                          "- [Beta](#beta)\n"
                          "\n"
                          "## Alpha\ntext\n"
                          "## Beta\ntext\n"));
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/a.md"}), Kind::TocGap), 0);
}

// INV-8 — a duplicate TOC entry (same slug twice) is a TocGap.
TEST(DocIntegrity, DuplicateTocEntry) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "## Contents\n"
                          "\n"
                          "- [Alpha](#alpha)\n"
                          "- [Alpha again](#alpha)\n"
                          "\n"
                          "## Alpha\ntext\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_TRUE(hasMention(fs, Kind::TocGap, "duplicate"));
}

// INV-9 — a doc with no TOC region never produces a TocGap.
TEST(DocIntegrity, NoTocNoGap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md", "## Alpha\n## Beta\ntext\n"));
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/a.md"}), Kind::TocGap), 0);
}

// INV-12 — pure: two runs identical, no files written.
TEST(DocIntegrity, PureNoState) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md", "# H\n[x](#missing)\n"));
    const int before = QDir(root + "/docs").entryList(QDir::Files).size();
    const auto a = DocIntegrity::check(root, {"docs/a.md"});
    const auto b = DocIntegrity::check(root, {"docs/a.md"});
    const int after = QDir(root + "/docs").entryList(QDir::Files).size();
    EXPECT_EQ(a.size(), b.size());
    EXPECT_EQ(before, after);  // no cache/output file created
}

// INV-14 — no more than maxDocsPerRun docs are read.
TEST(DocIntegrity, DocsCap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    QStringList rel;
    for (int i = 0; i < 7; ++i) {
        const QString p = QStringLiteral("docs/d%1.md").arg(i);
        ASSERT_TRUE(writeFile(root + "/" + p, "# H\n"));
        rel << p;
    }
    DocIntegrity::Options opts;
    opts.maxDocsPerRun = 3;
    QStringList checked;
    DocIntegrity::check(root, rel, opts, &checked);
    EXPECT_EQ(checked.size(), 3);
}

// INV-15 — a relDocs entry naming no openable file is silently excluded.
TEST(DocIntegrity, MissingDocSkipped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/real.md", "# H\n"));
    QStringList checked;
    DocIntegrity::check(root, {"docs/real.md", "docs/typo.md"}, {}, &checked);
    ASSERT_EQ(checked.size(), 1);
    EXPECT_EQ(checked.first(), QStringLiteral("docs/real.md"));
}

// INV-17 — cross-doc anchor resolution opens only in-scope docs; an
// out-of-scope existing target gets no anchor check, a missing one is broken.
TEST(DocIntegrity, CrossDocReadBound) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    // out.md exists but is NOT in relDocs; its #foo anchor is bogus but must
    // NOT be flagged (its body is never opened for anchor checking).
    ASSERT_TRUE(writeFile(root + "/docs/out.md", "# Something Else\n"));
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# H\n"
                          "[a](out.md#foo)\n"     // out.md exists, out of scope
                          "[b](gone.md#x)\n"));    // missing → broken
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});  // out.md NOT in scope
    EXPECT_EQ(countKind(fs, Kind::BrokenLink), 1);
    EXPECT_TRUE(hasMention(fs, Kind::BrokenLink, "gone.md"));
    EXPECT_EQ(countKind(fs, Kind::DeadAnchor), 0);  // out.md#foo not anchor-checked

    // With out.md IN scope, its bogus anchor now flags.
    const auto fs2 = DocIntegrity::check(root, {"docs/a.md", "docs/out.md"});
    EXPECT_EQ(countKind(fs2, Kind::DeadAnchor), 1);
}

// Per-doc caps — headings/links capped, bytes past maxDocBytes unread.
TEST(DocIntegrity, PerDocCaps) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    // A doc whose dead anchors sit past a tiny byte budget are never seen.
    QString body = "# H\n";
    body += QString(4000, 'x') + "\n";       // padding past maxDocBytes
    body += "[a](#missing)\n";               // beyond the read budget
    ASSERT_TRUE(writeFile(root + "/docs/a.md", body));
    DocIntegrity::Options opts;
    opts.maxDocBytes = 64;                    // read only the first 64 bytes
    const auto fs = DocIntegrity::check(root, {"docs/a.md"}, opts);
    EXPECT_EQ(countKind(fs, Kind::DeadAnchor), 0);  // the link was never read
}
