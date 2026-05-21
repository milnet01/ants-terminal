// ANTS-1257 — AuditDialog v2 UI affordances feature test (GUI bundle).
//
// INV-11  visibleSinceBaseline predicate (changed-line ∧ not-in-baseline).
// INV-13  appendAllowlistEntry write + reload + match contract.
// INV-14  foldFindingsIntoRoadmap orchestration (counter bump + block).
// See tests/features/audit_dialog_v2/spec.md + docs/specs/ANTS-1111.md §12.

#include "auditdialog.h"
#include "auditengine.h"
#include "config.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTemporaryDir>

namespace {

// Subclass exposing the protected v2 helpers the tests drive.
class Dlg : public AuditDialog {
public:
    using AuditDialog::AuditDialog;
    using AuditDialog::appendAllowlistEntry;
    using AuditDialog::allowlisted;
    using AuditDialog::loadAllowlist;
    using AuditDialog::actionableFindings;
    using AuditDialog::foldFindingsIntoRoadmap;
    using AuditDialog::visibleSinceBaseline;
};

bool writeFile(const QString &path, const QString &body) {
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(body.toUtf8());
    return true;
}

QString readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

Finding mkFinding(const QString &checkId, const QString &file, int line,
                  const QString &msg, const QString &dedupKey,
                  int confidence = -1) {
    Finding f;
    f.checkId = checkId;
    f.file = file;
    f.line = line;
    f.message = msg;
    f.dedupKey = dedupKey;
    f.confidence = confidence;
    return f;
}

}  // namespace

// INV-11 — visibleSinceBaseline keeps changed-line, not-in-baseline findings.
TEST(AuditDialogV2, INV11_VisibleSinceBaseline) {
    QHash<QString, QSet<int>> recentLines;
    recentLines["src/foo.cpp"] = QSet<int>{10, 11, 12};
    QSet<QString> baseline{QStringLiteral("oldkey")};

    // Changed line, fresh dedupKey → kept.
    EXPECT_TRUE(Dlg::visibleSinceBaseline(
        mkFinding("r", "src/foo.cpp", 11, "m", "newkey"),
        recentLines, baseline));

    // In baseline → dropped (even on a changed line).
    EXPECT_FALSE(Dlg::visibleSinceBaseline(
        mkFinding("r", "src/foo.cpp", 10, "m", "oldkey"),
        recentLines, baseline));

    // Filed finding on a non-changed line → dropped.
    EXPECT_FALSE(Dlg::visibleSinceBaseline(
        mkFinding("r", "src/foo.cpp", 99, "m", "k2"),
        recentLines, baseline));

    // File not in the changed set at all → dropped.
    EXPECT_FALSE(Dlg::visibleSinceBaseline(
        mkFinding("r", "src/other.cpp", 5, "m", "k3"),
        recentLines, baseline));

    // Unfiled finding (no file/line) → passes the recent test, kept.
    EXPECT_TRUE(Dlg::visibleSinceBaseline(
        mkFinding("r", "", -1, "m", "k4"),
        recentLines, baseline));

    // Suffix-path match (scanner reports a longer path than git diff).
    EXPECT_TRUE(Dlg::visibleSinceBaseline(
        mkFinding("r", "proj/src/foo.cpp", 12, "m", "k5"),
        recentLines, baseline));
}

// INV-13 — appendAllowlistEntry writes a matching entry; reload makes
// allowlisted(f) true; an unrelated finding stays false.
TEST(AuditDialogV2, INV13_AppendAllowlistEntry) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    Config cfg;
    Dlg dlg(root, nullptr, &cfg);

    const Finding f =
        mkFinding("clazy-qstring-arg", "src/foo.cpp", 10,
                  "QString allocation in loop", "deadbeef01", 50);

    ASSERT_TRUE(dlg.appendAllowlistEntry(f, QStringLiteral("known FP")));

    // File written with exactly one matching entry.
    const QString json = readFile(root + "/.audit_allowlist.json");
    ASSERT_FALSE(json.isEmpty());
    const QJsonArray arr =
        QJsonDocument::fromJson(json.toUtf8()).object().value("allowlist").toArray();
    ASSERT_EQ(arr.size(), 1);
    const QJsonObject e = arr.at(0).toObject();
    EXPECT_EQ(e.value("rule").toString().toStdString(), "clazy-qstring-arg");
    EXPECT_EQ(e.value("path_glob").toString().toStdString(), "src/foo.cpp");
    EXPECT_EQ(e.value("reason").toString().toStdString(), "known FP");
    EXPECT_FALSE(e.value("line_regex").toString().isEmpty());

    // After reload the finding is allowlisted; an unrelated one isn't.
    dlg.loadAllowlist();
    EXPECT_TRUE(dlg.allowlisted(f));
    EXPECT_FALSE(dlg.allowlisted(
        mkFinding("clazy-qstring-arg", "src/bar.cpp", 1, "different", "k")));
}

// INV-14 — fold orchestration advances the counter by K and inserts the
// dated block after the named heading; a wrong heading is a no-op.
TEST(AuditDialogV2, INV14_FoldFindingsIntoRoadmap) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    const QString heading =
        QStringLiteral("## 0.7.92 — current (target: 2026-05)");
    ASSERT_TRUE(writeFile(root + "/ROADMAP.md",
        "# Roadmap\n\n" + heading + "\n\n### Existing\n- prior item\n"));
    ASSERT_TRUE(writeFile(root + "/.roadmap-counter", "2000\n"));

    Config cfg;
    Dlg dlg(root, nullptr, &cfg);

    const QList<Finding> findings{
        mkFinding("clazy-x", "src/foo.cpp", 10, "redundant arg in foo", "k1", 85),
        mkFinding("cppcheck-y", "src/bar.cpp", 20, "null deref in bar", "k2", 90),
    };

    ASSERT_TRUE(dlg.foldFindingsIntoRoadmap(findings, heading));

    // Counter advanced by exactly K (2000 -> 2002).
    EXPECT_EQ(readFile(root + "/.roadmap-counter").trimmed().toStdString(), "2002");

    // ROADMAP carries the dated block + both allocated ids after the heading.
    const QString rm = readFile(root + "/ROADMAP.md");
    EXPECT_TRUE(rm.contains(QStringLiteral("### 🔍 Audit fold-in (")))
        << rm.toStdString();
    EXPECT_TRUE(rm.contains(QStringLiteral("[ANTS-2001]"))) << rm.toStdString();
    EXPECT_TRUE(rm.contains(QStringLiteral("[ANTS-2002]"))) << rm.toStdString();
    const int hPos = rm.indexOf(heading);
    const int bPos = rm.indexOf(QStringLiteral("### 🔍 Audit fold-in"));
    EXPECT_GT(bPos, hPos) << "block not inserted after the heading";

    // A wrong heading is a no-op (no further counter advance, no second block).
    EXPECT_FALSE(dlg.foldFindingsIntoRoadmap(
        findings, QStringLiteral("## no-such-heading")));
    EXPECT_EQ(readFile(root + "/.roadmap-counter").trimmed().toStdString(), "2002");
}

// Smoke — a fresh dialog (no completed run) has no actionable findings.
TEST(AuditDialogV2, ActionableEmptyOnFreshDialog) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    Config cfg;
    Dlg dlg(QFileInfo(tmp.path()).canonicalFilePath(), nullptr, &cfg);
    EXPECT_TRUE(dlg.actionableFindings().isEmpty());
}
