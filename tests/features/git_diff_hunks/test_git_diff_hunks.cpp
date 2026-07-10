// Feature-conformance test for spec.md (ANTS-3377).
//
// Behavioral + pure: drives GitWrap::parseDiffHunks against literal
// unified-diff fixtures. No git, no QProcess, no repo — the parser is a
// pure byte→struct transform, so every case runs unconditionally.

#include "gitwrap.h"

#include <gtest/gtest.h>

using GitWrap::DiffFile;
using GitWrap::DiffHunk;

namespace {

const DiffFile *fileFor(const QVector<DiffFile> &v, const QString &path) {
    for (const DiffFile &f : v)
        if (f.path == path) return &f;
    return nullptr;
}

}  // namespace

// INV-1 — a modified file yields its `@@` hunks with parsed start/count and
// the full header line; path comes from the `+++ b/` line.
TEST(GitDiffHunks, ModifiedFileTwoHunks) {
    const QByteArray diff =
        "diff --git a/src/foo.cpp b/src/foo.cpp\n"
        "index 1111111..2222222 100644\n"
        "--- a/src/foo.cpp\n"
        "+++ b/src/foo.cpp\n"
        "@@ -10,3 +10,4 @@ void Foo::bar() {\n"
        " keep\n"
        "-drop\n"
        "+add1\n"
        "+add2\n"
        "@@ -30 +31,2 @@\n"
        "-x\n"
        "+y\n"
        "+z\n";
    const QVector<DiffFile> r = GitWrap::parseDiffHunks(diff, false);
    ASSERT_EQ(r.size(), 1);
    const DiffFile *f = fileFor(r, QStringLiteral("src/foo.cpp"));
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->hunks.size(), 2);

    const DiffHunk &h0 = f->hunks.at(0);
    EXPECT_EQ(h0.oldStart, 10);
    EXPECT_EQ(h0.oldCount, 3);
    EXPECT_EQ(h0.newStart, 10);
    EXPECT_EQ(h0.newCount, 4);
    EXPECT_EQ(h0.header,
              QStringLiteral("@@ -10,3 +10,4 @@ void Foo::bar() {"));
    // includeLines=false → no body captured.
    EXPECT_TRUE(h0.lines.isEmpty());

    // INV-2 — an omitted count defaults to 1 (`@@ -30 +31,2 @@`).
    const DiffHunk &h1 = f->hunks.at(1);
    EXPECT_EQ(h1.oldStart, 30);
    EXPECT_EQ(h1.oldCount, 1);   // omitted → 1
    EXPECT_EQ(h1.newStart, 31);
    EXPECT_EQ(h1.newCount, 2);
}

// INV-3 — an added file (`--- /dev/null`) takes its path from `+++ b/` and
// the pre-image hunk range is 0,0.
TEST(GitDiffHunks, AddedFileDevNull) {
    const QByteArray diff =
        "diff --git a/new.txt b/new.txt\n"
        "new file mode 100644\n"
        "index 0000000..e69de29\n"
        "--- /dev/null\n"
        "+++ b/new.txt\n"
        "@@ -0,0 +1,2 @@\n"
        "+hello\n"
        "+world\n";
    const QVector<DiffFile> r = GitWrap::parseDiffHunks(diff, false);
    ASSERT_EQ(r.size(), 1);
    const DiffFile *f = fileFor(r, QStringLiteral("new.txt"));
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->hunks.size(), 1);
    EXPECT_EQ(f->hunks.at(0).oldStart, 0);
    EXPECT_EQ(f->hunks.at(0).oldCount, 0);
    EXPECT_EQ(f->hunks.at(0).newStart, 1);
    EXPECT_EQ(f->hunks.at(0).newCount, 2);
}

// INV-4 — a deleted file (`+++ /dev/null`) takes its path from `--- a/`.
TEST(GitDiffHunks, DeletedFileDevNull) {
    const QByteArray diff =
        "diff --git a/gone.txt b/gone.txt\n"
        "deleted file mode 100644\n"
        "index e69de29..0000000\n"
        "--- a/gone.txt\n"
        "+++ /dev/null\n"
        "@@ -1,2 +0,0 @@\n"
        "-bye\n"
        "-now\n";
    const QVector<DiffFile> r = GitWrap::parseDiffHunks(diff, false);
    ASSERT_EQ(r.size(), 1);
    EXPECT_NE(fileFor(r, QStringLiteral("gone.txt")), nullptr);
}

// INV-5 — includeLines captures the raw body (marker included), and a body
// line that itself begins `+++ ` is NOT mistaken for a file header once a
// hunk has opened (the unified-diff ambiguity).
TEST(GitDiffHunks, IncludeLinesAmbiguity) {
    const QByteArray diff =
        "diff --git a/a.txt b/a.txt\n"
        "--- a/a.txt\n"
        "+++ b/a.txt\n"
        "@@ -1,2 +1,3 @@\n"
        " ctx\n"
        "-old\n"
        "+++ new-body-line\n"   // added line whose text starts with "++ "
        "+tail\n";
    const QVector<DiffFile> r = GitWrap::parseDiffHunks(diff, true);
    ASSERT_EQ(r.size(), 1);
    // Path must still be a.txt — the "+++ new-body-line" body line did not
    // hijack the path.
    const DiffFile *f = fileFor(r, QStringLiteral("a.txt"));
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->hunks.size(), 1);
    const QStringList &body = f->hunks.at(0).lines;
    ASSERT_EQ(body.size(), 4);
    EXPECT_EQ(body.at(0), QStringLiteral(" ctx"));
    EXPECT_EQ(body.at(1), QStringLiteral("-old"));
    EXPECT_EQ(body.at(2), QStringLiteral("+++ new-body-line"));
    EXPECT_EQ(body.at(3), QStringLiteral("+tail"));
}

// INV-6 — a file with no content hunk (pure rename / mode change) is omitted;
// only the hunk-bearing file survives.
TEST(GitDiffHunks, PureRenameOmitted) {
    const QByteArray diff =
        "diff --git a/old-name.txt b/new-name.txt\n"
        "similarity index 100%\n"
        "rename from old-name.txt\n"
        "rename to new-name.txt\n"
        "diff --git a/edited.cpp b/edited.cpp\n"
        "--- a/edited.cpp\n"
        "+++ b/edited.cpp\n"
        "@@ -5,1 +5,1 @@\n"
        "-a\n"
        "+b\n";
    const QVector<DiffFile> r = GitWrap::parseDiffHunks(diff, false);
    ASSERT_EQ(r.size(), 1);
    EXPECT_NE(fileFor(r, QStringLiteral("edited.cpp")), nullptr);
    EXPECT_EQ(fileFor(r, QStringLiteral("new-name.txt")), nullptr);
}

// INV-7 — empty / hunk-less input yields an empty result (no crash).
TEST(GitDiffHunks, EmptyInput) {
    EXPECT_TRUE(GitWrap::parseDiffHunks(QByteArray(), false).isEmpty());
    EXPECT_TRUE(GitWrap::parseDiffHunks(QByteArray(), true).isEmpty());
}
