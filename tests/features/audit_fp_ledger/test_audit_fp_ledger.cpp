// Feature-conformance test for tests/features/audit_fp_ledger/spec.md.
//
// ANTS-1708 — fingerprint-keyed learned-FP ledger. Exercises the pure
// ledger module (ants::auditfp) and the AuditEngine suppression filter.

#include "auditengine.h"
#include "auditfpledger.h"
#include "auditrunner.h"  // ANTS-1820 — headless suppression hook

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

using ants::auditfp::Entry;

// INV-1 — fingerprint is line-independent.
TEST(AuditFpLedger, FingerprintIsLineIndependent) {
    const QString a = ants::auditfp::computeFingerprint(
        "src/foo.cpp", "clang_tidy",
        "src/foo.cpp:42:7: warning: bogus thing [-Wunused]");
    const QString b = ants::auditfp::computeFingerprint(
        "src/foo.cpp", "clang_tidy",
        "src/foo.cpp:118:7: warning: bogus thing [-Wunused]");
    EXPECT_EQ(a, b) << "line shift must not change the fingerprint";
    EXPECT_EQ(a.size(), 16);
}

// INV-2 — fingerprint discriminates on file / check / descriptive message.
TEST(AuditFpLedger, FingerprintDiscriminates) {
    const QString base = ants::auditfp::computeFingerprint(
        "src/foo.cpp", "clang_tidy", "src/foo.cpp:1: warning: thing A");
    EXPECT_NE(base, ants::auditfp::computeFingerprint(
        "src/bar.cpp", "clang_tidy", "src/bar.cpp:1: warning: thing A"));
    EXPECT_NE(base, ants::auditfp::computeFingerprint(
        "src/foo.cpp", "cppcheck", "src/foo.cpp:1: warning: thing A"));
    EXPECT_NE(base, ants::auditfp::computeFingerprint(
        "src/foo.cpp", "clang_tidy", "src/foo.cpp:1: warning: thing B"));
}

// INV-3 — append → load round-trip.
TEST(AuditFpLedger, AppendLoadRoundTrip) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Entry e;
    e.fingerprint = "00112233445566aa";
    e.rule = "unsafe_c_funcs";
    e.reason = "fixture string, not a real call";
    ASSERT_TRUE(ants::auditfp::appendEntry(dir.path(), e));

    const QList<Entry> loaded = ants::auditfp::loadEntries(dir.path());
    ASSERT_EQ(loaded.size(), 1);
    EXPECT_EQ(loaded.first().fingerprint, e.fingerprint);
    EXPECT_EQ(loaded.first().rule, e.rule);
    EXPECT_FALSE(loaded.first().timestamp.isEmpty()) << "timestamp auto-stamped";
    EXPECT_TRUE(ants::auditfp::fingerprintSet(loaded).contains(e.fingerprint));
}

// INV-4 — missing file empty; malformed line skipped.
TEST(AuditFpLedger, MissingEmptyAndMalformedSkipped) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_TRUE(ants::auditfp::loadEntries(dir.path()).isEmpty())
        << "missing ledger → empty";

    ASSERT_TRUE(QDir(dir.path()).mkpath(".audit_cache"));
    const QString path = dir.path() + "/.audit_cache/learned-fp.jsonl";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("# header comment\n");
    f.write("this is not json\n");
    f.write("{\"fingerprint\":\"abcdef0123456789\",\"rule\":\"r\"}\n");
    f.write("{\"fingerprint\":\"short\",\"rule\":\"bad\"}\n");  // invalid fp
    f.close();

    const QList<Entry> loaded = ants::auditfp::loadEntries(dir.path());
    ASSERT_EQ(loaded.size(), 1) << "only the one valid 16-hex entry survives";
    EXPECT_EQ(loaded.first().fingerprint, "abcdef0123456789");
}

// INV-5 — append dedups.
TEST(AuditFpLedger, AppendDedups) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Entry e;
    e.fingerprint = "dededededededede";
    e.rule = "weak_crypto";
    ASSERT_TRUE(ants::auditfp::appendEntry(dir.path(), e));
    ASSERT_TRUE(ants::auditfp::appendEntry(dir.path(), e))
        << "second append of same fingerprint is a no-op that still succeeds";
    EXPECT_EQ(ants::auditfp::loadEntries(dir.path()).size(), 1);
}

// INV-6 — filter marks matching findings suppressed.
TEST(AuditFpLedger, FilterMarksMatchingSuppressed) {
    Finding hit;
    hit.checkId = "clang_tidy";
    hit.file = "src/foo.cpp";
    hit.message = "src/foo.cpp:42:7: warning: bogus thing [-Wunused]";

    Finding miss;
    miss.checkId = "clang_tidy";
    miss.file = "src/bar.cpp";
    miss.message = "src/bar.cpp:9:1: warning: different thing";

    QList<Finding> findings = {hit, miss};
    QSet<QString> learned;
    learned.insert(ants::auditfp::computeFingerprint(
        hit.file, hit.checkId, hit.message));

    const int marked = AuditEngine::applyLearnedFpSuppressions(findings, learned);
    EXPECT_EQ(marked, 1);
    EXPECT_TRUE(findings[0].suppressed) << "matching finding must be suppressed";
    EXPECT_FALSE(findings[1].suppressed) << "non-matching finding untouched";
    EXPECT_FALSE(findings[0].aiReasoning.isEmpty())
        << "a learned-FP note must be recorded";
}

// INV-7 — the headless audit_run runner consumes the same ledger. A learned
// FP recorded via the GUI (keyed on the full-line Finding::message) suppresses
// the matching finding the runner sees in raw tool output (stripped message).
TEST(AuditFpLedger, HeadlessRunnerSuppressesLearnedFp) {
    QSet<QString> learned;
    learned.insert(ants::auditfp::computeFingerprint(
        "src/foo.cpp", "cppcheck",
        "src/foo.cpp:42:7: warning: bogus thing [-Wunused]"));

    // cppcheck is line-based; the runner re-keys by (file, tool-name, message).
    const QString raw =
        "src/foo.cpp:42:7: warning: bogus thing [-Wunused]\n"
        "src/bar.cpp:9:1: warning: a real finding\n";

    const auto counts = AuditRunner::internal::parseWithSuppression(
        "cppcheck", raw, 10, learned);
    EXPECT_EQ(counts.rawCount, 2) << "rawCount keeps the tool's raw total";
    EXPECT_EQ(counts.afterFilterCount, 1) << "the learned FP is filtered out";
    EXPECT_EQ(counts.sampleCount, 1) << "a suppressed finding yields no sample";
}

// INV-7 — an empty ledger suppresses nothing.
TEST(AuditFpLedger, HeadlessRunnerEmptyLedgerNoSuppression) {
    const QString raw =
        "src/foo.cpp:42:7: warning: bogus thing [-Wunused]\n"
        "src/bar.cpp:9:1: warning: a real finding\n";
    const auto counts = AuditRunner::internal::parseWithSuppression(
        "cppcheck", raw, 10, {});
    EXPECT_EQ(counts.rawCount, 2);
    EXPECT_EQ(counts.afterFilterCount, 2);
    EXPECT_EQ(counts.sampleCount, 2);
}
