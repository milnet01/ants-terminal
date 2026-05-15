// Feature-conformance test for ANTS-1364 serialize-once refactor.
// See tests/features/session_memory_serialize_once/spec.md.

#include <gtest/gtest.h>

#include "sessionmemoryengine.h"

#include <QFile>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

namespace SME = SessionMemoryEngine;

namespace {

struct Workspace {
    QTemporaryDir td;
    QString root() const { return td.path(); }
    bool valid() const { return td.isValid(); }
};

QByteArray slurpStore(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

}  // namespace

// INV-2 after Set — totalBytes matches the bytes on disk.
TEST(SessionMemorySerializeOnce, TotalBytesMatchesDiskAfterSet) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    const QString cwd = QStringLiteral("/proj/test");

    // Seed: one key already present.
    auto seed = SME::execute(cwd, SME::Op::Set,
        QStringLiteral("seed_key"),
        QJsonValue(QStringLiteral("seed_value")),
        ws.root());
    ASSERT_TRUE(seed.ok) << "seed Set failed: " << seed.error.toStdString();

    // Set a new key.
    auto r = SME::execute(cwd, SME::Op::Set,
        QStringLiteral("new_key"),
        QJsonValue(QStringLiteral("new_value")),
        ws.root());
    ASSERT_TRUE(r.ok) << "Set failed: " << r.error.toStdString();

    const QByteArray onDisk = slurpStore(r.path);
    ASSERT_FALSE(onDisk.isEmpty()) << "on-disk store empty";
    EXPECT_EQ(r.totalBytes, onDisk.size())
        << "totalBytes (" << r.totalBytes
        << ") must equal on-disk size (" << onDisk.size() << ")";
}

// INV-2 after Delete — totalBytes matches the bytes on disk.
TEST(SessionMemorySerializeOnce, TotalBytesMatchesDiskAfterDelete) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    const QString cwd = QStringLiteral("/proj/test");

    // Seed two keys so the post-delete store isn't empty.
    auto s1 = SME::execute(cwd, SME::Op::Set,
        QStringLiteral("k1"), QJsonValue(QStringLiteral("v1")),
        ws.root());
    ASSERT_TRUE(s1.ok);
    auto s2 = SME::execute(cwd, SME::Op::Set,
        QStringLiteral("k2"), QJsonValue(QStringLiteral("v2")),
        ws.root());
    ASSERT_TRUE(s2.ok);

    // Delete one.
    auto r = SME::execute(cwd, SME::Op::Delete,
        QStringLiteral("k1"), QJsonValue(), ws.root());
    ASSERT_TRUE(r.ok) << "Delete failed: " << r.error.toStdString();
    ASSERT_TRUE(r.found) << "k1 was seeded and should have been found";

    const QByteArray onDisk = slurpStore(r.path);
    ASSERT_FALSE(onDisk.isEmpty()) << "on-disk store empty post-delete";
    EXPECT_EQ(r.totalBytes, onDisk.size())
        << "totalBytes (" << r.totalBytes
        << ") must equal on-disk size (" << onDisk.size() << ")";
}

// Delete-of-absent-key still reports totalBytes equal to the
// (unchanged) on-disk store. Guards the had=false arm of the
// refactored Delete branch.
TEST(SessionMemorySerializeOnce, DeleteAbsentKeyTotalBytesUnchanged) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    const QString cwd = QStringLiteral("/proj/test");

    auto seed = SME::execute(cwd, SME::Op::Set,
        QStringLiteral("present"), QJsonValue(QStringLiteral("v")),
        ws.root());
    ASSERT_TRUE(seed.ok);

    auto r = SME::execute(cwd, SME::Op::Delete,
        QStringLiteral("never_set"), QJsonValue(), ws.root());
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.found) << "never-set key should report found=false";

    const QByteArray onDisk = slurpStore(r.path);
    EXPECT_EQ(r.totalBytes, onDisk.size())
        << "had=false arm must still report totalBytes equal to disk";
}
