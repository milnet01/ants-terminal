// Feature-conformance test for ANTS-1319 ColdEyesEngine.
// See tests/features/cold_eyes_engine/spec.md.

#include <gtest/gtest.h>

#include "coldeyesengine.h"
#include "indiereviewengine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

bool writeFile(const QString &absPath, const QString &body) {
    QFileInfo fi(absPath);
    if (!fi.dir().exists() && !QDir().mkpath(fi.absolutePath())) return false;
    QFile f(absPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(body.toUtf8()) == body.toUtf8().size();
}

struct Workspace {
    QTemporaryDir td;
    QString root() const { return td.path(); }
    bool valid() const { return td.isValid(); }

    bool writeRel(const QString &rel, const QString &body) {
        return writeFile(root() + QChar('/') + rel, body);
    }
};

QString stubRoadmap(const QList<QPair<int, QString>> &entries) {
    QString out;
    out += QStringLiteral("# Roadmap\n\n");
    out += QStringLiteral("## 0.7.92 (target: 2026-W21)\n\n");
    for (const auto &[id, status] : entries) {
        out += QStringLiteral("- ") + status
             + QStringLiteral(" [ANTS-") + QString::number(id)
             + QStringLiteral("] **Stub entry.** body. Kind: implement.\n");
    }
    return out;
}

}  // namespace

// ENG-1
TEST(ColdEyesEngine, DerivePartitionBuildsCanonicalLanes) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CLAUDE.md",   "# CLAUDE\n"));
    ASSERT_TRUE(ws.writeRel("README.md",   "# README\n"));
    ASSERT_TRUE(ws.writeRel("ROADMAP.md",
        stubRoadmap({{1287, QStringLiteral("✅")}})));
    ASSERT_TRUE(ws.writeRel("CHANGELOG.md","# Changelog\n"));
    ASSERT_TRUE(ws.writeRel("docs/standards/coding.md",   "# Coding\n"));
    ASSERT_TRUE(ws.writeRel("docs/decisions/0001-x.md",   "# ADR\n"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    ASSERT_GE(r.lanes.size(), 3);

    QSet<QString> names;
    for (const auto &l : r.lanes) names.insert(l.name);
    EXPECT_TRUE(names.contains("contracts"))
        << "default partition missing contracts lane";
    EXPECT_TRUE(names.contains("standards"));
    EXPECT_TRUE(names.contains("decisions"));
    EXPECT_FALSE(r.truncated);
    EXPECT_EQ(r.overridePath, QStringLiteral("<default>"));
}

// ENG-2
TEST(ColdEyesEngine, ActiveSpecsExcludesShipped) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("ROADMAP.md", stubRoadmap({
        {1287, QStringLiteral("✅")},  // shipped
        {1319, QStringLiteral("📋")},  // active planned
        {1320, QStringLiteral("🚧")},  // active in-progress
        {1321, QStringLiteral("💭")},  // dream
    })));

    const auto ids = ColdEyesEngine::activeSpecIds(ws.root());
    EXPECT_FALSE(ids.contains(1287)) << "shipped ✅ should not be active";
    EXPECT_TRUE(ids.contains(1319));
    EXPECT_TRUE(ids.contains(1320));
    EXPECT_FALSE(ids.contains(1321)) << "💭 dream should not be active";
}

// ENG-3
TEST(ColdEyesEngine, SpecLaneCapHonoured) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());

    QList<QPair<int, QString>> entries;
    for (int i = 0; i < 15; ++i) {
        const int id = 2000 + i;
        entries.append({id, QStringLiteral("📋")});
        ASSERT_TRUE(ws.writeRel(
            QStringLiteral("docs/specs/ANTS-%1.md").arg(id),
            QStringLiteral("# stub\n")));
    }
    ASSERT_TRUE(ws.writeRel("ROADMAP.md", stubRoadmap(entries)));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    int specCount = 0;
    for (const auto &l : r.lanes) {
        if (l.name.startsWith("spec/")) ++specCount;
    }
    EXPECT_EQ(specCount, ColdEyesEngine::kMaxSpecLanes);
    EXPECT_TRUE(r.truncated)
        << "INV-2: spec count > kMaxSpecLanes must set truncated=true";
    EXPECT_EQ(r.scopedCount, 15);
}

// ENG-4
TEST(ColdEyesEngine, BriefManifestIsPathsOnly) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-9999.md",
        QStringLiteral("# Spec\nMASSIVE_INLINED_SECRET_BODY\n")));

    ColdEyesEngine::Lane lane;
    lane.name = QStringLiteral("spec/ANTS-9999");
    lane.summary = QStringLiteral("test lane");
    lane.docPaths << QStringLiteral("docs/specs/ANTS-9999.md");

    const auto m = ColdEyesEngine::assembleBriefManifest(ws.root(), lane);
    EXPECT_TRUE(m.brief.contains(QStringLiteral("Read each doc")));
    EXPECT_FALSE(m.brief.contains(QStringLiteral("MASSIVE_INLINED_SECRET_BODY")))
        << "INV-3: brief must NOT inline doc bodies";
    EXPECT_EQ(m.docPaths, lane.docPaths);
}

// ENG-5
TEST(ColdEyesEngine, CrossReferenceDocsAreContractTrio) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CLAUDE.md", "x"));
    ASSERT_TRUE(ws.writeRel("README.md", "x"));
    ASSERT_TRUE(ws.writeRel("ROADMAP.md", stubRoadmap({})));
    ASSERT_TRUE(ws.writeRel("CHANGELOG.md", "x"));

    ColdEyesEngine::Lane lane;
    lane.name = QStringLiteral("standards");
    lane.docPaths << QStringLiteral("docs/standards/coding.md");
    ASSERT_TRUE(ws.writeRel("docs/standards/coding.md", "x"));

    const auto m = ColdEyesEngine::assembleBriefManifest(ws.root(), lane);
    // Order-insensitive: assembleBriefManifest does not promise a stable
    // order for the contract trio. Check membership and count instead so
    // a refactor that reorders the four files doesn't fail the test
    // without any semantic regression.
    EXPECT_EQ(m.crossReferenceDocs.size(), 4);
    EXPECT_TRUE(m.crossReferenceDocs.contains(QStringLiteral("CLAUDE.md")));
    EXPECT_TRUE(m.crossReferenceDocs.contains(QStringLiteral("README.md")));
    EXPECT_TRUE(m.crossReferenceDocs.contains(QStringLiteral("ROADMAP.md")));
    EXPECT_TRUE(m.crossReferenceDocs.contains(QStringLiteral("CHANGELOG.md")));

    // De-dup: contracts lane already contains all four; no double-emit.
    ColdEyesEngine::Lane contracts;
    contracts.name = QStringLiteral("contracts");
    contracts.docPaths << QStringLiteral("CLAUDE.md")
                       << QStringLiteral("README.md")
                       << QStringLiteral("ROADMAP.md")
                       << QStringLiteral("CHANGELOG.md");
    const auto m2 =
        ColdEyesEngine::assembleBriefManifest(ws.root(), contracts);
    EXPECT_TRUE(m2.crossReferenceDocs.isEmpty())
        << "INV-4 de-dup: contract trio already in laneDocs → no echo";
}

// ENG-6
TEST(ColdEyesEngine, ExtractCitedCodePathsResolvesRealPaths) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("src/coldeyesengine.h",   "x"));
    ASSERT_TRUE(ws.writeRel("src/coldeyesengine.cpp", "x"));
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-9999.md",
        QStringLiteral(
            "# Spec\nSee `src/coldeyesengine.h` and "
            "`src/coldeyesengine.cpp`. Bogus: `src/does_not_exist.h`.\n")));

    const auto out = ColdEyesEngine::extractCitedCodePaths(
        ws.root(),
        QStringList{QStringLiteral("docs/specs/ANTS-9999.md")});
    EXPECT_TRUE(out.contains(QStringLiteral("src/coldeyesengine.h")));
    EXPECT_TRUE(out.contains(QStringLiteral("src/coldeyesengine.cpp")));
    EXPECT_FALSE(out.contains(QStringLiteral("src/does_not_exist.h")))
        << "non-existent paths must be filtered";
}

// ENG-7
TEST(ColdEyesEngine, CrossDocDiffFromDirEmptyDirReturnsEmpty) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(QDir().mkpath(ws.root()
                              + QStringLiteral("/.cold-eyes/reports")));
    int reportsRead = -1;
    const auto found = ColdEyesEngine::crossDocDiffFromDir(
        ws.root(),
        QStringLiteral(".cold-eyes/reports"),
        2, &reportsRead);
    EXPECT_TRUE(found.isEmpty());
    EXPECT_EQ(reportsRead, 0);
}

// ENG-7b (ANTS-1626) — inline `reports` path corroborates findings
// across two distinct lanes without touching disk. The /cold-eyes
// skill bundles agent reports inline in the orchestrator's context
// (one report per `Agent` tool result); the path must work without
// the caller having to spill them to disk just to call this verb.
TEST(ColdEyesEngine, CrossDocDiffFromReportsCorroboratesAcrossLanes) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    // extractFileLineCitations() filters out paths that don't exist
    // under projectPath (defends against fabricated citations), so
    // touch the three files the reports cite.
    ASSERT_TRUE(ws.writeRel("src/foo.cpp", "// stub\n"));
    ASSERT_TRUE(ws.writeRel("src/bar.cpp", "// stub\n"));
    ASSERT_TRUE(ws.writeRel("src/baz.cpp", "// stub\n"));
    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("lane_alpha"),
        QStringLiteral("# Alpha report\n\n"
                       "- Issue at src/foo.cpp:42 — symptom A.\n"
                       "- Another at src/bar.cpp:7 — symptom B.\n"));
    reports.insert(QStringLiteral("lane_beta"),
        QStringLiteral("# Beta report\n\n"
                       "- Same issue at src/foo.cpp:42 — symptom A.\n"
                       "- Unique at src/baz.cpp:99 — symptom C.\n"));
    const auto found = ColdEyesEngine::crossDocDiffFromReports(
        ws.root(), reports, /*minLanes=*/2);
    // foo.cpp:42 is the only (file, line) cited by ≥ 2 lanes.
    ASSERT_EQ(1, found.size())
        << "expected exactly one corroborated finding (foo.cpp:42)";
    EXPECT_EQ(QStringLiteral("src/foo.cpp"), found.first().file);
    EXPECT_EQ(42, found.first().line);
    QSet<QString> lanes(found.first().citingLanes.begin(),
                        found.first().citingLanes.end());
    EXPECT_TRUE(lanes.contains(QStringLiteral("lane_alpha")));
    EXPECT_TRUE(lanes.contains(QStringLiteral("lane_beta")));
}

// ENG-8
TEST(ColdEyesEngine, FoldInBlockHeadingMatchesInv7) {
    QList<IndieReviewEngine::CorroboratedFinding> actionable;
    IndieReviewEngine::CorroboratedFinding f;
    f.file = QStringLiteral("docs/specs/ANTS-1281.md");
    f.line = 42;
    f.citingLanes << QStringLiteral("contracts")
                  << QStringLiteral("spec/ANTS-1281");
    actionable.append(f);

    const QString block = ColdEyesEngine::templateColdEyesFoldInBlock(
        actionable, QList<int>{1335}, QStringLiteral("2026-05-14"));
    static const QRegularExpression rxHeading(
        QStringLiteral("^### 📝 Cold-eyes \\d{4}-\\d{2}-\\d{2}$"),
        QRegularExpression::MultilineOption);
    EXPECT_TRUE(rxHeading.match(block).hasMatch())
        << "INV-7 heading regex failed; block:\n" << block.toStdString();
    EXPECT_TRUE(block.contains(QStringLiteral("[ANTS-1335]")));
    EXPECT_TRUE(block.contains(QStringLiteral(
        "docs/specs/ANTS-1281.md:42")));
}

// ENG-9
TEST(ColdEyesEngine, PathRuleDefenceOnCitedCodePaths) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-9999.md",
        QStringLiteral("See `src/../../etc/passwd.h` and "
                       "`src/legit.h`.\n")));
    ASSERT_TRUE(ws.writeRel("src/legit.h", "x"));

    const auto out = ColdEyesEngine::extractCitedCodePaths(
        ws.root(),
        QStringList{QStringLiteral("docs/specs/ANTS-9999.md")});
    for (const QString &p : out) {
        EXPECT_FALSE(p.contains(QStringLiteral("..")))
            << "path-escape candidate slipped through: " << p.toStdString();
        EXPECT_FALSE(p.contains(QStringLiteral("etc/passwd")));
    }
}

// ENG-10
TEST(ColdEyesEngine, ParseScopeCoversAllEnumValues) {
    ColdEyesEngine::Scope s = ColdEyesEngine::Scope::ContractsOnly;
    EXPECT_TRUE(ColdEyesEngine::parseScope(QString(), &s));
    EXPECT_EQ(s, ColdEyesEngine::Scope::Default);
    EXPECT_TRUE(ColdEyesEngine::parseScope("default", &s));
    EXPECT_EQ(s, ColdEyesEngine::Scope::Default);
    EXPECT_TRUE(ColdEyesEngine::parseScope("docs_only", &s));
    EXPECT_EQ(s, ColdEyesEngine::Scope::DocsOnly);
    EXPECT_TRUE(ColdEyesEngine::parseScope("contracts_only", &s));
    EXPECT_EQ(s, ColdEyesEngine::Scope::ContractsOnly);
    EXPECT_FALSE(ColdEyesEngine::parseScope("weird_scope", &s));
}

// ANTS-1571 INV-A — community-contract docs (CONTRIBUTING / SECURITY /
// LEGAL / CODE_OF_CONDUCT) fold into the contracts lane when present.
TEST(ColdEyesEngine, ContractsLaneAbsorbsCommunityDocs) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CLAUDE.md",          "# CLAUDE\n"));
    ASSERT_TRUE(ws.writeRel("README.md",          "# README\n"));
    ASSERT_TRUE(ws.writeRel("ROADMAP.md",
        stubRoadmap({{1287, QStringLiteral("✅")}})));
    ASSERT_TRUE(ws.writeRel("CHANGELOG.md",       "# Changelog\n"));
    ASSERT_TRUE(ws.writeRel("CONTRIBUTING.md",    "# Contributing\n"));
    ASSERT_TRUE(ws.writeRel("SECURITY.md",        "# Security\n"));
    ASSERT_TRUE(ws.writeRel("LEGAL.md",           "# Legal\n"));
    ASSERT_TRUE(ws.writeRel("CODE_OF_CONDUCT.md", "# CoC\n"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    const ColdEyesEngine::Lane *contracts = nullptr;
    for (const auto &l : r.lanes) {
        if (l.name == QStringLiteral("contracts")) {
            contracts = &l; break;
        }
    }
    ASSERT_TRUE(contracts != nullptr) << "contracts lane missing";
    EXPECT_TRUE(contracts->docPaths.contains(QStringLiteral("CONTRIBUTING.md")));
    EXPECT_TRUE(contracts->docPaths.contains(QStringLiteral("SECURITY.md")));
    EXPECT_TRUE(contracts->docPaths.contains(QStringLiteral("LEGAL.md")));
    EXPECT_TRUE(contracts->docPaths.contains(QStringLiteral("CODE_OF_CONDUCT.md")));
    // Summary tracks doc_paths (ANTS-1506 contract).
    EXPECT_TRUE(contracts->summary.contains(QStringLiteral("CONTRIBUTING.md")));
}

// ANTS-1571 INV-B — community-contract case-insensitive: `Contributing.md`,
// `security.md` resolve through the same case-tolerant walk that
// canonical contracts get.
TEST(ColdEyesEngine, ContractsLaneCommunityDocsCaseInsensitive) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("README.md",       "x"));
    ASSERT_TRUE(ws.writeRel("Contributing.md", "x"));  // alt case
    ASSERT_TRUE(ws.writeRel("security.md",     "x"));  // lowercase
    const auto r = ColdEyesEngine::derivePartition(ws.root());
    const ColdEyesEngine::Lane *contracts = nullptr;
    for (const auto &l : r.lanes) {
        if (l.name == QStringLiteral("contracts")) {
            contracts = &l; break;
        }
    }
    ASSERT_TRUE(contracts != nullptr);
    EXPECT_TRUE(contracts->docPaths.contains(QStringLiteral("Contributing.md")));
    EXPECT_TRUE(contracts->docPaths.contains(QStringLiteral("security.md")));
}

// ANTS-1571 INV-C — standards-name fallback fires when docs/standards/
// is absent AND a docs/*STANDARD*.md ≥ 100 lines exists.
TEST(ColdEyesEngine, StandardsNameGlobFallbackFiresWhenDirAbsent) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    // Build a 120-line body so the >= 100 line threshold passes.
    QString body = QStringLiteral("# Design standards\n");
    for (int i = 0; i < 120; ++i) {
        body += QStringLiteral("line %1\n").arg(i);
    }
    ASSERT_TRUE(ws.writeRel("docs/PROJECT_DESIGN_STANDARDS.md", body));
    // No docs/standards/ at all.
    const auto r = ColdEyesEngine::derivePartition(ws.root());
    const ColdEyesEngine::Lane *stds = nullptr;
    for (const auto &l : r.lanes) {
        if (l.name == QStringLiteral("standards")) {
            stds = &l; break;
        }
    }
    ASSERT_TRUE(stds != nullptr) << "standards lane should fall back to glob";
    EXPECT_TRUE(stds->docPaths.contains(
        QStringLiteral("docs/PROJECT_DESIGN_STANDARDS.md")));
    EXPECT_TRUE(stds->summary.contains(QStringLiteral("fallback")))
        << "fallback summary must declare itself; got " << stds->summary.toStdString();
}

// ANTS-1571 INV-D — sub-threshold stub files are NOT promoted.
TEST(ColdEyesEngine, StandardsNameGlobFallbackRejectsShortStubs) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    // 10-line stub, well under the 100-line floor.
    ASSERT_TRUE(ws.writeRel("docs/STYLE.md",
        QStringLiteral("# Style\nL1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\n")));
    const auto r = ColdEyesEngine::derivePartition(ws.root());
    for (const auto &l : r.lanes) {
        if (l.name == QStringLiteral("standards")) {
            FAIL() << "stub standards file (< 100 lines) was promoted";
        }
    }
}

// ANTS-1411 INV-A — non-ANTS-shape filenames (e.g. DS01.md, FP05.md,
// P04.md) get spec lanes even without a matching ROADMAP entry.
// Previously the partition would silently report zero spec lanes on
// projects whose specs don't follow `ANTS-NNNN.md` naming.
TEST(ColdEyesEngine, SpecLanesIncludeNonAntsFilenames) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    // No ROADMAP.md at all — non-ANTS specs should still surface.
    ASSERT_TRUE(ws.writeRel("docs/specs/DS01.md", "# DS01\nbody\n"));
    ASSERT_TRUE(ws.writeRel("docs/specs/FP05.md", "# FP05\nbody\n"));
    ASSERT_TRUE(ws.writeRel("docs/specs/P04.md",  "# P04\nbody\n"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    QSet<QString> names;
    for (const auto &l : r.lanes) names.insert(l.name);
    EXPECT_TRUE(names.contains(QStringLiteral("spec/DS01")))
        << "DS01.md missing from spec lanes";
    EXPECT_TRUE(names.contains(QStringLiteral("spec/FP05")));
    EXPECT_TRUE(names.contains(QStringLiteral("spec/P04")));
}

// ANTS-1411 INV-B — ANTS-NNNN.md files still gated on ROADMAP active
// set. Shipped specs must not surface as lanes — preserves the
// legacy Ants Terminal behaviour.
TEST(ColdEyesEngine, AntsShapedSpecsStillGatedByActiveRoadmap) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("ROADMAP.md", stubRoadmap({
        {1000, QStringLiteral("📋")},  // active
        {1001, QStringLiteral("✅")},  // shipped
    })));
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-1000.md", "# stub\n"));
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-1001.md", "# stub\n"));
    // Non-ANTS shape — always included.
    ASSERT_TRUE(ws.writeRel("docs/specs/GENERIC.md",   "# stub\n"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    QSet<QString> names;
    for (const auto &l : r.lanes) names.insert(l.name);
    EXPECT_TRUE(names.contains(QStringLiteral("spec/ANTS-1000")))
        << "active ANTS-1000 must surface";
    EXPECT_FALSE(names.contains(QStringLiteral("spec/ANTS-1001")))
        << "shipped ANTS-1001 must NOT surface (active filter)";
    EXPECT_TRUE(names.contains(QStringLiteral("spec/GENERIC")))
        << "non-ANTS-shape file must always surface";
}

// ANTS-1412 INV-A — malformed partition.json surfaces a warning and
// the partition falls back to the default lanes.
TEST(ColdEyesEngine, MalformedPartitionOverrideSurfacesWarning) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CLAUDE.md",  "x"));
    ASSERT_TRUE(ws.writeRel("README.md",  "x"));
    ASSERT_TRUE(ws.writeRel("ROADMAP.md", stubRoadmap({})));
    // Garbage JSON.
    ASSERT_TRUE(ws.writeRel(".cold-eyes/partition.json", "{not json"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    EXPECT_FALSE(r.overrideWarning.isEmpty())
        << "malformed JSON must set overrideWarning";
    EXPECT_TRUE(r.overrideWarning.contains(QStringLiteral("parse error"))
                || r.overrideWarning.contains(QStringLiteral("parser")))
        << "warning should name the failure: " << r.overrideWarning.toStdString();
    EXPECT_EQ(r.overridePath, QStringLiteral("<default>"))
        << "fell back to default partition";
}

// ANTS-1412 INV-B — wrong version surfaces a warning naming the
// expected schema.
TEST(ColdEyesEngine, PartitionOverrideWrongVersionSurfacesSchemaHint) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("ROADMAP.md", stubRoadmap({})));
    ASSERT_TRUE(ws.writeRel(".cold-eyes/partition.json",
        QStringLiteral("{\"version\":2, \"lanes\":[]}")));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    EXPECT_FALSE(r.overrideWarning.isEmpty());
    EXPECT_TRUE(r.overrideWarning.contains(QStringLiteral("version")))
        << "warning should name version mismatch: "
        << r.overrideWarning.toStdString();
    EXPECT_TRUE(r.overrideWarning.contains(QStringLiteral("doc_paths")))
        << "warning should embed the expected schema example";
}

// ANTS-1412 INV-C — a valid override loads cleanly with no warning.
TEST(ColdEyesEngine, ValidPartitionOverrideHasNoWarning) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("ROADMAP.md", stubRoadmap({})));
    ASSERT_TRUE(ws.writeRel("docs/custom-lane.md", "x"));
    ASSERT_TRUE(ws.writeRel(".cold-eyes/partition.json",
        QStringLiteral("{\"version\":1,\"lanes\":[{"
            "\"name\":\"custom\",\"summary\":\"x\","
            "\"doc_paths\":[\"docs/custom-lane.md\"]}]}")));
    const auto r = ColdEyesEngine::derivePartition(ws.root());
    EXPECT_TRUE(r.overrideWarning.isEmpty())
        << "clean override must not emit a warning; got "
        << r.overrideWarning.toStdString();
    EXPECT_EQ(r.overridePath, QStringLiteral(".cold-eyes/partition.json"));
    ASSERT_EQ(r.lanes.size(), 1);
    EXPECT_EQ(r.lanes[0].name, QStringLiteral("custom"));
}

// ANTS-1440 INV-A — spec-lane brief carries the parsed H1 as summary,
// not the generic "Single spec lane" placeholder.
TEST(ColdEyesEngine, SpecLaneSummaryIsParsedH1) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-9999.md",
        QStringLiteral("# ANTS-9999 — sample title with full sentence\n"
                       "\nbody body body\n")));
    ColdEyesEngine::Lane lane;
    lane.name = QStringLiteral("spec/ANTS-9999");
    lane.summary = QStringLiteral("Single spec lane");
    lane.docPaths << QStringLiteral("docs/specs/ANTS-9999.md");
    const auto m = ColdEyesEngine::assembleBriefManifest(ws.root(), lane);
    EXPECT_EQ(m.summary,
        QStringLiteral("ANTS-9999 — sample title with full sentence"));
    EXPECT_TRUE(m.brief.contains(m.summary))
        << "brief text must echo the parsed summary";
}

// ANTS-1440 INV-B — Pairs-with markers in the spec header surface as
// extra cross-reference docs (alongside the legacy contract trio).
TEST(ColdEyesEngine, SpecLanePairsWithExpandsCrossRefs) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CLAUDE.md",  "x"));
    ASSERT_TRUE(ws.writeRel("README.md",  "x"));
    ASSERT_TRUE(ws.writeRel("ROADMAP.md", stubRoadmap({})));
    ASSERT_TRUE(ws.writeRel("CHANGELOG.md", "x"));
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-1234.md",
        QStringLiteral("# ANTS-1234 — primary\n\n"
                       "**Pairs with:** ANTS-1100, ANTS-1101.\n\n"
                       "## Problem\nbody\n")));
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-1100.md", "# 1100\n"));
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-1101.md", "# 1101\n"));
    // ANTS-9000 is named in the body but not in the header — should NOT appear.
    ColdEyesEngine::Lane lane;
    lane.name = QStringLiteral("spec/ANTS-1234");
    lane.docPaths << QStringLiteral("docs/specs/ANTS-1234.md");
    const auto m = ColdEyesEngine::assembleBriefManifest(ws.root(), lane);
    EXPECT_TRUE(m.crossReferenceDocs.contains(
        QStringLiteral("docs/specs/ANTS-1100.md")));
    EXPECT_TRUE(m.crossReferenceDocs.contains(
        QStringLiteral("docs/specs/ANTS-1101.md")));
    // Legacy contract trio still present.
    EXPECT_TRUE(m.crossReferenceDocs.contains(QStringLiteral("CLAUDE.md")));
}

// ANTS-1413 INV-A — single-doc brief enumerates same-dir siblings,
// standards lane, and root contracts.
TEST(ColdEyesEngine, SingleDocBriefAssemblesNeighbourhood) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CLAUDE.md",  "x"));
    ASSERT_TRUE(ws.writeRel("README.md",  "x"));
    ASSERT_TRUE(ws.writeRel("ROADMAP.md", stubRoadmap({})));
    ASSERT_TRUE(ws.writeRel("docs/standards/coding.md", "x"));
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-9000.md",
        QStringLiteral("# ANTS-9000 — primary\nbody\n")));
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-9001.md", "# 9001\n"));
    ASSERT_TRUE(ws.writeRel("docs/specs/ANTS-9002.md", "# 9002\n"));

    const auto b = ColdEyesEngine::assembleSingleDocBrief(
        ws.root(), QStringLiteral("docs/specs/ANTS-9000.md"));
    EXPECT_EQ(b.docPath, QStringLiteral("docs/specs/ANTS-9000.md"));
    EXPECT_TRUE(b.summary.contains(QStringLiteral("ANTS-9000")));
    EXPECT_TRUE(b.sameDirSiblings.contains(
        QStringLiteral("docs/specs/ANTS-9001.md")));
    EXPECT_TRUE(b.sameDirSiblings.contains(
        QStringLiteral("docs/specs/ANTS-9002.md")));
    EXPECT_FALSE(b.sameDirSiblings.contains(
        QStringLiteral("docs/specs/ANTS-9000.md")))
        << "primary doc must not appear in its own siblings";
    EXPECT_TRUE(b.standards.contains(
        QStringLiteral("docs/standards/coding.md")));
    EXPECT_TRUE(b.rootContracts.contains(QStringLiteral("CLAUDE.md")));
    EXPECT_FALSE(b.recommendedReviewers.isEmpty())
        << "default reviewer list must be non-empty";
}

// ANTS-1571 INV-E — when docs/standards/ exists, the name-glob fallback
// is NOT applied (the canonical dir is the source of truth).
TEST(ColdEyesEngine, StandardsNameGlobFallbackSkippedWhenDirPresent) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/standards/coding.md", "x"));
    // Also drop a name-glob hit at top level — should NOT be picked up.
    QString body = QStringLiteral("# Design\n");
    for (int i = 0; i < 200; ++i) body += QStringLiteral("line\n");
    ASSERT_TRUE(ws.writeRel("docs/EXTRA_DESIGN_GUIDE.md", body));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    const ColdEyesEngine::Lane *stds = nullptr;
    for (const auto &l : r.lanes) {
        if (l.name == QStringLiteral("standards")) { stds = &l; break; }
    }
    ASSERT_TRUE(stds != nullptr);
    EXPECT_TRUE(stds->docPaths.contains(
        QStringLiteral("docs/standards/coding.md")));
    EXPECT_FALSE(stds->docPaths.contains(
        QStringLiteral("docs/EXTRA_DESIGN_GUIDE.md")))
        << "canonical docs/standards/ must win over name-glob fallback";
}

// ANTS-1619 INV-1 + INV-2 — discovered + missing contract files are
// tracked separately. A workspace with two of the four canonical
// contracts surfaces two entries in discoveredContractFiles and two
// in missingContractFiles.
TEST(ColdEyesEngine, Ants1619DiscoveredAndMissingContractsTracked) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CLAUDE.md", "# CLAUDE\n"));
    ASSERT_TRUE(ws.writeRel("README.md", "# README\n"));
    // ROADMAP.md and CHANGELOG.md intentionally absent.

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    EXPECT_TRUE(r.discoveredContractFiles.contains(QStringLiteral("CLAUDE.md")));
    EXPECT_TRUE(r.discoveredContractFiles.contains(QStringLiteral("README.md")));
    EXPECT_TRUE(r.missingContractFiles.contains(QStringLiteral("ROADMAP.md")));
    EXPECT_TRUE(r.missingContractFiles.contains(QStringLiteral("CHANGELOG.md")));
}

// ANTS-1619 INV-2 — community-contract misses surface explicitly.
// `CONTRIBUTING.md` / `SECURITY.md` / `LEGAL.md` / `CODE_OF_CONDUCT.md`
// are documented community docs the partition probes; absence is
// reported in missingContractFiles so callers see the full survey.
TEST(ColdEyesEngine, Ants1619CommunityContractMissesTracked) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CLAUDE.md", "x"));
    const auto r = ColdEyesEngine::derivePartition(ws.root());
    EXPECT_TRUE(r.missingContractFiles.contains(
        QStringLiteral("CONTRIBUTING.md")));
    EXPECT_TRUE(r.missingContractFiles.contains(
        QStringLiteral("SECURITY.md")));
    EXPECT_TRUE(r.missingContractFiles.contains(
        QStringLiteral("LEGAL.md")));
    EXPECT_TRUE(r.missingContractFiles.contains(
        QStringLiteral("CODE_OF_CONDUCT.md")));
}

// ANTS-1619 INV-3 — `data/changelog.yaml` resolves and lands in
// contracts.docPaths alongside the root CHANGELOG.md. No double-list.
TEST(ColdEyesEngine, Ants1619DataChangelogPromoted) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CHANGELOG.md",       "# CHANGELOG\n"));
    ASSERT_TRUE(ws.writeRel("data/changelog.yaml","[]\n"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    const ColdEyesEngine::Lane *c = nullptr;
    for (const auto &l : r.lanes) {
        if (l.name == QStringLiteral("contracts")) { c = &l; break; }
    }
    ASSERT_TRUE(c != nullptr);
    EXPECT_TRUE(c->docPaths.contains(QStringLiteral("CHANGELOG.md")));
    EXPECT_TRUE(c->docPaths.contains(QStringLiteral("data/changelog.yaml")));
    EXPECT_TRUE(r.discoveredContractFiles.contains(
        QStringLiteral("data/changelog.yaml")));
}

// ANTS-1619 INV-3 — `data/changelog.yaml` resolves standalone even
// when the root `CHANGELOG.md` is absent.
TEST(ColdEyesEngine, Ants1619DataChangelogStandaloneWhenRootAbsent) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CLAUDE.md",          "x"));
    ASSERT_TRUE(ws.writeRel("data/changelog.yaml","[]\n"));
    // No root CHANGELOG.md.

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    const ColdEyesEngine::Lane *c = nullptr;
    for (const auto &l : r.lanes) {
        if (l.name == QStringLiteral("contracts")) { c = &l; break; }
    }
    ASSERT_TRUE(c != nullptr);
    EXPECT_TRUE(c->docPaths.contains(QStringLiteral("data/changelog.yaml")));
}

// ANTS-1619 INV-4 — partitionSource defaults to "default" when no
// override file exists.
TEST(ColdEyesEngine, Ants1619PartitionSourceDefault) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("CLAUDE.md", "x"));
    const auto r = ColdEyesEngine::derivePartition(ws.root());
    EXPECT_EQ(r.partitionSource, QStringLiteral("default"));
}

// ANTS-1619 INV-5 — partitionSource flips to "override" when
// `.cold-eyes/partition.json` parses cleanly.
TEST(ColdEyesEngine, Ants1619PartitionSourceOverride) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/custom.md", "x"));
    ASSERT_TRUE(ws.writeRel(".cold-eyes/partition.json",
        QStringLiteral("{\"version\":1,\"lanes\":[{"
            "\"name\":\"custom\",\"summary\":\"x\","
            "\"doc_paths\":[\"docs/custom.md\"]}]}")));
    const auto r = ColdEyesEngine::derivePartition(ws.root());
    EXPECT_EQ(r.partitionSource, QStringLiteral("override"));
}

// ANTS-1619 INV-6 — partitionSource stays "default" when the
// override file exists but is malformed. overrideWarning is
// non-empty so the caller can disambiguate the four states.
TEST(ColdEyesEngine, Ants1619PartitionSourceMalformedFallsBack) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel(".cold-eyes/partition.json", "{not json"));
    const auto r = ColdEyesEngine::derivePartition(ws.root());
    EXPECT_EQ(r.partitionSource, QStringLiteral("default"));
    EXPECT_FALSE(r.overrideWarning.isEmpty());
}

// ANTS-1631 INV-2 — private/internal/fork spec dirs are walked alongside
// the canonical docs/specs/ dir.
TEST(ColdEyesEngine, Ants1631PrivateInternalForkSpecsSurface) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/private/specs/PRIVATE-1.md",
        QStringLiteral("# PRIVATE-1\n")));
    ASSERT_TRUE(ws.writeRel("docs/internal/specs/INTERNAL-1.md",
        QStringLiteral("# INTERNAL-1\n")));
    ASSERT_TRUE(ws.writeRel("docs/fork/specs/FORK-1.md",
        QStringLiteral("# FORK-1\n")));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    QStringList allDocs;
    for (const auto &l : r.lanes) allDocs.append(l.docPaths);
    EXPECT_TRUE(allDocs.contains(
        QStringLiteral("docs/private/specs/PRIVATE-1.md")));
    EXPECT_TRUE(allDocs.contains(
        QStringLiteral("docs/internal/specs/INTERNAL-1.md")));
    EXPECT_TRUE(allDocs.contains(
        QStringLiteral("docs/fork/specs/FORK-1.md")));
}

// ANTS-1631 INV-3 — when the same basename appears under two probe
// roots (a project moving specs in-flight), the lane appears once.
TEST(ColdEyesEngine, Ants1631DedupAcrossSpecRoots) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/specs/SHARED.md", "# A\n"));
    ASSERT_TRUE(ws.writeRel("docs/private/specs/SHARED.md", "# B\n"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    int sharedHits = 0;
    QStringList laneNames;
    for (const auto &l : r.lanes) {
        laneNames.append(l.name);
        for (const QString &p : l.docPaths) {
            if (p.endsWith(QStringLiteral("/SHARED.md"))) ++sharedHits;
        }
    }
    EXPECT_GT(sharedHits, 0);
    EXPECT_LE(sharedHits, 1)
        << "INV-3: SHARED.md must appear once across the probe roots; "
        << "lanes: " << laneNames.join(", ").toStdString();
}

// ANTS-1631 INV-4 — docs/engine/<sub>/spec.md (Vestige 3D Engine layout)
// surfaces in some lane.
TEST(ColdEyesEngine, Ants1631DocsEngineSpecMdSurfaces) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/engine/audio/spec.md",
        QStringLiteral("# audio\n")));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    QStringList allDocs;
    for (const auto &l : r.lanes) allDocs.append(l.docPaths);
    EXPECT_TRUE(allDocs.contains(QStringLiteral("docs/engine/audio/spec.md")));
}

// ANTS-1631 INV-5 — src/<module>/spec.md (MAME Curator layout) surfaces.
TEST(ColdEyesEngine, Ants1631SrcModuleSpecMdSurfaces) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("src/parser/spec.md", QStringLiteral("# p\n")));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    QStringList allDocs;
    for (const auto &l : r.lanes) allDocs.append(l.docPaths);
    EXPECT_TRUE(allDocs.contains(QStringLiteral("src/parser/spec.md")));
}

// ANTS-1631 INV-6 — docs/phases/*.md surfaces (phased-design doc layout).
TEST(ColdEyesEngine, Ants1631DocsPhasesSurfaces) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/phases/phase_1_design.md",
        QStringLiteral("# p1\n")));
    ASSERT_TRUE(ws.writeRel("docs/phases/2026-04-27-architecture.md",
        QStringLiteral("# arch\n")));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    QStringList allDocs;
    for (const auto &l : r.lanes) allDocs.append(l.docPaths);
    EXPECT_TRUE(allDocs.contains(
        QStringLiteral("docs/phases/phase_1_design.md")));
    EXPECT_TRUE(allDocs.contains(
        QStringLiteral("docs/phases/2026-04-27-architecture.md")));
}

// ANTS-1631 INV-7 — date-prefix YYYY-MM-DD-*.md filenames in
// docs/specs/ surface even without a matching ROADMAP entry.
TEST(ColdEyesEngine, Ants1631DatePrefixedSpecsSurface) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/specs/2026-04-27-rfc.md",
        QStringLiteral("# rfc\n")));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    QStringList allDocs;
    for (const auto &l : r.lanes) allDocs.append(l.docPaths);
    EXPECT_TRUE(allDocs.contains(
        QStringLiteral("docs/specs/2026-04-27-rfc.md")));
}

// ANTS-1631 INV-8 — deeper-than-one-level files under
// docs/engine/<sub>/sub/spec.md are NOT walked. Vendor-tree guard.
TEST(ColdEyesEngine, Ants1631DeepNestedNotSurfaced) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/engine/audio/deeper/spec.md",
        QStringLiteral("# deeper\n")));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    QStringList allDocs;
    for (const auto &l : r.lanes) allDocs.append(l.docPaths);
    EXPECT_FALSE(allDocs.contains(
        QStringLiteral("docs/engine/audio/deeper/spec.md")))
        << "INV-8: walker must be one level deep only";
}

// ANTS-1634 INV-3 — audit-infra lane fires when ≥ 2 .md files live
// under docs/audit/.
TEST(ColdEyesEngine, Ants1634AuditInfraLaneFires) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/audit/suppressions.md", "# s\n"));
    ASSERT_TRUE(ws.writeRel("docs/audit/allowlist.md",    "# a\n"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    const ColdEyesEngine::Lane *infra = nullptr;
    for (const auto &l : r.lanes) {
        if (l.name == QStringLiteral("audit-infra")) { infra = &l; break; }
    }
    ASSERT_TRUE(infra != nullptr) << "audit-infra lane missing";
    EXPECT_TRUE(infra->docPaths.contains(
        QStringLiteral("docs/audit/suppressions.md")));
    EXPECT_TRUE(infra->docPaths.contains(
        QStringLiteral("docs/audit/allowlist.md")));
}

// ANTS-1634 INV-4 — < 2 .md files: no audit-infra lane.
TEST(ColdEyesEngine, Ants1634AuditInfraLaneRespectsThreshold) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/audit/only-one.md", "# s\n"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    for (const auto &l : r.lanes) {
        EXPECT_NE(l.name, QStringLiteral("audit-infra"))
            << "INV-4: sub-threshold (1 .md) should NOT produce lane";
    }
}

// ANTS-1634 INV-5 — audit-infra falls back to docs/private/audit/ when
// docs/audit/ is absent.
TEST(ColdEyesEngine, Ants1634AuditInfraPrivatePath) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/private/audit/suppress.md", "# s\n"));
    ASSERT_TRUE(ws.writeRel("docs/private/audit/allow.md",    "# a\n"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    const ColdEyesEngine::Lane *infra = nullptr;
    for (const auto &l : r.lanes) {
        if (l.name == QStringLiteral("audit-infra")) { infra = &l; break; }
    }
    ASSERT_TRUE(infra != nullptr);
    EXPECT_TRUE(infra->docPaths.contains(
        QStringLiteral("docs/private/audit/suppress.md")));
}

// ANTS-1634 INV-6 — audit-infra walker is one level deep.
// Subdirectory files don't count toward the threshold or doc list.
TEST(ColdEyesEngine, Ants1634AuditInfraOneLevelDeep) {
    Workspace ws;
    ASSERT_TRUE(ws.valid());
    ASSERT_TRUE(ws.writeRel("docs/audit/sub/one.md", "# s\n"));
    ASSERT_TRUE(ws.writeRel("docs/audit/sub/two.md", "# s\n"));

    const auto r = ColdEyesEngine::derivePartition(ws.root());
    for (const auto &l : r.lanes) {
        EXPECT_NE(l.name, QStringLiteral("audit-infra"))
            << "INV-6: subdir .md files must not produce lane";
    }
}
