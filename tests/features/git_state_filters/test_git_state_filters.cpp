// Feature-conformance test for spec.md (ANTS-5508).
//
// Pure: drives the GitWrap helpers behind git_state's added-lines diff,
// log stat and rename sweep against literal git output. No git, no
// QProcess, no repo.

#include "gitwrap.h"

#include <gtest/gtest.h>

using GitWrap::AddedLine;
using GitWrap::DiffFile;
using GitWrap::GrepHit;
using GitWrap::LogRecord;

namespace {

const QByteArray kDiff =
    "diff --git a/src/foo.cpp b/src/foo.cpp\n"
    "--- a/src/foo.cpp\n"
    "+++ b/src/foo.cpp\n"
    "@@ -10,3 +10,4 @@ void Foo::bar() {\n"
    " keep\n"
    "-drop\n"
    "+newName(1);\n"
    "+other();\n"
    " keep2\n"
    "@@ -30 +31,2 @@\n"
    "-x\n"
    "+NewName(2);\n"
    "+++ y\n"
    "\\ No newline at end of file\n";

// `git grep -z` output with each NUL written as `|`.
QByteArray z(const char *s) {
    QByteArray b(s);
    b.replace('|', '\0');
    return b;
}

DiffFile parsedFile() {
    const QVector<DiffFile> v = GitWrap::parseDiffHunks(kDiff, true);
    return v.isEmpty() ? DiffFile() : v.first();
}

}  // namespace

// INV-1 — every `+` line, numbered in the new file, marker stripped.
TEST(GitStateFilters, AddedLinesCarryNewFileLineNumbers) {
    const QVector<AddedLine> a = GitWrap::addedLines(parsedFile(), {});
    ASSERT_EQ(a.size(), 4);
    EXPECT_EQ(a.at(0).line, 11);   // after one context line at 10
    EXPECT_EQ(a.at(0).text, QStringLiteral("newName(1);"));
    EXPECT_EQ(a.at(1).line, 12);
    EXPECT_EQ(a.at(1).text, QStringLiteral("other();"));
    EXPECT_EQ(a.at(2).line, 31);   // `-x` did not advance the count
    EXPECT_EQ(a.at(2).text, QStringLiteral("NewName(2);"));
    EXPECT_EQ(a.at(3).line, 32);
    EXPECT_EQ(a.at(3).text, QStringLiteral("++ y"));
}

// INV-2 — the match is a case-sensitive substring.
TEST(GitStateFilters, AddedMatchIsCaseSensitive) {
    const QVector<AddedLine> a =
        GitWrap::addedLines(parsedFile(), QStringLiteral("newName"));
    ASSERT_EQ(a.size(), 1);
    EXPECT_EQ(a.at(0).line, 11);
}

// INV-3 — records, fields, numstat rows, binary as -1, empty commit.
TEST(GitStateFilters, LogNumstatParsesRecords) {
    const QByteArray out =
        "\x1e" "abc1234\x1f" "First subject\x1f" "2026-10-22\x1d\n"
        "3\t1\tsrc/a.cpp\n"
        "-\t-\tdocs/pic.png\n"
        "\n"
        "\x1e" "def5678\x1f" "Empty commit\x1f" "2026-10-21\x1d\n";
    const QVector<LogRecord> r = GitWrap::parseLogNumstat(out);
    ASSERT_EQ(r.size(), 2);
    ASSERT_EQ(r.at(0).fields.size(), 3);
    EXPECT_EQ(r.at(0).fields.at(0), QStringLiteral("abc1234"));
    EXPECT_EQ(r.at(0).fields.at(1), QStringLiteral("First subject"));
    ASSERT_EQ(r.at(0).files.size(), 2);
    EXPECT_EQ(r.at(0).files.at(0).path, QStringLiteral("src/a.cpp"));
    EXPECT_EQ(r.at(0).files.at(0).added, 3);
    EXPECT_EQ(r.at(0).files.at(0).removed, 1);
    EXPECT_EQ(r.at(0).files.at(1).path, QStringLiteral("docs/pic.png"));
    EXPECT_EQ(r.at(0).files.at(1).added, -1);
    EXPECT_EQ(r.at(0).files.at(1).removed, -1);
    EXPECT_EQ(r.at(1).fields.at(0), QStringLiteral("def5678"));
    EXPECT_TRUE(r.at(1).files.isEmpty());
}

// INV-3 — a body field may hold newlines; they stay inside the field.
TEST(GitStateFilters, LogNumstatKeepsMultiLineBody) {
    const QByteArray out =
        "\x1e" "abc1234\x1f" "S\x1f" "2026-10-22\x1f" "line one\nline two\n"
        "\x1d\n"
        "1\t0\tREADME.md\n";
    const QVector<LogRecord> r = GitWrap::parseLogNumstat(out);
    ASSERT_EQ(r.size(), 1);
    ASSERT_EQ(r.at(0).fields.size(), 4);
    EXPECT_EQ(r.at(0).fields.at(3), QStringLiteral("line one\nline two\n"));
    ASSERT_EQ(r.at(0).files.size(), 1);
    EXPECT_EQ(r.at(0).files.at(0).path, QStringLiteral("README.md"));
}

// INV-4 — worktree form and revision form, text keeping its own colons.
TEST(GitStateFilters, GrepZParsesHits) {
    const QByteArray worktree =
        z("src/a.cpp|12|oldName(); // a:b\ndocs/x.md|3|use oldName\n");
    const QVector<GrepHit> w = GitWrap::parseGrepZ(worktree, {});
    ASSERT_EQ(w.size(), 2);
    EXPECT_EQ(w.at(0).path, QStringLiteral("src/a.cpp"));
    EXPECT_EQ(w.at(0).line, 12);
    EXPECT_EQ(w.at(0).text, QStringLiteral("oldName(); // a:b"));
    EXPECT_EQ(w.at(1).path, QStringLiteral("docs/x.md"));
    EXPECT_EQ(w.at(1).line, 3);

    const QByteArray atRev = z("HEAD:src/a.cpp|7|oldName\n");
    const QVector<GrepHit> h = GitWrap::parseGrepZ(atRev, QStringLiteral("HEAD"));
    ASSERT_EQ(h.size(), 1);
    EXPECT_EQ(h.at(0).path, QStringLiteral("src/a.cpp"));
    EXPECT_EQ(h.at(0).line, 7);
}

// INV-5 — the revision a range ends at.
TEST(GitStateFilters, RangeTip) {
    EXPECT_EQ(GitWrap::rangeTip(QStringLiteral("A..B")), QStringLiteral("B"));
    EXPECT_EQ(GitWrap::rangeTip(QStringLiteral("A...B")), QStringLiteral("B"));
    EXPECT_EQ(GitWrap::rangeTip(QStringLiteral("HEAD~3..")), QStringLiteral("HEAD"));
    EXPECT_EQ(GitWrap::rangeTip(QStringLiteral("v0.7.1")), QStringLiteral("v0.7.1"));
}
