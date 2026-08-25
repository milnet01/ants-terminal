// Feature-conformance test for the read_log MCP tool (ANTS-1855).
// Behavioural invariants exercise the pure ReadLog::filter helper;
// wiring invariants source-scrape the five registration sites.
// See spec.md + docs/specs/ANTS-1855.md.

#include "../../_support/expect.h"
#include "readlog.h"

#include <cstdio>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Write `lines` (each gets a trailing \n) to a fresh file; return path.
QString writeLog(const QTemporaryDir &dir, const QString &name,
                 const QStringList &lines) {
    const QString path = dir.path() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(lines.join('\n').toUtf8());
    if (!lines.isEmpty()) f.write("\n");
    f.close();
    return path;
}

QStringList linesOf(const QJsonObject &env) {
    QStringList out;
    for (const auto &v : env.value("lines").toArray()) out << v.toString();
    return out;
}

const QStringList kFixture = {
    "[2026-05-25T17:00:00.000] claude  alpha one",
    "[2026-05-25T17:05:00.000] vt      beta two",
    "[2026-05-25T17:10:00.000] claude  gamma three",
    "[2026-05-25T17:15:00.000] vt      delta four",
    "no-timestamp trailing note",
};

}  // namespace

// W1/W2 — wiring across the five registration sites.
TEST(McpReadLog, WiringContract) {
    expect_reset();
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpRemoteControl();
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string projCpp =
        ants_test::slurpFile(std::string(ANTS_SOURCE_DIR) + "/src/mcpprojection.cpp");

    expect(has(rcHdr, "cmdReadLog(const QJsonObject &req)"), "W1 decl",
           "remotecontrol.h missing cmdReadLog declaration");
    expect(has(rcCpp, "cmdReadLog") && has(rcCpp, "ReadLog::filter"),
           "W1 impl", "remotecontrol.cpp cmdReadLog must call ReadLog::filter");
    expect(has(rcCpp, "validatePath") && has(rcCpp, "read_log"),
           "W1 pathvalidation", "cmdReadLog must validate the path arg");
    expect(has(ciCpp, "\"read_log\""), "W1 contract+schema",
           "claudeintegration.cpp missing read_log registration");
    expect(has(ciCpp, "callerCwdContractFor") &&
           has(ciCpp, "C::Required") && has(ciCpp, "read_log"),
           "W1 Required", "read_log must be callerCwdContractFor → Required");

    const char *props[] = {
        "\"include\"", "\"exclude\"", "\"contains\"", "\"since\"",
        "\"tail\"", "\"max_bytes\"", "\"since_cursor\"",
    };
    for (const char *p : props) {
        char label[64]; std::snprintf(label, sizeof label, "W2 prop %s", p);
        expect(has(ciCpp, p), label, "read_log schema missing a property");
    }
    // ANTS-4524 — `fields=` needs no enrolment now; what read_log is still a
    // member of is the compaction table in mcpprojection.cpp.
    expect(has(projCpp, "read_log"), "W2 compact",
           "mcpprojection.cpp's compaction table must include read_log");
    expect(has(mwCpp, "registerToolProvider(\"read_log\"") &&
           has(mwCpp, "cmdReadLog"), "W2 dispatch",
           "mainwindow.cpp must register read_log → cmdReadLog");
    EXPECT_EQ(0, expect_failures());
}

// B-INV-4 — include/exclude regex.
TEST(McpReadLog, IncludeExclude) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeLog(dir, "a.log", kFixture);
    ReadLog::Options o; o.include = "claude"; o.exclude = "gamma";
    const QJsonObject env = ReadLog::filter(p, o);
    ASSERT_TRUE(env.value("ok").toBool());
    const QStringList ls = linesOf(env);
    EXPECT_EQ(ls.size(), 1);                       // claude lines minus gamma
    EXPECT_TRUE(ls.value(0).contains("alpha one"));
}

// B-INV-4 (refusal) — invalid regex → bad_args.
TEST(McpReadLog, BadRegex) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeLog(dir, "a.log", kFixture);
    ReadLog::Options o; o.include = "([unterminated";
    const QJsonObject env = ReadLog::filter(p, o);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString().toStdString(), "bad_args");
}

// B-INV-5 — contains substring.
TEST(McpReadLog, Contains) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeLog(dir, "a.log", kFixture);
    ReadLog::Options o; o.contains = "beta";
    const QJsonObject env = ReadLog::filter(p, o);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(linesOf(env).size(), 1);
    EXPECT_TRUE(linesOf(env).value(0).contains("beta two"));
}

// B-INV-6 — since lower bound on the [..] prefix; no-prefix dropped.
TEST(McpReadLog, Since) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeLog(dir, "a.log", kFixture);
    ReadLog::Options o; o.since = "2026-05-25T17:06";
    const QJsonObject env = ReadLog::filter(p, o);
    ASSERT_TRUE(env.value("ok").toBool());
    const QStringList ls = linesOf(env);
    EXPECT_EQ(ls.size(), 2);                       // 17:10 + 17:15; not 17:00/17:05; not the no-prefix line
    EXPECT_TRUE(ls.value(0).contains("gamma three"));
    EXPECT_TRUE(ls.value(1).contains("delta four"));
}

// B-INV-7 — tail clamps + returns last N; matched is full count.
TEST(McpReadLog, TailClamp) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeLog(dir, "a.log", kFixture);
    ReadLog::Options o; o.tail = 2;
    const QJsonObject env = ReadLog::filter(p, o);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(linesOf(env).size(), 2);
    EXPECT_EQ(env.value("matched").toInt(), kFixture.size());
    EXPECT_TRUE(linesOf(env).value(1).contains("trailing note"));
}

// B-INV-8 — byte cap drops the OLDEST (newest kept) + truncated.
TEST(McpReadLog, ByteCapDropsOldest) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeLog(dir, "a.log", kFixture);
    // ~45 bytes/line; cap to keep only the last couple.
    ReadLog::Options o; o.maxBytes = 80;
    const QJsonObject env = ReadLog::filter(p, o);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.value("truncated").toBool());
    const QStringList ls = linesOf(env);
    EXPECT_GE(ls.size(), 1);
    EXPECT_TRUE(ls.last().contains("trailing note"));   // newest kept
    EXPECT_FALSE(ls.join('\n').contains("alpha one"));  // oldest dropped
    EXPECT_GT(env.value("lines_dropped").toInt(), 0);
}

// B-INV-9 — counts present.
TEST(McpReadLog, Counts) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeLog(dir, "a.log", kFixture);
    ReadLog::Options o; o.contains = "claude";
    const QJsonObject env = ReadLog::filter(p, o);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("scanned").toInt(), kFixture.size());
    EXPECT_EQ(env.value("matched").toInt(), 2);     // two claude lines
}

// B-INV-10 — since_cursor reads only appended bytes; garbage cursor
// soft-falls-back (cursor_stale, not a refusal).
TEST(McpReadLog, SinceCursorIncremental) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeLog(dir, "a.log", kFixture);

    ReadLog::Options first;
    const QJsonObject e1 = ReadLog::filter(p, first);
    ASSERT_TRUE(e1.value("ok").toBool());
    const QString cursor = e1.value("cursor").toString();
    ASSERT_FALSE(cursor.isEmpty());

    // Append one line, then incremental read.
    QFile f(p); ASSERT_TRUE(f.open(QIODevice::Append));
    f.write("[2026-05-25T18:00:00.000] claude  epsilon five\n");
    f.close();

    ReadLog::Options inc; inc.hasSinceCursor = true; inc.sinceCursor = cursor;
    const QJsonObject e2 = ReadLog::filter(p, inc);
    ASSERT_TRUE(e2.value("ok").toBool());
    const QStringList ls = linesOf(e2);
    EXPECT_EQ(ls.size(), 1);
    EXPECT_TRUE(ls.value(0).contains("epsilon five"));

    // Garbage cursor → soft-fallback, full re-read, never a refusal.
    ReadLog::Options bad; bad.hasSinceCursor = true; bad.sinceCursor = "not-a-number";
    const QJsonObject e3 = ReadLog::filter(p, bad);
    EXPECT_TRUE(e3.value("ok").toBool());
    EXPECT_TRUE(e3.value("cursor_stale").toBool());
    EXPECT_GT(linesOf(e3).size(), 1);
}

// B-not_found — unopenable path.
TEST(McpReadLog, NotFound) {
    const QJsonObject env =
        ReadLog::filter("/nonexistent/ants-1855/nope.log", ReadLog::Options{});
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString().toStdString(), "not_found");
}
