// Feature-conformance test for ANTS-1870 — audit_run since-last-run
// findings delta + carry-forward SARIF merge.
//
// Pure helpers (computeDelta, the parser hoist, the fingerprint identity)
// are exercised directly; the sidecar round-trip + reaper run against a
// QTemporaryDir; the runAudit / envelope wiring is pinned by source-scrape
// (the audit_run_since_last_run pattern). GUI-free.

#include "auditcache.h"
#include "auditdelta.h"
#include "auditfpledger.h"
#include "auditrunner.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <string>

namespace {

QJsonObject mk(const QString &file, int line, const QString &rule,
               const QString &msg) {
    QJsonObject o;
    o["file"]     = file;
    o["line"]     = line;
    o["rule"]     = rule;
    o["severity"] = QStringLiteral("UNKNOWN");
    o["message"]  = msg;
    o["fp"] = ants::auditfp::computeFingerprint(file, rule, msg);
    return o;
}

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── INV-1 — full findings vs capped samples ───────────────────────────

TEST(AuditRunDelta, Inv1FullFindingsUncapped) {
    // 25 cppcheck-style located lines; sampleCap = 10.
    QString raw;
    for (int i = 1; i <= 25; ++i)
        raw += QStringLiteral("src/f.cpp:%1: warning: unused var v%1\n")
                   .arg(i);
    const auto c = AuditRunner::internal::parseWithSuppression(
        QStringLiteral("cppcheck"), raw, 10, {});
    EXPECT_EQ(c.sampleCount, 10) << "samples keep the sampleCap preview";
    EXPECT_EQ(c.findingsCount, 25) << "findings collect the full set";
    EXPECT_TRUE(c.allFindingsHaveHexFp) << "every finding carries a 16-hex fp";
    EXPECT_FALSE(c.findingsTruncated) << "25 < kSarifFindingsMax";
}

// ── INV-2 — line-insensitive identity ─────────────────────────────────

TEST(AuditRunDelta, Inv2FingerprintLineInsensitive) {
    // Same finding reported at two different lines (the location prefix in
    // the message differs only in the line number) → equal fp.
    const QString a = ants::auditfp::computeFingerprint(
        QStringLiteral("a.cpp"), QStringLiteral("cppcheck"),
        QStringLiteral("a.cpp:10:5: unused variable x"));
    const QString b = ants::auditfp::computeFingerprint(
        QStringLiteral("a.cpp"), QStringLiteral("cppcheck"),
        QStringLiteral("a.cpp:99:5: unused variable x"));
    EXPECT_EQ(a, b) << "fp must survive a line shift";

    // Consequence in computeDelta: a finding that only moved lines is
    // carried-forward, never added+removed.
    QJsonObject cur  = mk(QStringLiteral("a.cpp"), 10,
                          QStringLiteral("cppcheck"), QStringLiteral("dup msg"));
    QJsonObject prev = mk(QStringLiteral("a.cpp"), 99,
                          QStringLiteral("cppcheck"), QStringLiteral("dup msg"));
    const auto d = AuditDelta::computeDelta(
        QJsonArray{cur}, QJsonArray{prev}, {QStringLiteral("a.cpp")});
    EXPECT_EQ(d.addedCount, 0);
    EXPECT_EQ(d.removedCount, 0);
    EXPECT_EQ(d.carriedForwardCount, 1);
}

// ── INV-3 — partition + degenerate inputs ─────────────────────────────

TEST(AuditRunDelta, Inv3Partition) {
    const QString A = QStringLiteral("a.cpp");   // changed
    const QString B = QStringLiteral("b.cpp");   // untouched
    const QJsonObject fixed     = mk(A, 1, QStringLiteral("cppcheck"), QStringLiteral("fixed"));
    const QJsonObject unchanged = mk(A, 2, QStringLiteral("cppcheck"), QStringLiteral("unchanged"));
    const QJsonObject added     = mk(A, 3, QStringLiteral("cppcheck"), QStringLiteral("new"));
    const QJsonObject b1        = mk(B, 1, QStringLiteral("cppcheck"), QStringLiteral("b-one"));
    const QJsonObject b2        = mk(B, 2, QStringLiteral("cppcheck"), QStringLiteral("b-two"));

    const QJsonArray current{added, unchanged};
    const QJsonArray prior{fixed, unchanged, b1, b2};
    const auto d = AuditDelta::computeDelta(current, prior, {A});
    EXPECT_EQ(d.addedCount, 1);              // new
    EXPECT_EQ(d.removedCount, 1);            // fixed
    EXPECT_EQ(d.carriedForwardCount, 3);     // unchanged + b1 + b2
    EXPECT_EQ(d.merged.size(), 4);           // current(2) ∪ untouched(2)

    // Degenerate: empty current → every priorOnChanged removed.
    const auto e1 = AuditDelta::computeDelta({}, QJsonArray{fixed}, {A});
    EXPECT_EQ(e1.removedCount, 1);
    EXPECT_EQ(e1.addedCount, 0);
    // Degenerate: empty prior → every current added.
    const auto e2 = AuditDelta::computeDelta(QJsonArray{added}, {}, {A});
    EXPECT_EQ(e2.addedCount, 1);
    EXPECT_EQ(e2.removedCount, 0);
    // Degenerate: both empty → all arrays empty.
    const auto e3 = AuditDelta::computeDelta({}, {}, {A});
    EXPECT_EQ(e3.addedCount, 0);
    EXPECT_EQ(e3.removedCount, 0);
    EXPECT_EQ(e3.carriedForwardCount, 0);
    EXPECT_EQ(e3.merged.size(), 0);
}

// ── INV-4 — sidecar round-trip ────────────────────────────────────────

TEST(AuditRunDelta, Inv4SidecarRoundTrip) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString proj = tmp.path();

    QJsonObject lastRun;
    lastRun["iso_timestamp"] = QStringLiteral("2026-06-17T18:30:00Z");
    lastRun["commit"]        = QStringLiteral("abc1234");
    lastRun["scope"]         = QStringLiteral("since-last-run");
    lastRun["sarif"]         = QStringLiteral("audit-2026-06-17T18-30-00Z-abc1234.sarif");

    const QJsonArray merged{
        mk(QStringLiteral("x.cpp"), 42, QStringLiteral("cppcheck"),
           QStringLiteral("leak"))};
    QJsonObject prior;
    const auto rec = AuditCache::recordRun(proj, lastRun, &prior, merged);
    ASSERT_TRUE(rec.ok);

    // Manifest carries the sidecar basename.
    const QString basename =
        AuditCache::loadManifest(proj).lastRun.value(
            QStringLiteral("findings_file")).toString();
    ASSERT_FALSE(basename.isEmpty());
    EXPECT_TRUE(basename.startsWith(QStringLiteral("findings-")));
    EXPECT_FALSE(basename.contains(QLatin1Char(':')))
        << "basename iso is hyphen-form";

    // Round-trip the findings.
    const QJsonArray back = AuditCache::loadFindingsSidecar(proj, basename);
    ASSERT_EQ(back.size(), 1);
    EXPECT_EQ(back.at(0).toObject().value(QStringLiteral("fp")).toString(),
              merged.at(0).toObject().value(QStringLiteral("fp")).toString());

    // The body iso_timestamp keeps colons; truncated mirrors last_run.
    const auto load = AuditCache::readFindingsSidecar(proj, basename);
    EXPECT_TRUE(load.present);
    EXPECT_TRUE(load.valid);
    EXPECT_FALSE(load.truncated);

    // A version:2 sidecar loads empty (forward-compat).
    const QString v2path = proj + QStringLiteral("/.audit_cache/findings-v2.json");
    QFile f(v2path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 2},
        {QStringLiteral("findings"), QJsonArray{merged.at(0)}}})
            .toJson(QJsonDocument::Compact));
    f.close();
    EXPECT_TRUE(AuditCache::loadFindingsSidecar(
        proj, QStringLiteral("findings-v2.json")).isEmpty());
}

// ── INV-5 — reaper deletes dropped findings sidecars ──────────────────

TEST(AuditRunDelta, Inv5ReaperDeletesSidecars) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString proj = tmp.path();
    const QJsonArray merged{
        mk(QStringLiteral("x.cpp"), 1, QStringLiteral("cppcheck"),
           QStringLiteral("m"))};

    QString firstBasename;
    // 12 runs (distinct sha → distinct sidecar) — after migration the
    // oldest ages out of the 10-deep history and is reaped.
    for (int i = 1; i <= 12; ++i) {
        QJsonObject lastRun;
        lastRun["iso_timestamp"] = QStringLiteral("2026-06-17T18:30:00Z");
        lastRun["commit"]        = QStringLiteral("sha%1").arg(i, 3, 10, QChar('0'));
        lastRun["scope"]         = QStringLiteral("auto");
        lastRun["sarif"] = QStringLiteral("audit-iso-%1.sarif").arg(i);
        QJsonObject prior;
        const auto rec = AuditCache::recordRun(proj, lastRun, &prior, merged);
        ASSERT_TRUE(rec.ok);
        if (i == 1)
            firstBasename = AuditCache::loadManifest(proj).lastRun.value(
                QStringLiteral("findings_file")).toString();
    }
    ASSERT_FALSE(firstBasename.isEmpty());
    const QString firstAbs =
        proj + QStringLiteral("/.audit_cache/") + firstBasename;
    EXPECT_FALSE(QFile::exists(firstAbs))
        << "run 1's sidecar must be reaped after it ages out";

    // A non-history sentinel (GUI-style) is never touched by the reaper.
    const QString sentinel =
        proj + QStringLiteral("/.audit_cache/gui-manual.json");
    QFile s(sentinel);
    ASSERT_TRUE(s.open(QIODevice::WriteOnly));
    s.write("{}");
    s.close();
    QJsonObject lastRun;
    lastRun["iso_timestamp"] = QStringLiteral("2026-06-17T18:30:00Z");
    lastRun["commit"]        = QStringLiteral("sha999");
    lastRun["scope"]         = QStringLiteral("auto");
    lastRun["sarif"]         = QStringLiteral("audit-iso-99.sarif");
    QJsonObject prior;
    AuditCache::recordRun(proj, lastRun, &prior, merged);
    EXPECT_TRUE(QFile::exists(sentinel))
        << "reaper must never delete a file it did not name";
}

// ── INV-9 — computeDelta is pure (source-scrape) ──────────────────────

TEST(AuditRunDelta, Inv9DeltaModulePure) {
    const std::string src = ants_test::slurpFile(SRC_AUDITDELTA_CPP_PATH);
    ASSERT_FALSE(src.empty());
    EXPECT_FALSE(has(src, "QProcess")) << "delta must not spawn a process";
    EXPECT_FALSE(has(src, "runGit"))   << "delta must not call git";
    EXPECT_FALSE(has(src, "QFile"))    << "delta must not touch the filesystem";
}

// ── INV-6/7/8 — runAudit delta wiring (source-scrape) ─────────────────

TEST(AuditRunDelta, Inv678RunAuditWiring) {
    const std::string src = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());
    // INV-6 carry-forward SARIF: merged set + tagged carried-forward run.
    EXPECT_TRUE(has(src, "computeDelta"));
    EXPECT_TRUE(has(src, "carried_forward"));
    EXPECT_TRUE(has(src, "carried-forward"));
    // INV-7 chain integrity: the whole-tree merged set is recorded.
    EXPECT_TRUE(has(src, "mergedForRecord"));
    EXPECT_TRUE(has(src, "readFindingsSidecar"));
    // INV-8 honest fallback: the three reasons, data-dependency ordered.
    EXPECT_TRUE(has(src, "no_prior_findings"));
    EXPECT_TRUE(has(src, "prior_findings_unreadable"));
    EXPECT_TRUE(has(src, "findings_truncated"));
}

// ── INV-10 — envelope mutual exclusion (source-scrape) ────────────────

TEST(AuditRunDelta, Inv10EnvelopeFields) {
    const std::string src = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(src.empty());
    for (const char *key : {"delta", "delta_unavailable_reason",
                            "findings_truncated"})
        EXPECT_TRUE(has(src, key)) << key;
}
