// ANTS-5047 — the last-touch blame's FINISHED handler still parses on the
// GUI thread, still re-reads ROADMAP.md with no size cap (racing a
// concurrent write), and still spawns --line-porcelain (tens of MB at
// today's file size) rather than --porcelain.
// Contract: spec.md beside this file.
//
// INV-1, INV-2 and INV-5 are pure-function tests: no git process, no
// RoadmapDialog construction. INV-3 and INV-4 are source scrapes over
// src/roadmapdialog.cpp.
//
// INV-1 through INV-4 are expected to FAIL against the code as filed —
// that is the point of this contract. INV-5 is a green regression guard.
//
// Blame payloads are built as plain QByteArray concatenation, never
// through QStringLiteral(...).arg(...)/QLatin1String — that path does NOT
// decode a multi-byte UTF-8 sequence (QStringLiteral's QT_UNICODE_LITERAL
// is a literal `u"" str` concatenation: each raw byte of a non-ASCII
// character is zero-extended into its own UTF-16 code unit instead of
// being UTF-8-decoded). See the project memory note
// feedback_qstringliteral_utf8_emoji_trap.md. Byte-for-byte QByteArray
// concatenation sidesteps it entirely: the bytes below are 🚧's real UTF-8
// encoding (f0 9f 9a a7, confirmed against the sibling test's fixture),
// verbatim, so a future correct implementation that QString::fromUtf8()s
// blame's own content lines sees the real glyph rather than mangled bytes.

#include "config.h"
#include "roadmapdialog.h"

#include "../../_support/srcgrep.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

// Write `bytes` to `path` verbatim; returns false on any I/O error so the
// caller can ASSERT_TRUE(...) at the call site. (Deliberately NOT an
// ASSERT_* inside a void helper: gtest only unwinds the helper itself on a
// fatal failure there, not the calling TEST — the caller has to check.) No
// trailing newline is added — a caller testing EOF-without-a-blank-line
// behaviour controls that via what it passes.
bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(bytes) == bytes.size();
}

// One --line-porcelain-style record: "<40-hex> <line> <line>\nauthor-time
// <when>\n\t<text>\n". `text` is raw UTF-8 bytes (a plain narrow literal is
// fine — the source file itself is UTF-8 and QByteArray copies bytes
// verbatim, no re-encoding).
QByteArray blameRecord(int line, qint64 when, const char *text) {
    QByteArray out(40, 'a');
    out += ' ';
    out += QByteArray::number(line);
    out += ' ';
    out += QByteArray::number(line);
    out += '\n';
    out += "author-time ";
    out += QByteArray::number(when);
    out += '\n';
    out += '\t';
    out += text;
    out += '\n';
    return out;
}

}  // namespace

// INV-1 — the date belongs to what blame's own text says, not to a re-read
// of the file taken after blame already finished.
TEST(RoadmapLastTouchBlameWorker, Inv1DateComesFromBlameTextNotAFileReread) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ROADMAP.md"));

    // Blame's own line 5 names ANTS-7001.
    QByteArray blame;
    blame += blameRecord(1, 1000, "# ROADMAP");
    blame += blameRecord(2, 1000, "");
    blame += blameRecord(3, 1000, "## Now");
    blame += blameRecord(4, 1000, "");
    blame += blameRecord(5, 5000,
        "- \xF0\x9F\x9A\xA7 [ANTS-7001] **A thing blame actually saw.**");
    blame += blameRecord(6, 5000, "  Kind: implement.");
    blame += blameRecord(7, 1000, "");

    // The file on disk names a DIFFERENT id at the same line — standing in
    // for a roadmap_log write that landed between blame finishing and
    // lastTouchFromBlame()'s re-read, exactly the ANTS-5047 race.
    ASSERT_TRUE(writeFile(path,
        "# ROADMAP\n"
        "\n"
        "## Now\n"
        "\n"
        "- \xF0\x9F\x9A\xA7 [ANTS-8002] **A different thing the write replaced it with.**\n"
        "  Kind: implement.\n"
        "\n"));

    const auto out = RoadmapDialog::lastTouchFromBlame(blame);

    EXPECT_TRUE(out.contains(QStringLiteral("ANTS-7001")))
        << "blame's own text names ANTS-7001 with a \xF0\x9F\x9A\xA7 bullet at "
           "author-time 5000 -- the date must come from blame's text, not "
           "from a re-read of the file that changed in between";
    EXPECT_EQ(out.value(QStringLiteral("ANTS-7001")), 5000);
    EXPECT_FALSE(out.contains(QStringLiteral("ANTS-8002")))
        << "ANTS-8002 is not named anywhere in blame's own content lines -- "
           "it exists only in a LATER write to the file. Dating it is the "
           "ANTS-5047 write-race: blame's date landed on whatever id a "
           "later write happened to put at the same line number.";
}

// INV-2 — a --porcelain-shaped group that omits its header (because the
// commit already printed one earlier) must still get that commit's OWN
// author-time, not whatever commit's header most recently appeared.
TEST(RoadmapLastTouchBlameWorker, Inv2ReusedPorcelainCommitKeepsItsOwnTime) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ROADMAP.md"));

    const QByteArray shaA(40, 'a');
    const QByteArray shaB(40, 'b');

    QByteArray blame;
    // Line 1 — commit A's first appearance: full group, author-time 1111.
    blame += shaA + " 1 1 3\nauthor-time 1111\n\t# ROADMAP\n";
    // Line 2 — commit B's only appearance: author-time 8888. Under a
    // parser holding one running "current author-time" variable, this is
    // what lines 3-6 (commit A again, header omitted) are left reading.
    blame += shaB + " 2 2 1\nauthor-time 8888\n\t\n";
    // Lines 3-6 — commit A again, non-contiguous with line 1, so
    // --porcelain omits the header block entirely: just the
    // sha/orig/final line, then the content line straight after.
    blame += shaA + " 3 3\n\t## Now\n";
    blame += shaA + " 4 4\n\t\n";
    blame += shaA + " 5 5\n\t- \xF0\x9F\x9A\xA7 [ANTS-3001] **thing**\n";
    blame += shaA + " 6 6\n\t  Kind: implement.\n";

    // The file mirrors blame's text exactly, so this isolates the sha-reuse
    // bug from the separate file-vs-blame-text bug INV-1 covers.
    ASSERT_TRUE(writeFile(path,
        "# ROADMAP\n"
        "\n"
        "## Now\n"
        "\n"
        "- \xF0\x9F\x9A\xA7 [ANTS-3001] **thing**\n"
        "  Kind: implement.\n"));

    const auto out = RoadmapDialog::lastTouchFromBlame(blame);

    ASSERT_TRUE(out.contains(QStringLiteral("ANTS-3001")));
    EXPECT_EQ(out.value(QStringLiteral("ANTS-3001")), 1111)
        << "lines 5-6 belong to commit " << shaA.left(8).constData()
        << "..., whose own author-time is 1111. A parser tracking one "
           "running \"current author-time\" instead of a per-sha lookup is "
           "left holding commit " << shaB.left(8).constData()
        << "...'s 8888 (from line 2's header) once headers stop repeating "
           "for a reused commit under --porcelain.";
}

// INV-3 — lastTouchBlameArgs() must pass --porcelain, never --line-porcelain.
TEST(RoadmapLastTouchBlameWorker, Inv3BlameArgsUsePorcelainNotLinePorcelain) {
    const std::string src = ants_test::slurpFile(ROADMAPDIALOG_CPP);
    ASSERT_FALSE(src.empty()) << "could not read " << ROADMAPDIALOG_CPP;

    const std::string body = ants_test::stripComments(ants_test::slurpFunctionBody(
        src, "QStringList RoadmapDialog::lastTouchBlameArgs("));
    ASSERT_FALSE(body.empty())
        << "could not locate RoadmapDialog::lastTouchBlameArgs(...) in "
        << ROADMAPDIALOG_CPP;

    EXPECT_NE(body.find("--porcelain"), std::string::npos)
        << "lastTouchBlameArgs() must pass --porcelain (header block once "
           "per commit, looked up per sha) rather than --line-porcelain "
           "(header repeated on every line) -- the whole-file output is "
           "tens of MB at today's ROADMAP.md size against the ~4 MB "
           "transient docs/specs/ANTS-1237.md section 6 budgets.\n"
           "function body:\n" << body;
    EXPECT_EQ(body.find("--line-porcelain"), std::string::npos)
        << "still passing --line-porcelain -- the whole-file blame is "
           "exactly the size problem ANTS-5047 reports.\nfunction body:\n"
        << body;
}

// INV-4 — the finished handler's parse must run on a worker thread, never
// directly on the GUI thread.
TEST(RoadmapLastTouchBlameWorker, Inv4ParseRunsOnAWorkerThreadNotTheGuiThread) {
    const std::string src = ants_test::slurpFile(ROADMAPDIALOG_CPP);
    ASSERT_FALSE(src.empty()) << "could not read " << ROADMAPDIALOG_CPP;

    const std::string body = ants_test::stripComments(ants_test::slurpFunctionBody(
        src, "void RoadmapDialog::refreshLastTouchDatesIfStale("));
    ASSERT_FALSE(body.empty())
        << "could not locate RoadmapDialog::refreshLastTouchDatesIfStale(...) "
           "in " << ROADMAPDIALOG_CPP;

    const auto threadPos = body.find("QThread::create(");
    ASSERT_NE(threadPos, std::string::npos)
        << "refreshLastTouchDatesIfStale() has no QThread::create(...) "
           "worker anywhere in its body -- the blame parse still runs "
           "inline in the finished handler, on the GUI thread.\n"
           "function body:\n" << body;

    const auto callPos = body.find("lastTouchFromBlame(");
    ASSERT_NE(callPos, std::string::npos)
        << "lastTouchFromBlame(...) is no longer called from "
           "refreshLastTouchDatesIfStale() at all.\nfunction body:\n" << body;

    EXPECT_GT(callPos, threadPos)
        << "lastTouchFromBlame(...) (offset " << callPos << " in the "
           "comment-stripped body) must be called from INSIDE the "
           "QThread::create(...) worker lambda (offset " << threadPos
        << "), not directly in the GUI-thread finished handler.\n"
           "function body:\n" << body;
}

// INV-5 (GUARD, not a red invariant) — the block rule (bullet line + every
// contiguous 2-space-indented continuation, MAX, stopping at blank /
// new-bullet / EOF) is unchanged by *where* the line text comes from. Blame
// and file agree here, so this passes identically under today's
// file-reading implementation and under a fixed blame-text implementation.
TEST(RoadmapLastTouchBlameWorker, Inv5GuardBlockRuleSurvivesEitherTextSource) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ROADMAP.md"));

    QByteArray blame;
    blame += blameRecord(1, 1000, "# ROADMAP");
    blame += blameRecord(2, 1000, "");
    blame += blameRecord(3, 1000, "## Now");
    blame += blameRecord(4, 1000, "");
    blame += blameRecord(5, 2000,
        "- \xF0\x9F\x9A\xA7 [ANTS-4001] **First, block ends at a new bullet.**");
    blame += blameRecord(6, 9000, "  Kind: implement.");   // MAX for ANTS-4001
    blame += blameRecord(7, 3000, "  Source: fixture.");
    blame += blameRecord(8, 4000,
        "- \xF0\x9F\x9A\xA7 [ANTS-4002] **Second, block ends at EOF.**");
    blame += blameRecord(9, 9500, "  Kind: fix.");          // MAX for ANTS-4002

    // File text matches blame's content lines exactly, with NO trailing
    // newline after line 9 — the EOF-without-a-blank-line case.
    const QByteArray fileText =
        "# ROADMAP\n"
        "\n"
        "## Now\n"
        "\n"
        "- \xF0\x9F\x9A\xA7 [ANTS-4001] **First, block ends at a new bullet.**\n"
        "  Kind: implement.\n"
        "  Source: fixture.\n"
        "- \xF0\x9F\x9A\xA7 [ANTS-4002] **Second, block ends at EOF.**\n"
        "  Kind: fix.";
    ASSERT_TRUE(writeFile(path, fileText));

    const auto out = RoadmapDialog::lastTouchFromBlame(blame);

    ASSERT_TRUE(out.contains(QStringLiteral("ANTS-4001")));
    EXPECT_EQ(out.value(QStringLiteral("ANTS-4001")), 9000)
        << "must take MAX over lines 5-7 and stop at line 8's new bullet, "
           "not run past it";
    ASSERT_TRUE(out.contains(QStringLiteral("ANTS-4002")));
    EXPECT_EQ(out.value(QStringLiteral("ANTS-4002")), 9500)
        << "block ending at EOF (no trailing blank line after line 9) must "
           "still take MAX up to the file's last line";
}
