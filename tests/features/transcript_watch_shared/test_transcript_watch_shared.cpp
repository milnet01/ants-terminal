// Feature-conformance test for ANTS-1285 — both transcript trackers share
// one ClaudeTranscriptWatch. Behavioural: real trackers, a real file.
//
// INV labels qualified ANTS-1285-INV-N. See spec.md.

#include "../../_support/expect.h"
#include "claudebgtasks.h"
#include "claudetasklist.h"
#include "claudetranscriptwatch.h"

#include <gtest/gtest.h>
#include <QFile>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <string>

ANTS_TEST_SCOPE();

namespace {

QString writeTranscript(const QTemporaryDir &dir, const QString &name,
                        const QByteArray &body) {
    QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(body);
    return path;
}

constexpr char kLine[] = "{\"type\":\"user\",\"message\":{\"content\":\"hi\"}}\n";

void testInv1_oneWatchCounted() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1285-INV-1 setup"); return; }
    const QString a = writeTranscript(dir, QStringLiteral("a.jsonl"), kLine);
    const QString b = writeTranscript(dir, QStringLiteral("b.jsonl"), kLine);
    ClaudeTranscriptWatch &hub = ClaudeTranscriptWatch::instance();

    auto tasks = std::make_unique<ClaudeTaskListTracker>();
    auto bg = std::make_unique<ClaudeBgTaskTracker>();
    tasks->setTranscriptPath(a);
    bg->setTranscriptPath(a);
    expect(hub.subscribers(a) == 2,
           "ANTS-1285-INV-1: two trackers on one path are two subscribers",
           "got " + std::to_string(hub.subscribers(a)));
    expect(hub.isWatching(a),
           "ANTS-1285-INV-1: the shared path is watched");

    tasks->setTranscriptPath(b);
    expect(hub.subscribers(a) == 1 && hub.isWatching(a),
           "ANTS-1285-INV-1: re-pointing one tracker keeps the other's watch",
           "subscribers " + std::to_string(hub.subscribers(a)));

    bg.reset();
    expect(hub.subscribers(a) == 0 && !hub.isWatching(a),
           "ANTS-1285-INV-1: destroying the last subscriber drops the watch");
    tasks.reset();
    expect(hub.subscribers(b) == 0 && !hub.isWatching(b),
           "ANTS-1285-INV-1: a destroyed tracker releases its path");
}

void testInv2_appendReachesHub() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1285-INV-2 setup"); return; }
    const QString path = writeTranscript(dir, QStringLiteral("t.jsonl"), kLine);
    ClaudeTaskListTracker tracker;
    tracker.setTranscriptPath(path);

    QSignalSpy spy(&ClaudeTranscriptWatch::instance(),
                   &ClaudeTranscriptWatch::changed);
    {
        QFile f(path);
        if (!f.open(QIODevice::Append)) {
            expect(false, "ANTS-1285-INV-2 append");
            return;
        }
        f.write(kLine);
    }
    bool seen = false;
    for (int i = 0; i < 20 && !seen; ++i) {
        spy.wait(100);
        for (const auto &args : spy)
            if (args.value(0).toString() == path) seen = true;
    }
    expect(seen, "ANTS-1285-INV-2: an append emits changed(path) on the hub");
}

void testInv3_rewatchAfterReplace() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1285-INV-3 setup"); return; }
    const QString path = writeTranscript(dir, QStringLiteral("t.jsonl"), kLine);
    ClaudeBgTaskTracker tracker;
    tracker.setTranscriptPath(path);
    ClaudeTranscriptWatch &hub = ClaudeTranscriptWatch::instance();

    // Atomic replace: write a sibling, rename it over the transcript.
    const QString tmp = writeTranscript(dir, QStringLiteral("t.jsonl.tmp"),
                                        QByteArray(kLine) + kLine);
    QFile::remove(path);
    expect(QFile::rename(tmp, path), "ANTS-1285-INV-3 rename");
    // Let the watcher see the inode go away.
    QSignalSpy spy(&hub, &ClaudeTranscriptWatch::changed);
    spy.wait(500);

    tracker.poll();
    expect(hub.isWatching(path),
           "ANTS-1285-INV-3: poll() re-watches a replaced transcript");
}

} // namespace

TEST(TranscriptWatchShared, Inv1OneWatchCounted) {
    const int before = expect_failures();
    testInv1_oneWatchCounted();
    if (expect_failures() > before) FAIL();
}

TEST(TranscriptWatchShared, Inv2AppendReachesHub) {
    const int before = expect_failures();
    testInv2_appendReachesHub();
    if (expect_failures() > before) FAIL();
}

TEST(TranscriptWatchShared, Inv3RewatchAfterReplace) {
    const int before = expect_failures();
    testInv3_rewatchAfterReplace();
    if (expect_failures() > before) FAIL();
}
