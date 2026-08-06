// Feature-conformance test for ANTS-1878 — `roadmap_log op:create_section`.
// See tests/features/mcp_roadmap_log_create_section/spec.md and
// docs/specs/ANTS-1878.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <cstdio>
#include "remotecontrol.h"

#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

namespace {


// Minimal roadmap: one ## section + a placeholder bullet so the parser
// recognises ants-v1 format and lineEnd is well-defined.
QString minimalRoadmap() {
    return QStringLiteral(
        "# Test Roadmap\n"
        "\n"
        "## Performance\n"
        "\n"
        "- 📋 [ANTS-9001] **Existing bullet.**\n"
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

QString readRoadmap(const QString &dir) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QJsonObject baseReq(const QString &dir) {
    QJsonObject r;
    r["caller_cwd"]    = dir;
    r["op"]            = QStringLiteral("create_section");
    r["after_section"] = QStringLiteral("performance");
    r["level"]         = 2;
    r["title"]         = QStringLiteral("Phase 10.6: Multi-Threading & Concurrency");
    return r;
}

}  // namespace

// INV-1 — dispatch source-grep.
TEST(McpRoadmapLogCreateSection, Inv1DispatchWiredInCmdRoadmapLog) {
    const std::string src = ants_test::slurpRemoteControl();
    EXPECT_NE(src.find("\"create_section\""), std::string::npos)
        << "cmdRoadmapLog must dispatch op:create_section";
    EXPECT_NE(src.find("cmdRoadmapLogCreateSection("), std::string::npos)
        << "the handler must be called from the dispatch ladder";
}

// INV-2 — required fields.
TEST(McpRoadmapLogCreateSection, Inv2MissingFieldsRefuse) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), minimalRoadmap());
    RemoteControl rc(nullptr);
    // Missing caller_cwd
    {
        auto req = baseReq(dir.path());
        req.remove("caller_cwd");
        const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(req).object();
        EXPECT_FALSE(out["ok"].toBool());
        EXPECT_EQ(out["code"].toString(), QStringLiteral("missing_field"));
    }
    // Missing after_section
    {
        auto req = baseReq(dir.path());
        req.remove("after_section");
        const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(req).object();
        EXPECT_EQ(out["code"].toString(), QStringLiteral("missing_field"));
    }
    // Missing level
    {
        auto req = baseReq(dir.path());
        req.remove("level");
        const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(req).object();
        EXPECT_EQ(out["code"].toString(), QStringLiteral("missing_field"));
    }
    // Missing title
    {
        auto req = baseReq(dir.path());
        req.remove("title");
        const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(req).object();
        EXPECT_EQ(out["code"].toString(), QStringLiteral("missing_field"));
    }
}

// INV-3 — bad_level.
TEST(McpRoadmapLogCreateSection, Inv3BadLevel) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), minimalRoadmap());
    RemoteControl rc(nullptr);
    for (int badLevel : {1, 4, 5, 0, -1}) {
        auto req = baseReq(dir.path());
        req["level"] = badLevel;
        const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(req).object();
        EXPECT_FALSE(out["ok"].toBool()) << "level=" << badLevel;
        EXPECT_EQ(out["code"].toString(), QStringLiteral("bad_level"))
            << "level=" << badLevel;
    }
}

// INV-4 — bad_section + bad_case.
TEST(McpRoadmapLogCreateSection, Inv4BadSectionAndBadCase) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), minimalRoadmap());
    RemoteControl rc(nullptr);
    // Totally unknown.
    {
        auto req = baseReq(dir.path());
        req["after_section"] = QStringLiteral("does-not-exist");
        const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(req).object();
        EXPECT_EQ(out["code"].toString(), QStringLiteral("bad_section"));
    }
    // Case-only mismatch.
    {
        auto req = baseReq(dir.path());
        req["after_section"] = QStringLiteral("Performance");  // capital P
        const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(req).object();
        EXPECT_EQ(out["code"].toString(), QStringLiteral("bad_case"));
        EXPECT_EQ(out["canonical_slug"].toString(),
                  QStringLiteral("performance"));
    }
}

// INV-5 — slug computed via slugifyHeading. Behavioural assertion.
TEST(McpRoadmapLogCreateSection, Inv5SlugComputation) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), minimalRoadmap());
    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(baseReq(dir.path())).object();
    ASSERT_TRUE(out["ok"].toBool()) << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out["slug"].toString(),
              QStringLiteral("phase-10-6-multi-threading-concurrency"));
}

// INV-6 — slug_collision.
TEST(McpRoadmapLogCreateSection, Inv6SlugCollision) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), minimalRoadmap());
    RemoteControl rc(nullptr);
    auto req = baseReq(dir.path());
    req["title"] = QStringLiteral("Performance");  // would compute "performance"
    const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(req).object();
    EXPECT_FALSE(out["ok"].toBool());
    EXPECT_EQ(out["code"].toString(), QStringLiteral("slug_collision"));
    EXPECT_EQ(out["computed_slug"].toString(), QStringLiteral("performance"));
}

// INV-7 — line is 1-based heading line; heading appears verbatim at
// that line in the on-disk file.
TEST(McpRoadmapLogCreateSection, Inv7LineAndHeadingPosition) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), minimalRoadmap());
    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(baseReq(dir.path())).object();
    ASSERT_TRUE(out["ok"].toBool());
    const int line = out["line"].toInt();
    ASSERT_GT(line, 0);

    const QString updated = readRoadmap(dir.path());
    const QStringList lines = updated.split('\n');
    ASSERT_GE(lines.size(), line);
    EXPECT_EQ(lines[line - 1],
              QStringLiteral("## Phase 10.6: Multi-Threading & Concurrency"));
}

// INV-7b — heading inserted at lineEnd boundary (immediately before
// the next ## heading).
TEST(McpRoadmapLogCreateSection, Inv7HeadingInsertedAtSectionEnd) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), minimalRoadmap());
    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(baseReq(dir.path())).object();
    ASSERT_TRUE(out["ok"].toBool());

    const QString updated = readRoadmap(dir.path());
    // New heading appears BEFORE "## Trailing Section".
    const int newIdx   = updated.indexOf(QStringLiteral("## Phase 10.6"));
    const int trailIdx = updated.indexOf(QStringLiteral("## Trailing Section"));
    ASSERT_NE(newIdx, -1);
    ASSERT_NE(trailIdx, -1);
    EXPECT_LT(newIdx, trailIdx) << "new heading must come before next section";
}

// INV-8 — source-grep: handler uses QSaveFile::commit().
TEST(McpRoadmapLogCreateSection, Inv8AtomicWriteViaQSaveFile) {
    const std::string src = ants_test::slurpRemoteControl();
    // Find the handler block and confirm QSaveFile … commit() pair appears.
    const auto handlerPos = src.find("cmdRoadmapLogCreateSection(");
    ASSERT_NE(handlerPos, std::string::npos);
    // Slice from the definition to the next handler definition (or EOF).
    const auto defPos = src.find(
        "QJsonDocument RemoteControl::cmdRoadmapLogCreateSection(");
    ASSERT_NE(defPos, std::string::npos);
    const auto nextDef = src.find("QJsonDocument RemoteControl::", defPos + 20);
    const std::string body =
        src.substr(defPos, (nextDef == std::string::npos ? src.size() - defPos
                                                          : nextDef - defPos));
    EXPECT_NE(body.find("QSaveFile"),  std::string::npos)
        << "handler must use QSaveFile for atomic write";
    EXPECT_NE(body.find(".commit()"),  std::string::npos)
        << "handler must call QSaveFile::commit()";
}

// INV-10 — bad_intro positive case.
TEST(McpRoadmapLogCreateSection, Inv10BadIntroPositive) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), minimalRoadmap());
    RemoteControl rc(nullptr);
    auto req = baseReq(dir.path());
    req["intro_body"] = QStringLiteral("## stray heading inside intro");
    const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(req).object();
    EXPECT_FALSE(out["ok"].toBool());
    EXPECT_EQ(out["code"].toString(), QStringLiteral("bad_intro"));
}

// INV-10 — bad_intro negative case (#1234 is NOT a heading).
TEST(McpRoadmapLogCreateSection, Inv10BadIntroNegativeHashref) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), minimalRoadmap());
    RemoteControl rc(nullptr);
    auto req = baseReq(dir.path());
    req["intro_body"] = QStringLiteral("#1234 ticket reference is allowed");
    const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(req).object();
    EXPECT_TRUE(out["ok"].toBool())
        << "#1234 must NOT trigger bad_intro: "
        << QJsonDocument(out).toJson().toStdString();
}

// Success envelope shape.
TEST(McpRoadmapLogCreateSection, SuccessEnvelopeShape) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), minimalRoadmap());
    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(baseReq(dir.path())).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["file"].toString(), QStringLiteral("ROADMAP.md"));
    EXPECT_TRUE(out.contains("slug"));
    EXPECT_TRUE(out.contains("line"));
    EXPECT_TRUE(out.contains("bytes_written"));
    EXPECT_GT(out["bytes_written"].toInt(), 0);
}
