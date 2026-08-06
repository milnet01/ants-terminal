// Feature-conformance test for ANTS-2219 — read_regions batched multi-selector
// read. RR-1..RR-5 drive the pure ReadRegion::extractBatch (no MainWindow);
// RR-6 source-greps the handler/schema/provider wiring sites. See spec.md.

#include "../../_support/expect.h"
#include "readregion.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QTemporaryDir>

#include <string>

#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

QString writeFile(const QString &root, const QString &name,
                  const QString &content) {
    QFile f(root + "/" + name);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content.toUtf8());
    f.close();
    return name;
}

QJsonObject lineItem(const QString &path, int start, int end) {
    QJsonObject o; o["path"] = path; o["start_line"] = start;
    o["end_line"] = end; return o;
}
QJsonObject symItem(const QString &path, const QString &sym) {
    QJsonObject o; o["path"] = path; o["symbol"] = sym; return o;
}
QJsonObject secItem(const QString &path, const QString &section) {
    QJsonObject o; o["path"] = path; o["section"] = section; return o;
}

QJsonObject runRegions(const QString &root, const QJsonArray &items,
                       int maxBytes = 0) {
    return ReadRegion::extractBatch(root, QJsonValue(items), maxBytes);
}

QString joinLines(const QJsonObject &entry) {
    QString s;
    for (const auto &v : entry.value("lines").toArray())
        s += v.toString() + QLatin1Char('\n');
    return s;
}

// A temp project root + two fixture files. Returns the canonical root.
struct Fixture {
    QTemporaryDir dir;
    QString root;
    Fixture() {
        root = QFileInfo(dir.path()).canonicalFilePath();
        QDir().mkpath(root + "/src");
        writeFile(root, "src/a.cpp",
                  "void alpha() {\n"      // 1
                  "    stepA();\n"        // 2
                  "}\n"                   // 3
                  "void beta() {\n"       // 4
                  "    stepB();\n"        // 5
                  "}\n");                 // 6
        writeFile(root, "doc.md",
                  "# Doc\n"               // 1
                  "## 4.1 Intro\n"        // 2
                  "intro body\n"          // 3
                  "## 4.2 Details\n"      // 4
                  "detail body\n");       // 5
    }
};

}  // namespace

// RR-1 — three selectors (symbol, line range, md section) across two files in
// one call; results in order, each project-relative.
TEST(ReadRegions, BatchMultipleSlices) {
    Fixture fx;
    QJsonArray items;
    items.append(symItem("src/a.cpp", "alpha"));
    items.append(lineItem("src/a.cpp", 4, 5));
    items.append(secItem("doc.md", "4-2-details"));
    const QJsonObject env = runRegions(fx.root, items);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("count").toInt(), 3);
    const QJsonArray r = env.value("results").toArray();
    ASSERT_EQ(r.size(), 3);
    EXPECT_EQ(r.at(0).toObject().value("path").toString().toStdString(),
              "src/a.cpp");
    EXPECT_TRUE(joinLines(r.at(0).toObject()).contains("stepA"));
    EXPECT_FALSE(joinLines(r.at(0).toObject()).contains("beta"));  // capped
    EXPECT_TRUE(joinLines(r.at(1).toObject()).contains("stepB"));
    EXPECT_TRUE(joinLines(r.at(2).toObject()).contains("detail body"));
    EXPECT_EQ(r.at(2).toObject().value("section_slug").toString().toStdString(),
              "4-2-details");
}

// RR-2 — per-item etag → a matching etag_match collapses to an unchanged stub.
TEST(ReadRegions, PerItemEtag304) {
    Fixture fx;
    QJsonArray items; items.append(symItem("src/a.cpp", "alpha"));
    const QJsonObject first = runRegions(fx.root, items);
    const QJsonObject e0 = first.value("results").toArray().at(0).toObject();
    const QString etag = e0.value("etag").toString();
    ASSERT_FALSE(etag.isEmpty());

    QJsonObject it = symItem("src/a.cpp", "alpha");
    it["etag_match"] = etag;
    QJsonArray items2; items2.append(it);
    const QJsonObject second = runRegions(fx.root, items2);
    const QJsonObject e = second.value("results").toArray().at(0).toObject();
    EXPECT_TRUE(e.value("unchanged").toBool());
    EXPECT_EQ(e.value("etag").toString(), etag);
    EXPECT_FALSE(e.contains("lines"));  // body not re-sent
}

// RR-3 — a bad item path is isolated to its own result; the batch and the
// other items still succeed.
TEST(ReadRegions, PerItemFailureIsolation) {
    Fixture fx;
    QJsonArray items;
    items.append(symItem("src/a.cpp", "alpha"));
    items.append(lineItem("src/missing.cpp", 1, 2));
    const QJsonObject env = runRegions(fx.root, items);
    ASSERT_TRUE(env.value("ok").toBool());
    const QJsonArray r = env.value("results").toArray();
    ASSERT_EQ(r.size(), 2);
    EXPECT_TRUE(r.at(0).toObject().value("ok").toBool());
    EXPECT_FALSE(r.at(1).toObject().value("ok").toBool());
    EXPECT_EQ(r.at(1).toObject().value("code").toString().toStdString(),
              "not_found");
}

// RR-4 — arg validation: missing/empty items → bad_args; > 64 → too_many_items.
TEST(ReadRegions, ArgValidation) {
    Fixture fx;
    {
        // items absent / not an array → bad_args.
        const QJsonObject env =
            ReadRegion::extractBatch(fx.root, QJsonValue(), 0);
        EXPECT_FALSE(env.value("ok").toBool());
        EXPECT_EQ(env.value("code").toString().toStdString(), "bad_args");
    }
    {
        const QJsonObject env = runRegions(fx.root, QJsonArray{});
        EXPECT_FALSE(env.value("ok").toBool());
        EXPECT_EQ(env.value("code").toString().toStdString(), "bad_args");
    }
    {
        QJsonArray big;
        for (int i = 0; i < 65; ++i) big.append(lineItem("src/a.cpp", 1, 1));
        const QJsonObject env = runRegions(fx.root, big);
        EXPECT_FALSE(env.value("ok").toBool());
        EXPECT_EQ(env.value("code").toString().toStdString(),
                  "too_many_items");
    }
}

// RR-5 — a tiny shared budget exhausts across the set and flags truncated.
TEST(ReadRegions, SharedBudgetTruncates) {
    Fixture fx;
    QJsonArray items;
    items.append(symItem("src/a.cpp", "alpha"));
    items.append(symItem("src/a.cpp", "beta"));
    const QJsonObject env = runRegions(fx.root, items, /*maxBytes=*/30);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.value("truncated").toBool());
}

// RR-6 — wiring contract (handler + schema + provider).
TEST(ReadRegions, WiringContract) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    const std::string ci =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(has(rc, "RemoteControl::cmdReadRegions") &&
               has(rc, "ReadRegion::extractBatch("),
           "RR-6a",
           "remotecontrol.cpp missing cmdReadRegions/extractBatch delegation");
    expect(has(ci, "\"read_regions\""),
           "RR-6b", "claudeintegration.cpp does not register read_regions");
    expect(has(mw, "registerToolProvider(\"read_regions\""),
           "RR-6c", "mainwindow.cpp does not register the read_regions provider");
    EXPECT_EQ(0, expect_failures())
        << expect_failures() << " ANTS-2219 wiring invariant(s) failed";
}

// RR-7 — ANTS-3500: cmdReadRegions accepts requests/paths/regions as aliases
// for the `items` batch key. Windowed source-grep (the alias fallback lives in
// the remotecontrol wrapper, not the pure extractBatch core RR-1..RR-5 drive),
// so dropping the fallback fails here.
TEST(ReadRegions, ItemsKeyAliases) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    const auto p = rc.find("RemoteControl::cmdReadRegions");
    ASSERT_NE(p, std::string::npos);
    const std::string body = rc.substr(p, 1500);
    expect(body.find("\"requests\"") != std::string::npos, "RR-7a",
           "cmdReadRegions missing `requests` alias for items");
    expect(body.find("\"paths\"") != std::string::npos, "RR-7b",
           "cmdReadRegions missing `paths` alias for items");
    expect(body.find("\"regions\"") != std::string::npos, "RR-7c",
           "cmdReadRegions missing `regions` alias for items");
    EXPECT_EQ(0, expect_failures())
        << expect_failures() << " ANTS-3500 alias wiring invariant(s) failed";
}

// RR-8 — ANTS-3568: the read_regions inputSchema must DECLARE the
// requests/paths/regions aliases. Without them, additionalProperties:false
// strips the alias keys before cmdReadRegions' RR-7 fallback ever runs — so
// the handler code is unreachable and RR-7's handler-grep passes while the
// alias is dead end-to-end. Window on the read_regions schema block.
TEST(ReadRegions, SchemaDeclaresAliases) {
    expect_reset();
    const std::string ci =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto p = ci.find("rrsTool[\"name\"] = \"read_regions\"");
    ASSERT_NE(p, std::string::npos);
    const auto end = ci.find("tools.append(rrsTool)", p);
    ASSERT_NE(end, std::string::npos);
    const std::string block = ci.substr(p, end - p);
    expect(block.find("\"requests\"") != std::string::npos, "RR-8a",
           "read_regions schema does not declare the `requests` alias");
    expect(block.find("\"paths\"") != std::string::npos, "RR-8b",
           "read_regions schema does not declare the `paths` alias");
    expect(block.find("\"regions\"") != std::string::npos, "RR-8c",
           "read_regions schema does not declare the `regions` alias");
    EXPECT_EQ(0, expect_failures())
        << expect_failures() << " ANTS-3568 schema-alias invariant(s) failed";
}

// RR-9 — ANTS-3589: a top-level `defaultPath` is the per-item `path` fallback.
// (a) an item that omits its own `path` reads from the default; (b) a per-item
// `path` still wins; (c) with neither, the item still fails bad_args.
TEST(ReadRegions, TopLevelDefaultPath) {
    Fixture fx;
    // (a) both items omit `path` → resolved from the default (src/a.cpp).
    QJsonArray items;
    { QJsonObject o; o["start_line"] = 1; o["end_line"] = 2; items.append(o); }
    { QJsonObject o; o["symbol"] = "beta"; items.append(o); }
    const QJsonObject env =
        ReadRegion::extractBatch(fx.root, QJsonValue(items), 0, "src/a.cpp");
    ASSERT_TRUE(env.value("ok").toBool());
    const QJsonArray r = env.value("results").toArray();
    ASSERT_EQ(r.size(), 2);
    EXPECT_TRUE(r.at(0).toObject().value("ok").toBool());
    EXPECT_EQ(r.at(0).toObject().value("path").toString().toStdString(),
              "src/a.cpp");
    EXPECT_TRUE(joinLines(r.at(0).toObject()).contains("alpha"));
    EXPECT_TRUE(joinLines(r.at(1).toObject()).contains("stepB"));

    // (b) a per-item `path` overrides the default.
    QJsonArray items2; items2.append(secItem("doc.md", "4-2-details"));
    const QJsonObject env2 =
        ReadRegion::extractBatch(fx.root, QJsonValue(items2), 0, "src/a.cpp");
    const QJsonObject e2 = env2.value("results").toArray().at(0).toObject();
    EXPECT_EQ(e2.value("path").toString().toStdString(), "doc.md");
    EXPECT_TRUE(joinLines(e2).contains("detail body"));

    // (c) no per-item path and an empty default → bad_args (prior behaviour).
    QJsonArray items3;
    { QJsonObject o; o["start_line"] = 1; items3.append(o); }
    const QJsonObject env3 =
        ReadRegion::extractBatch(fx.root, QJsonValue(items3), 0, QString());
    const QJsonObject e3 = env3.value("results").toArray().at(0).toObject();
    EXPECT_FALSE(e3.value("ok").toBool());
    EXPECT_EQ(e3.value("code").toString().toStdString(), "bad_args");
}

// RR-10 — ANTS-3589: the top-level `path` default must be (a) DECLARED in the
// read_regions schema so additionalProperties:false keeps it (and it drops out
// of ignored_args), and (b) read by cmdReadRegions and forwarded to
// extractBatch. Both are wiring sites the pure RR-9 core cannot cover.
TEST(ReadRegions, TopLevelPathWiring) {
    expect_reset();
    const std::string ci =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string rc = ants_test::slurpRemoteControl();
    const auto p = ci.find("rrsTool[\"name\"] = \"read_regions\"");
    ASSERT_NE(p, std::string::npos);
    const auto end = ci.find("tools.append(rrsTool)", p);
    ASSERT_NE(end, std::string::npos);
    const std::string block = ci.substr(p, end - p);
    expect(block.find("props[\"path\"]") != std::string::npos, "RR-10a",
           "read_regions schema does not declare a top-level `path` default");
    const auto h = rc.find("RemoteControl::cmdReadRegions");
    ASSERT_NE(h, std::string::npos);
    const std::string hbody = rc.substr(h, 2000);
    expect(hbody.find("extractBatch") != std::string::npos &&
               hbody.find("\"path\"") != std::string::npos, "RR-10b",
           "cmdReadRegions does not forward a top-level `path` to extractBatch");
    EXPECT_EQ(0, expect_failures())
        << expect_failures() << " ANTS-3589 wiring invariant(s) failed";
}
