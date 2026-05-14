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
    EXPECT_EQ(m.crossReferenceDocs, (QStringList{
        QStringLiteral("CLAUDE.md"),
        QStringLiteral("README.md"),
        QStringLiteral("ROADMAP.md"),
        QStringLiteral("CHANGELOG.md"),
    }));

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
