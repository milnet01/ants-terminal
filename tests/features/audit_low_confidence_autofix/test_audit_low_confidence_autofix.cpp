// ANTS-1719 — feature-conformance test for the native auto-fix engine.
// Behavioural coverage of ants::autofix (pure, Qt6::Core).
// See tests/features/audit_low_confidence_autofix/spec.md.

#include "auditautofix.h"
#include "auditengine.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

namespace {

Finding mk(const QString &checkId, const QString &message, int line) {
    Finding f;
    f.checkId = checkId;
    f.message = message;
    f.file = QStringLiteral("src/x.cpp");
    f.line = line;
    return f;
}

QString readFile(const QString &p) {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("<open-fail>");
    return QString::fromUtf8(f.readAll());
}

QString writeTmp(QTemporaryDir &dir, const QString &name, const QString &body) {
    const QString p = dir.path() + "/" + name;
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly)) return QStringLiteral("<open-fail>");
    f.write(body.toUtf8());
    f.close();
    return p;
}

const char *kVer = "0.7.92";

}  // namespace

// INV-1 — unused #include round-trips: plan removes the include line.
TEST(AuditAutofix, Inv1UnusedIncludeRoundTrip) {
    QTemporaryDir dir;
    const QString body =
        "#include <QString>\n#include <QDebug>\nint main(){}\n";
    const QString p = writeTmp(dir, "x.cpp", body);
    const Finding f = mk("cppcheck",
                         "x.cpp:2: style: Include file not used. [unusedInclude]", 2);
    const auto r = ants::autofix::planRepair(f, p, body, kVer);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->rule.toStdString(), "autofix.unused_include");
    EXPECT_TRUE(r->removeLine);
    ASSERT_TRUE(ants::autofix::applyRepair(*r));
    EXPECT_EQ(readFile(p).toStdString(),
              "#include <QString>\nint main(){}\n");
}

// INV-1 — dead Q_UNUSED round-trips.
TEST(AuditAutofix, Inv1DeadQUnusedRoundTrip) {
    QTemporaryDir dir;
    const QString body = "void g(){\n    Q_UNUSED(x);\n}\n";
    const QString p = writeTmp(dir, "x.cpp", body);
    const Finding f = mk("clazy", "dead Q_UNUSED(x) — x no longer declared", 2);
    const auto r = ants::autofix::planRepair(f, p, body, kVer);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->rule.toStdString(), "autofix.dead_q_unused");
    ASSERT_TRUE(ants::autofix::applyRepair(*r));
    EXPECT_EQ(readFile(p).toStdString(), "void g(){\n}\n");
}

// INV-1 — stale (past-version) TODO round-trips.
TEST(AuditAutofix, Inv1StaleTodoRoundTrip) {
    QTemporaryDir dir;
    const QString body =
        "int a;\n    // TODO: remove after 0.7.50 once migrated\nint b;\n";
    const QString p = writeTmp(dir, "x.cpp", body);
    const Finding f = mk("grep", "stale TODO marker", 2);
    const auto r = ants::autofix::planRepair(f, p, body, kVer);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->rule.toStdString(), "autofix.stale_todo");
    ASSERT_TRUE(ants::autofix::applyRepair(*r));
    EXPECT_EQ(readFile(p).toStdString(), "int a;\nint b;\n");
}

// INV-1 — comment-space round-trips (reformat, not removal).
TEST(AuditAutofix, Inv1CommentSpaceRoundTrip) {
    QTemporaryDir dir;
    const QString body = "code();\n  //tight comment\nmore();\n";
    const QString p = writeTmp(dir, "x.cpp", body);
    const Finding f = mk("clang-tidy", "comment should have a space", 2);
    const auto r = ants::autofix::planRepair(f, p, body, kVer);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->rule.toStdString(), "autofix.comment_space");
    EXPECT_FALSE(r->removeLine);
    ASSERT_TRUE(ants::autofix::applyRepair(*r));
    EXPECT_EQ(readFile(p).toStdString(),
              "code();\n  // tight comment\nmore();\n");
}

// INV-2 — unsafe findings are never auto-fixed.
TEST(AuditAutofix, Inv2UnsafeNeverFixed) {
    QTemporaryDir dir;
    // future-version TODO: not yet removable.
    {
        const QString body = "// TODO: remove after 0.9.0\n";
        const QString p = writeTmp(dir, "a.cpp", body);
        EXPECT_FALSE(ants::autofix::planRepair(
            mk("grep", "todo", 1), p, body, kVer).has_value());
    }
    // cppcheck unusedInclude but line is NOT an include -> refuse.
    {
        const QString body = "int notAnInclude = 1;\n";
        const QString p = writeTmp(dir, "b.cpp", body);
        EXPECT_FALSE(ants::autofix::planRepair(
            mk("cppcheck", "[unusedInclude]", 1), p, body, kVer).has_value());
    }
    // Q_UNUSED finding but line is real code -> refuse.
    {
        const QString body = "int y = Q_UNUSED_fake(z) + 1;\n";
        const QString p = writeTmp(dir, "c.cpp", body);
        EXPECT_FALSE(ants::autofix::planRepair(
            mk("clazy", "Q_UNUSED", 1), p, body, kVer).has_value());
    }
    // //word line but finding does not mention "comment" -> refuse.
    {
        const QString body = "//word\n";
        const QString p = writeTmp(dir, "d.cpp", body);
        EXPECT_FALSE(ants::autofix::planRepair(
            mk("perf_rule", "hot path", 1), p, body, kVer).has_value());
    }
    // line out of range -> refuse.
    {
        const QString body = "#include <X>\n";
        const QString p = writeTmp(dir, "e.cpp", body);
        EXPECT_FALSE(ants::autofix::planRepair(
            mk("cppcheck", "[unusedInclude]", 9), p, body, kVer).has_value());
    }
    // /// doc comment is not reformatted.
    {
        const QString body = "///doc\n";
        const QString p = writeTmp(dir, "f.cpp", body);
        EXPECT_FALSE(ants::autofix::planRepair(
            mk("lint", "comment", 1), p, body, kVer).has_value());
    }
}

// INV-3 — applyRepair refuses a stale plan and leaves the file untouched.
TEST(AuditAutofix, Inv3StalePlanRefused) {
    QTemporaryDir dir;
    const QString body = "#include <A>\n#include <B>\nx();\n";
    const QString p = writeTmp(dir, "x.cpp", body);
    ants::autofix::Repair r;
    r.rule = "autofix.unused_include";
    r.file = p;
    r.line = 2;
    r.original = "#include <DIFFERENT>";  // does not match line 2
    r.removeLine = true;
    EXPECT_FALSE(ants::autofix::applyRepair(r));
    EXPECT_EQ(readFile(p).toStdString(), body.toStdString());
}

// INV-4 — the log is append-only with a header on first write.
TEST(AuditAutofix, Inv4LogAppendOnly) {
    QTemporaryDir dir;
    const QString cache = dir.path() + "/.audit_cache";
    ants::autofix::Repair r;
    r.rule = "autofix.stale_todo";
    r.file = "src/x.cpp";
    r.line = 7;
    r.original = "// TODO: remove after 0.7.0";
    r.removeLine = true;
    ASSERT_TRUE(ants::autofix::logRepair(cache, r));
    ASSERT_TRUE(ants::autofix::logRepair(cache, r));

    // Exactly one log file for today.
    QDir d(cache);
    const QStringList logs = d.entryList({"autofix-*.jsonl"}, QDir::Files);
    ASSERT_EQ(logs.size(), 1);
    const QString text = readFile(cache + "/" + logs.first());
    EXPECT_TRUE(text.contains("ANTS-1719"));            // header present
    // header + two records => three newlines.
    EXPECT_EQ(text.count('\n'), 3);
    EXPECT_TRUE(text.contains("\"rule\":\"autofix.stale_todo\""));
}

// INV-5 — version comparison is conservative.
TEST(AuditAutofix, Inv5VersionLE) {
    EXPECT_TRUE(ants::autofix::versionLE("0.7.50", "0.7.92"));
    EXPECT_TRUE(ants::autofix::versionLE("0.7.92", "0.7.92"));
    EXPECT_TRUE(ants::autofix::versionLE("0.6.0", "0.7.0"));
    EXPECT_FALSE(ants::autofix::versionLE("0.8.0", "0.7.92"));
    EXPECT_FALSE(ants::autofix::versionLE("garbage", "0.7.92"));
    EXPECT_FALSE(ants::autofix::versionLE("0.7", "0.7.92"));
}
