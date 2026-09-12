// ANTS-5050 / ANTS-5134 — wall time of a Claude-transcript walk.
//
// ClaudeTaskListTracker::parseTranscript and ClaudeBgTaskTracker::parseTranscript
// each walk the transcript's 16 MiB tail and JSON-parse every line, on the GUI
// thread, on every debounced fileChanged. ANTS-5050 recorded the cost as "not
// measured", which is what this benchmark answers — and it is the gate for the
// incremental parse that item asks for: the appended-delta number has to be
// compared against the full-walk number on the same file, in one process.
//
// Links the real trackers (ants_claude_lib) rather than reproducing their walk,
// so the number is the walk the trackers actually run. A reproduction would
// measure the reproduction (ANTS-5126 records that cost on the audit side).
//
// The transcript is SYNTHETIC by default and written to a temp file: a real
// transcript is user conversation data, so a benchmark must not need one to
// run, and a checked-in fixture of a realistic size would be a ~20 MiB blob in
// the repo. Point ANTS_PERF_TRANSCRIPT=<path> at a real transcript to measure
// that instead — the shape below is modelled on one, but only a real file
// settles the constant factor.
//
// Default size is deliberately OVER the walker's 16 MiB cap, because the
// transcripts this fires on are: the largest under ~/.claude/projects measured
// 68 MB on 2026-09-12. Under the cap the walk is O(file); over it the walk is
// O(cap) and constant, which is the regime that matters.

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>

#include "claudebgtasks.h"
#include "claudetasklist.h"
#include "perf_metric.h"

namespace {

// One JSONL line of assistant prose, padded to `padBytes` so the synthetic
// file reaches a realistic size the way a real transcript does — most of a
// transcript's bytes are tool_result and text content, not tool_use payloads.
QByteArray fillerLine(int seq, int padBytes) {
    QByteArray pad(padBytes, 'x');
    return QByteArray(
               "{\"type\":\"assistant\",\"timestamp\":\"2026-09-12T10:00:00.000Z\","
               "\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"")
           + QByteArray::number(seq) + " " + pad + "\"}]}}\n";
}

// A TodoWrite snapshot — the event the task-list tracker keys on. Its handler
// clears and refills the list, so one of these late in the file determines the
// whole parse result.
QByteArray todoWriteLine(int nTodos) {
    QByteArray todos;
    for (int i = 0; i < nTodos; ++i) {
        if (i) todos += ",";
        todos += "{\"content\":\"task " + QByteArray::number(i)
                 + "\",\"status\":\"" + (i == 0 ? "in_progress" : "pending")
                 + "\",\"activeForm\":\"doing " + QByteArray::number(i) + "\"}";
    }
    return QByteArray(
               "{\"type\":\"assistant\",\"timestamp\":\"2026-09-12T10:00:00.000Z\","
               "\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_todo\","
               "\"name\":\"TodoWrite\",\"input\":{\"todos\":[")
           + todos + "]}}]}}\n";
}

// A background-task launch plus the user-side confirmation that gives it an
// id — the bg tracker drops a launch that never received one, so both lines
// are needed for the entry to survive its filter.
QByteArray bgLaunchLines(int seq) {
    const QByteArray tu = "toolu_bg" + QByteArray::number(seq);
    const QByteArray bg = "bgid" + QByteArray::number(seq);
    return QByteArray(
               "{\"type\":\"assistant\",\"timestamp\":\"2026-09-12T10:00:00.000Z\","
               "\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"")
           + tu
           + "\",\"name\":\"Bash\",\"input\":{\"command\":\"sleep 1\",\"description\":"
             "\"sleeper\",\"run_in_background\":true}}]}}\n"
             "{\"type\":\"user\",\"timestamp\":\"2026-09-12T10:00:01.000Z\","
             "\"toolUseResult\":{\"backgroundTaskId\":\"" + bg
           + "\"},\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\""
           + tu + "\",\"content\":\"Command running in background with ID: " + bg
           + ". Output is being written to: /tmp/nonexistent-" + bg + ".output\"}]}}\n";
}

// Write a synthetic transcript of at least `targetBytes`, returning its path.
QString writeSyntheticTranscript(const QString &path, qint64 targetBytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "bench_transcript_walk: cannot write %s\n",
                     qPrintable(path));
        return QString();
    }

    // Interleave the payload events through the filler so the parse does real
    // work across the whole tail rather than only at one end.
    int seq = 0;
    while (f.size() < targetBytes) {
        f.write(fillerLine(seq, 900));
        if (seq % 50 == 0) f.write(bgLaunchLines(seq));
        if (seq % 200 == 0) f.write(todoWriteLine(12));
        ++seq;
    }
    // Land a final TodoWrite at the tail: the tracker's visible result comes
    // from the last snapshot, so this is what a real session's chip reflects.
    f.write(todoWriteLine(12));
    f.close();
    return path;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const QByteArray realPath = qgetenv("ANTS_PERF_TRANSCRIPT");
    bool iterOk = false;
    const int iterEnv = qgetenv("ANTS_PERF_ITERATIONS").toInt(&iterOk);
    const int iterations = (iterOk && iterEnv > 0) ? iterEnv : 5;

    QTemporaryDir tmp;
    QString path;
    bool synthetic = true;

    if (!realPath.isEmpty()) {
        path = QString::fromLocal8Bit(realPath);
        synthetic = false;
        if (!QFileInfo::exists(path)) {
            std::fprintf(stderr,
                         "bench_transcript_walk: ANTS_PERF_TRANSCRIPT=%s "
                         "does not exist\n",
                         qPrintable(path));
            return 1;
        }
    } else {
        if (!tmp.isValid()) {
            std::fprintf(stderr, "bench_transcript_walk: no temp dir\n");
            return 1;
        }
        // 20 MiB — over the walker's 16 MiB cap, matching the real regime.
        path = writeSyntheticTranscript(tmp.filePath("transcript.jsonl"),
                                        20LL * 1024 * 1024);
        if (path.isEmpty()) return 1;
    }

    const qint64 fileBytes = QFileInfo(path).size();
    std::printf("bench_transcript_walk: %s transcript, %.1f MiB, %d iterations\n",
                synthetic ? "synthetic" : "real",
                double(fileBytes) / (1024.0 * 1024.0), iterations);
    std::printf("phase,ms_per_walk,entries\n");

    // Warm the page cache so the number is parse cost, not first-read I/O —
    // the trackers re-walk a file the OS has just been written into, so warm
    // is the honest case.
    (void)ClaudeTaskListTracker::parseTranscript(path);
    (void)ClaudeBgTaskTracker::parseTranscript(path);

    QElapsedTimer t;

    t.start();
    int taskEntries = 0;
    for (int i = 0; i < iterations; ++i)
        taskEntries = ClaudeTaskListTracker::parseTranscript(path).size();
    const double taskMs = double(t.nsecsElapsed()) / 1e6 / iterations;

    t.start();
    int bgEntries = 0;
    for (int i = 0; i < iterations; ++i)
        bgEntries = ClaudeBgTaskTracker::parseTranscript(path).size();
    const double bgMs = double(t.nsecsElapsed()) / 1e6 / iterations;

    std::printf("tasklist,%.4f,%d\n", taskMs, taskEntries);
    std::printf("bgtasks,%.4f,%d\n", bgMs, bgEntries);
    std::printf("both,%.4f,%d\n", taskMs + bgMs, taskEntries + bgEntries);

    // ANTS-5050 — the quantity the fix is about: what ONE debounced append
    // costs now. Prime both cursors on the whole file, then append a chunk
    // and resume, which is what a tracker does per fileChanged burst.
    //
    // Measured on a COPY so a real transcript named by ANTS_PERF_TRANSCRIPT
    // is never written to.
    double appendMs = -1.0;
    if (tmp.isValid()) {
        const QString work = tmp.filePath("append-probe.jsonl");
        QFile::remove(work);
        if (QFile::copy(path, work)) {
            ClaudeTranscript::Cursor taskCur;
            ClaudeTaskAccum taskAcc;
            ClaudeTranscript::Cursor bgCur;
            ClaudeBgTaskAccum bgAcc;
            // Prime: this is the one full walk a session pays at startup.
            ClaudeTaskListTracker::parseIncremental(work, taskCur, taskAcc);
            ClaudeBgTaskTracker::parseIncremental(work, bgCur, bgAcc);

            t.start();
            for (int i = 0; i < iterations; ++i) {
                QFile f(work);
                if (f.open(QIODevice::Append)) {
                    f.write(fillerLine(90000 + i, 900));
                    f.write(todoWriteLine(12));
                    f.close();
                }
                if (!ClaudeTranscript::canResume(work, taskCur)) {
                    taskCur = {};
                    taskAcc = {};
                }
                ClaudeTaskListTracker::parseIncremental(work, taskCur, taskAcc);
                if (!ClaudeTranscript::canResume(work, bgCur)) {
                    bgCur = {};
                    bgAcc = {};
                }
                ClaudeBgTaskTracker::parseIncremental(work, bgCur, bgAcc);
            }
            appendMs = double(t.nsecsElapsed()) / 1e6 / iterations;
            std::printf("append_both,%.4f,%d\n", appendMs, 0);
            AntsPerf::reportLowerBetter(
                "claude.transcript.append_both_ms", appendMs, "ms");
            if (appendMs > 0.0)
                AntsPerf::reportHigherBetter("claude.transcript.append_speedup",
                                             (taskMs + bgMs) / appendMs, "x");
        }
    }

    AntsPerf::reportLowerBetter("claude.transcript.tasklist_walk_ms", taskMs, "ms");
    AntsPerf::reportLowerBetter("claude.transcript.bgtasks_walk_ms", bgMs, "ms");
    // What one debounced append costs the GUI thread with both trackers
    // watching one transcript — the quantity ANTS-5050 is about.
    AntsPerf::reportLowerBetter("claude.transcript.both_walks_ms", taskMs + bgMs,
                                "ms");
    return 0;
}
