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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include "../../_support/srcgrep.h"  // ANTS-3833 — slurpRemoteControl

namespace {

QString readSource(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// A test fixture that isolates the spill cache under a unique per-test
// QTemporaryDir (ANTS-2154). The default cache root (GenericCacheLocation)
// has no per-process component, so under parallel ctest concurrent test
// binaries share one spill dir and the count/existence assertions race
// (pass in isolation, fail under `ctest -j3`). A throwaway dir per test
// removes the shared-path assumption entirely — and drops the prior
// approach's process-global QStandardPaths test-mode flip, which the
// ANTS-2151 note flagged as silently corrupting sibling tests' config
// isolation when left enabled.
class McpResultOffload : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(m_tmp.isValid());
        mcp::setSpillDirOverride(m_tmp.path());
        mcp::setOffloadConfig(true, 16384, 2048);   // enabled, defaults
    }
    void TearDown() override {
        mcp::setSpillDirOverride(QString());        // restore default root
    }
    // Matches mcp::spillPath()'s dir + handle + ".json" concatenation.
    QString spillDir() const { return m_tmp.path() + QStringLiteral("/"); }
    QTemporaryDir m_tmp;
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
    // The '€' run is long enough that the body clears the envelope-overhead
    // floor, so offload fires (INV-9 fail-open only declines a body that can't
    // be shrunk — Inv9BaseEnvelopeNeverExceedsBody). The head cut is unaffected: it always lands in the
    // first '€' and retreats to byte 254.
    QString body = QString(254, QLatin1Char('a'));
    for (int i = 0; i < 400; ++i) body += QString::fromUtf8("\xE2\x82\xAC");
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

// ───────────────────────────────────────────────────────────────────
// ANTS-3552 — edge assertions the ANTS-2094 spec listed as INV-1/INV-6
// follow-ups (behaviour was already correct but unexercised).
// ───────────────────────────────────────────────────────────────────

// INV-6 — read_spill max_bytes<=0 defaults to the 512 KiB page (NOT a
// zero-length read): a small body comes back whole and untruncated. The free
// function treats negative max_bytes identically; the bad_args refusal for a
// negative arg is imposed one layer up, at cmdReadSpill (scraped below).
TEST_F(McpResultOffload, Inv6ReadSpillMaxBytesDefaultsToPage) {
    const QString body = QString(50000, QLatin1Char('z'));  // well under 512 KiB
    const QString handle = QJsonDocument::fromJson(
        mcp::offloadBody(QStringLiteral("codebase_index"), body).toUtf8())
        .object().value("handle").toString();
    for (qint64 mb : {qint64(0), qint64(-1)}) {
        const mcp::SpillSlice s = mcp::readSpill(handle, 0, mb);
        ASSERT_TRUE(s.ok) << "max_bytes=" << mb;
        EXPECT_EQ(s.content, body)
            << "max_bytes=" << mb << " must default to the full page, not 0-length";
        EXPECT_EQ(s.bytes, body.toUtf8().size());
        EXPECT_FALSE(s.truncated);
    }
}

// INV-6 — the negative-arg → bad_args refusal lives at the dispatch
// (cmdReadSpill byte mode), which validates before calling readSpill.
TEST_F(McpResultOffload, Inv6ReadSpillNegativeArgGateAtDispatch) {
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    EXPECT_TRUE(rc.contains(
        QStringLiteral("offset < 0 || (mbV.isDouble() && maxBytes < 0)")))
        << "cmdReadSpill must reject a negative byte offset/max_bytes";
    EXPECT_TRUE(rc.contains(QStringLiteral(
        "read_spill: \\\"offset\\\"/\\\"max_bytes\\\" must be >= 0")))
        << "the negative-byte-arg refusal carries the bad_args message";
}

// INV-1 — the offload boundary, now behaviourally testable via the extracted
// mcp::shouldOffload predicate: fires AT the threshold (>=), and the > head
// guard binds when the head budget meets/exceeds the threshold (INV-12 lets
// the two clamp ranges overlap).
TEST_F(McpResultOffload, Inv1ShouldOffloadBoundary) {
    mcp::setOffloadConfig(true, 16384, 2048);   // threshold 16384, head 2048
    EXPECT_FALSE(mcp::shouldOffload(16383)) << "below threshold: no offload";
    EXPECT_TRUE(mcp::shouldOffload(16384))  << "at threshold: offload (>=, inclusive)";
    EXPECT_TRUE(mcp::shouldOffload(20000));
    // Head budget >= threshold: the > head guard is now the binding constraint.
    mcp::setOffloadConfig(true, 4096, 16384);   // threshold 4096, head 16384
    EXPECT_FALSE(mcp::shouldOffload(5000))
        << "over threshold but within head: no offload (envelope wouldn't save)";
    EXPECT_FALSE(mcp::shouldOffload(16384)) << "== head: no offload (> head is exclusive)";
    EXPECT_TRUE(mcp::shouldOffload(16385))  << "over both threshold and head: offload";
}

// INV-1 — offloadBody has NO threshold guard: handed a body BELOW the offload
// threshold (yet still large enough that the head+pointer envelope saves
// bytes), it spills anyway. The threshold is purely the dispatch's concern
// (shouldOffload) — which is why the dispatch must gate the call. (offloadBody
// does fail open when the envelope wouldn't shrink the body — the separate
// Inv9 case — so this body clears head + envelope overhead, not the threshold.)
TEST_F(McpResultOffload, Inv1OffloadBodyHasNoInternalThresholdGuard) {
    // Fixture config: threshold 16384, head 2048. A 5 KB body is below the
    // threshold but well above head + envelope overhead, so offloadBody spills.
    const QString body = QString(5000, QLatin1Char('h'));
    ASSERT_LT(body.toUtf8().size(), mcp::offloadThresholdBytes());
    const QJsonObject o = QJsonDocument::fromJson(
        mcp::offloadBody(QStringLiteral("get_text"), body).toUtf8()).object();
    EXPECT_TRUE(o.value("offloaded").toBool())
        << "offloadBody spills a below-threshold body; the size gate is the dispatch's";
    EXPECT_EQ(o.value("handle").toString().size(), 64);
}

// INV-1 — the dispatch gates offloadBody through mcp::shouldOffload (not an
// inline compare), keeping the boundary in one behaviourally-tested predicate.
TEST_F(McpResultOffload, Inv1DispatchUsesShouldOffload) {
    const QString ci = readSource(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    EXPECT_TRUE(ci.contains(QStringLiteral("mcp::shouldOffload(")))
        << "the dispatch must gate offload via the shouldOffload predicate";
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
        // Body sized above the envelope-overhead floor so offload fires (so the
        // returned envelope carries a handle); ~3 KB each keeps the file-COUNT
        // cap (not the 64 MiB byte cap) the binding eviction constraint.
        const QString body =
            QStringLiteral("body-%1-").arg(i) + QString(3000, QLatin1Char('w'));
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

    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    EXPECT_TRUE(rc.contains(QStringLiteral("RemoteControl::cmdReadSpill")));
    EXPECT_TRUE(rc.contains(QStringLiteral("^[0-9a-f]{64}$")));
}

// INV-11 — fail-open: offloadBody returns the original body on a write
// failure (no error, no dangling pointer). Source-scrape the fail-open path.
TEST_F(McpResultOffload, Inv11FailOpenWiring) {
    const QString src = readSource(SRC_MCPSPILL_CPP_PATH);
    ASSERT_FALSE(src.isEmpty());
    // All four fail-open exits — ensurePrivateDir / open / short-write /
    // commit — return the original body unchanged.
    EXPECT_TRUE(src.contains(QStringLiteral("if (!ensurePrivateDir(spillDir())) return body;")));
    EXPECT_TRUE(src.contains(QStringLiteral("if (!f.open(QIODevice::WriteOnly)) return body;")));
    EXPECT_TRUE(src.contains(QStringLiteral("f.cancelWriting(); return body;")));
    EXPECT_TRUE(src.contains(QStringLiteral("if (!f.commit()) return body;")));
}

// INV-11 — fail-open, behavioural: when the spill dir can't be created (its
// parent is a regular FILE, so ensurePrivateDir's mkpath can never succeed),
// offloadBody returns the original body verbatim — no offloaded:true envelope,
// no stray spill file left behind. The runtime complement to the source-scrape
// above. A file-as-parent forces the failure regardless of uid (no
// chmod-vs-root fragility a read-only-dir approach would carry).
TEST_F(McpResultOffload, Inv11FailOpenFaultInjection) {
    QTemporaryFile blocker;                        // a regular file...
    ASSERT_TRUE(blocker.open());
    const QString spill = blocker.fileName() + QStringLiteral("/spill");
    mcp::setSpillDirOverride(spill);               // ...as the spill dir's parent
    const QString body = QString(20000, QLatin1Char('f'));
    const QString env = mcp::offloadBody(QStringLiteral("workspace_search"), body);
    EXPECT_EQ(env, body) << "fail-open must return the body verbatim";
    EXPECT_FALSE(QJsonDocument::fromJson(env.toUtf8()).object()
                     .value("offloaded").toBool());
    EXPECT_FALSE(QFileInfo(spill).exists()) << "no spill file (nor temp) on fail-open";
    mcp::setSpillDirOverride(m_tmp.path());         // restore the fixture's writable dir
}

// ANTS-3538 — helper: parse an offload envelope string to its JSON object.
namespace {
QJsonObject offloadEnv(const QString &tool, const QString &body) {
    return QJsonDocument::fromJson(
        mcp::offloadBody(tool, body).toUtf8()).object();
}
QString compact(const QJsonObject &o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
}  // namespace

// INV-13 — structured preview happy path: a workspace_search-shaped body
// with N > kHeadRowsMax rows carries head_rows_key / row_count / head_rows /
// head_rows_truncated; head_rows holds the first K complete rows (K capped by
// kHeadRowsMax here), deep-equal to the body's array; the byte-prefix
// head / head_truncated are unchanged; and the envelope stays < bytes (INV-9).
TEST_F(McpResultOffload, Inv13StructuredPreviewHappyPath) {
    QJsonArray arr;
    for (int i = 0; i < 2000; ++i) { QJsonObject r; r["i"] = i; arr.append(r); }
    QJsonObject b; b["matches"] = arr; b["pattern"] = QStringLiteral("x");
    const QString body = compact(b);
    ASSERT_GT(body.toUtf8().size(), 16384);   // crosses the default threshold

    const QJsonObject o = offloadEnv(QStringLiteral("workspace_search"), body);
    EXPECT_TRUE(o.value("offloaded").toBool());
    EXPECT_TRUE(o.value("head_truncated").toBool());        // INV-2 unchanged
    EXPECT_TRUE(body.toUtf8().startsWith(o.value("head").toString().toUtf8()));
    EXPECT_EQ(o.value("head_rows_key").toString(), QStringLiteral("matches"));
    EXPECT_EQ(o.value("row_count").toInt(), 2000);
    const QJsonArray hr = o.value("head_rows").toArray();
    EXPECT_LE(hr.size(), mcp::kHeadRowsMax);
    EXPECT_GT(hr.size(), 0);
    EXPECT_TRUE(o.value("head_rows_truncated").toBool());    // 2000 > K
    for (int i = 0; i < hr.size(); ++i)                      // deep-equal
        EXPECT_EQ(hr.at(i), arr.at(i)) << i;
    // INV-9: the offloaded envelope is strictly smaller than the raw body.
    EXPECT_LT(compact(o).toUtf8().size(), body.toUtf8().size());
}

// INV-13 — budget-driven truncation: a few large-but-fitting rows so the byte
// budget (not kHeadRowsMax) forces the cut. head_rows.size() < row_count and
// < kHeadRowsMax, head_rows_truncated == true.
TEST_F(McpResultOffload, Inv13BudgetDrivenTruncation) {
    QJsonArray arr;
    for (int i = 0; i < 10; ++i) {
        QJsonObject r; r["s"] = QString(900, QLatin1Char('a')); arr.append(r);
    }
    QJsonObject b; b["matches"] = arr;
    b["pad"] = QString(10000, QLatin1Char('p'));   // push body over threshold
    const QString body = compact(b);
    ASSERT_GT(body.toUtf8().size(), 16384);

    const QJsonObject o = offloadEnv(QStringLiteral("find_sources"), body);
    const QJsonArray hr = o.value("head_rows").toArray();
    EXPECT_EQ(o.value("row_count").toInt(), 10);
    EXPECT_GT(hr.size(), 0);
    EXPECT_LT(hr.size(), 10);                       // cut before all 10
    EXPECT_LT(hr.size(), mcp::kHeadRowsMax);        // by budget, not the cap
    EXPECT_TRUE(o.value("head_rows_truncated").toBool());
    EXPECT_LT(compact(o).toUtf8().size(), body.toUtf8().size());
}

// INV-13 — tie-break: two equal-length root arrays resolve to the
// lexicographically-first key (QJsonObject iteration order).
TEST_F(McpResultOffload, Inv13TieBreakLexicographicKey) {
    QJsonObject b;
    b["zzz"] = QJsonArray{4, 5, 6};
    b["aaa"] = QJsonArray{1, 2, 3};
    b["pad"] = QString(20000, QLatin1Char('x'));
    const QJsonObject o = offloadEnv(QStringLiteral("roadmap_query"), compact(b));
    EXPECT_EQ(o.value("head_rows_key").toString(), QStringLiteral("aaa"));
    EXPECT_EQ(o.value("row_count").toInt(), 3);
}

// INV-13 — omission edges (all-four-or-none), each still carrying head /
// head_truncated: scalar-only, empty-array member, and an unparseable body.
TEST_F(McpResultOffload, Inv13OmissionScalarEmptyUnparseable) {
    auto assertNoPreview = [](const QJsonObject &o) {
        EXPECT_TRUE(o.value("head_truncated").toBool());     // head still there
        EXPECT_FALSE(o.contains("head_rows"));
        EXPECT_FALSE(o.contains("head_rows_key"));
        EXPECT_FALSE(o.contains("row_count"));
        EXPECT_FALSE(o.contains("head_rows_truncated"));
    };
    // Scalar-only (no array member).
    QJsonObject scalar; scalar["text"] = QString(20000, QLatin1Char('x'));
    assertNoPreview(offloadEnv(QStringLiteral("get_text"), compact(scalar)));
    // Empty array member (the "non-empty" detection filter).
    QJsonObject empty; empty["matches"] = QJsonArray();
    empty["pad"] = QString(20000, QLatin1Char('x'));
    assertNoPreview(offloadEnv(QStringLiteral("workspace_search"), compact(empty)));
    // Unparseable body (not valid JSON).
    assertNoPreview(offloadEnv(QStringLiteral("read_log"),
                               QString(20000, QLatin1Char('x'))));
}

// INV-13 — single oversized row: the body offloads (default head 2048 ≪ body)
// but the first array element exceeds the row budget, so all four structured
// fields are omitted while the byte-prefix head / head_truncated remain
// (best-effort preview, not a guarantee). The head≈threshold config where the
// whole offload fails open is covered separately by
// Inv9BaseEnvelopeNeverExceedsBody (ANTS-3540).
TEST_F(McpResultOffload, Inv13SingleOversizedRowOmitted) {
    // Default config from SetUp (threshold 16384, head 2048).
    QJsonObject b;
    b["matches"] = QJsonArray{QString(3000, QLatin1Char('a'))};  // one big row
    b["pad"] = QString(16000, QLatin1Char('p'));                 // over threshold
    const QString body = compact(b);
    ASSERT_GT(body.toUtf8().size(), 16384);
    const QJsonObject o = offloadEnv(QStringLiteral("codebase_index"), body);
    EXPECT_TRUE(o.value("offloaded").toBool());   // envelope (head 2048) ≪ body
    EXPECT_TRUE(o.value("head_truncated").toBool());
    EXPECT_FALSE(o.contains("head_rows"));         // 3000-byte row > 2048 budget
    EXPECT_FALSE(o.contains("head_rows_key"));
    EXPECT_LT(compact(o).toUtf8().size(), body.toUtf8().size());  // INV-9
}

// INV-13 — parse cap: a body over kStructuredParseMaxBytes skips the parse
// (no structured preview), bounding the transient parse footprint (§ 4).
TEST_F(McpResultOffload, Inv13ParseCapSkipped) {
    QJsonObject b;
    b["matches"] = QJsonArray{QString(1100000, QLatin1Char('a'))};  // > 1 MiB
    const QString body = compact(b);
    ASSERT_GT(body.toUtf8().size(), mcp::kStructuredParseMaxBytes);
    const QJsonObject o = offloadEnv(QStringLiteral("get_scrollback"), body);
    EXPECT_TRUE(o.value("offloaded").toBool());
    EXPECT_FALSE(o.contains("head_rows_key"));
}

// INV-13 — tabular (ANTS-2090) interaction: a body whose only root array was
// packed into {__cols__,__rows__} exposes no root array → byte-head only;
// but a leftover sibling scalar array is still legitimately previewed.
TEST_F(McpResultOffload, Inv13TabularSiblingAndPacked) {
    // Fully packed: the sole array member is now a {__cols__,__rows__} object.
    QJsonObject cols; cols["__cols__"] = QJsonArray{QStringLiteral("a")};
    cols["__rows__"] = QJsonArray{QJsonArray{1}};
    QJsonObject packed; packed["files"] = cols;
    packed["pad"] = QString(20000, QLatin1Char('x'));
    const QJsonObject po = offloadEnv(QStringLiteral("find_sources"), compact(packed));
    EXPECT_FALSE(po.contains("head_rows_key"));   // no root array → byte-head

    // Leftover sibling scalar array (find_sources' unmatched_terms) survives
    // tabular and is previewed.
    QJsonObject sib; sib["files"] = cols;
    sib["unmatched_terms"] = QJsonArray{QStringLiteral("p"), QStringLiteral("q"),
                                        QStringLiteral("r")};
    sib["pad"] = QString(20000, QLatin1Char('x'));
    const QJsonObject so = offloadEnv(QStringLiteral("find_sources"), compact(sib));
    EXPECT_EQ(so.value("head_rows_key").toString(),
              QStringLiteral("unmatched_terms"));
    EXPECT_EQ(so.value("row_count").toInt(), 3);
}

// ANTS-3540 / INV-9 — the base (pre-3538) head+pointer envelope must itself be
// a strict net saving. At a head≈threshold config the fixed envelope overhead
// (handle + hint + keys ≈ 330 B) plus a full head can exceed a body that only
// just clears the § 2.1 head guard. offloadBody now measures the finished
// envelope and fails open (returns the body unchanged, per INV-11) whenever it
// would not be strictly smaller — so an offloaded envelope is *always* < bytes,
// at every config. Scalar-only body so the 3538 structured path stays inert and
// only the base envelope is exercised. (The fail-open is a facet of INV-9's
// net-saving guarantee, not a new invariant — hence the Inv9 prefix.)
TEST_F(McpResultOffload, Inv9BaseEnvelopeNeverExceedsBody) {
    mcp::setOffloadConfig(true, 4096, 16384);   // head 16384 ≈ the body size
    QJsonObject b; b["text"] = QString(16400, QLatin1Char('x'));
    const QString body = compact(b);
    ASSERT_GT(body.toUtf8().size(), 16384);      // clears the head guard...
    const QString env = mcp::offloadBody(QStringLiteral("get_text"), body);
    // ...but head(16384) + ~330 B overhead > body, so offload can save nothing:
    // fail open, returning the untrimmed body verbatim (no larger envelope).
    EXPECT_EQ(env, body);
    const QJsonObject o = QJsonDocument::fromJson(env.toUtf8()).object();
    EXPECT_FALSE(o.value("offloaded").toBool());
    // The core INV-9 predicate holds unconditionally: output ≤ body bytes.
    EXPECT_LE(env.toUtf8().size(), body.toUtf8().size());
}

// INV-14 (ANTS-3545) — read_spill row-paging: mcp::readSpillRows pages the
// dominant array by row, sharing offloadBody's dominant-array detection so the
// paged `key` equals the offload preview's `head_rows_key`. Direct-call test
// (like INV-5/6 call readSpill); must-fail-first is the link-time absence of
// readSpillRows pre-feature. Covers continuity incl. a final partial page, the
// past-end + default-page + negative-arg + not_found edges.
TEST_F(McpResultOffload, Inv14ReadSpillRowPaging) {
    const int N = 47;                       // not a multiple of the page K
    QJsonArray arr;
    for (int i = 0; i < N; ++i) {
        QJsonObject r; r["i"] = i; r["v"] = QStringLiteral("row%1").arg(i);
        arr.append(r);
    }
    QJsonObject b; b["matches"] = arr; b["pattern"] = QStringLiteral("x");
    b["pad"] = QString(20000, QLatin1Char('p'));   // cross the offload threshold
    const QString body = compact(b);
    const QJsonObject env = offloadEnv(QStringLiteral("workspace_search"), body);
    const QString handle = env.value("handle").toString();
    ASSERT_FALSE(handle.isEmpty());

    // Page [0,K), [K,2K), … contiguous + deep-equal to the spilled rows at
    // absolute indices; the last page is the partial [40,47).
    const int K = 10;
    int seen = 0;
    qint64 off = 0;
    for (int guard = 0; guard < 100; ++guard) {
        const mcp::SpillRows p = mcp::readSpillRows(handle, off, K);
        ASSERT_TRUE(p.ok) << p.code.toStdString();
        EXPECT_EQ(p.key, QStringLiteral("matches"));       // == head_rows_key
        EXPECT_EQ(p.totalRows, N);
        EXPECT_EQ(p.rowOffset, off);
        for (int j = 0; j < p.rows.size(); ++j)
            EXPECT_EQ(p.rows.at(j), arr.at(static_cast<int>(off) + j)) << off + j;
        seen += p.rows.size();
        off += p.rows.size();
        if (!p.truncated) break;
    }
    EXPECT_EQ(seen, N);
    // The paged key equals the offloaded envelope's head_rows_key.
    ASSERT_TRUE(env.contains("head_rows_key"));
    EXPECT_EQ(env.value("head_rows_key").toString(), QStringLiteral("matches"));

    // Final partial page: [40,47) → 7 rows, truncated:false.
    const mcp::SpillRows last = mcp::readSpillRows(handle, 40, K);
    ASSERT_TRUE(last.ok);
    EXPECT_EQ(last.rows.size(), 7);
    EXPECT_FALSE(last.truncated);

    // Past-end → empty, truncated:false.
    const mcp::SpillRows past = mcp::readSpillRows(handle, N + 5, K);
    ASSERT_TRUE(past.ok);
    EXPECT_TRUE(past.rows.isEmpty());
    EXPECT_FALSE(past.truncated);

    // row_count == 0 → default page (kSpillRowsDefault = 100 > N → whole array).
    const mcp::SpillRows def = mcp::readSpillRows(handle, 0, 0);
    ASSERT_TRUE(def.ok);
    EXPECT_EQ(def.rows.size(), N);
    EXPECT_FALSE(def.truncated);

    // Negative arg → bad_args (gate lives in readSpillRows, directly testable).
    EXPECT_EQ(mcp::readSpillRows(handle, -1, K).code, QStringLiteral("bad_args"));
    EXPECT_EQ(mcp::readSpillRows(handle, 0, -5).code, QStringLiteral("bad_args"));

    // Unknown handle → not_found (inherited INV-5).
    const mcp::SpillRows missing =
        mcp::readSpillRows(QString(64, QLatin1Char('a')), 0, K);
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.code, QStringLiteral("not_found"));

    // A non-array (scalar-only) body → not_array.
    QJsonObject sb; sb["only"] = QString(20000, QLatin1Char('s'));
    const QString shandle =
        offloadEnv(QStringLiteral("get_text"), compact(sb)).value("handle").toString();
    ASSERT_FALSE(shandle.isEmpty());
    EXPECT_EQ(mcp::readSpillRows(shandle, 0, K).code, QStringLiteral("not_array"));
}

// INV-14 (ANTS-3545) — row-mode wiring source-scrape (the bundle links
// ants_core_lib, not RemoteControl, so cmdReadSpill is not runtime-callable —
// only its literals can be asserted). cmdReadSpill routes numeric row args to
// readSpillRows and emits mode:"rows"; the read_spill schema declares the two
// row props. The mode:"rows" literal is absent from pre-feature code (RED).
TEST_F(McpResultOffload, Inv14RowModeWiring) {
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    EXPECT_TRUE(rc.contains(QStringLiteral("readSpillRows")));
    EXPECT_TRUE(rc.contains(QStringLiteral("\"row_offset\"")));
    EXPECT_TRUE(rc.contains(QStringLiteral("\"row_count\"")));
    // The observable row-mode discriminator: mode == "rows".
    EXPECT_TRUE(rc.contains(QStringLiteral("QStringLiteral(\"rows\")")));
    // The too_large / not_array refusals each carry a byte-paging redirect
    // `hint`. That hint is built in cmdReadSpill (the SpillRows struct
    // readSpillRows returns has no `hint` field), so it is asserted here by
    // source-scrape, not on the direct readSpillRows call.
    EXPECT_TRUE(rc.contains(QStringLiteral("byte mode does not")))
        << "too_large refusal must redirect the caller to byte paging";
    EXPECT_TRUE(rc.contains(QStringLiteral("byte-page it via offset/max_bytes instead")))
        << "not_array refusal must redirect the caller to byte paging";

    const QString ci = readSource(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    EXPECT_TRUE(ci.contains(QStringLiteral("props[\"row_offset\"]")));
    EXPECT_TRUE(ci.contains(QStringLiteral("props[\"row_count\"]")));
}

// INV-14 (ANTS-3545) — row-mode refusal edges the Inv14ReadSpillRowPaging
// happy-path run doesn't reach: the stat-before-read `too_large` gate (a
// > 1 MiB body refused without loading it — mirrors Inv13ParseCapSkipped's
// body, here exercising the ROW path), and the two `not_array` shapes beyond
// scalar-only — an empty-array member (domCount == 0) and a bare root array
// (parses, but not a JSON object).
TEST_F(McpResultOffload, Inv14RowModeRefusalEdges) {
    // too_large — a > 1 MiB spilled body refuses before parsing (stat gate).
    {
        QJsonObject b;
        b["matches"] = QJsonArray{QString(1100000, QLatin1Char('a'))};  // > 1 MiB
        const QString body = compact(b);
        ASSERT_GT(body.toUtf8().size(), mcp::kStructuredParseMaxBytes);
        const QString handle =
            offloadEnv(QStringLiteral("get_scrollback"), body).value("handle").toString();
        ASSERT_FALSE(handle.isEmpty());
        EXPECT_EQ(mcp::readSpillRows(handle, 0, 10).code, QStringLiteral("too_large"));
    }
    // not_array — an empty array member (domCount == 0).
    {
        QJsonObject b;
        b["matches"] = QJsonArray();
        b["pad"] = QString(20000, QLatin1Char('p'));
        const QString handle = offloadEnv(QStringLiteral("workspace_search"),
                                          compact(b)).value("handle").toString();
        ASSERT_FALSE(handle.isEmpty());
        EXPECT_EQ(mcp::readSpillRows(handle, 0, 10).code, QStringLiteral("not_array"));
    }
    // not_array — a bare root array (valid JSON, but doc.isObject() is false).
    {
        QJsonArray arr;
        for (int i = 0; i < 4000; ++i) arr.append(i);   // large enough to offload
        const QString body = QString::fromUtf8(
            QJsonDocument(arr).toJson(QJsonDocument::Compact));
        ASSERT_GT(body.toUtf8().size(), 16384);
        const QString handle =
            offloadEnv(QStringLiteral("roadmap_query"), body).value("handle").toString();
        ASSERT_FALSE(handle.isEmpty());
        EXPECT_EQ(mcp::readSpillRows(handle, 0, 10).code, QStringLiteral("not_array"));
    }
}

// ANTS-4375 — a spilled array shorter than the population it came from must
// not read as complete.
//
// An offloaded roadmap_query returned total_rows:11 and truncated:false on
// the last page while the roadmap had TWELVE active bullets. The missing one
// was last in document order and was caught only because a parallel
// headline_only call on the identical filter returned 12. A truncation that
// announces itself costs a page; this one read as completeness — on the
// single most common "what should I do next?" call, so that session picked
// its work from a list silently one item short.
//
// `total_rows` deliberately stays the number of rows IN the file: paging is
// over those rows, and making it the population would leave `truncated` true
// on the last page forever and page a caller into nothing. The population is
// reported beside it, and the disagreement is what gets flagged.
TEST_F(McpResultOffload, Ants4375ShortSpillIsFlaggedNotSilent) {
    const auto writeSpill = [&](const QString &handle, int rows, int total) {
        QJsonArray arr;
        for (int i = 0; i < rows; ++i) {
            QJsonObject o;
            o[QStringLiteral("id")] = QStringLiteral("ANTS-%1").arg(i);
            arr.append(o);
        }
        QJsonObject obj;
        obj[QStringLiteral("bullets")] = arr;
        if (total >= 0) obj[QStringLiteral("total")] = total;
        QFile f(spillDir() + handle + QStringLiteral(".json"));
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    };

    // The reported shape: 11 rows spilled, 12 in the population.
    writeSpill(QStringLiteral("short"), 11, 12);
    const auto shortRes =
        mcp::readSpillRows(QStringLiteral("short"), 0, 100);
    ASSERT_TRUE(shortRes.ok);
    EXPECT_EQ(shortRes.totalRows, 11)
        << "total_rows stays the rows IN the file — paging is over those, and "
           "the population would leave truncated:true on the last page";
    EXPECT_EQ(shortRes.population, 12);
    EXPECT_TRUE(shortRes.rowsArePartial)
        << "the disagreement is the signal: paging to the end of this handle "
           "does not reach the whole answer";
    EXPECT_FALSE(shortRes.truncated)
        << "…and `truncated` still means \"more rows in THIS file\", which is "
           "exactly why it could not carry this";

    // A complete spill must not raise the flag — it has to stay rare enough
    // to mean something.
    writeSpill(QStringLiteral("whole"), 12, 12);
    const auto whole = mcp::readSpillRows(QStringLiteral("whole"), 0, 100);
    ASSERT_TRUE(whole.ok);
    EXPECT_EQ(whole.population, 12);
    EXPECT_FALSE(whole.rowsArePartial);

    // No population reported at all → nothing to compare, and no claim made.
    writeSpill(QStringLiteral("bare"), 5, -1);
    const auto bare = mcp::readSpillRows(QStringLiteral("bare"), 0, 100);
    ASSERT_TRUE(bare.ok);
    EXPECT_EQ(bare.population, -1);
    EXPECT_FALSE(bare.rowsArePartial);
}

// ANTS-4397 — a shape summary when the row BODIES do not all fit.
//
// On a markdown file whose rows are single very long lines (a status table),
// the spill preview carried the identical content in `head` (a JSON string)
// and `head_rows` (the parsed array), both cut at the same point. Measured:
// an 80-line request returned 20,529 bytes conveying 7 lines, one of them
// twice. The preview exists to let a caller decide whether to page the spill,
// and there it could not — it showed one truncated row and said nothing about
// which of the 80 rows were large.
//
// Purely additive: `head_rows` is untouched in every case, because a prefix
// of complete rows is genuinely the right preview for many SMALL rows, which
// is what ANTS-3538 was built for. It is the opposite shape that fails.
TEST_F(McpResultOffload, Ants4397ShapeSummaryForLongRows) {
    // The reporter's shape: 80 rows, each ~1.9 KB.
    QJsonArray arr;
    for (int i = 0; i < 80; ++i)
        arr.append(QStringLiteral("| %1 | ").arg(i) +
                   QString(1900, QLatin1Char('x')));
    QJsonObject b; b["lines"] = arr;
    const QString body = compact(b);
    ASSERT_GT(body.toUtf8().size(), 16384);

    const QJsonObject o = offloadEnv(QStringLiteral("read_region"), body);
    ASSERT_TRUE(o.value("offloaded").toBool());

    const QJsonArray shape = o.value("rows_preview").toArray();
    ASSERT_FALSE(shape.isEmpty())
        << "the case the preview could not answer must now be answered";
    EXPECT_EQ(o.value("rows_preview_key").toString(), QStringLiteral("lines"));

    // The point of the summary: it covers EVERY row, where the prefix covered
    // one. That is what lets a caller pick which rows to page.
    EXPECT_EQ(shape.size(), 80)
        << "80 shape rows must fit where 1 body row did — got " << shape.size();
    EXPECT_FALSE(o.value("rows_preview_truncated").toBool());

    // Each entry says how big its row is, which is the field that decides
    // what to fetch.
    const bool headsOmitted = o.value("rows_preview_heads_omitted").toBool();
    for (int i = 0; i < shape.size(); ++i) {
        const QJsonObject r = shape.at(i).toObject();
        EXPECT_EQ(r.value("index").toInt(), i);
        EXPECT_GT(r.value("bytes").toInt(), 1900);
        if (headsOmitted) {
            EXPECT_FALSE(r.contains("head"));
        } else {
            EXPECT_LT(r.value("head").toString().size(), 100)
                << "the head is a SAMPLE, not the row — duplicating the row "
                   "is the defect being fixed";
        }
    }
    // Covering every row at this width means the heads had to give way, and
    // the envelope must SAY so rather than leaving a caller to notice a
    // missing field. `bytes` is what decides where to page; the head is a
    // convenience, so the head is what gives way.
    EXPECT_TRUE(headsOmitted)
        << "80 rows of 1.9 KB cannot carry text samples within the budget — "
           "if this ever passes with heads intact, the budget grew and the "
           "fallback is no longer exercised here";

    // And the whole envelope still beats the body (INV-9), which is the
    // constraint the old preview was spending its budget against.
    EXPECT_LT(compact(o).toUtf8().size(), body.toUtf8().size());
}
