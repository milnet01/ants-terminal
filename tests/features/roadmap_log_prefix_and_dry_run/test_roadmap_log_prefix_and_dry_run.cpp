// ANTS-2076 / ANTS-2077 — feature-conformance test for roadmap_log's
// counter-prefix fallback (project-dir / explicit id_prefix) and the
// dry_run preview on op:append + op:append_batch. Behavioural against the
// *ForTest entry points over a temp project, mirroring the
// roadmap_log_possible_duplicates harness. Spec:
// tests/features/roadmap_log_prefix_and_dry_run/spec.md.

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include "remotecontrol.h"

namespace {

// A fresh roadmap: one id-LESS ants-v1 bullet so parseBullets returns a
// non-empty set (dodging the unrecognised_format gate) while leaving the
// counter-prefix sniffer with nothing to find → the leaf-dir fallback
// fires. fromUtf8 (not QStringLiteral) for the 📋 UTF-8 bytes.
QString freshRoadmap() {
    return QString::fromUtf8(
        "# Fresh Roadmap\n"
        "\n"
        "## Backlog\n"
        "\n"
        "- \xF0\x9F\x93\x8B An idea with no allocated id yet.\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n");
}

// A roadmap whose existing IDs would sniff to "ANTS" — used to prove
// id_prefix overrides the sniff.
QString antsRoadmap() {
    return QString::fromUtf8(
        "# Test Roadmap\n"
        "\n"
        "## Backlog\n"
        "\n"
        "- \xF0\x9F\x93\x8B [ANTS-9001] **An existing allocated bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n");
}

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

// Build a project dir with a deterministic leaf name so the leaf-derived
// prefix is stable ("DOOM_Fixture" → "DOOM"). Returns the leaf path, or
// empty on failure.
QString makeProject(QTemporaryDir &base, const QString &roadmap,
                    qint64 counter) {
    if (!base.isValid()) return {};
    const QString leaf =
        base.path() + QStringLiteral("/DOOM_Fixture");
    if (!QDir().mkpath(leaf)) return {};
    if (!writeRoadmap(leaf, roadmap)) return {};
    if (!writeCounter(leaf, counter)) return {};
    return leaf;
}

QJsonObject appendReq(const QString &dir, const QString &headline) {
    QJsonObject r;
    r["caller_cwd"] = dir;
    r["op"]         = QStringLiteral("append");
    r["section"]    = QStringLiteral("backlog");
    r["status"]     = QStringLiteral("planned");
    r["headline"]   = headline;
    r["kind"]       = QStringLiteral("implement");
    r["source"]     = QStringLiteral("test");
    return r;
}

QJsonObject batchBullet(const QString &hl) {
    QJsonObject b;
    b["headline"] = hl;
    b["status"]   = QStringLiteral("planned");
    b["kind"]     = QStringLiteral("implement");
    b["source"]   = QStringLiteral("test");
    return b;
}

}  // namespace

// INV-1 — fresh/id-less roadmap derives the prefix from the leaf dir.
TEST(roadmap_log_prefix, Inv1LeafDirFallback) {
    QTemporaryDir base;
    const QString dir = makeProject(base, freshRoadmap(), 0);
    ASSERT_FALSE(dir.isEmpty());
    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(
        appendReq(dir, QStringLiteral("First real item."))).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out["id"].toString(), QStringLiteral("DOOM-0001"))
        << "INV-1: a fresh roadmap must derive the prefix from the leaf "
           "directory, never the hardcoded ANTS";
    EXPECT_TRUE(readRoadmap(dir).contains(QStringLiteral("DOOM-0001")));
}

// INV-2 — explicit id_prefix overrides the sniffed prefix.
TEST(roadmap_log_prefix, Inv2ExplicitPrefixOverridesSniff) {
    QTemporaryDir base;
    const QString dir = makeProject(base, antsRoadmap(), 9100);
    ASSERT_FALSE(dir.isEmpty());
    RemoteControl rc(nullptr);
    QJsonObject req = appendReq(dir, QStringLiteral("Pinned-prefix item."));
    req["id_prefix"] = QStringLiteral("ZOOM");
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out["id"].toString(), QStringLiteral("ZOOM-9101"))
        << "INV-2: id_prefix must override the ANTS sniff";
}

// INV-3 — malformed id_prefix refuses with bad_args, writes nothing.
TEST(roadmap_log_prefix, Inv3BadPrefixRefuses) {
    QTemporaryDir base;
    const QString dir = makeProject(base, freshRoadmap(), 0);
    ASSERT_FALSE(dir.isEmpty());
    RemoteControl rc(nullptr);
    QJsonObject req = appendReq(dir, QStringLiteral("Should not land."));
    req["id_prefix"] = QStringLiteral("1bad");  // leading digit
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();
    EXPECT_FALSE(out["ok"].toBool());
    EXPECT_EQ(out["code"].toString(), QStringLiteral("bad_args"));
    EXPECT_EQ(readCounter(dir), 0)
        << "INV-3: a refused call must not bump the counter";
}

// INV-4 — dry_run on op:append previews without writing.
TEST(roadmap_log_prefix, Inv4AppendDryRunNoWrite) {
    QTemporaryDir base;
    const QString dir = makeProject(base, freshRoadmap(), 0);
    ASSERT_FALSE(dir.isEmpty());
    const QString before = readRoadmap(dir);
    RemoteControl rc(nullptr);
    QJsonObject req = appendReq(dir, QStringLiteral("Preview only."));
    req["dry_run"] = true;
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_TRUE(out["dry_run"].toBool());
    EXPECT_EQ(out["id"].toString(), QStringLiteral("DOOM-0001"))
        << "INV-4: preview reports the would-be id";
    EXPECT_TRUE(out.contains("bullet"));
    EXPECT_EQ(readRoadmap(dir), before)
        << "INV-4: ROADMAP.md must be untouched by a dry_run";
    EXPECT_EQ(readCounter(dir), 0)
        << "INV-4: .roadmap-counter must be untouched by a dry_run";
}

// INV-5 — dry_run on op:append_batch previews ids without writing.
TEST(roadmap_log_prefix, Inv5BatchDryRunNoWrite) {
    QTemporaryDir base;
    const QString dir = makeProject(base, freshRoadmap(), 0);
    ASSERT_FALSE(dir.isEmpty());
    const QString before = readRoadmap(dir);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(batchBullet(QStringLiteral("First batch item.")));
    bs.append(batchBullet(QStringLiteral("Second batch item.")));
    QJsonObject req;
    req["caller_cwd"] = dir;
    req["op"]         = QStringLiteral("append_batch");
    req["section"]    = QStringLiteral("backlog");
    req["bullets"]    = bs;
    req["dry_run"]    = true;
    const QJsonObject out =
        rc.cmdRoadmapLogAppendBatchForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_TRUE(out["dry_run"].toBool());
    EXPECT_EQ(out["applied_count"].toInt(), 0);
    EXPECT_EQ(out["would_apply_count"].toInt(), 2);
    const QJsonArray ids = out["ids"].toArray();
    ASSERT_EQ(ids.size(), 2);
    EXPECT_EQ(ids[0].toString(), QStringLiteral("DOOM-0001"));
    EXPECT_EQ(ids[1].toString(), QStringLiteral("DOOM-0002"));
    EXPECT_EQ(readRoadmap(dir), before)
        << "INV-5: batch dry_run must not write ROADMAP.md";
    EXPECT_EQ(readCounter(dir), 0)
        << "INV-5: batch dry_run must not bump the counter";
}

// INV-6 — append_batch on a fresh roadmap derives the leaf prefix for
// every bullet and actually writes them.
TEST(roadmap_log_prefix, Inv6BatchLeafPrefix) {
    QTemporaryDir base;
    const QString dir = makeProject(base, freshRoadmap(), 0);
    ASSERT_FALSE(dir.isEmpty());
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(batchBullet(QStringLiteral("First batch item.")));
    bs.append(batchBullet(QStringLiteral("Second batch item.")));
    QJsonObject req;
    req["caller_cwd"] = dir;
    req["op"]         = QStringLiteral("append_batch");
    req["section"]    = QStringLiteral("backlog");
    req["bullets"]    = bs;
    const QJsonObject out =
        rc.cmdRoadmapLogAppendBatchForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    const QJsonArray ids = out["ids"].toArray();
    ASSERT_EQ(ids.size(), 2);
    EXPECT_EQ(ids[0].toString(), QStringLiteral("DOOM-0001"));
    EXPECT_EQ(ids[1].toString(), QStringLiteral("DOOM-0002"));
    EXPECT_EQ(readCounter(dir), 2)
        << "INV-6: a real batch must bump the counter to the last id";
}
