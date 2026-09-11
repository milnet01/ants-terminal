// Feature-conformance test for ANTS-1163 — Task List dialog
// surfaces stale tasks from the previous Claude Code session
// after a fresh launch. Extended for ANTS-5048 — sessionPathForCwd
// opens and JSON-parses every transcript in the project dir on
// every call instead of stopping once mtime order rules a
// candidate out.
//
// INV labels qualified ANTS-1163-INV-N / ANTS-5048-INV-N (same
// running sequence, see spec.md § Invariants). See spec.md.
//
// Test shape:
//  * INV-1 / INV-9 / INV-10: live-PID probe + source-grep.
//  * INV-2..8 / INV-11: build a tmp HOME with two JSONL fixtures
//    of controlled mtimes + tail content, call
//    ClaudeIntegration::sessionPathForCwd directly.
//  * INV-17..22 (ANTS-5048): per-case tmp HOME with a forged
//    content timestamp that lies about being newer than its own
//    mtime — the only way to observe, from the return value
//    alone, whether a candidate was ever opened.

#include "../../_support/expect.h"
#include "claudeintegration.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

ANTS_TEST_SCOPE();

namespace {



[[noreturn]] void die(const std::string &msg) {
    std::fprintf(stderr, "setup-fail: %s\n", msg.c_str());
    std::exit(2);
}

#define DIE_IF_FALSE(expr, label) \
    do { if (!(expr)) die(label); } while (0)


bool writeWithMtime(const QString &path,
                    const QByteArray &content,
                    qint64 epochSec) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(content);
    f.close();
    struct utimbuf t{};
    t.actime = static_cast<time_t>(epochSec);
    t.modtime = static_cast<time_t>(epochSec);
    return ::utime(path.toLocal8Bit().constData(), &t) == 0;
}

QString encodedDir(const QString &projectsRoot, const QString &cwd) {
    return projectsRoot + "/" + ClaudeIntegration::encodeProjectPath(cwd);
}

QByteArray timestampedAssistantEvent(const QString &iso) {
    return QStringLiteral(
        R"({"type":"assistant","timestamp":"%1","message":{"role":"assistant","content":[{"type":"text","text":"x"}]}})")
        .arg(iso).toUtf8();
}

QByteArray metadataEvent() {
    return QByteArray(R"({"type":"last-prompt","sessionId":"abc"})");
}

QByteArray fixtureBody(const QList<QByteArray> &lines) {
    QByteArray out;
    for (const auto &l : lines) { out += l; out += '\n'; }
    return out;
}

QString isoUtc(qint64 epochMs) {
    return QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::utc())
               .toString(Qt::ISODateWithMs);
}

void testProcessStartTimeMs() {
    // INV-1: helper returns >0 for a live PID (this process), 0 for
    // dead / nonsense PIDs.
    const qint64 selfMs = ClaudeIntegration::processStartTimeMs(::getpid());
    const qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
    expect(selfMs > 0,
           "ANTS-1163-INV-1: processStartTimeMs(self) > 0",
           "got " + std::to_string(selfMs));
    // Liberal window: this test process started within the last hour
    // and no later than now (+1 s slack for clock jitter).
    expect(selfMs <= nowMs + 1000 && selfMs >= nowMs - 60LL*60*1000,
           "ANTS-1163-INV-1: processStartTimeMs(self) is plausible (within last hour)",
           "selfMs=" + std::to_string(selfMs) +
                " nowMs=" + std::to_string(nowMs));
    expect(ClaudeIntegration::processStartTimeMs(0) == 0,
           "ANTS-1163-INV-1: processStartTimeMs(0) == 0");
    expect(ClaudeIntegration::processStartTimeMs(0x7FFFFFFE) == 0,
           "ANTS-1163-INV-1: processStartTimeMs(huge_pid) == 0");
}

void testLastEventTimestampMs(QTemporaryDir &dir) {
    const QString p1 = dir.path() + "/has_ts.jsonl";
    {
        QByteArray body = fixtureBody({
            timestampedAssistantEvent(QStringLiteral("2026-01-01T00:00:00.000Z")),
            timestampedAssistantEvent(QStringLiteral("2026-05-07T19:36:43.393Z")),
        });
        DIE_IF_FALSE(writeWithMtime(p1, body, 1714000000),
                     "write " + p1.toStdString());
    }
    const qint64 expected = QDateTime::fromString(
        QStringLiteral("2026-05-07T19:36:43.393Z"), Qt::ISODateWithMs)
            .toMSecsSinceEpoch();
    const qint64 ts = ClaudeIntegration::lastEventTimestampMs(p1);
    expect(ts == expected,
           "ANTS-1163-INV-2: lastEventTimestampMs returns latest ISO 8601 ts",
           "got " + std::to_string(ts) +
                " expected " + std::to_string(expected));

    // INV-3: walks past trailing metadata events without `timestamp`.
    const QString p2 = dir.path() + "/has_ts_then_meta.jsonl";
    {
        QByteArray body = fixtureBody({
            timestampedAssistantEvent(QStringLiteral("2026-05-07T19:36:43.393Z")),
            metadataEvent(),
            metadataEvent(),
        });
        DIE_IF_FALSE(writeWithMtime(p2, body, 1714000000),
                     "write " + p2.toStdString());
    }
    expect(ClaudeIntegration::lastEventTimestampMs(p2) == expected,
           "ANTS-1163-INV-3: lastEventTimestampMs skips trailing metadata events");

    // INV-2 (negative): metadata-only file returns 0.
    const QString p3 = dir.path() + "/no_ts.jsonl";
    {
        QByteArray body = fixtureBody({metadataEvent(), metadataEvent()});
        DIE_IF_FALSE(writeWithMtime(p3, body, 1714000000),
                     "write " + p3.toStdString());
    }
    expect(ClaudeIntegration::lastEventTimestampMs(p3) == 0,
           "ANTS-1163-INV-2 (neg): metadata-only file returns 0");
}

void testFilterAndPick(QTemporaryDir &home) {
    // Restore HOME on scope exit; otherwise when the caller's
    // QTemporaryDir is destroyed, $HOME continues pointing at a deleted
    // directory for the rest of the test_core bundle's lifetime.
    const bool hadHome = qEnvironmentVariableIsSet("HOME");
    const QByteArray priorHome = hadHome ? qgetenv("HOME") : QByteArray();
    auto restoreHome = qScopeGuard([hadHome, priorHome]() {
        if (hadHome) qputenv("HOME", priorHome);
        else qunsetenv("HOME");
    });
    ::setenv("HOME", home.path().toLocal8Bit().constData(), 1);

    const QString cwd = home.path() + "/projA";
    QDir().mkpath(cwd);

    const QString projectsRoot = home.path() + "/.claude/projects";
    const QString projDir = encodedDir(projectsRoot, cwd);
    DIE_IF_FALSE(QDir().mkpath(projDir), projDir.toStdString());

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 nowSec = nowMs / 1000;
    const qint64 claudeStartSec = nowSec - 30;
    const qint64 claudeStartMs  = claudeStartSec * 1000;

    const QString oldPath = projDir + "/old-session.jsonl";
    const QString newPath = projDir + "/new-session.jsonl";

    // OLD: last event 30 minutes ago. mtime same.
    {
        const qint64 oldTsMs = nowMs - 30LL*60*1000;
        QByteArray body = fixtureBody({
            timestampedAssistantEvent(isoUtc(oldTsMs)),
        });
        DIE_IF_FALSE(writeWithMtime(oldPath, body, oldTsMs / 1000),
                     "write " + oldPath.toStdString());
    }
    // NEW: last event 5 s ago.
    {
        const qint64 newTsMs = nowMs - 5LL*1000;
        QByteArray body = fixtureBody({
            timestampedAssistantEvent(isoUtc(newTsMs)),
        });
        DIE_IF_FALSE(writeWithMtime(newPath, body, newTsMs / 1000),
                     "write " + newPath.toStdString());
    }

    // INV-4: with claudeStartMs as boundary, OLD is dropped, NEW wins.
    {
        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/claudeStartMs, /*nowMs=*/0);
        expect(picked == newPath,
               "ANTS-1163-INV-4: process-anchored filter drops pre-process JSONL",
               "picked=" + picked.toStdString());
    }

    // INV-7: with a far-future boundary, no candidate survives.
    {
        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/nowMs + 1000LL*60*60, /*nowMs=*/0);
        expect(picked.isEmpty(),
               "ANTS-1163-INV-7: empty result when no candidate survives boundary");
    }

    // INV-8: legacy form (no boundary) still returns a transcript.
    // Pre-fix this returned newest by mtime; post-fix it returns
    // newest by effective last-event ts. With our fixtures,
    // new-session.jsonl wins in both regimes.
    {
        const QString picked = ClaudeIntegration::sessionPathForCwd(cwd);
        expect(picked == newPath,
               "ANTS-1163-INV-8: legacy call (no boundary) returns newest");
    }

    // INV-5: 24h liveness floor — a 48h-old transcript is dropped
    // even with no process boundary.
    {
        const qint64 ancientTsMs = nowMs - 48LL*60*60*1000;
        QByteArray body = fixtureBody({
            timestampedAssistantEvent(isoUtc(ancientTsMs)),
        });
        DIE_IF_FALSE(writeWithMtime(oldPath, body, ancientTsMs / 1000),
                     "rewrite " + oldPath.toStdString());
        QFile::remove(newPath);

        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/0, /*nowMs=*/nowMs);
        expect(picked.isEmpty(),
               "ANTS-1163-INV-5: 24h liveness floor drops 48h-old transcript");
    }

    // INV-11 (ANTS-2191): a metadata-only file (no content timestamp) is
    // REJECTED when a live PID anchor exists (minLastEventMs > 0), rather than
    // falling back to the same-UID-spoofable file mtime. Pre-fix (ANTS-1163)
    // this adopted the mtime — a tampered transcript's mtime could pass the
    // freshness filter and bind the wrong session (narrowed re-open of the
    // ANTS-1163 wrong-session bind).
    {
        const qint64 freshSec = nowSec - 5;
        QByteArray body = fixtureBody({metadataEvent(), metadataEvent()});
        DIE_IF_FALSE(writeWithMtime(oldPath, body, freshSec),
                     "rewrite " + oldPath.toStdString());
        QFile::remove(newPath);

        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/claudeStartMs, /*nowMs=*/0);
        expect(picked.isEmpty(),
               "ANTS-2191-INV-11: metadata-only file rejected (no mtime "
               "fallback) when a live PID anchor exists",
               "picked=" + picked.toStdString());
    }

    // INV-11b (ANTS-2191): with NO PID anchor (minLastEventMs == 0) the mtime
    // fallback is retained — it is the only freshness signal available, and
    // the liveness floor (b) still bounds staleness. A fresh metadata-only
    // file is adopted.
    {
        const qint64 freshSec = nowSec - 5;
        QByteArray body = fixtureBody({metadataEvent(), metadataEvent()});
        DIE_IF_FALSE(writeWithMtime(oldPath, body, freshSec),
                     "rewrite " + oldPath.toStdString());
        QFile::remove(newPath);

        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/0, /*nowMs=*/nowMs);
        expect(picked == oldPath,
               "ANTS-2191-INV-11b: metadata-only file still uses mtime "
               "fallback when no PID anchor is known",
               "picked=" + picked.toStdString());
    }

    // INV-12 (ANTS-1163 follow-up 2026-05-08): cold-start tight floor.
    // When minLastEventMs == 0 (no Claude PID detected yet), the
    // liveness floor must be tight — yesterday's transcript (within
    // 24h, the previous wide floor) must be DROPPED. The window where
    // m_claudePid is briefly 0 is 1-3 seconds; 5 minutes is generous.
    // User repro: relaunched Ants + Claude, opened Task List dialog,
    // saw 27 done tasks from yesterday's session because the wide
    // 24h floor let them through.
    {
        const qint64 yesterdayTsMs = nowMs - 12LL*60*60*1000;  // 12h ago
        QByteArray body = fixtureBody({
            timestampedAssistantEvent(isoUtc(yesterdayTsMs)),
        });
        DIE_IF_FALSE(writeWithMtime(oldPath, body, yesterdayTsMs / 1000),
                     "rewrite " + oldPath.toStdString());
        QFile::remove(newPath);

        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/0, /*nowMs=*/nowMs);
        expect(picked.isEmpty(),
               "ANTS-1163-INV-12: tight floor on cold start drops "
               "yesterday's 12h-old transcript when no Claude PID known");
    }

    // INV-13 (ANTS-1163 follow-up 2026-05-08): tight floor stays
    // tight only on cold start. With Claude PID known (minLastEventMs > 0),
    // the wide 24h floor still applies — a long-running Claude session
    // that's been idle for hours but has events from after Claude
    // started must NOT be dropped.
    {
        const qint64 hoursAgoTsMs = nowMs - 3LL*60*60*1000;   // 3h ago
        QByteArray body = fixtureBody({
            timestampedAssistantEvent(isoUtc(hoursAgoTsMs)),
        });
        DIE_IF_FALSE(writeWithMtime(oldPath, body, hoursAgoTsMs / 1000),
                     "rewrite " + oldPath.toStdString());
        // Claude started 4h ago; idle session with last event 3h ago
        // is OLDER than start anchor + leeway (good for filter (a))
        // but newer than the 24h floor (good for filter (b) wide).
        const qint64 claudeStart4hMs = nowMs - 4LL*60*60*1000;
        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/claudeStart4hMs, /*nowMs=*/nowMs);
        expect(picked == oldPath,
               "ANTS-1163-INV-13: idle long-running Claude session not "
               "dropped by liveness floor when PID is known",
               "picked=" + picked.toStdString());
    }
}

// ANTS-5048 — status-tick cost: sessionPathForCwd reads and JSON-parses
// EVERY *.jsonl in a project dir on every call, even though the loop
// already asks the filesystem for newest-mtime-first order and never
// uses it. The fix (not made here — locked against current code):
// break the mtime-sorted loop once a file's mtime (+ leeway) can no
// longer pass the floor, the identity filter, or beat the best
// effective-timestamp already found — because a transcript's content
// timestamp is never later than its own mtime (an event is written
// when appended), so a file that already fails on mtime alone cannot
// legitimately win once opened either.
//
// Each case below forges a transcript whose CONTENT timestamp lies
// about being newer than its own mtime — impossible for a real
// Claude Code transcript, but the only way to observe, from the
// return value alone, whether a file was ever opened at all. Current
// (unfixed) code opens every file regardless of mtime order, so the
// forged content timestamp wins and these assertions fail red.
void testMtimeShortCircuit(QTemporaryDir &home) {
    const bool hadHome = qEnvironmentVariableIsSet("HOME");
    const QByteArray priorHome = hadHome ? qgetenv("HOME") : QByteArray();
    auto restoreHome = qScopeGuard([hadHome, priorHome]() {
        if (hadHome) qputenv("HOME", priorHome);
        else qunsetenv("HOME");
    });
    ::setenv("HOME", home.path().toLocal8Bit().constData(), 1);

    const QString projectsRoot = home.path() + "/.claude/projects";
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 nowSec = nowMs / 1000;

    // Each sub-case gets its own cwd (hence its own encoded project
    // dir) so leftover fixtures from one case can never be scanned by
    // another — sessionPathForCwd reads every *.jsonl in the dir.
    auto mkCase = [&](const QString &name) {
        const QString cwd = home.path() + "/" + name;
        DIE_IF_FALSE(QDir().mkpath(cwd), cwd.toStdString());
        const QString projDir = encodedDir(projectsRoot, cwd);
        DIE_IF_FALSE(QDir().mkpath(projDir), projDir.toStdString());
        return QPair<QString, QString>(cwd, projDir);
    };

    // --- ANTS-5048-INV-17: no PID known (minLastEventMs == 0), tight
    // 5-minute floor. A day-old mtime fails that floor outright; a
    // transcript this stale must never be opened, whatever its
    // content claims.
    {
        const auto [cwd, projDir] = mkCase("inv1");
        const QString freshPath = projDir + "/fresh.jsonl";
        const QString stalePath = projDir + "/stale.jsonl";

        const qint64 freshTsMs = nowMs - 10'000;   // 10 s ago
        const qint64 staleMtimeSec = nowSec - 86'400;  // 1 day ago
        const qint64 staleForgedTsMs = nowMs - 2'000;  // forged: 2 s ago

        DIE_IF_FALSE(writeWithMtime(freshPath,
                          fixtureBody({timestampedAssistantEvent(isoUtc(freshTsMs))}),
                          nowSec - 10),
                     "write " + freshPath.toStdString());
        DIE_IF_FALSE(writeWithMtime(stalePath,
                          fixtureBody({timestampedAssistantEvent(isoUtc(staleForgedTsMs))}),
                          staleMtimeSec),
                     "write " + stalePath.toStdString());

        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/0, /*nowMs=*/nowMs);
        expect(picked == freshPath,
               "ANTS-5048-INV-17: a transcript whose mtime already fails "
               "the liveness floor is never opened, even with a "
               "forged-fresh content timestamp",
               "picked=" + picked.toStdString() +
                   " fresh(mtime=" + std::to_string(nowSec - 10) +
                   ",contentTsMs=" + std::to_string(freshTsMs) + ")=" +
                   freshPath.toStdString() +
                   " stale(mtime=" + std::to_string(staleMtimeSec) +
                   ",forgedContentTsMs=" + std::to_string(staleForgedTsMs) +
                   ")=" + stalePath.toStdString());
    }

    // --- ANTS-5048-INV-18: PID known (minLastEventMs = claudeStartMs).
    // The identity floor is claudeStartMs - kLeewayMs (5 s). A day-old
    // mtime fails that floor by a wide margin; must never be opened.
    {
        const auto [cwd, projDir] = mkCase("inv2");
        const QString freshPath = projDir + "/fresh.jsonl";
        const QString stalePath = projDir + "/stale.jsonl";

        const qint64 claudeStartMs = nowMs - 30'000;  // Claude started 30 s ago
        const qint64 freshTsMs = nowMs - 10'000;
        const qint64 staleMtimeSec = nowSec - 86'400;
        const qint64 staleForgedTsMs = nowMs - 2'000;

        DIE_IF_FALSE(writeWithMtime(freshPath,
                          fixtureBody({timestampedAssistantEvent(isoUtc(freshTsMs))}),
                          nowSec - 10),
                     "write " + freshPath.toStdString());
        DIE_IF_FALSE(writeWithMtime(stalePath,
                          fixtureBody({timestampedAssistantEvent(isoUtc(staleForgedTsMs))}),
                          staleMtimeSec),
                     "write " + stalePath.toStdString());

        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/claudeStartMs, /*nowMs=*/0);
        expect(picked == freshPath,
               "ANTS-5048-INV-18: a transcript whose mtime is older than "
               "minLastEventMs minus the leeway is never opened, even "
               "with a forged-fresh content timestamp",
               "picked=" + picked.toStdString() +
                   " claudeStartMs=" + std::to_string(claudeStartMs) +
                   " fresh(mtime=" + std::to_string(nowSec - 10) +
                   ",contentTsMs=" + std::to_string(freshTsMs) + ")=" +
                   freshPath.toStdString() +
                   " stale(mtime=" + std::to_string(staleMtimeSec) +
                   ",forgedContentTsMs=" + std::to_string(staleForgedTsMs) +
                   ")=" + stalePath.toStdString());
    }

    // --- ANTS-5048-INV-19: both candidates are within the floor, but
    // B's mtime (+ leeway) cannot beat A's already-found best — A was
    // opened first (newer mtime), so B must never be opened even
    // though its forged content claims a later event than A's.
    {
        const auto [cwd, projDir] = mkCase("inv3");
        const QString pathA = projDir + "/a.jsonl";
        const QString pathB = projDir + "/b.jsonl";

        const qint64 tMs = nowMs - 10'000;       // A: honest content == mtime
        const qint64 aMtimeSec = nowSec - 10;
        const qint64 bMtimeSec = nowSec - 20;    // < (tMs - leeway) = nowMs-15000
        const qint64 bForgedTsMs = tMs + 2'000;  // forged: "newer than A"

        DIE_IF_FALSE(writeWithMtime(pathA,
                          fixtureBody({timestampedAssistantEvent(isoUtc(tMs))}),
                          aMtimeSec),
                     "write " + pathA.toStdString());
        DIE_IF_FALSE(writeWithMtime(pathB,
                          fixtureBody({timestampedAssistantEvent(isoUtc(bForgedTsMs))}),
                          bMtimeSec),
                     "write " + pathB.toStdString());

        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/0, /*nowMs=*/nowMs);
        expect(picked == pathA,
               "ANTS-5048-INV-19: a candidate whose mtime cannot beat the "
               "best effective timestamp already found is never opened, "
               "even with a forged-newer content timestamp",
               "picked=" + picked.toStdString() +
                   " A(mtime=" + std::to_string(aMtimeSec) +
                   ",contentTsMs=" + std::to_string(tMs) + ")=" +
                   pathA.toStdString() +
                   " B(mtime=" + std::to_string(bMtimeSec) +
                   ",forgedContentTsMs=" + std::to_string(bForgedTsMs) +
                   ")=" + pathB.toStdString());
    }

    // --- Guards (ANTS-5048-INV-20/21/22): must pass before AND after
    // the mtime-short-circuit fix — they exercise no forged fixture.

    // INV-20: freshest valid transcript still wins among several, all
    // honest (content ts == mtime).
    {
        const auto [cwd, projDir] = mkCase("inv4a");
        const QString p1 = projDir + "/p1.jsonl";
        const QString p2 = projDir + "/p2.jsonl";
        const QString p3 = projDir + "/p3.jsonl";  // freshest

        DIE_IF_FALSE(writeWithMtime(p1,
                          fixtureBody({timestampedAssistantEvent(isoUtc(nowMs - 30'000))}),
                          nowSec - 30),
                     "write " + p1.toStdString());
        DIE_IF_FALSE(writeWithMtime(p2,
                          fixtureBody({timestampedAssistantEvent(isoUtc(nowMs - 20'000))}),
                          nowSec - 20),
                     "write " + p2.toStdString());
        DIE_IF_FALSE(writeWithMtime(p3,
                          fixtureBody({timestampedAssistantEvent(isoUtc(nowMs - 5'000))}),
                          nowSec - 5),
                     "write " + p3.toStdString());

        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/0, /*nowMs=*/nowMs);
        expect(picked == p3,
               "ANTS-5048-INV-20 (guard): freshest valid transcript still "
               "returned among several",
               "picked=" + picked.toStdString() +
                   " p1(mtime=" + std::to_string(nowSec - 30) + ")=" +
                   p1.toStdString() +
                   " p2(mtime=" + std::to_string(nowSec - 20) + ")=" +
                   p2.toStdString() +
                   " p3(mtime=" + std::to_string(nowSec - 5) + ")=" +
                   p3.toStdString());
    }

    // INV-21: metadata-only file (no content timestamp) still falls back
    // to mtime when no PID anchor is known.
    {
        const auto [cwd, projDir] = mkCase("inv4b");
        const QString metaPath = projDir + "/meta.jsonl";
        const qint64 metaMtimeSec = nowSec - 5;

        DIE_IF_FALSE(writeWithMtime(metaPath,
                          fixtureBody({metadataEvent(), metadataEvent()}),
                          metaMtimeSec),
                     "write " + metaPath.toStdString());

        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/0, /*nowMs=*/nowMs);
        expect(picked == metaPath,
               "ANTS-5048-INV-21 (guard): metadata-only transcript still "
               "falls back to mtime when no PID anchor is known",
               "picked=" + picked.toStdString() +
                   " meta(mtime=" + std::to_string(metaMtimeSec) + ")=" +
                   metaPath.toStdString());
    }

    // INV-22: an all-stale directory still returns empty.
    {
        const auto [cwd, projDir] = mkCase("inv4c");
        const QString stalePath = projDir + "/ancient.jsonl";
        const qint64 ancientTsMs = nowMs - 2LL * 24 * 60 * 60 * 1000;  // 2 days
        const qint64 ancientMtimeSec = ancientTsMs / 1000;

        DIE_IF_FALSE(writeWithMtime(stalePath,
                          fixtureBody({timestampedAssistantEvent(isoUtc(ancientTsMs))}),
                          ancientMtimeSec),
                     "write " + stalePath.toStdString());

        const QString picked = ClaudeIntegration::sessionPathForCwd(
            cwd, /*minLastEventMs=*/0, /*nowMs=*/nowMs);
        expect(picked.isEmpty(),
               "ANTS-5048-INV-22 (guard): an all-stale directory still "
               "returns empty",
               "picked=" + picked.toStdString() +
                   " ancient(mtime=" + std::to_string(ancientMtimeSec) +
                   ",contentTsMs=" + std::to_string(ancientTsMs) + ")=" +
                   stalePath.toStdString());
    }
}

// ANTS-1192: encodeProjectPath must collapse BOTH `/` AND `_` into `-`,
// matching Claude Code's actual on-disk encoding under
// ~/.claude/projects/<encoded>/. Latent for any cwd containing
// underscores; surfaced 2026-05-08 by the project rename
// Ants-Terminal → Ants_Terminal.
void testEncodeProjectPath() {
    // INV-14: pure-slash path encodes to leading-dash + dashes.
    expect(ClaudeIntegration::encodeProjectPath("/mnt/Storage/Scripts/Linux/Ants-Terminal")
               == QStringLiteral("-mnt-Storage-Scripts-Linux-Ants-Terminal"),
           "ANTS-1192-INV-14: slash-only path encodes correctly",
           ClaudeIntegration::encodeProjectPath(
               "/mnt/Storage/Scripts/Linux/Ants-Terminal").toStdString());

    // INV-15: underscores in path components also collapse to `-`.
    // This is the bug fix — previously underscores were preserved,
    // mismatching Claude Code's directory.
    expect(ClaudeIntegration::encodeProjectPath("/mnt/Storage/Scripts/Linux/Ants_Terminal")
               == QStringLiteral("-mnt-Storage-Scripts-Linux-Ants-Terminal"),
           "ANTS-1192-INV-15: underscore in path component also folds to dash",
           ClaudeIntegration::encodeProjectPath(
               "/mnt/Storage/Scripts/Linux/Ants_Terminal").toStdString());

    // INV-16: mixed underscore/dash + multiple underscores collapse uniformly.
    expect(ClaudeIntegration::encodeProjectPath("/home/user/my_proj_v2/sub-dir")
               == QStringLiteral("-home-user-my-proj-v2-sub-dir"),
           "ANTS-1192-INV-16: multiple underscores all collapse to dash",
           ClaudeIntegration::encodeProjectPath(
               "/home/user/my_proj_v2/sub-dir").toStdString());
}

void testWiring() {
    const std::string cicpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // Locate the body of activeSessionPath.
    const std::string sigPrefix =
        "QString ClaudeIntegration::activeSessionPath(";
    const auto start = cicpp.find(sigPrefix);
    expect(start != std::string::npos,
           "ANTS-1163-INV-9/10 setup: activeSessionPath signature locatable");
    if (start == std::string::npos) return;

    // Function body runs from `start` to the next top-level free or
    // member function definition. The next definition starts at column
    // 0 — we look for `\n}\n` followed by another C++ definition. The
    // simplest reliable scan: grab a generous slice.
    const std::string body = cicpp.substr(start, 4096);

    expect(body.find("processStartTimeMs") != std::string::npos,
           "ANTS-1163-INV-9: activeSessionPath calls processStartTimeMs");
    expect(body.find("currentMSecsSinceEpoch") != std::string::npos,
           "ANTS-1163-INV-10: activeSessionPath threads currentMSecsSinceEpoch as nowMs");
}

}  // namespace

TEST(ClaudeSessionFreshness, ProcessStartTimeMs) {
    const int before = expect_failures();
    testProcessStartTimeMs();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeSessionFreshness, LastEventTimestampMs) {
    QTemporaryDir dir;
    if (!dir.isValid()) FAIL() << "cannot create QTemporaryDir";
    const int before = expect_failures();
    testLastEventTimestampMs(dir);
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeSessionFreshness, FilterAndPick) {
    QTemporaryDir home;
    if (!home.isValid()) FAIL() << "cannot create QTemporaryDir";
    const int before = expect_failures();
    testFilterAndPick(home);
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeSessionFreshness, MtimeShortCircuit) {
    QTemporaryDir home;
    if (!home.isValid()) FAIL() << "cannot create QTemporaryDir";
    const int before = expect_failures();
    testMtimeShortCircuit(home);
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeSessionFreshness, EncodeProjectPath) {
    const int before = expect_failures();
    testEncodeProjectPath();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeSessionFreshness, Wiring) {
    const int before = expect_failures();
    testWiring();
    if (expect_failures() > before) FAIL();
}

