// Feature-conformance test for ANTS-1879 — `roadmap_log op:append_batch`.
// See tests/features/mcp_roadmap_log_append_batch/spec.md and
// docs/specs/ANTS-1879.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include "remotecontrol.h"

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

namespace {


QString minimalRoadmap() {
    return QStringLiteral(
        "# Test Roadmap\n"
        "\n"
        "## Performance\n"
        "\n"
        "- 📋 [ANTS-9001] **Pre-existing bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n"
        "## Trailing Section\n"
        "\n"
        "- 📋 [ANTS-9002] **Another bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n");
}

void writeRoadmap(const QString &dir, const QString &content) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(content.toUtf8());
    f.close();
}

void writeCounter(const QString &dir, qint64 value) {
    QFile f(dir + QStringLiteral("/.roadmap-counter"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write((QString::number(value) + QChar('\n')).toUtf8());
    f.close();
}

QString readRoadmap(const QString &dir) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

qint64 readCounter(const QString &dir) {
    QFile f(dir + QStringLiteral("/.roadmap-counter"));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    return QString::fromUtf8(f.readAll().trimmed()).toLongLong();
}

QJsonObject bullet(const char *headline,
                   const char *status = "planned",
                   const char *kind = "implement",
                   const char *source = "test") {
    QJsonObject b;
    b["headline"] = QString::fromLatin1(headline);
    b["status"]   = QString::fromLatin1(status);
    b["kind"]     = QString::fromLatin1(kind);
    b["source"]   = QString::fromLatin1(source);
    return b;
}

QJsonObject baseReq(const QString &dir, const QJsonArray &bullets) {
    QJsonObject r;
    r["caller_cwd"] = dir;
    r["op"]         = QStringLiteral("append_batch");
    r["section"]    = QStringLiteral("performance");
    r["bullets"]    = bullets;
    return r;
}

void setupProject(QTemporaryDir &dir, qint64 counter = 9100) {
    writeRoadmap(dir.path(), minimalRoadmap());
    writeCounter(dir.path(), counter);
}

}  // namespace

// INV-1 — dispatch wired.
TEST(McpRoadmapLogAppendBatch, Inv1DispatchWired) {
    const std::string src = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    EXPECT_NE(src.find("\"append_batch\""), std::string::npos);
    EXPECT_NE(src.find("cmdRoadmapLogAppendBatch("), std::string::npos);
}

// INV-2 — required fields.
TEST(McpRoadmapLogAppendBatch, Inv2MissingFieldsRefuse) {
    QTemporaryDir dir;
    setupProject(dir);
    RemoteControl rc(nullptr);
    // Missing bullets[]
    {
        auto req = baseReq(dir.path(), {});
        req.remove("bullets");
        const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
        EXPECT_FALSE(out["ok"].toBool());
        EXPECT_EQ(out["code"].toString(), QStringLiteral("missing_field"));
    }
    // Empty bullets[]
    {
        const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
            baseReq(dir.path(), {})).object();
        EXPECT_EQ(out["code"].toString(), QStringLiteral("missing_field"));
    }
    // Missing section
    {
        QJsonArray bs; bs.append(bullet("Foo."));
        auto req = baseReq(dir.path(), bs);
        req.remove("section");
        const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
        EXPECT_EQ(out["code"].toString(), QStringLiteral("missing_field"));
    }
}

// INV-3 — mixed validity: 1 accepted + 1 skipped per failure code.
TEST(McpRoadmapLogAppendBatch, Inv3MixedValidityHeadlineEmpty) {
    QTemporaryDir dir;
    setupProject(dir);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("Good bullet."));
    QJsonObject bad = bullet("anything");
    bad.remove("headline");  // headline_empty
    bs.append(bad);
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool()) << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out["applied_count"].toInt(), 1);
    EXPECT_EQ(out["skipped_count"].toInt(), 1);
    const QJsonArray skipped = out["skipped"].toArray();
    ASSERT_EQ(skipped.size(), 1);
    EXPECT_EQ(skipped[0].toObject()["bullet_index"].toInt(), 1);
    EXPECT_EQ(skipped[0].toObject()["code"].toString(),
              QStringLiteral("headline_empty"));
}

TEST(McpRoadmapLogAppendBatch, Inv3MixedValidityBadStatus) {
    QTemporaryDir dir;
    setupProject(dir);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("Good."));
    bs.append(bullet("Bad.", "weird_status"));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["applied_count"].toInt(), 1);
    EXPECT_EQ(out["skipped"].toArray()[0].toObject()["code"].toString(),
              QStringLiteral("bad_status"));
}

TEST(McpRoadmapLogAppendBatch, Inv3MixedValidityBadKind) {
    QTemporaryDir dir;
    setupProject(dir);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("Good."));
    bs.append(bullet("Bad.", "planned", "weird_kind"));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["applied_count"].toInt(), 1);
    EXPECT_EQ(out["skipped"].toArray()[0].toObject()["code"].toString(),
              QStringLiteral("bad_kind"));
}

// INV-4 — all skipped → ok:true with empty ids, files untouched.
TEST(McpRoadmapLogAppendBatch, Inv4AllSkipped) {
    QTemporaryDir dir;
    setupProject(dir);
    const QString preRoadmap = readRoadmap(dir.path());
    const qint64  preCounter = readCounter(dir.path());

    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("X.", "weird1"));
    bs.append(bullet("Y.", "weird2"));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    EXPECT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["applied_count"].toInt(), 0);
    EXPECT_EQ(out["skipped_count"].toInt(), 2);
    EXPECT_EQ(out["ids"].toArray().size(), 0);

    EXPECT_EQ(readRoadmap(dir.path()), preRoadmap)
        << "ROADMAP.md must be untouched when all bullets skipped";
    EXPECT_EQ(readCounter(dir.path()), preCounter)
        << ".roadmap-counter must be untouched when all bullets skipped";
}

// INV-5 — contiguous counter allocation.
TEST(McpRoadmapLogAppendBatch, Inv5ContiguousCounter) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9100);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    for (int i = 0; i < 3; ++i)
        bs.append(bullet(("Bullet " + QString::number(i) + ".").toUtf8().constData()));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool()) << QJsonDocument(out).toJson().toStdString();
    const QJsonArray ids = out["ids"].toArray();
    ASSERT_EQ(ids.size(), 3);
    EXPECT_EQ(ids[0].toString(), QStringLiteral("ANTS-9101"));
    EXPECT_EQ(ids[1].toString(), QStringLiteral("ANTS-9102"));
    EXPECT_EQ(ids[2].toString(), QStringLiteral("ANTS-9103"));
    EXPECT_EQ(readCounter(dir.path()), 9103);
}

// INV-6 — id_hint only honoured on the first bullet.
TEST(McpRoadmapLogAppendBatch, Inv6IdHintOnFirstBulletOnly) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9100);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    QJsonObject b0 = bullet("First.");
    b0["id_hint"] = 9500;
    bs.append(b0);
    QJsonObject b1 = bullet("Second.");
    b1["id_hint"] = 9700;   // SHOULD BE IGNORED
    bs.append(b1);
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool());
    const QJsonArray ids = out["ids"].toArray();
    ASSERT_EQ(ids.size(), 2);
    EXPECT_EQ(ids[0].toString(), QStringLiteral("ANTS-9500"));
    EXPECT_EQ(ids[1].toString(), QStringLiteral("ANTS-9501"));  // first_id+1
}

// INV-7 — order preservation: bullets appear in input order in
// the spliced output.
TEST(McpRoadmapLogAppendBatch, Inv7OrderPreservation) {
    QTemporaryDir dir;
    setupProject(dir);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("Alpha bullet."));
    bs.append(bullet("Beta bullet."));
    bs.append(bullet("Gamma bullet."));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool());

    const QString roadmap = readRoadmap(dir.path());
    const int alpha = roadmap.indexOf(QStringLiteral("Alpha bullet"));
    const int beta  = roadmap.indexOf(QStringLiteral("Beta bullet"));
    const int gamma = roadmap.indexOf(QStringLiteral("Gamma bullet"));
    ASSERT_NE(alpha, -1);
    ASSERT_NE(beta,  -1);
    ASSERT_NE(gamma, -1);
    EXPECT_LT(alpha, beta);
    EXPECT_LT(beta, gamma);
}

// INV-9 — unrecognised_format short-circuits the whole batch.
TEST(McpRoadmapLogAppendBatch, Inv9UnrecognisedFormatShortCircuits) {
    QTemporaryDir dir;
    // Make a large file with NO parseable bullets.
    QString garbage = QStringLiteral("# Roadmap\n\nNo bullets here.\n");
    while (garbage.size() < 2000) garbage += QStringLiteral("filler line\n");
    writeRoadmap(dir.path(), garbage);
    writeCounter(dir.path(), 9100);

    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("X."));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    EXPECT_FALSE(out["ok"].toBool());
    EXPECT_EQ(out["code"].toString(),
              QStringLiteral("unrecognised_format"));
}

// Bad section short-circuits the batch.
TEST(McpRoadmapLogAppendBatch, BadSectionShortCircuits) {
    QTemporaryDir dir;
    setupProject(dir);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("X."));
    auto req = baseReq(dir.path(), bs);
    req["section"] = QStringLiteral("no-such-section");
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    EXPECT_EQ(out["code"].toString(), QStringLiteral("bad_section"));
}

// INV-10 — formatRoadmapBullet is shared between append and append_batch.
TEST(McpRoadmapLogAppendBatch, Inv10FormatHelperShared) {
    const std::string src = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // Definition + at least 2 call sites total.
    const auto defPos =
        src.find("QString RemoteControl::formatRoadmapBullet(");
    ASSERT_NE(defPos, std::string::npos)
        << "formatRoadmapBullet helper must exist";

    // Find call sites of `formatRoadmapBullet(` excluding the definition line
    // (which is the only occurrence inside the QString signature).
    int callSites = 0;
    size_t pos = 0;
    while ((pos = src.find("formatRoadmapBullet(", pos)) != std::string::npos) {
        // Skip the definition itself (the substring starting `QString RemoteControl::formatRoadmapBullet(`).
        const size_t header = src.rfind("QString RemoteControl::", pos);
        const bool isDefinitionLine =
            (header != std::string::npos && pos - header < 50);
        if (!isDefinitionLine) ++callSites;
        pos += 5;
    }
    EXPECT_GE(callSites, 2)
        << "formatRoadmapBullet must be called from at least 2 sites "
           "(cmdRoadmapLogAppend + cmdRoadmapLogAppendBatch); found "
        << callSites;

    // Both handler names must reference the helper inside their bodies.
    const auto appendPos = src.find(
        "QJsonDocument RemoteControl::cmdRoadmapLogAppend(");
    const auto batchPos = src.find(
        "QJsonDocument RemoteControl::cmdRoadmapLogAppendBatch(");
    ASSERT_NE(appendPos, std::string::npos);
    ASSERT_NE(batchPos,  std::string::npos);
    // Slice each handler to its next handler boundary.
    const auto appendEnd = src.find(
        "QJsonDocument RemoteControl::", appendPos + 20);
    const auto batchEnd = src.find(
        "QJsonDocument RemoteControl::", batchPos + 20);
    const std::string appendBody =
        src.substr(appendPos, (appendEnd == std::string::npos ? src.size() - appendPos
                                                              : appendEnd - appendPos));
    const std::string batchBody =
        src.substr(batchPos, (batchEnd == std::string::npos ? src.size() - batchPos
                                                            : batchEnd - batchPos));
    EXPECT_NE(appendBody.find("formatRoadmapBullet("), std::string::npos)
        << "cmdRoadmapLogAppend must call formatRoadmapBullet";
    EXPECT_NE(batchBody.find("formatRoadmapBullet("), std::string::npos)
        << "cmdRoadmapLogAppendBatch must call formatRoadmapBullet";
}

// Success envelope shape.
TEST(McpRoadmapLogAppendBatch, SuccessEnvelopeShape) {
    QTemporaryDir dir;
    setupProject(dir);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("One."));
    bs.append(bullet("Two."));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["applied_count"].toInt(), 2);
    EXPECT_EQ(out["ids"].toArray().size(), 2);
    EXPECT_EQ(out["lines"].toArray().size(), 2);
    EXPECT_GT(out["bytes_written"].toInt(), 0);
    EXPECT_EQ(out["file"].toString(), QStringLiteral("ROADMAP.md"));
}

// ───────────────────────────────────────────────────────────────────
// ANTS-2054 — op:append / op:append_batch infer the project's own
// counter prefix from the existing roadmap instead of hardcoding
// "ANTS-". A project whose bullets are `mame-curator-NNNN` must get
// `mame-curator-NNNN` back, never a mixed-in `ANTS-NNNN`.
// ───────────────────────────────────────────────────────────────────

QString mameCuratorRoadmap() {
    return QStringLiteral(
        "# Test Roadmap\n"
        "\n"
        "## Performance\n"
        "\n"
        "- 📋 [mame-curator-1065] **Pre-existing bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n"
        "- 📋 [mame-curator-1066] **Second bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n");
}

TEST(McpRoadmapLogAppendBatch, Ants2054BatchInfersProjectPrefix) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), mameCuratorRoadmap());
    writeCounter(dir.path(), 1072);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("New one."));
    bs.append(bullet("New two."));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    const QJsonArray ids = out["ids"].toArray();
    ASSERT_EQ(ids.size(), 2);
    EXPECT_EQ(ids[0].toString(), QStringLiteral("mame-curator-1073"));
    EXPECT_EQ(ids[1].toString(), QStringLiteral("mame-curator-1074"));
    // The rendered bullet text carries the project prefix, not ANTS.
    const QString roadmap = readRoadmap(dir.path());
    EXPECT_NE(roadmap.indexOf(QStringLiteral("[mame-curator-1073]")), -1);
    EXPECT_EQ(roadmap.indexOf(QStringLiteral("[ANTS-1073]")), -1);
}

TEST(McpRoadmapLogAppendBatch, Ants2054SingleInfersProjectPrefix) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), mameCuratorRoadmap());
    writeCounter(dir.path(), 1072);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = dir.path();
    req["op"]         = QStringLiteral("append");
    req["section"]    = QStringLiteral("performance");
    req["headline"]   = QStringLiteral("Single new bullet.");
    req["kind"]       = QStringLiteral("implement");
    req["source"]     = QStringLiteral("test");
    req["status"]     = QStringLiteral("planned");
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out["id"].toString(), QStringLiteral("mame-curator-1073"));
}

// Back-compat — the canonical ANTS roadmap still renders ANTS- ids.
TEST(McpRoadmapLogAppendBatch, Ants2054BackCompatAntsRoadmap) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9100);  // minimalRoadmap() has ANTS ids
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("Stay ants."));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["ids"].toArray()[0].toString(),
              QStringLiteral("ANTS-9101"));
}

// ───────────────────────────────────────────────────────────────────
// ANTS-2055 — op:append / op:append_batch into a parent `##` section
// whose body is `###` subsections must refuse (section_has_subsections)
// rather than orphan the bullet past the last child + closing `---`.
// ───────────────────────────────────────────────────────────────────

QString milestoneWithChildrenRoadmap() {
    return QStringLiteral(
        "# Test Roadmap\n"
        "\n"
        "## 1.3.0 Milestone\n"
        "\n"
        "### Alpha subsection\n"
        "\n"
        "- 📋 [ANTS-9001] **Child bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n"
        "---\n"
        "\n"
        "## 1.4.0 Next\n"
        "\n"
        "- 📋 [ANTS-9002] **Leaf bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n");
}

TEST(McpRoadmapLogAppendBatch, Ants2055RefusesParentSectionWithChildren) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), milestoneWithChildrenRoadmap());
    writeCounter(dir.path(), 9100);
    RemoteControl rc(nullptr);
    QJsonArray bs; bs.append(bullet("X."));
    auto req = baseReq(dir.path(), bs);
    req["section"] = QStringLiteral("1-3-0-milestone");  // has ### child
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    EXPECT_FALSE(out["ok"].toBool());
    EXPECT_EQ(out["code"].toString(),
              QStringLiteral("section_has_subsections"));
    const QJsonArray kids = out["child_slugs"].toArray();
    ASSERT_EQ(kids.size(), 1);
    EXPECT_EQ(kids[0].toString(), QStringLiteral("alpha-subsection"));
    EXPECT_EQ(readCounter(dir.path()), 9100)
        << ".roadmap-counter must be untouched on refusal";
}

TEST(McpRoadmapLogAppendBatch, Ants2055SingleRefusesParentSection) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), milestoneWithChildrenRoadmap());
    writeCounter(dir.path(), 9100);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = dir.path();
    req["op"]         = QStringLiteral("append");
    req["section"]    = QStringLiteral("1-3-0-milestone");
    req["headline"]   = QStringLiteral("Nope.");
    req["kind"]       = QStringLiteral("implement");
    req["source"]     = QStringLiteral("test");
    req["status"]     = QStringLiteral("planned");
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();
    EXPECT_FALSE(out["ok"].toBool());
    EXPECT_EQ(out["code"].toString(),
              QStringLiteral("section_has_subsections"));
}

// Leaf section (no children) still appends normally — no regression.
TEST(McpRoadmapLogAppendBatch, Ants2055LeafSectionStillAppends) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), milestoneWithChildrenRoadmap());
    writeCounter(dir.path(), 9100);
    RemoteControl rc(nullptr);
    QJsonArray bs; bs.append(bullet("Leaf append."));
    auto req = baseReq(dir.path(), bs);
    req["section"] = QStringLiteral("1-4-0-next");  // leaf
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out["applied_count"].toInt(), 1);
}

// ───────────────────────────────────────────────────────────────────
// ANTS-2078 — per-bullet stable_id for custom-prefix bulk inserts.
// id_strategy:"stable_prefix" skips .roadmap-counter entirely; each
// bullet carries its own full ID string written verbatim.
// ───────────────────────────────────────────────────────────────────

QJsonObject stableBullet(const char *headline, const char *stableId) {
    QJsonObject b = bullet(headline);
    b["stable_id"] = QString::fromLatin1(stableId);
    return b;
}

TEST(McpRoadmapLogAppendBatch, Ants2078StablePrefixWritesIdsVerbatim) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9100);   // counter present but must be ignored
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(stableBullet("Alpha.", "Ts20-SP6"));
    bs.append(stableBullet("Beta.",  "Ts20-SP7"));
    auto req = baseReq(dir.path(), bs);
    req["id_strategy"] = QStringLiteral("stable_prefix");
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    const QJsonArray ids = out["ids"].toArray();
    ASSERT_EQ(ids.size(), 2);
    EXPECT_EQ(ids[0].toString(), QStringLiteral("Ts20-SP6"));
    EXPECT_EQ(ids[1].toString(), QStringLiteral("Ts20-SP7"));
    const QString roadmap = readRoadmap(dir.path());
    EXPECT_NE(roadmap.indexOf(QStringLiteral("[Ts20-SP6]")), -1);
    EXPECT_NE(roadmap.indexOf(QStringLiteral("[Ts20-SP7]")), -1);
    // Counter MUST be untouched under stable_prefix.
    EXPECT_EQ(readCounter(dir.path()), 9100)
        << ".roadmap-counter must not move under stable_prefix";
}

TEST(McpRoadmapLogAppendBatch, Ants2078StablePrefixMissingIdSkips) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9100);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(stableBullet("Has id.", "Ts20-SP6"));
    bs.append(bullet("No stable_id."));   // missing stable_id → skipped
    auto req = baseReq(dir.path(), bs);
    req["id_strategy"] = QStringLiteral("stable_prefix");
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["applied_count"].toInt(), 1);
    ASSERT_EQ(out["skipped"].toArray().size(), 1);
    EXPECT_EQ(out["skipped"].toArray()[0].toObject()["code"].toString(),
              QStringLiteral("missing_field"));
}

TEST(McpRoadmapLogAppendBatch, Ants2078StablePrefixIntraBatchDupSkips) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9100);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(stableBullet("First.",     "Ts20-SP6"));
    bs.append(stableBullet("Duplicate.", "Ts20-SP6"));   // same id → id_taken
    auto req = baseReq(dir.path(), bs);
    req["id_strategy"] = QStringLiteral("stable_prefix");
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["applied_count"].toInt(), 1);
    EXPECT_EQ(out["skipped"].toArray()[0].toObject()["code"].toString(),
              QStringLiteral("id_taken"));
}

TEST(McpRoadmapLogAppendBatch, Ants2078BadIdStrategyRefuses) {
    QTemporaryDir dir;
    setupProject(dir);
    RemoteControl rc(nullptr);
    QJsonArray bs; bs.append(bullet("X."));
    auto req = baseReq(dir.path(), bs);
    req["id_strategy"] = QStringLiteral("nonsense");
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    EXPECT_FALSE(out["ok"].toBool());
    EXPECT_EQ(out["code"].toString(), QStringLiteral("bad_args"));
}

// ───────────────────────────────────────────────────────────────────
// ANTS-2080 — return:"headline_only" echoes the just-written bullets in
// compact {id, status, headline_oneline} form (confirm-after).
// ───────────────────────────────────────────────────────────────────

TEST(McpRoadmapLogAppendBatch, Ants2080BatchReturnHeadlineOnly) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9100);
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("First echo."));
    bs.append(bullet("Second echo.", "in-progress"));
    auto req = baseReq(dir.path(), bs);
    req["return"] = QStringLiteral("headline_only");
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool());
    const QJsonArray pb = out["post_bullets"].toArray();
    ASSERT_EQ(pb.size(), 2);
    EXPECT_EQ(pb[0].toObject()["id"].toString(), QStringLiteral("ANTS-9101"));
    EXPECT_EQ(pb[0].toObject()["status"].toString(), QStringLiteral("planned"));
    EXPECT_EQ(pb[0].toObject()["headline_oneline"].toString(),
              QStringLiteral("First echo."));
    EXPECT_EQ(pb[1].toObject()["status"].toString(),
              QStringLiteral("in-progress"));
}

TEST(McpRoadmapLogAppendBatch, Ants2080OmittedReturnNoPostBullets) {
    QTemporaryDir dir;
    setupProject(dir);
    RemoteControl rc(nullptr);
    QJsonArray bs; bs.append(bullet("Lean."));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_FALSE(out.contains("post_bullets"))
        << "post_bullets must be absent when return is omitted";
}

TEST(McpRoadmapLogAppendBatch, Ants2080SingleAppendReturnHeadlineOnly) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9100);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = dir.path();
    req["op"]         = QStringLiteral("append");
    req["section"]    = QStringLiteral("performance");
    req["headline"]   = QStringLiteral("Single echo.");
    req["kind"]       = QStringLiteral("implement");
    req["source"]     = QStringLiteral("test");
    req["status"]     = QStringLiteral("planned");
    req["return"]     = QStringLiteral("headline_only");
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    const QJsonArray pb = out["post_bullets"].toArray();
    ASSERT_EQ(pb.size(), 1);
    EXPECT_EQ(pb[0].toObject()["id"].toString(), QStringLiteral("ANTS-9101"));
    EXPECT_EQ(pb[0].toObject()["headline_oneline"].toString(),
              QStringLiteral("Single echo."));
}

// ───────────────────────────────────────────────────────────────────
// ANTS-2179 — the .roadmap-counter is only a hint. When it LAGS the
// file's true max [PREFIX-NNNN] id, counter+1 must not reissue a live id
// (never-reuse invariant). Reconcile against the file max and self-heal
// the counter. minimalRoadmap()'s max id is ANTS-9002.
// ───────────────────────────────────────────────────────────────────

QJsonObject singleAppendReq(const QString &dir) {
    QJsonObject req;
    req["caller_cwd"] = dir;
    req["op"]         = QStringLiteral("append");
    req["section"]    = QStringLiteral("performance");
    req["headline"]   = QStringLiteral("Reconcile me.");
    req["kind"]       = QStringLiteral("implement");
    req["source"]     = QStringLiteral("test");
    req["status"]     = QStringLiteral("planned");
    return req;
}

TEST(McpRoadmapLogAppendBatch, Ants2179SingleReconcilesLaggingCounter) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9000);   // BELOW the file max of 9002
    RemoteControl rc(nullptr);
    const QJsonObject out =
        rc.cmdRoadmapLogAppendForTest(singleAppendReq(dir.path())).object();
    ASSERT_TRUE(out["ok"].toBool()) << QJsonDocument(out).toJson().toStdString();
    // 9000+1 would be ANTS-9001 — a live id. Must skip past the file max.
    EXPECT_EQ(out["id"].toString(), QStringLiteral("ANTS-9003"));
    EXPECT_EQ(out["counter_advanced_to"].toInt(), 9003);
    EXPECT_EQ(readCounter(dir.path()), 9003)
        << ".roadmap-counter must self-heal to the new high-water";
}

TEST(McpRoadmapLogAppendBatch, Ants2179BatchReconcilesLaggingCounter) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9000);   // BELOW the file max of 9002
    RemoteControl rc(nullptr);
    QJsonArray bs;
    bs.append(bullet("First."));
    bs.append(bullet("Second."));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool()) << QJsonDocument(out).toJson().toStdString();
    const QJsonArray ids = out["ids"].toArray();
    ASSERT_EQ(ids.size(), 2);
    EXPECT_EQ(ids[0].toString(), QStringLiteral("ANTS-9003"));   // not 9001
    EXPECT_EQ(ids[1].toString(), QStringLiteral("ANTS-9004"));   // not 9002
    EXPECT_EQ(out["counter_advanced_to"].toInt(), 9004);
    EXPECT_EQ(readCounter(dir.path()), 9004);
    // No duplicate ANTS-9001/9002 bullets appeared.
    const QString roadmap = readRoadmap(dir.path());
    EXPECT_EQ(roadmap.count(QStringLiteral("[ANTS-9001]")), 1);
    EXPECT_EQ(roadmap.count(QStringLiteral("[ANTS-9002]")), 1);
}

// No-regression: a counter AHEAD of the file max allocates from the
// counter and never flags a reconcile.
TEST(McpRoadmapLogAppendBatch, Ants2179NoReconcileWhenCounterAhead) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9100);   // ABOVE the file max of 9002
    RemoteControl rc(nullptr);
    QJsonArray bs; bs.append(bullet("Ahead."));
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(
        baseReq(dir.path(), bs)).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["ids"].toArray()[0].toString(), QStringLiteral("ANTS-9101"));
    EXPECT_FALSE(out.contains("counter_advanced_to"))
        << "counter_advanced_to must be absent when the counter leads the file";
}

// An explicit id_hint that collides with a live id the lagging counter
// never knew about is refused, not silently written as a duplicate.
TEST(McpRoadmapLogAppendBatch, Ants2179SingleIdHintCollisionRefused) {
    QTemporaryDir dir;
    setupProject(dir, /*counter=*/9000);   // lags; file max is 9002
    RemoteControl rc(nullptr);
    QJsonObject req = singleAppendReq(dir.path());
    req["id_hint"] = 9001;                  // > counter 9000, but ALIVE
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();
    EXPECT_FALSE(out["ok"].toBool());
    EXPECT_EQ(out["code"].toString(), QStringLiteral("id_taken"));
    EXPECT_EQ(readCounter(dir.path()), 9000)
        << ".roadmap-counter must be untouched on a refused collision";
}
