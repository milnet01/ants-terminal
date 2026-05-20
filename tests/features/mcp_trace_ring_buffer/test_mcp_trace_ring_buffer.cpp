// Feature-conformance test for ANTS-1360 mcp_trace ring buffer.
// See tests/features/mcp_trace_ring_buffer/spec.md.

#include <gtest/gtest.h>

#include "claudeintegration.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>

namespace {

QJsonObject simpleArgs() {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = QStringLiteral("/proj/x");
    return o;
}

qint64 argBytesOf(const QJsonObject &args) {
    return QJsonDocument(args).toJson(QJsonDocument::Compact).size();
}

void recordOk(ClaudeIntegration &ci, const QString &tool,
              const QJsonObject &args, qint64 durationUs = 100,
              bool cacheHit = false) {
    ci.recordMcpTraceForTest(tool, args, argBytesOf(args), 64,
                             durationUs, cacheHit,
                             QStringLiteral("ok"));
}

}  // namespace

// INV-3 + INV-11 boundary — empty ring returns empty array with
// next_id == since and ring_size == 0.
TEST(McpTraceRingBuffer, EmptyRingReturnsEmpty) {
    ClaudeIntegration ci;
    const QJsonObject out = ci.queryMcpTraceForTest(/*since*/0,
                                                    /*limit*/50);
    EXPECT_TRUE(out.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(out.value(QStringLiteral("records")).toArray().size(), 0);
    EXPECT_EQ(out.value(QStringLiteral("next_id")).toInt(), 0);
    EXPECT_EQ(out.value(QStringLiteral("ring_size")).toInt(), 0);
    EXPECT_EQ(out.value(QStringLiteral("ring_capacity")).toInt(), 200);
}

// INV-2 + per-record shape — first record carries id=1 and the
// expected field set.
TEST(McpTraceRingBuffer, SingleRecordPreservesShape) {
    ClaudeIntegration ci;
    recordOk(ci, QStringLiteral("get_cwd"), simpleArgs());
    const QJsonObject out = ci.queryMcpTraceForTest(0, 50);
    const QJsonArray records = out.value(QStringLiteral("records"))
                                   .toArray();
    ASSERT_EQ(records.size(), 1);
    const QJsonObject r = records.at(0).toObject();
    EXPECT_EQ(r.value(QStringLiteral("id")).toInt(), 1);
    EXPECT_EQ(r.value(QStringLiteral("tool")).toString(),
              QStringLiteral("get_cwd"));
    EXPECT_EQ(r.value(QStringLiteral("result")).toString(),
              QStringLiteral("ok"));
    const QJsonObject argKeys = r.value(QStringLiteral("arg_keys"))
                                  .toObject();
    EXPECT_EQ(argKeys.value(QStringLiteral("caller_cwd")).toString(),
              QStringLiteral("string"));
}

// INV-2 — ids increment monotonically.
TEST(McpTraceRingBuffer, MonotonicIds) {
    ClaudeIntegration ci;
    for (int i = 0; i < 5; ++i) {
        recordOk(ci, QStringLiteral("get_cwd"), simpleArgs());
    }
    const QJsonObject out = ci.queryMcpTraceForTest(0, 50);
    const QJsonArray records = out.value(QStringLiteral("records"))
                                   .toArray();
    ASSERT_EQ(records.size(), 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(records.at(i).toObject()
                  .value(QStringLiteral("id")).toInt(), i + 1);
    }
    EXPECT_EQ(out.value(QStringLiteral("next_id")).toInt(), 6);
}

// INV-1 — record past cap evicts the oldest; ids of the survivors
// are the most-recent 200 (51..250 after 250 inserts).
TEST(McpTraceRingBuffer, CapEvictsOldest) {
    ClaudeIntegration ci;
    for (int i = 0; i < 250; ++i) {
        recordOk(ci, QStringLiteral("get_cwd"), simpleArgs());
    }
    EXPECT_EQ(ci.mcpTraceSizeForTest(), 200);
    const QJsonObject out = ci.queryMcpTraceForTest(0, 200);
    const QJsonArray records = out.value(QStringLiteral("records"))
                                   .toArray();
    ASSERT_EQ(records.size(), 200);
    EXPECT_EQ(records.at(0).toObject()
              .value(QStringLiteral("id")).toInt(), 51);
    EXPECT_EQ(records.at(199).toObject()
              .value(QStringLiteral("id")).toInt(), 250);
}

// INV-3 — since filter is inclusive (id >= since).
TEST(McpTraceRingBuffer, SinceFilter) {
    ClaudeIntegration ci;
    for (int i = 0; i < 10; ++i) {
        recordOk(ci, QStringLiteral("get_cwd"), simpleArgs());
    }
    // since=5 returns ids 5..10 (six records).
    const QJsonObject out = ci.queryMcpTraceForTest(5, 50);
    const QJsonArray records = out.value(QStringLiteral("records"))
                                   .toArray();
    ASSERT_EQ(records.size(), 6);
    EXPECT_EQ(records.at(0).toObject()
              .value(QStringLiteral("id")).toInt(), 5);
    EXPECT_EQ(records.at(5).toObject()
              .value(QStringLiteral("id")).toInt(), 10);
    EXPECT_EQ(out.value(QStringLiteral("next_id")).toInt(), 11);
    // Empty result: next_id echoes since input.
    const QJsonObject empty = ci.queryMcpTraceForTest(11, 50);
    EXPECT_EQ(empty.value(QStringLiteral("records"))
                   .toArray().size(), 0);
    EXPECT_EQ(empty.value(QStringLiteral("next_id")).toInt(), 11);
}

// INV-4 — limit clamps the returned count (cap below available).
TEST(McpTraceRingBuffer, LimitClampsCount) {
    ClaudeIntegration ci;
    for (int i = 0; i < 20; ++i) {
        recordOk(ci, QStringLiteral("get_cwd"), simpleArgs());
    }
    const QJsonObject out = ci.queryMcpTraceForTest(0, 3);
    const QJsonArray records = out.value(QStringLiteral("records"))
                                   .toArray();
    ASSERT_EQ(records.size(), 3);
    EXPECT_EQ(records.at(0).toObject()
              .value(QStringLiteral("id")).toInt(), 1);
    EXPECT_EQ(records.at(2).toObject()
              .value(QStringLiteral("id")).toInt(), 3);
}

// INV-4 — limit < 1 clamps up to 1; limit > cap clamps down to cap.
TEST(McpTraceRingBuffer, LimitMinClamp) {
    ClaudeIntegration ci;
    for (int i = 0; i < 5; ++i) {
        recordOk(ci, QStringLiteral("get_cwd"), simpleArgs());
    }
    const QJsonObject zeroLim = ci.queryMcpTraceForTest(0, 0);
    EXPECT_EQ(zeroLim.value(QStringLiteral("records"))
                     .toArray().size(), 1);
    const QJsonObject hugeLim = ci.queryMcpTraceForTest(0, 999);
    EXPECT_EQ(hugeLim.value(QStringLiteral("records"))
                     .toArray().size(), 5);
}

// INV-5 — mcp_trace records itself NEVER.
TEST(McpTraceRingBuffer, NoSelfRecording) {
    ClaudeIntegration ci;
    ci.recordMcpTraceForTest(QStringLiteral("mcp_trace"), QJsonObject{},
                             0, 0, 0, false, QStringLiteral("ok"));
    EXPECT_EQ(ci.mcpTraceSizeForTest(), 0);
    // ...and other tools after still get id=1 (next id not bumped).
    recordOk(ci, QStringLiteral("get_cwd"), simpleArgs());
    const QJsonObject out = ci.queryMcpTraceForTest(0, 10);
    ASSERT_EQ(out.value(QStringLiteral("records")).toArray().size(), 1);
    EXPECT_EQ(out.value(QStringLiteral("records")).toArray()
              .at(0).toObject().value(QStringLiteral("id")).toInt(), 1);
}

// INV-7 — unknown-tool dispatch records too.
TEST(McpTraceRingBuffer, UnknownToolStillRecorded) {
    ClaudeIntegration ci;
    ci.recordMcpTraceForTest(QStringLiteral("nonexistent_tool"),
                             simpleArgs(), argBytesOf(simpleArgs()),
                             0, 25, false,
                             QStringLiteral("tool_not_found"));
    const QJsonObject out = ci.queryMcpTraceForTest(0, 10);
    const QJsonArray records = out.value(QStringLiteral("records"))
                                   .toArray();
    ASSERT_EQ(records.size(), 1);
    const QJsonObject r = records.at(0).toObject();
    EXPECT_EQ(r.value(QStringLiteral("result")).toString(),
              QStringLiteral("tool_not_found"));
    EXPECT_EQ(r.value(QStringLiteral("id")).toInt(), 1);
}

// INV-6 — arg_keys carries shape only; no value text.
TEST(McpTraceRingBuffer, ArgRedactionShapeOnly) {
    ClaudeIntegration ci;
    QJsonObject args;
    args[QStringLiteral("tab_id")] = 3;
    args[QStringLiteral("text")] = QStringLiteral("super-secret-payload");
    recordOk(ci, QStringLiteral("send-text"), args);
    const QJsonObject out = ci.queryMcpTraceForTest(0, 10);
    const QJsonObject r = out.value(QStringLiteral("records"))
                              .toArray().at(0).toObject();
    const QString serialised = QString::fromUtf8(
        QJsonDocument(r).toJson(QJsonDocument::Compact));
    EXPECT_FALSE(serialised.contains(
        QStringLiteral("super-secret-payload")))
        << "raw value leaked: " << serialised.toStdString();
    const QJsonObject argKeys = r.value(QStringLiteral("arg_keys"))
                                   .toObject();
    EXPECT_EQ(argKeys.value(QStringLiteral("tab_id")).toString(),
              QStringLiteral("int"));
    EXPECT_EQ(argKeys.value(QStringLiteral("text")).toString(),
              QStringLiteral("string"));
}

// INV-6 — args_sha16 is deterministic for identical args.
TEST(McpTraceRingBuffer, ArgsSha16IsDeterministic) {
    ClaudeIntegration ci;
    QJsonObject args;
    args[QStringLiteral("caller_cwd")] = QStringLiteral("/proj/x");
    args[QStringLiteral("limit")] = 50;
    recordOk(ci, QStringLiteral("tab_list"), args);
    recordOk(ci, QStringLiteral("tab_list"), args);
    const QJsonObject out = ci.queryMcpTraceForTest(0, 10);
    const QJsonArray records = out.value(QStringLiteral("records"))
                                   .toArray();
    ASSERT_EQ(records.size(), 2);
    EXPECT_EQ(records.at(0).toObject()
              .value(QStringLiteral("args_sha16")).toString(),
              records.at(1).toObject()
              .value(QStringLiteral("args_sha16")).toString());
}

// INV-8 — cache_hit input flag propagates to the record verbatim.
TEST(McpTraceRingBuffer, CacheHitFlagPropagates) {
    ClaudeIntegration ci;
    recordOk(ci, QStringLiteral("tab_list"), simpleArgs(),
             /*durationUs*/2, /*cacheHit*/true);
    const QJsonObject out = ci.queryMcpTraceForTest(0, 10);
    const QJsonObject r = out.value(QStringLiteral("records"))
                              .toArray().at(0).toObject();
    EXPECT_TRUE(r.value(QStringLiteral("cache_hit")).toBool());
}

// INV-11 — wrap detection fires after eviction; does NOT fire on
// an exactly-full but pristine ring (since=0 + records[0].id==1).
TEST(McpTraceRingBuffer, WrapDetectableViaRingSize) {
    ClaudeIntegration ci;
    // 250 inserts → ring wrapped (first 50 evicted).
    for (int i = 0; i < 250; ++i) {
        recordOk(ci, QStringLiteral("get_cwd"), simpleArgs());
    }
    const QJsonObject wrapped = ci.queryMcpTraceForTest(0, 200);
    const QJsonArray wrappedRecs = wrapped.value(QStringLiteral("records"))
                                          .toArray();
    const int capacity = wrapped.value(QStringLiteral("ring_capacity"))
                                .toInt();
    const int size = wrapped.value(QStringLiteral("ring_size")).toInt();
    EXPECT_EQ(size, capacity);
    // 250 inserts into a `capacity`-deep ring evict the oldest
    // (250 - capacity) records, so records[0].id is the first
    // survivor. Precise mirror of the pristine id==1 co-test below.
    EXPECT_EQ(wrappedRecs.at(0).toObject()
              .value(QStringLiteral("id")).toInt(),
              250 - capacity + 1);  // wrap detected: oldest evicted.

    // Boundary co-test: exactly 200 inserts → full but pristine.
    ClaudeIntegration ci2;
    for (int i = 0; i < 200; ++i) {
        recordOk(ci2, QStringLiteral("get_cwd"), simpleArgs());
    }
    const QJsonObject full = ci2.queryMcpTraceForTest(0, 200);
    const QJsonArray fullRecs = full.value(QStringLiteral("records"))
                                    .toArray();
    EXPECT_EQ(full.value(QStringLiteral("ring_size")).toInt(),
              full.value(QStringLiteral("ring_capacity")).toInt());
    // records[0].id == 1 == max(since=0, 1) → NO wrap.
    EXPECT_EQ(fullRecs.at(0).toObject()
              .value(QStringLiteral("id")).toInt(), 1);
}

// INV-3 — polling client tracks the cursor without gaps.
TEST(McpTraceRingBuffer, NextIdContinuityWithoutWrap) {
    ClaudeIntegration ci;
    for (int i = 0; i < 10; ++i) {
        recordOk(ci, QStringLiteral("get_cwd"), simpleArgs());
    }
    const QJsonObject first = ci.queryMcpTraceForTest(0, 100);
    const qint64 nextId = first.value(QStringLiteral("next_id")).toInt();
    EXPECT_EQ(nextId, 11);
    const QJsonObject second = ci.queryMcpTraceForTest(
        static_cast<quint64>(nextId), 100);
    EXPECT_EQ(second.value(QStringLiteral("records"))
                    .toArray().size(), 0);
    EXPECT_EQ(second.value(QStringLiteral("next_id")).toInt(),
              nextId);
    // Now record one more — next poll picks it up with id==nextId.
    recordOk(ci, QStringLiteral("get_cwd"), simpleArgs());
    const QJsonObject third = ci.queryMcpTraceForTest(
        static_cast<quint64>(nextId), 100);
    ASSERT_EQ(third.value(QStringLiteral("records"))
                   .toArray().size(), 1);
    EXPECT_EQ(third.value(QStringLiteral("records")).toArray()
              .at(0).toObject().value(QStringLiteral("id")).toInt(),
              nextId);
}

// INV-12 — failure records carry resp_bytes:0 + non-negative
// duration_us.
TEST(McpTraceRingBuffer, FailurePathRespBytesZero) {
    ClaudeIntegration ci;
    ci.recordMcpTraceForTest(QStringLiteral("nonexistent"),
                             simpleArgs(), argBytesOf(simpleArgs()),
                             /*respBytes*/0, /*durationUs*/15, false,
                             QStringLiteral("tool_not_found"));
    const QJsonObject out = ci.queryMcpTraceForTest(0, 10);
    const QJsonObject r = out.value(QStringLiteral("records"))
                              .toArray().at(0).toObject();
    EXPECT_EQ(r.value(QStringLiteral("resp_bytes")).toInt(), 0);
    EXPECT_GE(r.value(QStringLiteral("duration_us")).toInt(), 0);
    EXPECT_EQ(r.value(QStringLiteral("result")).toString(),
              QStringLiteral("tool_not_found"));
}

// INV-6 — nested objects report `object<N>` without recursing into
// inner keys; no inner-value leak.
TEST(McpTraceRingBuffer, NestedArgsShapeOnly) {
    ClaudeIntegration ci;
    QJsonObject inner;
    inner[QStringLiteral("inner_key")] = QStringLiteral("secret-data");
    inner[QStringLiteral("count")] = 3;
    QJsonObject args;
    args[QStringLiteral("outer")] = inner;
    recordOk(ci, QStringLiteral("workspace_search"), args);
    const QJsonObject out = ci.queryMcpTraceForTest(0, 10);
    const QJsonObject r = out.value(QStringLiteral("records"))
                              .toArray().at(0).toObject();
    const QString serialised = QString::fromUtf8(
        QJsonDocument(r).toJson(QJsonDocument::Compact));
    EXPECT_FALSE(serialised.contains(
        QStringLiteral("secret-data")));
    EXPECT_FALSE(serialised.contains(
        QStringLiteral("inner_key")));
    const QJsonObject argKeys = r.value(QStringLiteral("arg_keys"))
                                   .toObject();
    EXPECT_EQ(argKeys.value(QStringLiteral("outer")).toString(),
              QStringLiteral("object<2>"));
}
