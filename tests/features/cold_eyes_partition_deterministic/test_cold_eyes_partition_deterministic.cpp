// Feature-conformance test for tests/features/cold_eyes_partition_deterministic/spec.md.
//
// ANTS-1345 — derivePartition's spec-lane selection tiebreaks within
// a 1 s mtime bucket on path-lex ascending. The bug surface is the
// kMaxSpecLanes truncation cutoff: when more candidates exist than
// the cap allows, *which* candidates make the cut must not depend on
// filesystem enumeration order. The post-cut presentation order
// continues to be path-lex ascending (untouched by this fix).

#include "coldeyesengine.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <fcntl.h>
#include <sys/stat.h>

namespace {

// Linux-only: utimensat for ms-precision mtime control. QFile::setFileTime
// proved unreliable on the test sandbox (returns true, but the inode time
// isn't actually updated). Ants is Linux-only — no portability concern.
QString writeSpec(const QString &projectRoot, const QString &name,
                  qint64 mtimeMs) {
    const QString specsDir = projectRoot + QStringLiteral("/docs/specs");
    QDir().mkpath(specsDir);
    const QString fullPath = specsDir + QChar('/') + name;
    QFile f(fullPath);
    // ANTS-1611 — ADD_FAILURE + early return, not EXPECT_TRUE: this is a
    // setup helper returning QString, so gtest's ASSERT_* (which expands
    // to `return;`) won't compile here, and a bare EXPECT_TRUE would let
    // the helper keep writing to a closed QFile and hand back a path to
    // an empty/missing spec. ADD_FAILURE marks the calling TEST failed
    // with a clear cause; the empty return short-circuits the bad write.
    if (!f.open(QIODevice::WriteOnly)) {
        ADD_FAILURE() << "writeSpec: cannot open " << qUtf8Printable(fullPath);
        return {};
    }
    f.write(QByteArrayLiteral("# spec\n"));
    f.close();
    struct timespec ts[2];
    ts[0].tv_sec  = mtimeMs / 1000;
    ts[0].tv_nsec = (mtimeMs % 1000) * 1000000;
    ts[1] = ts[0];
    const int rc = utimensat(AT_FDCWD,
                             fullPath.toLocal8Bit().constData(),
                             ts, 0);
    EXPECT_EQ(rc, 0) << "utimensat failed for " << fullPath.toStdString();
    return fullPath;
}

QStringList specLaneNames(const ColdEyesEngine::PartitionResult &r) {
    QStringList out;
    for (const auto &l : r.lanes) {
        if (l.name.startsWith(QStringLiteral("spec/"))) out << l.name;
    }
    return out;
}

}  // namespace

// INV-1 — same mtime, post-truncation cut picks lex-smallest names.
// Create kMaxSpecLanes+3 specs at identical mtimes; the kept set is
// the lex-smallest kMaxSpecLanes.
TEST(ColdEyesPartitionDeterministic, Inv1SameMtimeLexCutoff) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    constexpr qint64 t0 = 1700000000000LL;  // synthetic
    const int over = ColdEyesEngine::kMaxSpecLanes + 3;
    for (int i = 0; i < over; ++i) {
        writeSpec(root,
                  QStringLiteral("ANTS-%1.md").arg(1000 + i, 4, 10,
                                                  QChar('0')),
                  t0);
    }

    const auto r = ColdEyesEngine::derivePartition(
        root, ColdEyesEngine::Scope::DocsOnly);
    const QStringList lanes = specLaneNames(r);
    ASSERT_EQ(lanes.size(), ColdEyesEngine::kMaxSpecLanes);

    // The kept set is the lex-smallest kMaxSpecLanes (ANTS-1000 …
    // ANTS-1011), with the 3 highest-numbered dropped.
    EXPECT_EQ(lanes.first(), QStringLiteral("spec/ANTS-1000"));
    EXPECT_EQ(lanes.last(),
              QStringLiteral("spec/ANTS-%1").arg(
                  1000 + ColdEyesEngine::kMaxSpecLanes - 1, 4, 10,
                  QChar('0')));
    // Sanity: the 3 highest-numbered specs are NOT in the cut.
    for (int i = ColdEyesEngine::kMaxSpecLanes; i < over; ++i) {
        const QString name = QStringLiteral("spec/ANTS-%1").arg(
            1000 + i, 4, 10, QChar('0'));
        EXPECT_FALSE(lanes.contains(name))
            << name.toStdString() << " should have been dropped";
    }
}

// INV-2 — newer bucket always survives the cutoff; older-bucket cut
// is lex-deterministic.
TEST(ColdEyesPartitionDeterministic, Inv2NewerBucketAlwaysKept) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    // Need > kMaxSpecLanes total. Use kMaxSpecLanes-1 in old bucket +
    // 3 in new bucket = kMaxSpecLanes+2. The 3 new MUST stay; the cut
    // drops 2 from old by lex-largest (deterministic tiebreak).
    constexpr qint64 t0 = 1700000000000LL;
    const int oldCount = ColdEyesEngine::kMaxSpecLanes - 1;
    for (int i = 0; i < oldCount; ++i) {
        writeSpec(root,
                  QStringLiteral("ANTS-%1.md").arg(1000 + i, 4, 10,
                                                  QChar('0')),
                  t0);
    }
    // New bucket: alphabetically LAST, but newer so they survive.
    writeSpec(root, QStringLiteral("ANTS-9991.md"), t0 + 10 * 1000);
    writeSpec(root, QStringLiteral("ANTS-9992.md"), t0 + 10 * 1000);
    writeSpec(root, QStringLiteral("ANTS-9993.md"), t0 + 10 * 1000);

    const auto r = ColdEyesEngine::derivePartition(
        root, ColdEyesEngine::Scope::DocsOnly);
    const QStringList lanes = specLaneNames(r);
    ASSERT_EQ(lanes.size(), ColdEyesEngine::kMaxSpecLanes);

    // All three newer specs survive.
    EXPECT_TRUE(lanes.contains(QStringLiteral("spec/ANTS-9991")));
    EXPECT_TRUE(lanes.contains(QStringLiteral("spec/ANTS-9992")));
    EXPECT_TRUE(lanes.contains(QStringLiteral("spec/ANTS-9993")));

    // The two HIGHEST-numbered older specs are dropped (lex-largest
    // tiebreak losers within the older bucket).
    const QString drop1 = QStringLiteral("spec/ANTS-%1").arg(
        1000 + oldCount - 1, 4, 10, QChar('0'));
    const QString drop2 = QStringLiteral("spec/ANTS-%1").arg(
        1000 + oldCount - 2, 4, 10, QChar('0'));
    EXPECT_FALSE(lanes.contains(drop1));
    EXPECT_FALSE(lanes.contains(drop2));
}

// INV-3 — idempotency: same fixture, two calls, identical lane order.
TEST(ColdEyesPartitionDeterministic, Inv3Idempotent) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    constexpr qint64 t0 = 1700000000000LL;
    // 14 specs, all in same bucket. Truncation forces deterministic
    // lex-tiebreak.
    for (int i = 0; i < 14; ++i) {
        writeSpec(root,
                  QStringLiteral("ANTS-%1.md").arg(1000 + i, 4, 10,
                                                  QChar('0')),
                  t0);
    }

    const auto r1 = ColdEyesEngine::derivePartition(
        root, ColdEyesEngine::Scope::DocsOnly);
    const auto r2 = ColdEyesEngine::derivePartition(
        root, ColdEyesEngine::Scope::DocsOnly);

    EXPECT_EQ(specLaneNames(r1), specLaneNames(r2));
}
