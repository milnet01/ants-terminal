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
                           const QString &scope = {}, int minConfidence = 0) {
    AuditRunner::RunRequest req;
    req.projectRoot       = root;
    req.tools             = {QStringLiteral("mypy")};
    req.scope             = scope;
    req.capPerToolSeconds = 30;
    req.saveBaseline      = save;
    req.minConfidence     = minConfidence;
    return AuditRunner::runAudit(req);
}

QJsonObject finding(const QString &file, const QString &severity) {
    QJsonObject o;
    o["file"]     = file;
    o["line"]     = 1;
    o["severity"] = severity;
    o["message"]  = QStringLiteral("a finding message long enough to count");
    return o;
}

// One kept finding (65), one note (35) and one test-path error (45).
const char *kMixed =
    "src/a.py:1: error: keep me\n"
    "src/a.py:2: style: hide me\n"
    "tests/t.py:3: error: hide me too\n";

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
    EXPECT_NE(src.find("QStringLiteral(\"min_confidence\")"), std::string::npos);
    // ANTS-6221 — the reply's fields come from AuditRunner::doneEnvelope.
    AuditRunner::RunResult r;
    r.sinceBaseline = QJsonObject{{QStringLiteral("new_count"), 2}};
    r.baselineSaved = QJsonObject{{QStringLiteral("path"), QStringLiteral("b.json")}};
    r.minConfidence = 40;
    r.belowConfidenceFloor = 6;
    const QJsonObject env = AuditRunner::doneEnvelope(r);
    EXPECT_EQ(env.value(QStringLiteral("since_baseline")).toObject(), r.sinceBaseline);
    EXPECT_EQ(env.value(QStringLiteral("baseline_saved")).toObject(), r.baselineSaved);
    EXPECT_EQ(env.value(QStringLiteral("below_confidence_floor_count")).toInt(), 6);
    EXPECT_FALSE(AuditRunner::doneEnvelope(AuditRunner::RunResult{})
                     .contains(QStringLiteral("below_confidence_floor_count")))
        << "no floor, no count";
}

// INV-9 — the dialog's formula, fed from a headless finding.
TEST(AuditRunBaseline, Inv9ConfidenceForUsesTheDialogFormula) {
    using AuditRunner::internal::confidenceFor;
    // floor 10 + Critical 45 + external tool 10
    EXPECT_EQ(confidenceFor("mypy", finding("src/a.py", "error"), false), 65);
    EXPECT_EQ(confidenceFor("mypy", finding("src/a.py", "warning"), false), 50);
    EXPECT_EQ(confidenceFor("mypy", finding("src/a.py", "style"), false), 35);
    EXPECT_EQ(confidenceFor("mypy", finding("src/a.py", "error"), true), 85)
        << "INV-9: corroboration adds 20";
    EXPECT_EQ(confidenceFor("mypy", finding("tests/t.py", "error"), false), 45)
        << "INV-9: a relative tests/ path takes the test penalty";
    EXPECT_EQ(confidenceFor("mypy", finding("test/t.py", "error"), false), 45);
    EXPECT_EQ(confidenceFor("trivy", finding("go.sum", "error"), false), 55)
        << "INV-9: trivy is not in the external-tool set";
    EXPECT_EQ(confidenceFor("gitleaks", finding(".env", "error"), false), 65)
        << "INV-9: gitleaks scores as the secrets source";
}

// INV-9 — corroboration is another tool at the same file:line.
TEST(AuditRunBaseline, Inv9FloorCountsCorroboration) {
    QHash<QString, QJsonArray> byTool;
    byTool["mypy"]  = QJsonArray{finding("src/a.py", "style")};    // 35, +20 = 55
    byTool["ruff"]  = QJsonArray{finding("src/a.py", "style")};    // 55
    byTool["bandit"] = QJsonArray{finding("src/b.py", "style")};   // 35, alone
    QHash<QString, int> dropped;
    const auto kept =
        AuditRunner::internal::applyConfidenceFloor(byTool, 50, &dropped);
    EXPECT_EQ(kept.value("mypy").size(), 1);
    EXPECT_EQ(kept.value("ruff").size(), 1);
    EXPECT_EQ(kept.value("bandit").size(), 0);
    EXPECT_EQ(dropped.value("bandit"), 1);
    EXPECT_EQ(dropped.value("mypy"), 0);
    EXPECT_EQ(kept.value("mypy").at(0).toObject().value("confidence").toInt(), 55);
}

// INV-10 — the floor hides low findings from the reply's counts and samples.
TEST(AuditRunBaseline, Inv10FloorHidesLowFindings) {
    FakeMypy mypy;
    QTemporaryDir proj;
    ASSERT_TRUE(proj.isValid());
    mypy.prints(QString::fromLatin1(kMixed));

    const AuditRunner::RunResult r = run(proj.path(), false, {}, 50);
    ASSERT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();
    ASSERT_TRUE(r.byTool.contains(QStringLiteral("mypy")));
    const AuditRunner::ToolResult t = r.byTool.value(QStringLiteral("mypy"));
    EXPECT_EQ(t.rawCount, 3) << "the raw total is the tool's own";
    EXPECT_EQ(t.afterFilterCount, 1) << "INV-10: two findings are below 50";
    EXPECT_EQ(r.totalActionable, 1);
    EXPECT_EQ(r.belowConfidenceFloor, 2);
    EXPECT_EQ(r.minConfidence, 50);
    ASSERT_EQ(t.samples.size(), 1);
    const QJsonObject s0 = t.samples.at(0).toObject();
    EXPECT_EQ(s0.value("file").toString(), QStringLiteral("src/a.py"));
    ASSERT_TRUE(s0.value("confidence").isDouble())
        << "INV-10: a kept sample carries its confidence";
    EXPECT_EQ(s0.value("confidence").toInt(), 65);
}

// INV-11 — floor 0 is off; out of range refuses.
TEST(AuditRunBaseline, Inv11ZeroIsOffAndRangeIsChecked) {
    FakeMypy mypy;
    QTemporaryDir proj;
    ASSERT_TRUE(proj.isValid());
    mypy.prints(QString::fromLatin1(kMixed));

    const AuditRunner::RunResult r = run(proj.path(), false, {}, 0);
    ASSERT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();
    EXPECT_EQ(r.byTool.value(QStringLiteral("mypy")).afterFilterCount, 3);
    EXPECT_EQ(r.belowConfidenceFloor, 0);

    for (int bad : {-1, 101}) {
        const AuditRunner::RunResult rb = run(proj.path(), false, {}, bad);
        EXPECT_FALSE(rb.ok) << "INV-11: min_confidence " << bad;
        EXPECT_EQ(rb.code, QStringLiteral("bad_args")) << bad;
    }
}

// INV-12 — the floor filters the new-since-baseline view, never the saved
// baseline.
TEST(AuditRunBaseline, Inv12FloorNeverShrinksTheBaseline) {
    FakeMypy mypy;
    QTemporaryDir proj;
    ASSERT_TRUE(proj.isValid());
    mypy.prints(QString::fromLatin1(kMixed));

    const AuditRunner::RunResult saved = run(proj.path(), true, {}, 50);
    ASSERT_TRUE(saved.ok) << saved.code.toStdString();
    EXPECT_EQ(saved.baselineSaved.value("findings_count").toInt(), 3)
        << "INV-12: the baseline keeps the findings the floor hid";

    // Without a floor the hidden two are not new.
    const AuditRunner::RunResult noFloor = run(proj.path(), false);
    ASSERT_TRUE(noFloor.sinceBaseline.value("new_count").isDouble());
    EXPECT_EQ(noFloor.sinceBaseline.value("new_count").toInt(), 0);

    // A fresh low finding is new without a floor, hidden with one.
    mypy.prints(QString::fromLatin1(kMixed)
                + QStringLiteral("src/c.py:4: style: fresh and low\n"));
    EXPECT_EQ(run(proj.path(), false).sinceBaseline.value("new_count").toInt(), 1);
    const AuditRunner::RunResult floored = run(proj.path(), false, {}, 50);
    ASSERT_TRUE(floored.sinceBaseline.value("new_count").isDouble());
    EXPECT_EQ(floored.sinceBaseline.value("new_count").toInt(), 0)
        << "INV-12: since_baseline.new honours the floor";
}
