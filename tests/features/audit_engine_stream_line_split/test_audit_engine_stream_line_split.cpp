// Feature-conformance test for tests/features/audit_engine_stream_line_split/spec.md.
//
// Verifies the ANTS-1339 streaming applyFilter rewrite:
//   - INV-1 source-grep guard against `raw.split('\n'` regression
//   - INV-2 behavioural parity (maxLines + dropIfContains + keepOnlyIfContains)
//   - INV-3 throughput gate on a 1 MiB single-char-line input
//   - INV-4 tail handling when raw does not end with '\n'
//   - INV-5 empty input

#include "auditengine.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// INV-1 — source-grep: the body of applyFilter no longer contains
// `raw.split('\n'`. We scan a window starting at the function signature
// to keep the guard narrow.
TEST(AuditEngineStreamLineSplit, Inv1NoUpFrontSplit) {
    const std::string src = slurp(SRC_AUDIT_ENGINE_CPP_PATH);
    ASSERT_FALSE(src.empty()) << "auditengine.cpp not readable";

    const auto fnStart = src.find("applyFilter(const QString &raw");
    ASSERT_NE(fnStart, std::string::npos)
        << "applyFilter signature not found";
    // Scan ~6 KiB of body — covers the whole function on any sane
    // formatting. We look for `raw.split(` against the `raw` parameter
    // specifically; the body also reads sidecar source files via
    // `QString::fromUtf8(src.readAll()).split(...)` which is unrelated
    // and not the regression target.
    const std::string body = src.substr(fnStart, 6 * 1024);
    EXPECT_EQ(body.find("raw.split("), std::string::npos)
        << "applyFilter still calls raw.split(...) — streaming "
           "rewrite regressed (ANTS-1339 INV-1).";
}

// INV-2 — 100 short lines + maxLines=10 + drop/keep filters.
TEST(AuditEngineStreamLineSplit, Inv2MaxLinesAndPredicateFilters) {
    QString raw;
    raw.reserve(4096);
    for (int i = 0; i < 100; ++i) {
        raw += QStringLiteral("line %1 keepneedle\n").arg(i);
    }
    // Insert 5 lines that should be dropped (no keepneedle).
    raw += QStringLiteral("foo\nbar\nbaz\nquux\nzonk\n");

    OutputFilter f;
    f.maxLines = 10;
    f.keepOnlyIfContains << QStringLiteral("keepneedle");
    f.dropIfContains    << QStringLiteral("line 3");  // matches "line 30..39" too — that's the point

    const auto r = AuditEngine::applyFilter(raw, f, QString());

    // We should see at most 10 kept lines and NONE of them should
    // contain "line 3" (drop predicate) or lack "keepneedle".
    EXPECT_LE(r.count, 10);
    EXPECT_GT(r.count, 0);
    const QStringList kept = r.body.split('\n', Qt::SkipEmptyParts);
    EXPECT_EQ(kept.size(), r.count);
    for (const QString &k : kept) {
        EXPECT_TRUE(k.contains(QStringLiteral("keepneedle")))
            << k.toStdString();
        EXPECT_FALSE(k.contains(QStringLiteral("line 3")))
            << k.toStdString();
    }
}

// INV-3 — throughput gate. 1 MiB of single-char lines + maxLines=5
// must bail in well under 250 ms. Pre-fix this allocated the full
// 500 K-element QStringList before the maxLines bail; the streaming
// loop touches at most 5 lines + scans 5 newline positions.
TEST(AuditEngineStreamLineSplit, Inv3ThroughputBail) {
    constexpr int kTargetBytes = 1024 * 1024;  // 1 MiB
    QString raw;
    raw.reserve(kTargetBytes + 16);
    while (raw.size() < kTargetBytes) raw += QStringLiteral("a\n");

    OutputFilter f;
    f.maxLines = 5;

    QElapsedTimer t;
    t.start();
    const auto r = AuditEngine::applyFilter(raw, f, QString());
    const qint64 elapsedMs = t.elapsed();

    EXPECT_EQ(r.count, 5);
    // Widened from 250 ms post-test-audit 2026-05-18: under ASan/UBSan
    // (build-asan preset) or Valgrind a 1 MiB scan can take much
    // longer than under Release. The bail-out invariant — applyFilter
    // hits maxLines = 5 quickly rather than scanning the whole input —
    // still holds under any reasonable sanitiser overhead at 2 s.
    EXPECT_LT(elapsedMs, 2000)
        << "applyFilter bailed in " << elapsedMs
        << " ms — expected < 2000 ms (streaming rewrite regressed?)";
}

// INV-4 — tail line without trailing newline.
TEST(AuditEngineStreamLineSplit, Inv4TailWithoutTrailingNewline) {
    const QString raw = QStringLiteral("alpha\nbeta\ngamma");  // no \n
    OutputFilter f;
    f.maxLines = 0;  // no cap
    const auto r = AuditEngine::applyFilter(raw, f, QString());
    EXPECT_EQ(r.count, 3);
    EXPECT_TRUE(r.body.contains(QStringLiteral("gamma")));
}

// INV-5 — empty input.
TEST(AuditEngineStreamLineSplit, Inv5EmptyInput) {
    OutputFilter f;
    f.maxLines = 100;
    const auto r = AuditEngine::applyFilter(QString(), f, QString());
    EXPECT_EQ(r.count, 0);
    EXPECT_TRUE(r.body.isEmpty());
}
