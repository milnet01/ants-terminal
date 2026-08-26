// Feature-conformance test for ANTS-3397 — greenfield .roadmap-counter
// auto-init for the counter strategy. See
// tests/features/roadmap_log_greenfield_counter/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include "remotecontrol.h"

#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

namespace {

// Greenfield: a hand-authored roadmap with parseable bullets but NO ids
// of any kind, and (the test omits) NO .roadmap-counter.
const char *kGreenfieldRoadmap =
    "# My Project Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "- \xF0\x9F\x93\x8B **Set up the build.**\n"
    "- \xF0\x9F\x93\x8B **Write the README.**\n";

// A roadmap that already carries counter-style ids but (in the test) no
// .roadmap-counter — the fresh-clone state now that the counter is an
// untracked cache. ANTS-3450: the high-water mark is RECOVERED from these
// committed ids rather than refused.
const char *kIdBearingRoadmap =
    "# My Project Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "- \xF0\x9F\x93\x8B [ANTS-9001] **Pre-existing bullet.** Kind: chore.\n";

// ANTS-4691 — "to-do" here has a `###` child, so an append targeting it
// REFUSES with section_has_subsections. The refusal fires after the counter
// block, which is what made a dry run leave an artefact behind.
const char *kSubsectionedRoadmap =
    "# My Project Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "### Sub A\n"
    "\n"
    "- \xF0\x9F\x93\x8B **Set up the build.**\n";

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}
QString counterPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral(".roadmap-counter"));
}

qint64 readCounter(const QString &root) {
    QFile f(counterPath(root));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    return QString::fromUtf8(f.readAll().trimmed()).toLongLong();
}

// prefix empty → omit id_prefix so the verb sniffs the roadmap's own prefix
// (used by the ANTS-3450 recovery cases to get a natural <sniffed>-NNNN id).
QJsonObject appendReq(const QString &root,
                      const QString &prefix = QStringLiteral("CL")) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("section")]    = QStringLiteral("to-do");
    o[QStringLiteral("status")]     = QStringLiteral("planned");
    o[QStringLiteral("headline")]   = QStringLiteral("First real item.");
    o[QStringLiteral("kind")]       = QStringLiteral("chore");
    o[QStringLiteral("source")]     = QStringLiteral("ants-3397-test");
    if (!prefix.isEmpty())
        o[QStringLiteral("id_prefix")] = prefix;
    return o;
}

QJsonObject batchReq(const QString &root,
                     const QString &prefix = QStringLiteral("CL")) {
    QJsonObject b1;
    b1[QStringLiteral("headline")] = QStringLiteral("Batch item one.");
    b1[QStringLiteral("status")]   = QStringLiteral("planned");
    b1[QStringLiteral("kind")]     = QStringLiteral("chore");
    b1[QStringLiteral("source")]   = QStringLiteral("ants-3397-test");
    QJsonObject b2;
    b2[QStringLiteral("headline")] = QStringLiteral("Batch item two.");
    b2[QStringLiteral("status")]   = QStringLiteral("planned");
    b2[QStringLiteral("kind")]     = QStringLiteral("chore");
    b2[QStringLiteral("source")]   = QStringLiteral("ants-3397-test");
    QJsonArray bullets;
    bullets.append(b1);
    bullets.append(b2);

    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("op")]         = QStringLiteral("append_batch");
    o[QStringLiteral("section")]    = QStringLiteral("to-do");
    o[QStringLiteral("bullets")]    = bullets;
    if (!prefix.isEmpty())
        o[QStringLiteral("id_prefix")] = prefix;
    return o;
}

}  // namespace

// INV-1 — greenfield single append auto-creates the counter at 0 and
// allocates the first id.
TEST(RoadmapLogGreenfieldCounter, Inv1SingleAppendGreenfieldAutoInit) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()),
                          QByteArray(kGreenfieldRoadmap)));
    ASSERT_FALSE(QFile::exists(counterPath(tmp.path())));

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAppendForTest(appendReq(tmp.path())).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(),
              QStringLiteral("CL-0001"));
    EXPECT_TRUE(QFile::exists(counterPath(tmp.path())))
        << ".roadmap-counter must be auto-created";
    EXPECT_EQ(readCounter(tmp.path()), 1)
        << "counter must advance to the first allocated id";
}

// INV-2 — greenfield batch append auto-inits and allocates contiguously.
TEST(RoadmapLogGreenfieldCounter, Inv2BatchGreenfieldAutoInit) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()),
                          QByteArray(kGreenfieldRoadmap)));
    ASSERT_FALSE(QFile::exists(counterPath(tmp.path())));

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAppendBatchForTest(batchReq(tmp.path())).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(QFile::exists(counterPath(tmp.path())));
    EXPECT_EQ(readCounter(tmp.path()), 2)
        << "two bullets must consume ids 1 and 2";
}

// INV-3 (ANTS-3450) — a roadmap with counter-style ids but no counter file
// is the normal fresh-clone state now that the counter is an untracked
// cache. The high-water mark is RECOVERED from the committed corpus and the
// append proceeds above it, never reissuing a live id.
TEST(RoadmapLogGreenfieldCounter, Inv3ExistingIdsNoCounterRecovers) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()),
                          QByteArray(kIdBearingRoadmap)));
    ASSERT_FALSE(QFile::exists(counterPath(tmp.path())));

    RemoteControl rc(nullptr);
    // No id_prefix → the verb sniffs "ANTS" from [ANTS-9001] and allocates
    // the next id above the recovered high-water mark (9001 → 9002).
    const QJsonObject single =
        rc.cmdRoadmapLogAppendForTest(
            appendReq(tmp.path(), QString())).object();
    ASSERT_TRUE(single.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(single).toJson().toStdString();
    EXPECT_EQ(single.value(QStringLiteral("id")).toString(),
              QStringLiteral("ANTS-9002"))
        << "must allocate above the recovered high-water, not reissue 9001";
    EXPECT_TRUE(QFile::exists(counterPath(tmp.path())))
        << "recovery seeds the counter from the committed corpus";
    EXPECT_EQ(readCounter(tmp.path()), 9002);
}

// INV-3b — batch path recovers identically and allocates contiguously above
// the recovered high-water mark.
TEST(RoadmapLogGreenfieldCounter, Inv3bExistingIdsNoCounterRecoversBatch) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()),
                          QByteArray(kIdBearingRoadmap)));
    ASSERT_FALSE(QFile::exists(counterPath(tmp.path())));

    RemoteControl rc(nullptr);
    const QJsonObject batch =
        rc.cmdRoadmapLogAppendBatchForTest(
            batchReq(tmp.path(), QString())).object();
    ASSERT_TRUE(batch.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(batch).toJson().toStdString();
    EXPECT_TRUE(QFile::exists(counterPath(tmp.path())));
    EXPECT_EQ(readCounter(tmp.path()), 9003)
        << "two bullets consume 9002 and 9003 above the recovered mark";
}

// INV-4 — the greenfield discriminator helper is present in source.
TEST(RoadmapLogGreenfieldCounter, Inv4HelperPresent) {
    const std::string src = ants_test::slurpRemoteControl();
    EXPECT_NE(src.find("rlRoadmapHasAnyBulletId"), std::string::npos)
        << "the greenfield-vs-desync discriminator must exist";
}

// INV-5 (ANTS-4691) — a SUCCEEDING dry run allocates nothing and writes no
// counter file, while still reporting the id the real append would hand out.
TEST(RoadmapLogGreenfieldCounter, Inv5DryRunLeavesNoCounter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()),
                          QByteArray(kGreenfieldRoadmap)));
    ASSERT_FALSE(QFile::exists(counterPath(tmp.path())));

    QJsonObject req = appendReq(tmp.path());
    req[QStringLiteral("dry_run")] = true;

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());
    // Parity with INV-1's real append: same first id, no allocation.
    EXPECT_EQ(resp.value(QStringLiteral("would_be_id")).toString(),
              QStringLiteral("CL-0001"));
    EXPECT_FALSE(QFile::exists(counterPath(tmp.path())))
        << "a dry run must not create .roadmap-counter";
}

// INV-6 (ANTS-4691) — the reported case: a REFUSED dry run must leave the
// tree exactly as it found it. The counter seed runs before the section
// gate, so without the guard this call wrote a file having written no
// bullet, and the caller either committed the artefact or reported a dirty
// tree it had caused itself.
TEST(RoadmapLogGreenfieldCounter, Inv6RefusedDryRunLeavesNoCounter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()),
                          QByteArray(kSubsectionedRoadmap)));
    ASSERT_FALSE(QFile::exists(counterPath(tmp.path())));

    QJsonObject req = appendReq(tmp.path());
    req[QStringLiteral("dry_run")] = true;

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(req).object();

    ASSERT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << "the fixture must actually refuse, or this asserts nothing: "
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("section_has_subsections"));
    EXPECT_FALSE(QFile::exists(counterPath(tmp.path())))
        << "a REFUSED dry run must not create .roadmap-counter";
}

// INV-7 (ANTS-4691) — the same guard on op:append_batch, which carries its
// own copy of the auto-init. One path fixed and the other left is how this
// defect would come back.
TEST(RoadmapLogGreenfieldCounter, Inv7BatchDryRunLeavesNoCounter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()),
                          QByteArray(kGreenfieldRoadmap)));
    ASSERT_FALSE(QFile::exists(counterPath(tmp.path())));

    QJsonObject req = batchReq(tmp.path());
    req[QStringLiteral("dry_run")] = true;

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAppendBatchForTest(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_FALSE(QFile::exists(counterPath(tmp.path())))
        << "a batch dry run must not create .roadmap-counter";
}
