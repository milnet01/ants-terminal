// Feature-conformance test for ANTS-1430 ProjectLayoutEngine.
// See tests/features/mcp_project_layout_scan/spec.md.

#include <gtest/gtest.h>

#include "projectlayoutengine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

namespace PLE = ProjectLayoutEngine;

namespace {

void writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(body);
}

}  // namespace

// INV-1 — empty project: scan returns mostly-empty envelope but
// probedPaths enumerates every well-known path the scanner tried.
TEST(ProjectLayoutEngine, ScanEmptyDirReturnsEmptyEnvelope) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.rootCwd,                 td.path());
    EXPECT_TRUE(env.roadmap.path.isEmpty());
    EXPECT_TRUE(env.roadmap.format.isEmpty());
    EXPECT_EQ(env.roadmap.bulletCountEstimate, 0);
    EXPECT_EQ(env.roadmap.sizeBytes,           qint64(0));
    EXPECT_TRUE(env.changelog.path.isEmpty());
    EXPECT_TRUE(env.specsDir.isEmpty());
    EXPECT_TRUE(env.standardsDir.isEmpty());
    EXPECT_TRUE(env.adrDir.isEmpty());
    EXPECT_TRUE(env.appstreamMetainfo.isEmpty());
    EXPECT_TRUE(env.counterFile.isEmpty());
    // Probed paths include the names we tried even when absent —
    // future scans can re-stat them for invalidation.
    EXPECT_TRUE(env.probedPaths.contains(QStringLiteral("ROADMAP.md")));
    EXPECT_TRUE(env.probedPaths.contains(QStringLiteral("CHANGELOG.md")));
    EXPECT_TRUE(env.probedPaths.contains(QStringLiteral("docs/specs")));
    EXPECT_TRUE(env.probedPaths.contains(QStringLiteral("docs/standards")));
    EXPECT_TRUE(env.probedPaths.contains(QStringLiteral("docs/decisions")));
    EXPECT_TRUE(env.probedPaths.contains(QStringLiteral(".roadmap-counter")));
}

// INV-2a — Ants-format detection: marker present → "ants-v1".
TEST(ProjectLayoutEngine, ScanDetectsAntsV1Marker) {
    QTemporaryDir td;
    writeFile(td.path() + "/ROADMAP.md",
        "<!-- ants-roadmap-format: 1 -->\n"
        "# Roadmap\n\n"
        "## Open\n"
        "- ✅ [ANTS-0001] **Shipped item**\n"
        "- 📋 [ANTS-0002] **Planned item**\n"
        "- 🚧 [ANTS-0003] **In progress**\n");
    writeFile(td.path() + "/CHANGELOG.md", "# Changelog\n");
    QDir(td.path()).mkpath(QStringLiteral("docs/specs"));
    QDir(td.path()).mkpath(QStringLiteral("docs/standards"));
    writeFile(td.path() + "/.roadmap-counter", "42\n");

    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.roadmap.path,                QString("ROADMAP.md"));
    EXPECT_EQ(env.roadmap.format,              QString("ants-v1"));
    EXPECT_TRUE(env.roadmap.formatMarkerPresent);
    EXPECT_EQ(env.roadmap.bulletCountEstimate, 3);
    EXPECT_GT(env.roadmap.sizeBytes,           qint64(0));
    EXPECT_EQ(env.changelog.path,              QString("CHANGELOG.md"));
    EXPECT_EQ(env.specsDir,                    QString("docs/specs"));
    EXPECT_EQ(env.standardsDir,                QString("docs/standards"));
    EXPECT_TRUE(env.adrDir.isEmpty());  // not created
    EXPECT_EQ(env.counterFile,                 QString(".roadmap-counter"));
}

// INV-2b — GFM task-list detection: no marker, `- [ ]`/`- [x]`
// bullets → "github-task-list".
TEST(ProjectLayoutEngine, ScanDetectsGfmTaskList) {
    QTemporaryDir td;
    writeFile(td.path() + "/ROADMAP.md",
        "# Vestige Roadmap\n\n"
        "## Phase 11A\n"
        "- [x] **Sh4.** Shipped item\n"
        "- [ ] **Ed1.** Planned item\n"
        "- [ ] **Ed2.** Another planned item\n");

    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.roadmap.format,              QString("github-task-list"));
    EXPECT_FALSE(env.roadmap.formatMarkerPresent);
    EXPECT_EQ(env.roadmap.bulletCountEstimate, 3);
}

// INV-2c — unrecognised file: no marker, no GFM bullets → "unknown".
TEST(ProjectLayoutEngine, ScanReportsUnknownOnForeignFile) {
    QTemporaryDir td;
    writeFile(td.path() + "/ROADMAP.md",
        "# Random prose\n\n"
        "This file is not in any recognised roadmap format.\n");

    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.roadmap.format, QString("unknown"));
    EXPECT_EQ(env.roadmap.bulletCountEstimate, 0);
}

// INV-2d — AppStream metainfo glob discovers first .metainfo.xml.
TEST(ProjectLayoutEngine, ScanFindsAppStreamMetainfo) {
    QTemporaryDir td;
    writeFile(td.path() + "/packaging/com.example.app.metainfo.xml",
              "<component></component>");
    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.appstreamMetainfo,
              QString("packaging/com.example.app.metainfo.xml"));
}

// INV-3 — mtime invalidation: cache fresh when nothing changed;
// advance a probed path's mtime → isStale returns true.
TEST(ProjectLayoutEngine, IsStaleOnMtimeAdvance) {
    QTemporaryDir td;
    writeFile(td.path() + "/ROADMAP.md", "# Roadmap\n");
    auto env = PLE::scanLayout(td.path());
    // Pretend a small time has passed since the scan.
    const qint64 nowMs = env.scannedAtMs + 5;
    EXPECT_FALSE(PLE::isStale(env, nowMs));
    // QFileInfo::lastModified reports seconds-granularity on many
    // filesystems; backdate the cache so a touch-now is detectable.
    env.scannedAtMs = nowMs - 2000;
    // Wait long enough that the post-touch mtime crosses scannedAtMs.
    QThread::msleep(1100);
    writeFile(td.path() + "/ROADMAP.md", "# Roadmap (edited)\n");
    EXPECT_TRUE(PLE::isStale(env,
        QDateTime::currentMSecsSinceEpoch()));
}

// INV-4 — TTL invalidation: a cached envelope older than ttlDays
// is stale even if nothing changed on disk.
TEST(ProjectLayoutEngine, IsStaleOnTtlExpiry) {
    QTemporaryDir td;
    writeFile(td.path() + "/ROADMAP.md", "# Roadmap\n");
    auto env = PLE::scanLayout(td.path());
    const qint64 ttlMs =
        static_cast<qint64>(env.ttlDays) * 24 * 3600 * 1000;
    EXPECT_FALSE(PLE::isStale(env, env.scannedAtMs + ttlMs));
    EXPECT_TRUE (PLE::isStale(env, env.scannedAtMs + ttlMs + 1));
}

// INV-4b — scannedAtMs <= 0 (uninitialised cache) is always stale.
TEST(ProjectLayoutEngine, IsStaleOnUninitialised) {
    PLE::LayoutEnvelope env;
    EXPECT_TRUE(PLE::isStale(env, 1000));
}

// INV-5 / INV-6 — round-trip fidelity. fromJson(toJson(env)) yields
// an equivalent envelope.
TEST(ProjectLayoutEngine, JsonRoundTrip) {
    QTemporaryDir td;
    writeFile(td.path() + "/ROADMAP.md",
        "<!-- ants-roadmap-format: 1 -->\n"
        "- ✅ [ANTS-0001] **A**\n"
        "- 📋 [ANTS-0002] **B**\n");
    writeFile(td.path() + "/CHANGELOG.md", "# CHANGELOG\n");
    writeFile(td.path() + "/.roadmap-counter", "7\n");
    QDir(td.path()).mkpath(QStringLiteral("docs/specs"));
    QDir(td.path()).mkpath(QStringLiteral("docs/decisions"));
    writeFile(td.path() + "/packaging/com.example.app.metainfo.xml",
              "<component></component>");

    const auto a = PLE::scanLayout(td.path());
    const auto j = PLE::toJson(a);
    const auto b = PLE::fromJson(j);
    EXPECT_EQ(b.scannedAtMs,                a.scannedAtMs);
    EXPECT_EQ(b.ttlDays,                    a.ttlDays);
    EXPECT_EQ(b.rootCwd,                    a.rootCwd);
    EXPECT_EQ(b.roadmap.path,               a.roadmap.path);
    EXPECT_EQ(b.roadmap.format,             a.roadmap.format);
    EXPECT_EQ(b.roadmap.formatMarkerPresent,a.roadmap.formatMarkerPresent);
    EXPECT_EQ(b.roadmap.bulletCountEstimate,a.roadmap.bulletCountEstimate);
    EXPECT_EQ(b.roadmap.sizeBytes,          a.roadmap.sizeBytes);
    EXPECT_EQ(b.roadmap.mtimeMs,            a.roadmap.mtimeMs);
    EXPECT_EQ(b.changelog.path,             a.changelog.path);
    EXPECT_EQ(b.changelog.sizeBytes,        a.changelog.sizeBytes);
    EXPECT_EQ(b.specsDir,                   a.specsDir);
    EXPECT_EQ(b.standardsDir,               a.standardsDir);
    EXPECT_EQ(b.adrDir,                     a.adrDir);
    EXPECT_EQ(b.appstreamMetainfo,          a.appstreamMetainfo);
    EXPECT_EQ(b.counterFile,                a.counterFile);
    EXPECT_EQ(b.probedPaths,                a.probedPaths);
    // ANTS-1574 — standards_files[] round-trips even when empty.
    EXPECT_EQ(b.standardsFiles,             a.standardsFiles);
    // ANTS-1620 — probe-set version round-trips.
    EXPECT_EQ(b.probeSetVersion,            a.probeSetVersion);
}

// ANTS-1574 INV-A — data/changelog.yaml is recognised as the changelog
// when no CHANGELOG.{md,yaml,yml} exists at root.
TEST(ProjectLayoutEngine, DataChangelogYamlIsRecognised) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    writeFile(td.path() + "/data/changelog.yaml",
              "- version: 1.0\n  date: 2026-05-18\n");
    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.changelog.path, QString("data/changelog.yaml"));
    EXPECT_TRUE(env.discovered.contains(QString("data/changelog.yaml")));
}

// ANTS-1574 INV-B — root CHANGELOG.md still wins over data/changelog.yaml
// (probe order: canonical root first).
TEST(ProjectLayoutEngine, RootChangelogStillWinsOverDataYaml) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    writeFile(td.path() + "/CHANGELOG.md",         "# Changelog\n");
    writeFile(td.path() + "/data/changelog.yaml",  "- version: x\n");
    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.changelog.path, QString("CHANGELOG.md"));
}

// ANTS-1574 INV-C — standards-name fallback: when docs/standards/ is
// absent, a docs/*STANDARD*.md ≥ 100 lines populates standardsFiles[].
TEST(ProjectLayoutEngine, StandardsNameGlobFallbackPopulatesField) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    QByteArray body = "# Project design standards\n";
    for (int i = 0; i < 120; ++i) body.append("line\n");
    writeFile(td.path() + "/docs/RETRODB_DESIGN_STANDARDS.md", body);

    const auto env = PLE::scanLayout(td.path());
    EXPECT_TRUE(env.standardsDir.isEmpty())
        << "standards_dir must stay empty (no canonical dir found)";
    EXPECT_TRUE(env.standardsFiles.contains(
        QString("docs/RETRODB_DESIGN_STANDARDS.md")));
    EXPECT_TRUE(env.discovered.contains(
        QString("docs/RETRODB_DESIGN_STANDARDS.md")))
        << "fallback hit must also be folded into discovered[]";
}

// ANTS-1574 INV-D — sub-threshold stub files (< 100 lines) are NOT
// promoted by the name-glob fallback.
TEST(ProjectLayoutEngine, StandardsNameGlobFallbackRejectsShortStubs) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    writeFile(td.path() + "/docs/STYLE.md",
              "# Style\nL1\nL2\nL3\nL4\nL5\n");
    const auto env = PLE::scanLayout(td.path());
    EXPECT_TRUE(env.standardsFiles.isEmpty());
    EXPECT_FALSE(env.discovered.contains(QString("docs/STYLE.md")));
}

// ANTS-1632 INV-10a — mixed-format roadmaps: file contains BOTH
// GFM task-list and ants-v1 emoji bullets, no format marker → the
// sniffer returns "mixed" instead of dropping out to "unknown".
TEST(ProjectLayoutEngine, ScanDetectsMixedFormatRoadmap) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    writeFile(td.path() + "/ROADMAP.md",
        "# Roadmap\n\n"
        "## Newer slice (GFM)\n"
        "- [x] **Sh1.** Shipped item\n"
        "- [ ] **Ed1.** Planned item\n\n"
        "## Older slice (ants-v1)\n"
        "- ✅ [ANTS-0001] **Done**\n"
        "- 📋 [ANTS-0002] **Planned**\n"
        "- 🚧 [ANTS-0003] **In progress**\n");
    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.roadmap.format, QString("mixed"));
    EXPECT_FALSE(env.roadmap.formatMarkerPresent);
    // bullet_count_estimate counts the union of both shapes.
    EXPECT_EQ(env.roadmap.bulletCountEstimate, 5);
}

// ANTS-1632 INV-10b — ants-v1 emoji-only ROADMAP (no format marker,
// no GFM bullets) is detected as "ants-v1" not "unknown". Prior to
// 1632 the sniffer required the format marker to call a file ants-v1.
TEST(ProjectLayoutEngine, ScanDetectsAntsV1WithoutMarker) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    writeFile(td.path() + "/ROADMAP.md",
        "# Roadmap\n\n"
        "## Open\n"
        "- ✅ [ANTS-0001] **Done**\n"
        "- 📋 [ANTS-0002] **Planned**\n");
    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.roadmap.format, QString("ants-v1"));
    EXPECT_FALSE(env.roadmap.formatMarkerPresent)
        << "ANTS-1632: marker absent — format inferred from emoji "
           "bullets, formatMarkerPresent stays false";
    EXPECT_EQ(env.roadmap.bulletCountEstimate, 2);
}

// ANTS-1620 INV-9a — fresh scan tags the envelope with the current
// probe-set version; isStale returns false for the version it just
// produced.
TEST(ProjectLayoutEngine, ProbeSetVersionStampedOnScan) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.probeSetVersion, PLE::kProbeSetVersion);
    EXPECT_FALSE(PLE::isStale(env, env.scannedAtMs + 100));
}

// ANTS-1620 INV-9b — cached envelope with stale probe-set version
// (i.e. pre-widening) is invalidated regardless of mtime/TTL. This
// is the contract failure mode: pre-ANTS-1493 caches surfaced a
// narrow probed_paths[] echo until natural TTL expiry, hiding the
// fact that docs/private/ etc. were now probed.
TEST(ProjectLayoutEngine, IsStaleWhenProbeSetVersionRegressed) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    auto env = PLE::scanLayout(td.path());
    env.probeSetVersion = PLE::kProbeSetVersion - 1;
    EXPECT_TRUE(PLE::isStale(env, env.scannedAtMs + 100));
}

// ANTS-1620 INV-9c — fromJson on a pre-1620 envelope (no
// probe_set_version field) defaults the version to 0, which is
// < kProbeSetVersion ⇒ isStale invalidates.
TEST(ProjectLayoutEngine, LegacyEnvelopeWithoutProbeSetVersionIsStale) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    const auto fresh = PLE::scanLayout(td.path());
    auto j = PLE::toJson(fresh);
    j.remove(QStringLiteral("probe_set_version"));
    const auto legacy = PLE::fromJson(j);
    EXPECT_EQ(legacy.probeSetVersion, 0);
    EXPECT_TRUE(PLE::isStale(legacy, legacy.scannedAtMs + 100));
}

// ANTS-1620 INV-9d — fresh scan's probed_paths[] surfaces the
// widened ANTS-1493 candidates (docs/private/, docs/internal/,
// docs/fork/) even when none of those directories exist on disk.
// Contract: the echo must list every path actually probed.
TEST(ProjectLayoutEngine, ProbedPathsIncludeWidenedForkCandidates) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    const auto env = PLE::scanLayout(td.path());
    EXPECT_TRUE(env.probedPaths.contains(
        QStringLiteral("docs/private/ROADMAP.md")));
    EXPECT_TRUE(env.probedPaths.contains(
        QStringLiteral("docs/internal/ROADMAP.md")));
    EXPECT_TRUE(env.probedPaths.contains(
        QStringLiteral("docs/fork/ROADMAP.md")));
    EXPECT_TRUE(env.probedPaths.contains(
        QStringLiteral("docs/private/CHANGELOG.md")));
    EXPECT_TRUE(env.probedPaths.contains(
        QStringLiteral("docs/private/specs")));
}

// ANTS-1574 INV-E — when docs/standards/ exists, the fallback is
// suppressed (canonical dir wins; standards_files[] stays empty).
TEST(ProjectLayoutEngine, StandardsNameGlobFallbackSkippedWhenDirPresent) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    QDir(td.path()).mkpath(QStringLiteral("docs/standards"));
    writeFile(td.path() + "/docs/standards/coding.md", "x");
    QByteArray body = "# Design\n";
    for (int i = 0; i < 200; ++i) body.append("line\n");
    writeFile(td.path() + "/docs/EXTRA_DESIGN_GUIDE.md", body);

    const auto env = PLE::scanLayout(td.path());
    EXPECT_EQ(env.standardsDir, QString("docs/standards"));
    EXPECT_TRUE(env.standardsFiles.isEmpty())
        << "canonical docs/standards/ wins; fallback must not fire";
}
