// Feature-conformance test for ANTS-1283 SessionMemoryEngine.
// See tests/features/session_memory_engine/spec.md.

#include <gtest/gtest.h>

#include "sessionmemoryengine.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QTemporaryDir>

namespace SME = SessionMemoryEngine;

namespace {

struct Workspace {
    QTemporaryDir td;
    QString root() const { return td.path(); }
    bool valid() const { return td.isValid(); }
};

}  // namespace

// ENG-1
TEST(SessionMemoryEngine, CwdHashIsDeterministicAndHexSixteen) {
    const QString h1 = SME::cwdHash("/home/me/projects/foo");
    const QString h2 = SME::cwdHash("/home/me/projects/foo");
    const QString h3 = SME::cwdHash("/home/me/projects/bar");
    EXPECT_EQ(h1, h2) << "same cwd must yield same hash";
    EXPECT_NE(h1, h3) << "different cwd must yield different hash";
    EXPECT_EQ(h1.size(), 16) << "hash must be 16 hex chars";
    for (QChar c : h1) {
        const bool isHex =
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(isHex) << "non-hex char in cwdHash output";
    }
}

// ENG-2
TEST(SessionMemoryEngine, ValidKeyAcceptsCanonicalAlphabet) {
    EXPECT_TRUE(SME::isValidKey("a"));
    EXPECT_TRUE(SME::isValidKey("z"));
    EXPECT_TRUE(SME::isValidKey("A"));
    EXPECT_TRUE(SME::isValidKey("Z"));
    EXPECT_TRUE(SME::isValidKey("0"));
    EXPECT_TRUE(SME::isValidKey("9"));
    EXPECT_TRUE(SME::isValidKey("audit.last_ts"));
    EXPECT_TRUE(SME::isValidKey("tool-detect_v2"));
    EXPECT_TRUE(SME::isValidKey(QString(64, QChar('a'))))
        << "64-char key is at the boundary and must be accepted";
}

// ENG-3
TEST(SessionMemoryEngine, ValidKeyRejectsOutOfBand) {
    EXPECT_FALSE(SME::isValidKey(""))             << "empty key rejected";
    EXPECT_FALSE(SME::isValidKey("../etc/passwd"))<< "path traversal rejected";
    EXPECT_FALSE(SME::isValidKey("with space"))   << "whitespace rejected";
    EXPECT_FALSE(SME::isValidKey("with/slash"))   << "/ rejected";
    EXPECT_FALSE(SME::isValidKey("ctl\x01"))      << "control char rejected";
    EXPECT_FALSE(SME::isValidKey(QString(65, QChar('a'))))
        << "65-char key one over the cap rejected";
}

// ENG-4
TEST(SessionMemoryEngine, GetOnEmptyStoreReturnsNotFound) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    const auto r = SME::execute("/some/cwd", SME::Op::Get,
                                 "missing_key", QJsonValue(),
                                 ws.root());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.code, QString()) << "no error code on missing-key get";
    EXPECT_FALSE(r.found);
}

// ENG-5
TEST(SessionMemoryEngine, SetThenGetRoundTrip) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());

    QJsonObject payload;
    payload["last_ts"] = 1715789000;
    payload["tool"]    = QStringLiteral("cppcheck");

    const auto rSet = SME::execute("/some/cwd", SME::Op::Set,
                                    "audit", QJsonValue(payload),
                                    ws.root());
    ASSERT_TRUE(rSet.ok) << "set failed: " << rSet.error.toStdString();
    EXPECT_GT(rSet.bytesWritten, 0);

    const auto rGet = SME::execute("/some/cwd", SME::Op::Get,
                                    "audit", QJsonValue(),
                                    ws.root());
    ASSERT_TRUE(rGet.ok);
    EXPECT_TRUE(rGet.found);
    ASSERT_TRUE(rGet.value.isObject());
    EXPECT_EQ(rGet.value.toObject().value("last_ts").toInt(), 1715789000);
    EXPECT_EQ(rGet.value.toObject().value("tool").toString(),
              QStringLiteral("cppcheck"));
}

// ENG-6
TEST(SessionMemoryEngine, DeleteRemovesKey) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    SME::execute("/cwd", SME::Op::Set, "k", QJsonValue(QStringLiteral("v")), ws.root());

    const auto rDel = SME::execute("/cwd", SME::Op::Delete,
                                    "k", QJsonValue(), ws.root());
    ASSERT_TRUE(rDel.ok);
    EXPECT_TRUE(rDel.found) << "delete should report found=true for existing key";

    const auto rGet = SME::execute("/cwd", SME::Op::Get,
                                    "k", QJsonValue(), ws.root());
    ASSERT_TRUE(rGet.ok);
    EXPECT_FALSE(rGet.found) << "key should be absent after delete";

    const auto rDel2 = SME::execute("/cwd", SME::Op::Delete,
                                     "k", QJsonValue(), ws.root());
    ASSERT_TRUE(rDel2.ok);
    EXPECT_FALSE(rDel2.found) << "second delete reports found=false (INV-6)";
}

// ENG-7
TEST(SessionMemoryEngine, ListReturnsKeysOnly) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    SME::execute("/cwd", SME::Op::Set, "k1",
                 QJsonValue(QStringLiteral("hello world v1")), ws.root());
    SME::execute("/cwd", SME::Op::Set, "k2", QJsonValue(42),    ws.root());

    const auto rList = SME::execute("/cwd", SME::Op::List,
                                     QString(), QJsonValue(), ws.root());
    ASSERT_TRUE(rList.ok);
    EXPECT_EQ(rList.keys.size(), 2);

    // INV-5: each entry is {key, bytes}; never `value`.
    for (const auto &v : rList.keys) {
        ASSERT_TRUE(v.isObject());
        const auto o = v.toObject();
        EXPECT_TRUE(o.contains("key"));
        EXPECT_TRUE(o.contains("bytes"));
        EXPECT_FALSE(o.contains("value"))
            << "INV-5: list must not inline values";
    }
}

// ENG-8
TEST(SessionMemoryEngine, TotalStoreCapEnforced) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());

    // Fill the store close to the cap with several mid-sized values
    // (each ~12 KiB, well under the per-value cap of 16 KiB). 9
    // entries × ~12 KiB ≈ 108 KiB → first nine fit, last one trips.
    const QString blob = QString(12 * 1024, QChar('x'));
    int accepted = 0;
    bool sawCapExceeded = false;
    for (int i = 0; i < 10; ++i) {
        const auto r = SME::execute("/cwd", SME::Op::Set,
                                     QStringLiteral("k%1").arg(i),
                                     QJsonValue(blob), ws.root());
        if (r.ok) {
            ++accepted;
        } else {
            EXPECT_EQ(r.code, QStringLiteral("cap_exceeded"))
                << "INV-2: failure on cap overflow must be cap_exceeded; "
                << "got " << r.code.toStdString();
            sawCapExceeded = true;
            break;
        }
    }
    EXPECT_TRUE(sawCapExceeded)
        << "INV-2: 10 × 12 KiB values should overflow the 100 KiB cap";
    EXPECT_GE(accepted, 1);
    EXPECT_LT(accepted, 10);
}

// ENG-9
TEST(SessionMemoryEngine, PerValueCapEnforced) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    // 16 KiB + 1 char of payload — value cap is 16 KiB.
    const QString tooBig = QString(SME::kMaxValueBytes + 1, QChar('y'));

    const auto r = SME::execute("/cwd", SME::Op::Set,
                                 "huge", QJsonValue(tooBig), ws.root());
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("bad_value"))
        << "INV-8: value > 16 KiB must yield bad_value";
}

// ENG-10
TEST(SessionMemoryEngine, CorruptStoreTreatedAsEmpty) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());

    // Locate where the store will live for cwd "/cwd" under the
    // override base dir, then pre-fill with corrupt content.
    const QString path = SME::storePathFor("/cwd", ws.root());
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("{not valid json");
        f.close();
    }

    bool corrupt = false;
    const QJsonObject loaded = SME::loadStore(path, &corrupt);
    EXPECT_TRUE(corrupt) << "INV-11: corrupt flag should fire";
    EXPECT_TRUE(loaded.isEmpty()) << "corrupt store loads as empty object";

    // Subsequent set must succeed and overwrite cleanly.
    const auto r = SME::execute("/cwd", SME::Op::Set,
                                 "recovered",
                                 QJsonValue(QStringLiteral("ok")),
                                 ws.root());
    ASSERT_TRUE(r.ok);

    const auto rGet = SME::execute("/cwd", SME::Op::Get,
                                    "recovered", QJsonValue(), ws.root());
    EXPECT_TRUE(rGet.found);
    EXPECT_EQ(rGet.value.toString(), QStringLiteral("ok"));
}
