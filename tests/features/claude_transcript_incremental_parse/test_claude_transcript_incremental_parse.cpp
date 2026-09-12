// Feature-conformance test for ANTS-5050 — incremental Claude-transcript
// parse from a byte cursor. Links the real walker and both real trackers and
// drives them against transcripts written to a temp dir, so the resumable
// path under test is the one the trackers run.
//
// INV labels qualified ANTS-5050-INV-N. See spec.md.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "claudebgtasks.h"
#include "claudetasklist.h"
#include "claudetranscriptwalker.h"

#include <gtest/gtest.h>
#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

// --- fixture builders -------------------------------------------------

QByteArray todoWrite(const char *status, const char *subject) {
    return QByteArray(
               "{\"type\":\"assistant\",\"timestamp\":\"2026-09-12T10:00:00.000Z\","
               "\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_t\","
               "\"name\":\"TodoWrite\",\"input\":{\"todos\":[{\"content\":\"")
           + subject + "\",\"status\":\"" + status + "\",\"activeForm\":\"doing\"}]}}]}}\n";
}

QByteArray bgLaunch(const char *seq) {
    const QByteArray tu = QByteArray("toolu_") + seq;
    const QByteArray bg = QByteArray("bg") + seq;
    return QByteArray(
               "{\"type\":\"assistant\",\"timestamp\":\"2026-09-12T10:00:00.000Z\","
               "\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"")
           + tu
           + "\",\"name\":\"Bash\",\"input\":{\"command\":\"sleep 1\",\"description\":"
             "\"d\",\"run_in_background\":true}}]}}\n"
             "{\"type\":\"user\",\"timestamp\":\"2026-09-12T10:00:01.000Z\","
             "\"toolUseResult\":{\"backgroundTaskId\":\"" + bg
           + "\"},\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\""
           + tu + "\",\"content\":\"running in background with ID: " + bg
           + ". Output is being written to: /tmp/ants-5050-absent-" + bg + ".output\"}]}}\n";
}

// A line the walker must skip, so the cursor is exercised over non-events.
QByteArray blankAndJunk() {
    return QByteArray("\n[1,2,3]\n");
}

void writeAll(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(f.write(bytes), bytes.size());
}

void appendTo(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::Append));
    ASSERT_EQ(f.write(bytes), bytes.size());
}

// Count the events a walk yields, so INV-2 can be stated about visits rather
// than about a tracker's filtered output.
int visitCount(const QString &path, ClaudeTranscript::Cursor &cur) {
    int n = 0;
    ClaudeTranscript::walkFrom(path, cur,
        [&](const QJsonObject &, qint64) { ++n; });
    return n;
}

}  // namespace

// --- INV-1 — a partial trailing line is not consumed ------------------

TEST(TranscriptIncremental, PartialTrailingLineIsNotConsumed) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath("t.jsonl");

    // A complete event, then a line cut mid-write (no newline).
    QByteArray partial = todoWrite("pending", "one");
    partial += "{\"type\":\"assistant\",\"message\":{\"conte";
    writeAll(p, partial);

    ClaudeTranscript::Cursor cur;
    const int first = visitCount(p, cur);
    EXPECT_EQ(first, 1) << "only the complete line should be visited";
    // The cursor must sit at the first byte of the partial line, not at EOF.
    EXPECT_EQ(cur.offset, todoWrite("pending", "one").size())
        << "cursor consumed a line that had no newline";

    // Completing that line must make it visible exactly once.
    appendTo(p, "nt\":[]}}\n");
    const int second = visitCount(p, cur);
    EXPECT_EQ(second, 1) << "the completed line should be visited once";
}

// --- INV-2 — an incremental walk visits only the appended events ------

TEST(TranscriptIncremental, IncrementalVisitsOnlyAppendedEvents) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath("t.jsonl");

    writeAll(p, todoWrite("pending", "one") + blankAndJunk());

    ClaudeTranscript::Cursor cur;
    EXPECT_EQ(visitCount(p, cur), 1);

    appendTo(p, todoWrite("completed", "one"));
    EXPECT_EQ(visitCount(p, cur), 1)
        << "a resumed walk must not revisit events before the cursor";

    // Nothing appended — nothing visited.
    EXPECT_EQ(visitCount(p, cur), 0);
}

// --- INV-3 — the cursor tracks consumed bytes, not a size sample ------

TEST(TranscriptIncremental, CursorTracksConsumedNotSampledSize) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath("t.jsonl");

    const QByteArray body = todoWrite("pending", "one");
    writeAll(p, body);

    ClaudeTranscript::Cursor cur;
    visitCount(p, cur);
    EXPECT_EQ(cur.offset, body.size())
        << "cursor must land on the consumed-line boundary";

    // A write landing after the walk must leave the cursor behind EOF, so the
    // next walk picks those bytes up. ANTS-1458 INV-4 depends on this.
    appendTo(p, todoWrite("completed", "one"));
    EXPECT_LT(cur.offset, QFile(p).size());
}

// --- INV-4 — staleness forces a full re-walk --------------------------

TEST(TranscriptIncremental, TruncationForcesFullRewalk) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath("t.jsonl");

    writeAll(p, todoWrite("pending", "one") + todoWrite("pending", "two"));
    ClaudeTranscript::Cursor cur;
    visitCount(p, cur);
    EXPECT_TRUE(ClaudeTranscript::canResume(p, cur));

    // Truncate to nothing: the offset is now past EOF.
    writeAll(p, QByteArray());
    EXPECT_FALSE(ClaudeTranscript::canResume(p, cur));
}

TEST(TranscriptIncremental, ShrinkForcesFullRewalk) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath("t.jsonl");

    writeAll(p, todoWrite("pending", "one") + todoWrite("pending", "two"));
    ClaudeTranscript::Cursor cur;
    visitCount(p, cur);

    writeAll(p, todoWrite("pending", "one"));
    EXPECT_FALSE(ClaudeTranscript::canResume(p, cur))
        << "a shorter file must not be resumed against the old offset";
}

TEST(TranscriptIncremental, RewriteInPlaceForcesFullRewalk) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath("t.jsonl");

    writeAll(p, todoWrite("pending", "one"));
    ClaudeTranscript::Cursor cur;
    visitCount(p, cur);
    const qint64 offset = cur.offset;

    // Same length or longer, but the byte before the offset is no longer a
    // newline — the file was rewritten rather than appended to.
    QByteArray rewritten(offset, 'x');
    rewritten += "\n";
    rewritten += todoWrite("pending", "two");
    writeAll(p, rewritten);
    EXPECT_FALSE(ClaudeTranscript::canResume(p, cur))
        << "an in-place rewrite must not be resumed";
}

TEST(TranscriptIncremental, UnprimedCursorNeverResumes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath("t.jsonl");
    writeAll(p, todoWrite("pending", "one"));

    ClaudeTranscript::Cursor fresh;
    EXPECT_FALSE(ClaudeTranscript::canResume(p, fresh));
}

// --- INV-5 — the tail cap is a cold-walk rule only --------------------

TEST(TranscriptIncremental, ResumedWalkIgnoresTailCap) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath("t.jsonl");

    // Prime on a small file, then append: the appended event sits far below
    // any tail cap, and a resumed walk must still see it.
    writeAll(p, todoWrite("pending", "one"));
    ClaudeTranscript::Cursor cur;
    visitCount(p, cur);

    appendTo(p, todoWrite("completed", "one"));
    EXPECT_EQ(visitCount(p, cur), 1)
        << "a resumed walk must read to EOF regardless of the cap";
}

// --- INV-7 — a stepped parse equals a cold parse ----------------------

TEST(TranscriptIncremental, SteppedParseEqualsColdParseTaskList) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString stepped = dir.filePath("stepped.jsonl");
    const QString cold = dir.filePath("cold.jsonl");

    // Each chunk is one append, as Claude would write it.
    const QByteArray chunks[] = {
        todoWrite("pending", "one"),
        blankAndJunk(),
        todoWrite("in_progress", "one"),
        todoWrite("completed", "one"),
    };

    writeAll(stepped, QByteArray());
    ClaudeTranscript::Cursor cur;
    ClaudeTaskAccum acc;
    QList<ClaudeTask> steppedResult;
    QByteArray whole;
    for (const QByteArray &chunk : chunks) {
        appendTo(stepped, chunk);
        whole += chunk;
        if (!ClaudeTranscript::canResume(stepped, cur)) {
            cur = {};
            acc = {};
        }
        steppedResult =
            ClaudeTaskListTracker::parseIncremental(stepped, cur, acc);
    }

    writeAll(cold, whole);
    const QList<ClaudeTask> coldResult =
        ClaudeTaskListTracker::parseTranscript(cold);

    ASSERT_EQ(steppedResult.size(), coldResult.size())
        << "stepped and cold parses disagree on entry count";
    for (int i = 0; i < coldResult.size(); ++i) {
        EXPECT_EQ(steppedResult[i].subject, coldResult[i].subject);
        EXPECT_EQ(steppedResult[i].status, coldResult[i].status);
    }
}

TEST(TranscriptIncremental, SteppedParseEqualsColdParseBgTasks) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString stepped = dir.filePath("stepped.jsonl");
    const QString cold = dir.filePath("cold.jsonl");

    const QByteArray chunks[] = {
        bgLaunch("1"),
        blankAndJunk(),
        bgLaunch("2"),
    };

    writeAll(stepped, QByteArray());
    ClaudeTranscript::Cursor cur;
    ClaudeBgTaskAccum acc;
    QList<ClaudeBackgroundTask> steppedResult;
    QByteArray whole;
    for (const QByteArray &chunk : chunks) {
        appendTo(stepped, chunk);
        whole += chunk;
        if (!ClaudeTranscript::canResume(stepped, cur)) {
            cur = {};
            acc = {};
        }
        steppedResult = ClaudeBgTaskTracker::parseIncremental(stepped, cur, acc);
    }

    writeAll(cold, whole);
    const QList<ClaudeBackgroundTask> coldResult =
        ClaudeBgTaskTracker::parseTranscript(cold);

    ASSERT_EQ(steppedResult.size(), coldResult.size())
        << "stepped and cold parses disagree on entry count";
    for (int i = 0; i < coldResult.size(); ++i)
        EXPECT_EQ(steppedResult[i].id, coldResult[i].id);
}

// --- INV-8 — the full-parse contract is unchanged ---------------------

TEST(TranscriptIncremental, FullParseContractUnchanged) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString p = dir.filePath("t.jsonl");

    writeAll(p, todoWrite("pending", "one") + blankAndJunk()
                    + todoWrite("completed", "one"));

    // parseTranscript still takes a path and returns the tracker's list; the
    // last TodoWrite snapshot still wins.
    const QList<ClaudeTask> out = ClaudeTaskListTracker::parseTranscript(p);
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].status, QStringLiteral("completed"));

    // And walk() still reports the latest-event clock.
    const qint64 latest = ClaudeTranscript::walk(
        p, [](const QJsonObject &, qint64) {});
    EXPECT_GT(latest, 0);
}

// --- INV-6 — a path change resets the cursor --------------------------
//
// setTranscriptPath is where the reset lives; the tracker's cursor and
// accumulator are private, so this checks the reset at its source.

TEST(TranscriptIncremental, PathChangeResetsCursor) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDETASKLIST_CPP_PATH);
    ASSERT_FALSE(src.empty()) << "could not read claudetasklist.cpp";

    // The reset must sit in setTranscriptPath beside the existing
    // m_lastRescan* resets, or a path change can resume against the old file.
    const std::size_t fn = src.find("void ClaudeTaskListTracker::setTranscriptPath");
    ASSERT_NE(fn, std::string::npos);
    const std::size_t end = src.find("\n}", fn);
    ASSERT_NE(end, std::string::npos);
    const std::string body = src.substr(fn, end - fn);

    EXPECT_NE(body.find("m_cursor"), std::string::npos)
        << "setTranscriptPath does not reset the parse cursor";
    EXPECT_NE(body.find("m_acc"), std::string::npos)
        << "setTranscriptPath does not reset the parse accumulator";
}
