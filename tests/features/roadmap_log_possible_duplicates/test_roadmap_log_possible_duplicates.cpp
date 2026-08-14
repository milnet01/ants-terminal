// ANTS-2043 — feature-conformance test for roadmap_log's non-blocking
// near-duplicate content advisory (op:append + op:append_batch).
// Behavioural against the *ForTest entry points over a temp project,
// mirroring the mcp_roadmap_log_append_batch harness.

#include <gtest/gtest.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include "remotecontrol.h"

namespace {

// A roadmap with two pre-existing bullets whose headlines we'll probe
// against. The format sniffer reads these as ants-v1 emoji bullets.
QString roadmapWith() {
    // fromUtf8 (not QStringLiteral): the \xNN below are UTF-8 bytes for
    // the 📋 status emoji; QStringLiteral would build a u"…" literal and
    // mangle each byte into a wrong code unit, so the bullets wouldn't
    // parse as ants-v1.
    return QString::fromUtf8(
        "# Test Roadmap\n"
        "\n"
        "## Performance\n"
        "\n"
        "- \xF0\x9F\x93\x8B [ANTS-9001] **The widget cache grows without "
        "bound during long sessions.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n"
        "- \xF0\x9F\x93\x8B [ANTS-9002] **Totally unrelated audio "
        "resampler latency.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n");
}

// ANTS-2063 — return bool (not void) so a failed open fails the TEST at
// the call site; a void helper's ASSERT_TRUE only aborts the helper and
// leaves the TEST running on a half-built fixture.
bool writeRoadmap(const QString &dir, const QString &content) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(content.toUtf8());
    f.close();
    return true;
}
bool writeCounter(const QString &dir, qint64 value) {
    QFile f(dir + QStringLiteral("/.roadmap-counter"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write((QString::number(value) + QChar('\n')).toUtf8());
    f.close();
    return true;
}
qint64 readCounter(const QString &dir) {
    QFile f(dir + QStringLiteral("/.roadmap-counter"));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    return QString::fromUtf8(f.readAll().trimmed()).toLongLong();
}
QString readRoadmap(const QString &dir) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QJsonObject appendReq(const QString &dir, const QString &headline) {
    QJsonObject r;
    r["caller_cwd"] = dir;
    r["op"]         = QStringLiteral("append");
    r["section"]    = QStringLiteral("performance");
    r["status"]     = QStringLiteral("planned");
    r["headline"]   = headline;
    r["kind"]       = QStringLiteral("implement");
    r["source"]     = QStringLiteral("test");
    return r;
}

bool setup(QTemporaryDir &dir) {
    return dir.isValid()
        && writeRoadmap(dir.path(), roadmapWith())
        && writeCounter(dir.path(), 9100);
}

}  // namespace

// INV-1 — exact normalised match scores 100 and still appends.
TEST(roadmap_log_possible_duplicates, Inv1ExactMatchScore100) {
    QTemporaryDir dir; ASSERT_TRUE(setup(dir));
    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(
        appendReq(dir.path(),
            QStringLiteral("The widget cache grows without bound "
                           "during long sessions."))).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    ASSERT_TRUE(out.contains("possible_duplicates"))
        << "INV-1: an exact normalised match must surface the advisory";
    const QJsonArray dups = out["possible_duplicates"].toArray();
    ASSERT_GE(dups.size(), 1);
    EXPECT_EQ(dups[0].toObject()["id"].toString(),
              QStringLiteral("ANTS-9001"));
    EXPECT_EQ(dups[0].toObject()["score"].toInt(), 100);
}

// INV-2 — a near match (Jaccard ≥ 0.6) is surfaced with a sub-100 score.
TEST(roadmap_log_possible_duplicates, Inv2NearMatchSurfaced) {
    QTemporaryDir dir; ASSERT_TRUE(setup(dir));
    RemoteControl rc(nullptr);
    // Differs only by "in"/"during" + drops trailing punctuation; shares
    // 8 of 10 union tokens → Jaccard 0.8.
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(
        appendReq(dir.path(),
            QStringLiteral("The widget cache grows without bound in "
                           "long sessions."))).object();
    ASSERT_TRUE(out["ok"].toBool());
    ASSERT_TRUE(out.contains("possible_duplicates"))
        << "INV-2: a near match must surface the advisory";
    const QJsonArray dups = out["possible_duplicates"].toArray();
    ASSERT_GE(dups.size(), 1);
    const int score = dups[0].toObject()["score"].toInt();
    EXPECT_EQ(dups[0].toObject()["id"].toString(),
              QStringLiteral("ANTS-9001"));
    EXPECT_GE(score, 60);
    EXPECT_LT(score, 100);
}

// INV-3 — a clean headline omits the field entirely.
TEST(roadmap_log_possible_duplicates, Inv3CleanHeadlineNoField) {
    QTemporaryDir dir; ASSERT_TRUE(setup(dir));
    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(
        appendReq(dir.path(),
            QStringLiteral("Brand new keyboard shortcut palette "
                           "overlay."))).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_FALSE(out.contains("possible_duplicates"))
        << "INV-3: a clean headline must omit possible_duplicates "
           "(absent, not an empty array)";
}

// INV-4 — the advisory never blocks: the bullet is written + counter
// advances even on an exact-duplicate headline.
TEST(roadmap_log_possible_duplicates, Inv4AdvisoryNeverBlocks) {
    QTemporaryDir dir; ASSERT_TRUE(setup(dir));
    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(
        appendReq(dir.path(),
            QStringLiteral("The widget cache grows without bound "
                           "during long sessions."))).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["id"].toString(), QStringLiteral("ANTS-9101"));
    EXPECT_EQ(readCounter(dir.path()), 9101)
        << "INV-4: counter must advance — the advisory is not a refusal";
    EXPECT_TRUE(readRoadmap(dir.path()).contains(QStringLiteral("ANTS-9101")))
        << "INV-4: the bullet must be written to ROADMAP.md";
}

// INV-5 — append_batch attaches the advisory per accepted bullet.
TEST(roadmap_log_possible_duplicates, Inv5BatchPerBulletAdvisory) {
    QTemporaryDir dir; ASSERT_TRUE(setup(dir));
    RemoteControl rc(nullptr);
    auto bullet = [](const QString &hl) {
        QJsonObject b;
        b["headline"] = hl;
        b["status"]   = QStringLiteral("planned");
        b["kind"]     = QStringLiteral("implement");
        b["source"]   = QStringLiteral("test");
        return b;
    };
    QJsonArray bs;
    bs.append(bullet(QStringLiteral("The widget cache grows without "
                                    "bound during long sessions.")));  // dup of 9001
    bs.append(bullet(QStringLiteral("A genuinely novel telemetry "
                                    "exporter.")));                    // clean
    QJsonObject req;
    req["caller_cwd"] = dir.path();
    req["op"]         = QStringLiteral("append_batch");
    req["section"]    = QStringLiteral("performance");
    req["bullets"]    = bs;
    const QJsonObject out =
        rc.cmdRoadmapLogAppendBatchForTest(req).object();

    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out["applied_count"].toInt(), 2)
        << "INV-5: both bullets still apply — advisory is non-blocking";
    ASSERT_TRUE(out.contains("possible_duplicates"));
    const QJsonArray dups = out["possible_duplicates"].toArray();
    ASSERT_EQ(dups.size(), 1)
        << "INV-5: only the duplicate bullet carries an advisory entry";
    EXPECT_EQ(dups[0].toObject()["bullet_index"].toInt(), 0);
    const QJsonArray cands = dups[0].toObject()["candidates"].toArray();
    ASSERT_GE(cands.size(), 1);
    EXPECT_EQ(cands[0].toObject()["id"].toString(),
              QStringLiteral("ANTS-9001"));
    EXPECT_EQ(cands[0].toObject()["score"].toInt(), 100);
}

// ANTS-4377 — `note` is op:"flip"/"annotate"'s parameter and op:"append"
// takes `body`. Both are top-level optional strings on one verb, differing
// only by which op consumes them, and the unused one was DISCARDED IN
// SILENCE: a caller who sent 2.3 KB of body prose as `note` got ok:true with
// a plausible bytes_written — the headline, Kind and Source lines are real
// bytes — and a bullet with no body at all. They caught it only because the
// count looked small against the prose they sent.
TEST(roadmap_log_possible_duplicates, Ants4377NoteOnAppendRefusesInsteadOfDropping) {
    QTemporaryDir dir; ASSERT_TRUE(setup(dir));
    RemoteControl rc(nullptr);

    QJsonObject req = appendReq(dir.path(),
        QStringLiteral("A bullet whose body was sent under the wrong key."));
    req["note"] = QStringLiteral("Prose the caller believed would land in the "
                                 "body: the measurement, the reasoning and "
                                 "the blockers.");
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();

    EXPECT_FALSE(out["ok"].toBool()) << "silently dropping it is the defect";
    EXPECT_EQ(out["code"].toString(), QStringLiteral("bad_op_combo"));
    EXPECT_TRUE(out["error"].toString().contains(QStringLiteral("body")))
        << "the refusal must name the parameter the op actually takes";

    // Control — the same call with `body` still works, so this narrows one
    // wrong key rather than breaking the op.
    QJsonObject good = appendReq(dir.path(),
        QStringLiteral("A bullet whose body was sent under the right key."));
    good["body"] = QStringLiteral("Body prose.");
    EXPECT_TRUE(rc.cmdRoadmapLogAppendForTest(good).object()["ok"].toBool());
}
