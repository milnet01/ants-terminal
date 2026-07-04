// Feature-conformance test for ANTS-1691 — `roadmap_log op:bundle_row`.
// See tests/features/roadmap_log_bundle_row/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include "remotecontrol.h"

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

namespace {

// Roadmap with a populated "## 📊 Bundle progress" table plus a trailing
// section, so table-boundary detection has a following heading to stop on.
QString roadmapWithTable() {
    return QStringLiteral(
        "# Test Roadmap\n"
        "\n"
        "## 📊 Bundle progress\n"
        "\n"
        "| Bundle | Commit | Theme | Sites |\n"
        "| --- | --- | --- | --- |\n"
        "| 9 | aaa111 | early | a, b |\n"
        "| 78 | bbb222 | later | c |\n"
        "\n"
        "## Trailing Section\n"
        "\n"
        "- 📋 [ANTS-9002] **Another bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n");
}

// Roadmap whose Bundle-progress section has a heading but NO table yet.
QString roadmapNoTable() {
    return QStringLiteral(
        "# Test Roadmap\n"
        "\n"
        "## 📊 Bundle progress\n"
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

QJsonArray arr(const QStringList &items) {
    QJsonArray a;
    for (const QString &s : items) a << s;
    return a;
}

QJsonObject baseReq(const QString &dir, const QStringList &cells) {
    QJsonObject r;
    r["caller_cwd"] = dir;
    r["op"]         = QStringLiteral("bundle_row");
    r["section"]    = QStringLiteral("bundle-progress");
    r["cells"]      = arr(cells);
    return r;
}

}  // namespace

// INV-1 — dispatch source-grep, routed before the m_main guard.
TEST(RoadmapLogBundleRow, Inv1DispatchWired) {
    const std::string src = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    EXPECT_NE(src.find("\"bundle_row\""), std::string::npos);
    EXPECT_NE(src.find("cmdRoadmapLogBundleRow("), std::string::npos);
    // Route appears before the `if (!m_main)` guard in cmdRoadmapLog.
    const auto routePos = src.find("return cmdRoadmapLogBundleRow(req);");
    const auto guardPos = src.find("roadmap_log: no main window");
    ASSERT_NE(routePos, std::string::npos);
    ASSERT_NE(guardPos, std::string::npos);
    EXPECT_LT(routePos, guardPos);
}

// INV-2 — required fields.
TEST(RoadmapLogBundleRow, Inv2MissingFields) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), roadmapWithTable());
    RemoteControl rc(nullptr);
    const QStringList row{"79", "ccc333", "new", "d"};
    {
        auto req = baseReq(dir.path(), row); req.remove("caller_cwd");
        EXPECT_EQ(rc.cmdRoadmapLogBundleRowForTest(req).object()["code"]
                      .toString(), QStringLiteral("missing_field"));
    }
    {
        auto req = baseReq(dir.path(), row); req.remove("section");
        EXPECT_EQ(rc.cmdRoadmapLogBundleRowForTest(req).object()["code"]
                      .toString(), QStringLiteral("missing_field"));
    }
    {
        auto req = baseReq(dir.path(), row); req.remove("cells");
        EXPECT_EQ(rc.cmdRoadmapLogBundleRowForTest(req).object()["code"]
                      .toString(), QStringLiteral("missing_field"));
    }
    {   // empty cells array.
        auto req = baseReq(dir.path(), {});
        EXPECT_EQ(rc.cmdRoadmapLogBundleRowForTest(req).object()["code"]
                      .toString(), QStringLiteral("missing_field"));
    }
}

// INV-3 — bad_section + bad_case.
TEST(RoadmapLogBundleRow, Inv3BadSectionAndCase) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), roadmapWithTable());
    RemoteControl rc(nullptr);
    const QStringList row{"79", "ccc333", "new", "d"};
    {
        auto req = baseReq(dir.path(), row);
        req["section"] = QStringLiteral("does-not-exist");
        EXPECT_EQ(rc.cmdRoadmapLogBundleRowForTest(req).object()["code"]
                      .toString(), QStringLiteral("bad_section"));
    }
    {
        auto req = baseReq(dir.path(), row);
        req["section"] = QStringLiteral("Bundle-Progress");  // wrong case
        const auto out = rc.cmdRoadmapLogBundleRowForTest(req).object();
        EXPECT_EQ(out["code"].toString(), QStringLiteral("bad_case"));
        EXPECT_EQ(out["canonical_slug"].toString(),
                  QStringLiteral("bundle-progress"));
    }
}

// INV-4 — pipe escaping; column count preserved.
TEST(RoadmapLogBundleRow, Inv4PipeEscaping) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), roadmapWithTable());
    RemoteControl rc(nullptr);
    // Theme cell contains a literal pipe.
    const QStringList row{"79", "ccc333", "a | b pipe", "d"};
    const auto out = rc.cmdRoadmapLogBundleRowForTest(
        baseReq(dir.path(), row)).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    const QString md = readRoadmap(dir.path());
    EXPECT_NE(md.indexOf(QStringLiteral("a \\| b pipe")), -1)
        << "literal | must be backslash-escaped";
    // The emitted row keeps exactly 4 columns (5 pipes), unescaped.
    const QStringList mdLines = md.split('\n');
    QString newRow;
    for (const QString &l : mdLines)
        if (l.contains(QStringLiteral("ccc333"))) newRow = l;
    ASSERT_FALSE(newRow.isEmpty());
    int unescaped = 0;
    for (int i = 0; i < newRow.size(); ++i)
        if (newRow[i] == '|' && (i == 0 || newRow[i - 1] != '\\'))
            ++unescaped;
    EXPECT_EQ(unescaped, 5) << "4-column row must have 5 unescaped pipes";
}

// INV-5 — newline escaping → <br>.
TEST(RoadmapLogBundleRow, Inv5NewlineEscaping) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), roadmapWithTable());
    RemoteControl rc(nullptr);
    const QStringList row{"79", "ccc333",
                          QStringLiteral("line1\nline2"), "d"};
    ASSERT_TRUE(rc.cmdRoadmapLogBundleRowForTest(
        baseReq(dir.path(), row)).object()["ok"].toBool());
    const QString md = readRoadmap(dir.path());
    EXPECT_NE(md.indexOf(QStringLiteral("line1<br>line2")), -1);
}

// INV-6 — column_mismatch on an existing table.
TEST(RoadmapLogBundleRow, Inv6ColumnMismatch) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), roadmapWithTable());
    RemoteControl rc(nullptr);
    const QStringList tooFew{"79", "ccc333"};   // table has 4 columns
    const auto out = rc.cmdRoadmapLogBundleRowForTest(
        baseReq(dir.path(), tooFew)).object();
    EXPECT_EQ(out["code"].toString(), QStringLiteral("column_mismatch"));
    // No write occurred — original two data rows intact.
    EXPECT_EQ(readRoadmap(dir.path()), roadmapWithTable());
}

// INV-7 — find-or-create: no table + header → table created.
TEST(RoadmapLogBundleRow, Inv7CreateTable) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), roadmapNoTable());
    RemoteControl rc(nullptr);
    auto req = baseReq(dir.path(), {"1", "abc", "first"});
    req["header"] = arr({"Bundle", "Commit", "Theme"});
    const auto out = rc.cmdRoadmapLogBundleRowForTest(req).object();
    ASSERT_TRUE(out["ok"].toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_TRUE(out["created_table"].toBool());
    const QString md = readRoadmap(dir.path());
    EXPECT_NE(md.indexOf(QStringLiteral("| Bundle | Commit | Theme |")), -1);
    EXPECT_NE(md.indexOf(QStringLiteral("| --- | --- | --- |")), -1);
    EXPECT_NE(md.indexOf(QStringLiteral("| 1 | abc | first |")), -1);
    // Table sits under the Bundle-progress heading, before Trailing.
    EXPECT_LT(md.indexOf(QStringLiteral("| Bundle")),
              md.indexOf(QStringLiteral("## Trailing")));
}

// INV-8 — no table + no header → no_table.
TEST(RoadmapLogBundleRow, Inv8NoTableNoHeader) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), roadmapNoTable());
    RemoteControl rc(nullptr);
    const auto out = rc.cmdRoadmapLogBundleRowForTest(
        baseReq(dir.path(), {"1", "abc", "first"})).object();
    EXPECT_EQ(out["code"].toString(), QStringLiteral("no_table"));
}

// INV-9 — default append goes after the last data row, before next heading.
TEST(RoadmapLogBundleRow, Inv9AppendAtEnd) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), roadmapWithTable());
    RemoteControl rc(nullptr);
    const QStringList row{"79", "ccc333", "new", "d"};
    const auto out = rc.cmdRoadmapLogBundleRowForTest(
        baseReq(dir.path(), row)).object();
    ASSERT_TRUE(out["ok"].toBool());
    const QString md = readRoadmap(dir.path());
    const int idx78 = md.indexOf(QStringLiteral("| 78 |"));
    const int idx79 = md.indexOf(QStringLiteral("| 79 |"));
    const int idxTrail = md.indexOf(QStringLiteral("## Trailing"));
    EXPECT_GT(idx79, idx78) << "new row after existing last row";
    EXPECT_LT(idx79, idxTrail) << "new row before next heading";
    EXPECT_EQ(out["row_index"].toInt(), 3);
}

// INV-10 — sorted insert keeps sort_col ascending (numeric-aware).
TEST(RoadmapLogBundleRow, Inv10SortedInsert) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), roadmapWithTable());
    RemoteControl rc(nullptr);
    // Insert Bundle 40 — numerically between 9 and 78, so it must land
    // between them (a lexical sort would wrongly place "40" after "78").
    auto req = baseReq(dir.path(), {"40", "mid444", "middle", "x"});
    req["position"] = QStringLiteral("sorted");
    req["sort_col"] = 0;
    ASSERT_TRUE(rc.cmdRoadmapLogBundleRowForTest(req).object()["ok"].toBool());
    const QString md = readRoadmap(dir.path());
    const int i9  = md.indexOf(QStringLiteral("| 9 |"));
    const int i40 = md.indexOf(QStringLiteral("| 40 |"));
    const int i78 = md.indexOf(QStringLiteral("| 78 |"));
    EXPECT_LT(i9, i40);
    EXPECT_LT(i40, i78);
}

// INV-11 — atomic write via QSaveFile::commit() in the handler body.
TEST(RoadmapLogBundleRow, Inv11AtomicWrite) {
    const std::string src = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const auto defPos = src.find(
        "QJsonDocument RemoteControl::cmdRoadmapLogBundleRow(");
    ASSERT_NE(defPos, std::string::npos);
    const auto nextDef = src.find("QJsonDocument RemoteControl::", defPos + 20);
    const std::string body = src.substr(
        defPos, (nextDef == std::string::npos ? src.size() - defPos
                                              : nextDef - defPos));
    EXPECT_NE(body.find("QSaveFile"), std::string::npos);
    EXPECT_NE(body.find(".commit()"), std::string::npos);
}

// INV-12 — success envelope shape.
TEST(RoadmapLogBundleRow, Inv12EnvelopeShape) {
    QTemporaryDir dir;
    writeRoadmap(dir.path(), roadmapWithTable());
    RemoteControl rc(nullptr);
    const auto out = rc.cmdRoadmapLogBundleRowForTest(
        baseReq(dir.path(), {"79", "ccc333", "new", "d"})).object();
    ASSERT_TRUE(out["ok"].toBool());
    EXPECT_EQ(out["file"].toString(), QStringLiteral("ROADMAP.md"));
    EXPECT_EQ(out["section"].toString(), QStringLiteral("bundle-progress"));
    EXPECT_EQ(out["columns"].toInt(), 4);
    EXPECT_FALSE(out["created_table"].toBool());
    EXPECT_TRUE(out.contains("row_index"));
    EXPECT_GT(out["bytes_written"].toInt(), 0);
}

// INV-13 (ANTS-3432) — the roadmap_log inputSchema must DECLARE the
// bundle_row params. The handler has always read cells/header/position/
// sort_col, but they were never registered in the schema `properties`;
// with additionalProperties:false the MCP client stripped them, so `cells`
// reached the handler empty and every real bundle_row call refused
// missing_field. Every INV above exercises the handler test-seam, which
// bypasses the schema — hence the gap shipped invisibly. This source-grep
// locks the schema declaration so the op stays reachable end-to-end.
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
TEST(RoadmapLogBundleRow, Inv13SchemaDeclaresBundleRowParams) {
    const std::string ci =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    for (const char *key : {"props[\"cells\"]", "props[\"header\"]",
                            "props[\"position\"]", "props[\"sort_col\"]"}) {
        EXPECT_NE(ci.find(key), std::string::npos)
            << "roadmap_log inputSchema does not declare " << key
            << " — bundle_row args will be stripped by "
               "additionalProperties:false";
    }
}
