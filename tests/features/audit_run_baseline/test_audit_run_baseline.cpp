// Feature-conformance test for tests/features/audit_run_baseline/spec.md.
//
// ANTS-1706 — audit_run pins a run's findings as a baseline and reports the
// findings that are new since it. runAudit is driven end to end through a
// fake `mypy` first on PATH (the audit_tool_process_group_kill pattern), so
// no real linter is needed.

#include "auditcache.h"
#include "auditdelta.h"
#include "auditfpledger.h"
#include "auditrunner.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include "../../_support/xdg_guard.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <string>

namespace {

// One folder for the fake mypy in this binary: AuditRunner caches a tool's
// resolved path for 60 s, so every TEST must reach the same script. The
// script prints whatever `out.txt` beside it holds; each run rewrites it.
const QTemporaryDir &toolDir() {
    static const QTemporaryDir dir;
    return dir;
}

class FakeMypy {
public:
    FakeMypy() {
        const QString script = toolDir().path() + QStringLiteral("/mypy");
        QFile f(script);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write(QStringLiteral("#!/bin/sh\ncat '%1/out.txt'\n")
                        .arg(toolDir().path()).toUtf8());
            f.close();
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                             | QFileDevice::ExeOwner);
        }
        const QByteArray old = qgetenv("PATH");
        QByteArray next = toolDir().path().toUtf8();
        if (!old.isEmpty()) { next += ':'; next += old; }
        m_env.setEnv("PATH", next);
    }
    void prints(const QString &text) const {
        QFile f(toolDir().path() + QStringLiteral("/out.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(text.toUtf8());
            f.close();
        }
    }

private:
    ants_test::XdgGuard m_env;
};

AuditRunner::RunResult run(const QString &root, bool save,
                           const QString &scope = {}) {
    AuditRunner::RunRequest req;
    req.projectRoot       = root;
    req.tools             = {QStringLiteral("mypy")};
    req.scope             = scope;
    req.capPerToolSeconds = 30;
    req.saveBaseline      = save;
    return AuditRunner::runAudit(req);
}

QString baselinePath(const QString &root) {
    return root + QStringLiteral("/.audit_cache/baseline-findings.json");
}

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(body);
    f.close();
    return true;
}

QJsonObject mk(const QString &file, int line, const QString &rule,
               const QString &msg) {
    QJsonObject o;
    o["file"]    = file;
    o["line"]    = line;
    o["rule"]    = rule;
    o["message"] = msg;
    o["fp"]      = ants::auditfp::computeFingerprint(file, rule, msg);
    return o;
}

}  // namespace

// INV-1 — save_baseline writes an owner-only baseline holding this run's
// findings and the tools that ran, and the result says what it saved.
TEST(AuditRunBaseline, Inv1SaveWritesPrivateBaseline) {
    FakeMypy mypy;
    QTemporaryDir proj;
    ASSERT_TRUE(proj.isValid());
    mypy.prints(QStringLiteral("a.py:1: error: alpha\n"));

    const AuditRunner::RunResult r = run(proj.path(), true);
    ASSERT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();
    ASSERT_TRUE(r.byTool.contains(QStringLiteral("mypy")))
        << "test setup: the fake mypy never ran";

    ASSERT_TRUE(r.baselineSaved.value("findings_count").isDouble())
        << "INV-1: baseline_saved.findings_count must be reported";
    EXPECT_EQ(r.baselineSaved.value("findings_count").toInt(), 1);
    EXPECT_EQ(r.baselineSaved.value("tools").toArray(),
              QJsonArray{QStringLiteral("mypy")});
    EXPECT_EQ(r.baselineSaved.value("path").toString(),
              baselinePath(proj.path()));

    QFile f(baselinePath(proj.path()));
    ASSERT_TRUE(f.exists()) << "INV-1: the baseline file must be written";
    EXPECT_EQ(f.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther
                                 | QFileDevice::WriteGroup
                                 | QFileDevice::WriteOther),
              QFileDevice::Permissions{})
        << "INV-1: the baseline holds findings and must be owner-only";
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    EXPECT_EQ(doc.value("version").toInt(), 1);
    EXPECT_EQ(doc.value("findings").toArray().size(), 1);
    EXPECT_EQ(doc.value("tools").toArray(), QJsonArray{QStringLiteral("mypy")});
}

// INV-2 — a later run names only the findings the baseline lacks; a finding
// that moved lines is not new.
TEST(AuditRunBaseline, Inv2LaterRunReportsOnlyNewFindings) {
    FakeMypy mypy;
    QTemporaryDir proj;
    ASSERT_TRUE(proj.isValid());
    mypy.prints(QStringLiteral("a.py:1: error: alpha\n"));
    ASSERT_TRUE(run(proj.path(), true).ok);

    mypy.prints(QStringLiteral("a.py:7: error: alpha\nb.py:2: error: beta\n"));
    const AuditRunner::RunResult r = run(proj.path(), false);
    ASSERT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();

    const QJsonObject sb = r.sinceBaseline;
    ASSERT_TRUE(sb.value("new_count").isDouble())
        << "INV-2: since_baseline.new_count must be present";
    EXPECT_EQ(sb.value("new_count").toInt(), 1)
        << "INV-2: only beta is new; alpha only moved lines";
    const QJsonArray fresh = sb.value("new").toArray();
    ASSERT_EQ(fresh.size(), 1);
    EXPECT_EQ(fresh.at(0).toObject().value("file").toString(),
              QStringLiteral("b.py"));
    EXPECT_FALSE(fresh.at(0).toObject().contains("fp"))
        << "INV-2: the preview drops the internal fp";
    EXPECT_TRUE(sb.value("baseline_saved_at").isString());
    EXPECT_TRUE(sb.value("baseline_commit").isString());
    EXPECT_FALSE(sb.contains("unavailable_reason"));
}

// INV-3 — no baseline file, no since_baseline block.
TEST(AuditRunBaseline, Inv3NoBaselineNoBlock) {
    FakeMypy mypy;
    QTemporaryDir proj;
    ASSERT_TRUE(proj.isValid());
    mypy.prints(QStringLiteral("a.py:1: error: alpha\n"));
    const AuditRunner::RunResult r = run(proj.path(), false);
    ASSERT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();
    ASSERT_TRUE(r.byTool.contains(QStringLiteral("mypy")));
    EXPECT_TRUE(r.sinceBaseline.isEmpty());
    EXPECT_TRUE(r.baselineSaved.isEmpty());
    EXPECT_FALSE(QFile::exists(baselinePath(proj.path())))
        << "INV-3: a run without save_baseline writes no baseline";
}

// INV-4 — a tool the baseline never ran is named, and its findings are not
// counted as new.
TEST(AuditRunBaseline, Inv4ToolNotInBaselineIsNamed) {
    FakeMypy mypy;
    QTemporaryDir proj;
    ASSERT_TRUE(proj.isValid());
    ASSERT_TRUE(writeFile(baselinePath(proj.path()),
        R"({"version":1,"iso_timestamp":"2026-11-17T00:00:00Z","commit":"nogit",)"
        R"("truncated":false,"tools":["cppcheck"],"findings":[]})"));
    mypy.prints(QStringLiteral("a.py:1: error: alpha\n"));

    const AuditRunner::RunResult r = run(proj.path(), false);
    ASSERT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();
    const QJsonObject sb = r.sinceBaseline;
    ASSERT_TRUE(sb.value("new_count").isDouble());
    EXPECT_EQ(sb.value("new_count").toInt(), 0)
        << "INV-4: mypy's findings cannot be new against a baseline without mypy";
    EXPECT_EQ(sb.value("tools_not_in_baseline").toArray(),
              QJsonArray{QStringLiteral("mypy")});
}

// INV-5 — a narrowed run refuses to save, and writes nothing.
TEST(AuditRunBaseline, Inv5NarrowedRunRefusesSave) {
    FakeMypy mypy;
    QTemporaryDir proj;
    ASSERT_TRUE(proj.isValid());
    mypy.prints(QStringLiteral("a.py:1: error: alpha\n"));

    for (const QString &scope : {QStringLiteral("files"),
                                 QStringLiteral("branch-diff"),
                                 QStringLiteral("since-last-run")}) {
        const AuditRunner::RunResult r = run(proj.path(), true, scope);
        EXPECT_FALSE(r.ok) << "INV-5: scope " << scope.toStdString();
        EXPECT_EQ(r.code, QStringLiteral("bad_args")) << scope.toStdString();
    }
    AuditRunner::RunRequest req;
    req.projectRoot  = proj.path();
    req.tools        = {QStringLiteral("mypy")};
    req.paths        = {QStringLiteral("a.py")};
    req.saveBaseline = true;
    const AuditRunner::RunResult rp = AuditRunner::runAudit(req);
    EXPECT_FALSE(rp.ok);
    EXPECT_EQ(rp.code, QStringLiteral("bad_args")) << "INV-5: paths";
    EXPECT_FALSE(QFile::exists(baselinePath(proj.path())))
        << "INV-5: a refused save writes nothing";

    // Control: the whole-tree scopes are accepted.
    for (const QString &scope : {QString(), QStringLiteral("auto"),
                                 QStringLiteral("full")}) {
        const AuditRunner::RunResult r = run(proj.path(), true, scope);
        EXPECT_NE(r.code, QStringLiteral("bad_args"))
            << "INV-5: scope '" << scope.toStdString() << "' must be accepted";
    }
}

// INV-6 — a damaged or truncated baseline says so instead of guessing.
TEST(AuditRunBaseline, Inv6DamagedBaselineGivesReason) {
    FakeMypy mypy;
    QTemporaryDir proj;
    ASSERT_TRUE(proj.isValid());
    mypy.prints(QStringLiteral("a.py:1: error: alpha\n"));

    ASSERT_TRUE(writeFile(baselinePath(proj.path()), "not json"));
    AuditRunner::RunResult r = run(proj.path(), false);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.sinceBaseline.value("unavailable_reason").toString(),
              QStringLiteral("baseline_unreadable"));
    EXPECT_FALSE(r.sinceBaseline.contains("new_count"));

    ASSERT_TRUE(writeFile(baselinePath(proj.path()),
        R"({"version":1,"iso_timestamp":"x","commit":"nogit",)"
        R"("truncated":true,"tools":["mypy"],"findings":[]})"));
    r = run(proj.path(), false);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.sinceBaseline.value("unavailable_reason").toString(),
              QStringLiteral("baseline_truncated"));
    EXPECT_FALSE(r.sinceBaseline.contains("new_count"));
}

// INV-7 — the pure set difference, by fp, in order.
TEST(AuditRunBaseline, Inv7NewSinceBaselineIsFpDifference) {
    const QJsonObject a = mk("a.py", 1, "mypy", "error: alpha");
    const QJsonObject aMoved = mk("a.py", 9, "mypy", "error: alpha");
    const QJsonObject b = mk("b.py", 2, "mypy", "error: beta");
    const QJsonObject c = mk("c.py", 3, "mypy", "error: gamma");

    const QJsonArray out = AuditDelta::newSinceBaseline(
        QJsonArray{b, aMoved, c}, QJsonArray{a});
    ASSERT_EQ(out.size(), 2);
    EXPECT_EQ(out.at(0).toObject().value("file").toString(), "b.py");
    EXPECT_EQ(out.at(1).toObject().value("file").toString(), "c.py");

    EXPECT_EQ(AuditDelta::newSinceBaseline({}, QJsonArray{a}).size(), 0);
    EXPECT_EQ(AuditDelta::newSinceBaseline(QJsonArray{a}, {}).size(), 1);
}

// INV-8 — the MCP handler reads save_baseline and copies both blocks into
// its envelope. Source-scrape: the handler needs a live ClaudeIntegration.
TEST(AuditRunBaseline, Inv8EnvelopeCarriesBlocks) {
    const std::string src = ants_test::slurpFile(SRC_MCPTOOLREGISTRY_CPP_PATH);
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("QStringLiteral(\"save_baseline\")"), std::string::npos);
    EXPECT_NE(src.find("env[\"since_baseline\"] = r.sinceBaseline"),
              std::string::npos);
    EXPECT_NE(src.find("env[\"baseline_saved\"] = r.baselineSaved"),
              std::string::npos);
}
