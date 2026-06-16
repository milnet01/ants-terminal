// ANTS-2094 — feature-conformance test for proactive result offload
// (observation masking). Behavioural coverage of the mcp:: offload/spill
// API (pure Qt6::Core, in ants_core_lib) under an isolated test cache dir,
// plus source-scrapes for the dispatch-site + verb wiring invariants.
// See tests/features/mcp_result_offload/spec.md and docs/specs/ANTS-2094.md.

#include "mcpprojection.h"   // mcp::isOffloadEligible
#include "mcpspill.h"

#include <gtest/gtest.h>

#include <utime.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>

namespace {

QString spillDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
           + QStringLiteral("/ants-terminal/mcp-spill/");
}

QString readSource(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// A test fixture that isolates the spill cache under a throwaway test dir
// (QStandardPaths test mode) and starts each test from an empty dir.
class McpResultOffload : public ::testing::Test {
protected:
    void SetUp() override {
        QStandardPaths::setTestModeEnabled(true);
        QDir(spillDir()).removeRecursively();
        mcp::setOffloadConfig(true, 16384, 2048);   // enabled, defaults
    }
    void TearDown() override {
        QDir(spillDir()).removeRecursively();
    }
};

}  // namespace

// INV-1 — the offload-eligible set is the large-body read verbs, and is a
// deliberately separate set from isFieldProjectionTool; write/control-plane
// verbs are never eligible. offloadRequested resolves per-call over default.
TEST_F(McpResultOffload, Inv1EligibilityAndRequestResolution) {
    for (const char *t : {"get_scrollback", "get_text", "read_log",
                          "read_region", "workspace_search", "codebase_index",
                          "docs_index", "find_sources", "roadmap_query"})
        EXPECT_TRUE(mcp::isOffloadEligible(QString::fromUtf8(t))) << t;
    for (const char *t : {"apply_edits", "roadmap_log", "get_session_info",
                          "token_usage", "tool_info", "read_spill"})
        EXPECT_FALSE(mcp::isOffloadEligible(QString::fromUtf8(t))) << t;
    // get_scrollback is offload-eligible but NOT a field-projection tool —
    // the two predicates are independent.
    EXPECT_TRUE(mcp::isOffloadEligible(QStringLiteral("get_scrollback")));
    EXPECT_FALSE(mcp::isFieldProjectionTool(QStringLiteral("get_scrollback")));

    QJsonObject on; on["offload"] = true;
    QJsonObject off; off["offload"] = false;
    mcp::setOffloadConfig(false, 16384, 2048);          // default OFF
    EXPECT_FALSE(mcp::offloadRequested(QJsonObject{}));
    EXPECT_TRUE(mcp::offloadRequested(on));             // per-call wins
    mcp::setOffloadConfig(true, 16384, 2048);           // default ON
    EXPECT_TRUE(mcp::offloadRequested(QJsonObject{}));
    EXPECT_FALSE(mcp::offloadRequested(off));            // per-call wins
}

// INV-12 — config clamps: threshold to [4096,1048576], head to [256,16384].
TEST_F(McpResultOffload, Inv12ConfigClamps) {
    mcp::setOffloadConfig(true, 100, 100000);
    EXPECT_EQ(mcp::offloadThresholdBytes(), 4096);
    EXPECT_EQ(mcp::offloadHeadBytes(), 16384);
    mcp::setOffloadConfig(true, 5'000'000, 100);
    EXPECT_EQ(mcp::offloadThresholdBytes(), 1048576);
    EXPECT_EQ(mcp::offloadHeadBytes(), 256);
}

// INV-2/INV-3 — offloadBody returns the head+pointer envelope; the spill
// file holds the body verbatim and sha256(file) == handle.
TEST_F(McpResultOffload, Inv2And3EnvelopeAndSpillFile) {
    const QString body = QStringLiteral("{\"matches\":[") +
        QString(20000, QLatin1Char('x')) + QStringLiteral("]}");
    const QString env = mcp::offloadBody(QStringLiteral("workspace_search"), body);
    const QJsonObject o = QJsonDocument::fromJson(env.toUtf8()).object();

    EXPECT_TRUE(o.value("offloaded").toBool());
    const QString handle = o.value("handle").toString();
    EXPECT_EQ(handle.size(), 64);
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
                    .match(handle).hasMatch());
    EXPECT_EQ(o.value("bytes").toInt(), body.toUtf8().size());
    EXPECT_TRUE(o.value("head_truncated").toBool());
    EXPECT_TRUE(body.toUtf8().startsWith(o.value("head").toString().toUtf8()));
    EXPECT_TRUE(o.value("hint").toString().contains("read_spill"));

    // Spill file holds the body verbatim; sha256(file) == handle (INV-3).
    const QString path = spillDir() + handle + QStringLiteral(".json");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray fileBytes = f.readAll();
    f.close();
    EXPECT_EQ(fileBytes, body.toUtf8());
    EXPECT_EQ(QString::fromLatin1(QCryptographicHash::hash(
                  fileBytes, QCryptographicHash::Sha256).toHex()),
              handle);
}

// INV-4 — spill file is owner-only (0600): group/other bits clear.
TEST_F(McpResultOffload, Inv4OwnerOnlyPerms) {
    const QString body = QString(20000, QLatin1Char('y'));
    const QString env = mcp::offloadBody(QStringLiteral("get_scrollback"), body);
    const QString handle =
        QJsonDocument::fromJson(env.toUtf8()).object().value("handle").toString();
    const QFile::Permissions p =
        QFileInfo(spillDir() + handle + QStringLiteral(".json")).permissions();
    EXPECT_TRUE(p & QFileDevice::ReadOwner);
    EXPECT_TRUE(p & QFileDevice::WriteOwner);
    EXPECT_FALSE(p & QFileDevice::ReadGroup);
    EXPECT_FALSE(p & QFileDevice::ReadOther);
    EXPECT_FALSE(p & QFileDevice::WriteGroup);
    EXPECT_FALSE(p & QFileDevice::WriteOther);
}

// INV-12 — the head prefix is cut on a UTF-8 char boundary, never splitting
// a multi-byte sequence.
TEST_F(McpResultOffload, Inv12HeadCharBoundary) {
    mcp::setOffloadConfig(true, 4096, 256);   // head cut at byte 256
    // 254 ASCII then a 3-byte '€' occupying bytes 254..256 — the head cut at
    // 256 lands mid-sequence, so it must retreat to 254.
    QString body = QString(254, QLatin1Char('a'));
    for (int i = 0; i < 50; ++i) body += QString::fromUtf8("\xE2\x82\xAC");
    const QString env = mcp::offloadBody(QStringLiteral("read_log"), body);
    const QString head =
        QJsonDocument::fromJson(env.toUtf8()).object().value("head").toString();
    EXPECT_EQ(head.size(), 254);                       // retreated off the '€'
    EXPECT_FALSE(head.contains(QChar(0xFFFD)));         // no replacement char
    EXPECT_TRUE(head.endsWith(QLatin1Char('a')));
}

// INV-5/INV-6 — read_spill round-trips and byte-pages correctly, including
// the offset-past-end edge.
TEST_F(McpResultOffload, Inv5And6ReadSpillPaging) {
    const QString body = QString(50000, QLatin1Char('z'));
    const QString handle =
        QJsonDocument::fromJson(
            mcp::offloadBody(QStringLiteral("codebase_index"), body).toUtf8())
            .object().value("handle").toString();

    // Full read reproduces the body.
    const mcp::SpillSlice all = mcp::readSpill(handle, 0, 1 << 20);
    EXPECT_TRUE(all.ok);
    EXPECT_EQ(all.content, body);
    EXPECT_EQ(all.totalBytes, body.toUtf8().size());

    // Paging by the returned offset+bytes reassembles the body.
    QString reassembled;
    qint64 off = 0;
    for (int guard = 0; guard < 100; ++guard) {
        const mcp::SpillSlice s = mcp::readSpill(handle, off, 4096);
        ASSERT_TRUE(s.ok);
        reassembled += s.content;
        off += s.bytes;
        if (!s.truncated) break;
    }
    EXPECT_EQ(reassembled, body);

    // Offset past end → empty content, not truncated.
    const mcp::SpillSlice past = mcp::readSpill(handle, body.toUtf8().size() + 10, 100);
    EXPECT_TRUE(past.ok);
    EXPECT_TRUE(past.content.isEmpty());
    EXPECT_FALSE(past.truncated);

    // Unknown handle → not_found.
    const mcp::SpillSlice missing = mcp::readSpill(QString(64, QLatin1Char('a')), 0, 100);
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.code, QStringLiteral("not_found"));
}

// INV-8 — content-addressed: spilling the same body twice yields one file
// and a stable handle.
TEST_F(McpResultOffload, Inv8IdempotentReSpill) {
    const QString body = QString(20000, QLatin1Char('q'));
    const QString h1 = QJsonDocument::fromJson(
        mcp::offloadBody(QStringLiteral("get_text"), body).toUtf8())
        .object().value("handle").toString();
    const QString h2 = QJsonDocument::fromJson(
        mcp::offloadBody(QStringLiteral("get_text"), body).toUtf8())
        .object().value("handle").toString();
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(QDir(spillDir()).entryList(QStringList{QStringLiteral("*.json")},
                                         QDir::Files).size(), 1);
}

// INV-7 — eviction holds the file cap, never dropping the just-written
// handle; the 24 h sweep removes a backdated file.
TEST_F(McpResultOffload, Inv7EvictionAndSweep) {
    QString lastHandle;
    for (int i = 0; i < mcp::kSpillMaxFiles + 5; ++i) {
        const QString body =
            QStringLiteral("body-%1-").arg(i) + QString(400, QLatin1Char('w'));
        lastHandle = QJsonDocument::fromJson(
            mcp::offloadBody(QStringLiteral("find_sources"), body).toUtf8())
            .object().value("handle").toString();
    }
    const int count = QDir(spillDir()).entryList(
        QStringList{QStringLiteral("*.json")}, QDir::Files).size();
    EXPECT_LE(count, mcp::kSpillMaxFiles);
    EXPECT_TRUE(QFile::exists(spillDir() + lastHandle + QStringLiteral(".json")));

    // Sweep: backdate a fresh spill > 24 h and confirm spillSweep() drops it.
    QDir(spillDir()).removeRecursively();
    const QString h = QJsonDocument::fromJson(
        mcp::offloadBody(QStringLiteral("read_region"),
                         QString(20000, QLatin1Char('s'))).toUtf8())
        .object().value("handle").toString();
    const QString path = spillDir() + h + QStringLiteral(".json");
    ASSERT_TRUE(QFile::exists(path));
    struct utimbuf ut;
    ut.actime = ut.modtime = ::time(nullptr) - (25 * 60 * 60);
    ASSERT_EQ(::utime(path.toLocal8Bit().constData(), &ut), 0);
    mcp::spillSweep();
    EXPECT_FALSE(QFile::exists(path));
}

// INV-9 — the offload runs before the recordDispatch byte capture (so trace
// records the offloaded envelope's size). Source-scrape by symbol.
TEST_F(McpResultOffload, Inv9OffloadPrecedesRecordDispatch) {
    const QString src = readSource(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(src.isEmpty());
    const int offloadAt = src.indexOf(QStringLiteral("mcp::offloadBody("));
    ASSERT_GT(offloadAt, 0);
    // The dispatch-site recordDispatch call is the first one AFTER the offload
    // insertion (recordDispatch is also *defined* earlier in the file, so an
    // absolute first-occurrence search would find the definition, not the
    // call). Offload must precede that recording so the trace captures the
    // offloaded envelope's wire size.
    const int recordAfter = src.indexOf(QStringLiteral("recordDispatch("), offloadAt);
    ASSERT_GT(recordAfter, offloadAt);
    // The read_spill verb schema is registered in the tools/list.
    EXPECT_TRUE(src.contains(QStringLiteral("rsTool[\"name\"] = \"read_spill\"")));
}

// INV-10 — read_spill is wired like a read verb but with an Optional (not
// Required) caller_cwd contract, and the handler validates the handle regex.
TEST_F(McpResultOffload, Inv10ReadSpillWiring) {
    const QString mw = readSource(SRC_MAINWINDOW_CPP);
    ASSERT_FALSE(mw.isEmpty());
    const int reg = mw.indexOf(QStringLiteral("registerToolProvider(\"read_spill\""));
    ASSERT_GT(reg, 0);
    // The contract on that registration is Optional, not Required.
    const QString after = mw.mid(reg, 200);
    EXPECT_TRUE(after.contains(QStringLiteral("CallerCwdContract::Optional")));
    EXPECT_TRUE(after.contains(QStringLiteral("cmdReadSpill")));

    const QString rc = readSource(SRC_RC_CPP);
    ASSERT_FALSE(rc.isEmpty());
    EXPECT_TRUE(rc.contains(QStringLiteral("RemoteControl::cmdReadSpill")));
    EXPECT_TRUE(rc.contains(QStringLiteral("^[0-9a-f]{64}$")));
}

// INV-11 — fail-open: offloadBody returns the original body on a write
// failure (no error, no dangling pointer). Source-scrape the fail-open path.
TEST_F(McpResultOffload, Inv11FailOpenWiring) {
    const QString src = readSource(SRC_MCPSPILL_CPP_PATH);
    ASSERT_FALSE(src.isEmpty());
    // ensurePrivateDir / open / commit failures all `return body;`.
    EXPECT_TRUE(src.contains(QStringLiteral("if (!ensurePrivateDir(spillDir())) return body;")));
    EXPECT_TRUE(src.contains(QStringLiteral("if (!f.commit()) return body;")));
}
