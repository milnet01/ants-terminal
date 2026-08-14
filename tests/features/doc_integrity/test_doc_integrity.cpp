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

// INV-5d (ANTS-3635a) — a code span may cross a newline (CommonMark § 6.1),
// so the tail of a multi-line span is still prose about a link. This is the
// live docs/specs/ANTS-1150.md:197-198 shape: one span wrapping a C++ lambda
// whose second line reads `[this](int idx)`.
TEST(DocIntegrity, MultiLineInlineCodeSpanNotHarvested) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# H\n"
                          "The handler is `[this](gone-a.md)\n"
                          "and [more](gone-b.md)` — one span, two lines.\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::BrokenLink), 0)
        << "the tail of a multi-line code span must not be harvested";
}

// INV-5e (ANTS-3635a) — the converse guard on INV-5d. An UNMATCHED backtick
// run is literal text per CommonMark, so it must mask nothing; and the
// forward search for a closing run stops at a blank line, since an inline
// span cannot cross a paragraph break. Without both, one stray backtick
// would silently stop the checker on every line below it.
TEST(DocIntegrity, UnmatchedBacktickDoesNotSwallowLinks) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# H\n"
                          "A stray ` backtick with no partner.\n"
                          "Then [x](gone-a.md) below it.\n"
                          "\n"
                          "New paragraph, opens ` here and never closes.\n"
                          "So [y](gone-b.md) is still checked.\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::BrokenLink), 2)
        << "an unmatched backtick must leave following links harvestable";
    EXPECT_TRUE(hasMention(fs, Kind::BrokenLink, "gone-a.md"));
    EXPECT_TRUE(hasMention(fs, Kind::BrokenLink, "gone-b.md"));
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

// ANTS-3836 — a WRAPPED TOC entry does not end the Contents run. Before the
// fix, detectToc() broke at the first line that was not itself a list item, so
// an entry continued onto a second line truncated the TOC there and every
// section below was reported missing from a Contents list that named it.
//
// The continuation here begins "2.4 delta ·" — the real shape from
// docs/specs/ANTS-1870.md, and one no ordered-list pattern matches, since
// `2.4` is not `2.` followed by a space.
TEST(DocIntegrity, WrappedTocEntryDoesNotEndTheRun) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "## Contents\n"
                          "\n"
                          "1. [Alpha](#1-alpha)\n"
                          "2. [Beta](#2-beta) — 2.1 one · 2.2 two · 2.3 three ·\n"
                          "   2.4 delta · 2.5 echo\n"
                          "3. [Gamma](#3-gamma)\n"
                          "4. [Delta](#4-delta)\n"
                          "\n"
                          "## 1. Alpha\ntext\n"
                          "## 2. Beta\ntext\n"
                          "## 3. Gamma\ntext\n"
                          "## 4. Delta\ntext\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::TocGap), 0)
        << "entries 3 and 4 sit BELOW a wrapped entry and are listed in the "
           "Contents; neither is a gap";

    // The check still works through a wrap: drop Delta's entry, keep the wrap.
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "## Contents\n"
                          "\n"
                          "1. [Alpha](#1-alpha)\n"
                          "2. [Beta](#2-beta) — 2.1 one · 2.2 two · 2.3 three ·\n"
                          "   2.4 delta · 2.5 echo\n"
                          "3. [Gamma](#3-gamma)\n"
                          "\n"
                          "## 1. Alpha\ntext\n"
                          "## 2. Beta\ntext\n"
                          "## 3. Gamma\ntext\n"
                          "## 4. Delta\ntext\n"));
    const auto fs2 = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs2, Kind::TocGap), 1)
        << "a genuinely absent entry must still be reported past a wrap";
    EXPECT_TRUE(hasMention(fs2, Kind::TocGap, "Delta"));
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

// INV-9b (ANTS-3634) — a TOC region whose items carry no `#anchor` link at all
// yields no slugs to match against, so check 3 must stand down rather than
// report every section missing. Real shape: docs/specs/ANTS-2023.md, whose TOC
// lists plain-text `- §1 Problem` items — it produced 8 false TocGaps.
TEST(DocIntegrity, LinklessTocNotReportedAsGaps) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "## Contents\n"
                          "\n"
                          "- §1 Problem\n"
                          "- §2 Surface\n"
                          "\n"
                          "## 1. Problem\ntext\n"
                          "## 2. Surface\ntext\n"));
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/a.md"}), Kind::TocGap), 0);

    // A TOC that DOES use anchors keeps full coverage — the stand-down is
    // scoped to the zero-entry case, not "any linkless item" (§ 2.5 already
    // skips an individual linkless bullet without ending the run).
    ASSERT_TRUE(writeFile(root + "/docs/b.md",
                          "## Contents\n"
                          "\n"
                          "- Sections\n"            // linkless parent bullet
                          "- [Alpha](#alpha)\n"
                          "\n"
                          "## Alpha\ntext\n"
                          "## Beta\ntext\n"));
    const auto fs = DocIntegrity::check(root, {"docs/b.md"});
    EXPECT_EQ(countKind(fs, Kind::TocGap), 1);
    EXPECT_TRUE(hasMention(fs, Kind::TocGap, "Beta"));
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

// ---- ANTS-3700 — heading_sequence ------------------------------------------

// INV-16 — the reported shape: a sibling lower than its predecessor is an
// out-of-order finding, and the number that "went missing" ahead of it is NOT
// separately reported as a gap, because it turns up later under the same
// parent. This is the spec-format.md case that survived three cold-eyes loops.
TEST(DocIntegrity, HeadingSequenceSwappedSiblings) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# Doc\n\n"
                          "### 5.6 Sixth\n\n"
                          "### 5.8 Eighth\n\n"
                          "### 5.7 Seventh\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::HeadingSequence), 1)
        << "a swapped pair is ONE defect; reporting the hole at 5.8 as well "
           "would double-count it";
    EXPECT_TRUE(hasMention(fs, Kind::HeadingSequence,
                           QStringLiteral("5.7 is out of order")));
}

// INV-17 — a genuine hole (no sibling ever fills it) IS a gap, named.
TEST(DocIntegrity, HeadingSequenceRealGap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# Doc\n\n"
                          "## 1. One\n\n"
                          "## 2. Two\n\n"
                          "## 5. Five\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::HeadingSequence), 1);
    EXPECT_TRUE(hasMention(fs, Kind::HeadingSequence, QStringLiteral("skips 3, 4")))
        << "the gap must name the missing numbers, not just its own";
}

// INV-18 — a repeated number is a duplicate, distinct from a gap.
TEST(DocIntegrity, HeadingSequenceDuplicate) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# Doc\n\n"
                          "## 1. One\n\n"
                          "## 2. Two\n\n"
                          "## 2. Two again\n"));
    const auto fs = DocIntegrity::check(root, {"docs/a.md"});
    EXPECT_EQ(countKind(fs, Kind::HeadingSequence), 1);
    EXPECT_TRUE(hasMention(fs, Kind::HeadingSequence,
                           QStringLiteral("duplicate section number 2")));
}

// INV-19 — the quiet cases. Nested numbering is grouped by numeric parent, so
// interleaved depths are clean; prose headings, an H1 title, a group that
// simply starts above 1, and fenced examples are all untouched.
TEST(DocIntegrity, HeadingSequenceQuietCases) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# 9. Title numbered oddly\n\n"
                          "## 1. One\n\n"
                          "### 1.1 First child\n\n"
                          "### 1.2 Second child\n\n"
                          "## 2. Two\n\n"
                          "### 2.1 Child\n\n"
                          "## Prose heading\n\n"
                          "## 3. Three\n"));
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/a.md"}),
                        Kind::HeadingSequence), 0);

    // A doc whose sections start at 2 is an excerpt, not a defect.
    ASSERT_TRUE(writeFile(root + "/docs/b.md",
                          "# Doc\n\n## 2. Two\n\n## 3. Three\n"));
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/b.md"}),
                        Kind::HeadingSequence), 0);

    // Fenced headings are inert (fence-awareness is shared with checks 1-3).
    ASSERT_TRUE(writeFile(root + "/docs/c.md",
                          "# Doc\n\n## 1. One\n\n```\n## 9. Fenced\n```\n\n"
                          "## 2. Two\n"));
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/c.md"}),
                        Kind::HeadingSequence), 0);
}

// ---- ANTS-3719 — ungranted_tool --------------------------------------------
//
// Requested by the claude_config session. A Claude Code skill declares its
// tools in `allowed-tools:` frontmatter and names the MCP verbs it needs in its
// body; when the two drift the skill cannot be executed as written. Measured
// over the live ~/.claude corpus when this shipped: 18 skills carry
// allowed-tools, zero drift — so these fixtures are the whole test surface and
// the check ships as a regression guard, which is what the reporter asked for
// (the defect recurred once already, reintroduced by a commit that added a
// dependency without touching the frontmatter).

// INV-23 — a verb the body calls and the frontmatter never granted is reported
// once, naming the verb.
TEST(DocIntegrity, UngrantedToolReported) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/s.md",
                          "---\n"
                          "name: demo\n"
                          "allowed-tools: [Read, mcp__ants__doc_integrity]\n"
                          "---\n\n"
                          "# Demo\n\n"
                          "Run `mcp__ants__doc_integrity` first, then\n"
                          "`mcp__ants__doc_citations` over the same set.\n"));
    const auto fs = DocIntegrity::check(root, {"docs/s.md"});
    EXPECT_EQ(countKind(fs, Kind::UngrantedTool), 1)
        << "the granted verb must not be reported alongside the ungranted one";
    EXPECT_TRUE(hasMention(fs, Kind::UngrantedTool,
                           QStringLiteral("mcp__ants__doc_citations")));
}

// INV-24 — one finding per verb, at its first mention, however often the body
// repeats it. A skill naming its main verb a dozen times must not produce a
// dozen findings, or the check becomes the noise class it exists to avoid.
TEST(DocIntegrity, UngrantedToolDeduplicatesPerVerb) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/s.md",
                          "---\n"
                          "allowed-tools: [Read]\n"
                          "---\n\n"
                          "Call `mcp__ants__spec_query` here.\n"
                          "Then `mcp__ants__spec_query` again.\n"
                          "And once more: `mcp__ants__spec_query`.\n"));
    const auto fs = DocIntegrity::check(root, {"docs/s.md"});
    ASSERT_EQ(countKind(fs, Kind::UngrantedTool), 1);
    for (const Finding &f : fs)
        if (f.kind == Kind::UngrantedTool)
            EXPECT_EQ(f.line, 5) << "reported at the FIRST mention";
}

// INV-25 — the quiet cases, each of which would make this check unusable if it
// fired: a doc with no frontmatter at all (almost every doc in a docs tree), a
// skill that declares no allowed-tools key (nothing to contradict), a verb
// mentioned only inside a fence, and a verb named in a DIFFERENT frontmatter
// key — `description:` must not silently grant what allowed-tools omits.
TEST(DocIntegrity, UngrantedToolQuietCases) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);

    // (a) no frontmatter — an ordinary doc that happens to name a verb.
    ASSERT_TRUE(writeFile(root + "/docs/a.md",
                          "# Notes\n\nUse `mcp__ants__git_state` for status.\n"));
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/a.md"}),
                        Kind::UngrantedTool), 0);

    // (b) frontmatter without an allowed-tools key — declares nothing.
    ASSERT_TRUE(writeFile(root + "/docs/b.md",
                          "---\nname: x\n---\n\n`mcp__ants__git_state`\n"));
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/b.md"}),
                        Kind::UngrantedTool), 0);

    // (c) the only mention is inside a fence — an example, not a mandate.
    ASSERT_TRUE(writeFile(root + "/docs/c.md",
                          "---\nallowed-tools: [Read]\n---\n\n"
                          "```\nmcp__ants__git_state\n```\n"));
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/c.md"}),
                        Kind::UngrantedTool), 0);

    // (d) granted in a block list, not an inline array.
    ASSERT_TRUE(writeFile(root + "/docs/d.md",
                          "---\nallowed-tools:\n  - Read\n"
                          "  - mcp__ants__git_state\n---\n\n"
                          "`mcp__ants__git_state`\n"));
    EXPECT_EQ(countKind(DocIntegrity::check(root, {"docs/d.md"}),
                        Kind::UngrantedTool), 0);
}

// INV-26 — a verb named in `description:` is NOT granted. The granted set is
// read from allowed-tools' own value, so a skill whose description advertises a
// verb it never granted is still caught. This is the case a whole-frontmatter
// scan would silently pass.
TEST(DocIntegrity, UngrantedToolDescriptionDoesNotGrant) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp);
    ASSERT_TRUE(writeFile(root + "/docs/s.md",
                          "---\n"
                          "allowed-tools: [Read]\n"
                          "description: wraps mcp__ants__git_state for you\n"
                          "---\n\n"
                          "Call `mcp__ants__git_state`.\n"));
    const auto fs = DocIntegrity::check(root, {"docs/s.md"});
    EXPECT_EQ(countKind(fs, Kind::UngrantedTool), 1)
        << "description: must not act as a grant";
}
