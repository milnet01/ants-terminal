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
    const QString rc = readSource(SRC_RC_CPP);
    ASSERT_FALSE(rc.isEmpty());
    EXPECT_TRUE(rc.contains(QStringLiteral("readSpillRows")));
    EXPECT_TRUE(rc.contains(QStringLiteral("\"row_offset\"")));
    EXPECT_TRUE(rc.contains(QStringLiteral("\"row_count\"")));
    // The observable row-mode discriminator: mode == "rows".
    EXPECT_TRUE(rc.contains(QStringLiteral("QStringLiteral(\"rows\")")));

    const QString ci = readSource(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    EXPECT_TRUE(ci.contains(QStringLiteral("props[\"row_offset\"]")));
    EXPECT_TRUE(ci.contains(QStringLiteral("props[\"row_count\"]")));
}
