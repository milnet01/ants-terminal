// Why this exists: ANTS-6245 — the pure half of workspace_search's timeout
// fallback must never drop a file that can hold a match. Contract:
// tests/features/workspace_search_timeout_fallback/spec.md; design:
// docs/specs/ANTS-6245-search-timeout-git-fallback.md. Pure functions, no git.

#include "searchprefilter.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace {

QByteArray row(const char *tag, const char *mode, const char *stage,
               const char *path) {
    QByteArray r = QByteArray(tag) + ' ' + mode + ' '
                   + QByteArray(40, 'a') + ' ' + stage + '\t' + path;
    r.append('\0');
    return r;
}

QByteArray nul(const char *path) {
    QByteArray r(path);
    r.append('\0');
    return r;
}

}  // namespace

// ----------------------------------------------------------- needle (§ 4.2)

TEST(searchprefilter, needleLongestCleanRun) {
    EXPECT_EQ(SearchPrefilter::needle("zebrafoo"), QByteArray("zebrafoo"));
    EXPECT_EQ(SearchPrefilter::needle("ab kd efgh"), QByteArray("d efgh"))
        << "k breaks a run; the longest run wins";
}

TEST(searchprefilter, needleFirstWinsATie) {
    EXPECT_EQ(SearchPrefilter::needle("abckdef"), QByteArray("abc"));
    EXPECT_EQ(SearchPrefilter::needle("defKabc"), QByteArray("def"));
}

TEST(searchprefilter, needleBreakers) {
    EXPECT_EQ(SearchPrefilter::needle("abSdef"), QByteArray("def"))
        << "S breaks a run";
    EXPECT_EQ(SearchPrefilter::needle("abcsdefg"), QByteArray("defg"))
        << "s breaks a run";
    EXPECT_EQ(SearchPrefilter::needle("kKsS"), QByteArray())
        << "nothing but excluded letters leaves no needle";
    EXPECT_EQ(SearchPrefilter::needle(QByteArray("ab\xC3\xA9" "cde")),
              QByteArray("cde")) << "non-ASCII bytes break a run";
    EXPECT_EQ(SearchPrefilter::needle(QByteArray("abcd\x01" "efg")),
              QByteArray("abcd")) << "a control byte breaks a run";
    EXPECT_EQ(SearchPrefilter::needle(QByteArray("abc\x7F" "defg")),
              QByteArray("defg")) << "0x7F is not printable ASCII";
    EXPECT_EQ(SearchPrefilter::needle(QByteArray()), QByteArray());
}

// ------------------------------------------------------- needleFile (§ 4.3)

TEST(searchprefilter, needleFileBytes) {
    const QByteArray f = SearchPrefilter::needleFile("zebrafoo");
    QList<QByteArray> lines = f.split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    ASSERT_EQ(lines.size(), 3) << "expected three fixed strings, one per line; "
                               << "actual bytes (hex): "
                               << f.toHex().toStdString();
    EXPECT_EQ(lines.at(0), QByteArray("zebrafoo"));
    EXPECT_EQ(lines.at(1), QByteArray("\xFF\xFE", 2));
    EXPECT_EQ(lines.at(2), QByteArray("\xFE\xFF", 2));
}

// --------------------------------------------------------- INV-10 (§ 4.5)

TEST(searchprefilter, fallbackBudget) {
    for (int b = 1; b <= 30; ++b) {
        const int expected = std::min(b, 30 - b) * 1000;
        const int got = SearchPrefilter::fallbackBudgetMs(b * 1000);
        EXPECT_EQ(got, expected) << "timeout_sec " << b;
        EXPECT_LE(b * 1000 + std::max(got, 0), 30000)
            << "first scan plus fallback outlast the maximum at timeout_sec "
            << b;
    }
}

// ----------------------------------------------------------- skipList (§ 4.4)

TEST(searchprefilter, skipListKeepsOnlyCleanCachedFiles) {
    const QByteArray ls =
        row("H", "100644", "0", "clean.txt") +
        row("H", "100755", "0", "exec.sh") +
        row("H", "100644", "0", "modified.txt") +
        row("C", "100644", "0", "modified.txt") +
        row("h", "100644", "0", "assume.txt") +
        row("S", "100644", "0", "skipwt.txt") +
        row("M", "100644", "1", "unmerged.txt") +
        row("H", "100644", "2", "staged.txt") +
        row("H", "120000", "0", "link") +
        row("H", "160000", "0", "sub") +
        row("H", "100644", "0", "hit.txt") +
        row("H", "100644", "0", "attr.txt") +
        row("H", "100644", "0", "last.txt");
    const QStringList got = SearchPrefilter::skipList(
        ls, nul("hit.txt"), nul("attr.txt"));
    const QStringList want{QStringLiteral("clean.txt"), QStringLiteral("exec.sh"), QStringLiteral("last.txt")};
    EXPECT_EQ(got, want)
        << "expected only clean tag-H stage-0 100644/100755 files git proved "
           "empty of the needle, in input order";
}

TEST(searchprefilter, skipListEmptyInputs) {
    EXPECT_TRUE(SearchPrefilter::skipList(QByteArray(), QByteArray(),
                                          QByteArray()).isEmpty());
}

// ------------------------------------------------------- INV-7 globEscape

TEST(searchprefilter, globEscape) {
    for (const QString &c : {QStringLiteral("["), QStringLiteral("]"),
                             QStringLiteral("*"), QStringLiteral("?"),
                             QStringLiteral("{"), QStringLiteral("}"),
                             QStringLiteral("\\")}) {
        EXPECT_EQ(SearchPrefilter::globEscape(QStringLiteral("a") + c + "b"),
                  QStringLiteral("a\\") + c + "b")
            << "metacharacter " << c.toStdString() << " not escaped";
    }
    EXPECT_EQ(SearchPrefilter::globEscape(QStringLiteral("dir/plain name.txt")),
              QStringLiteral("dir/plain name.txt"))
        << "a space and a slash are not metacharacters";
    EXPECT_EQ(SearchPrefilter::globEscape(QStringLiteral("!a#b")),
              QStringLiteral("!a#b"))
        << "! and # are made safe by the leading '/', not by escaping";
}

TEST(searchprefilter, skipArgsPairs) {
    const QStringList args = SearchPrefilter::skipArgs({QStringLiteral("a[1].txt"), QStringLiteral("b.txt")});
    const QStringList want{QStringLiteral("--glob"), QStringLiteral("!/a\\[1\\].txt"), QStringLiteral("--glob"), QStringLiteral("!/b.txt")};
    EXPECT_EQ(args, want);
}

TEST(searchprefilter, skipArgsCapStopsEarlyAtAPairBoundary) {
    QStringList many;
    for (int i = 0; i < 100; ++i)
        many << QStringLiteral("some/long/directory/name/file%1.txt").arg(i);
    const QStringList full = SearchPrefilter::skipArgs(many);
    ASSERT_EQ(full.size(), 200);
    const QStringList cut = SearchPrefilter::skipArgs(many, 200);
    EXPECT_LT(cut.size(), full.size()) << "a 200-byte cap must cut the list";
    EXPECT_EQ(cut.size() % 2, 0) << "the cap must fall between pairs";
    EXPECT_EQ(cut, full.mid(0, cut.size())) << "the cut list is a prefix";
}
