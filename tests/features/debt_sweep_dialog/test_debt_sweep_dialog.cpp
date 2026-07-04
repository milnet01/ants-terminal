// ANTS-1259 — AuditDialog "Debt Sweep" tab feature test (GUI bundle).
//
// INV-D1  debtToAuditFinding field mapping.
// INV-D2  debtFixInline deletes an orphan Q_UNUSED line.
// INV-D3  debtDeferToRoadmap counter bump + block insert (+ no-op on bad heading).
// INV-D4  debtAllow writes a matching allowlist entry; reload makes it match.
// See tests/features/debt_sweep_dialog/spec.md + docs/specs/ANTS-1113.md §1.1.

#include "auditdialog.h"
#include "auditengine.h"
#include "debtsweepengine.h"
#include "config.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>

#ifndef SRC_AUDITDIALOG_CPP_PATH
#error "SRC_AUDITDIALOG_CPP_PATH compile definition required"
#endif

namespace {

class Dlg : public AuditDialog {
public:
    using AuditDialog::AuditDialog;
    using AuditDialog::debtToAuditFinding;
    using AuditDialog::debtFixInline;
    using AuditDialog::debtDeferToRoadmap;
    using AuditDialog::debtAllow;
    using AuditDialog::allowlisted;
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

DebtSweepEngine::Finding mkDebt(const QString &cat, const QString &detector,
                                const QString &file, int line,
                                const QString &msg, bool autoFixable) {
    DebtSweepEngine::Finding d;
    d.category = cat;
    d.detectorId = detector;
    d.file = file;
    d.line = line;
    d.message = msg;
    d.autoFixable = autoFixable;
    return d;
}

}  // namespace

// INV-D1 — field mapping.
TEST(DebtSweepDialog, INVD1_DebtToAuditFindingMapping) {
    const auto d = mkDebt("code_drift", "orphan_q_unused", "src/foo.cpp", 12,
                          "Q_UNUSED(stale) target not declared", true);
    const Finding f = Dlg::debtToAuditFinding(d);
    EXPECT_EQ(f.checkId.toStdString(), "orphan_q_unused");
    EXPECT_EQ(f.file.toStdString(), "src/foo.cpp");
    EXPECT_EQ(f.line, 12);
    EXPECT_EQ(f.message.toStdString(), "Q_UNUSED(stale) target not declared");
}

// INV-D2 — fix deletes the orphan Q_UNUSED line.
TEST(DebtSweepDialog, INVD2_DebtFixInlineDeletesLine) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + "/src/foo.cpp",
        "int main() {\n    Q_UNUSED(stale);\n    return 0;\n}\n"));
    Config cfg;
    Dlg dlg(root, nullptr, &cfg);

    const auto d = mkDebt("code_drift", "orphan_q_unused", "src/foo.cpp", 2,
                          "orphan Q_UNUSED(stale)", true);
    EXPECT_TRUE(dlg.debtFixInline(d));
    const QString body = readFile(root + "/src/foo.cpp");
    EXPECT_FALSE(body.contains(QStringLiteral("Q_UNUSED(stale)"))) << body.toStdString();
}

// INV-D3 — defer folds K findings into ROADMAP; wrong heading is a no-op.
TEST(DebtSweepDialog, INVD3_DebtDeferToRoadmap) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    const QString heading = QStringLiteral("## 0.7.92 — current (target: 2026-05)");
    ASSERT_TRUE(writeFile(root + "/ROADMAP.md",
        "# Roadmap\n\n" + heading + "\n\n### Existing\n- prior\n"));
    ASSERT_TRUE(writeFile(root + "/.roadmap-counter", "3000\n"));
    Config cfg;
    Dlg dlg(root, nullptr, &cfg);

    const QList<DebtSweepEngine::Finding> deferred{
        mkDebt("code_drift", "stale_type_comment", "src/a.cpp", 4, "stale FooBar ref", false),
        mkDebt("doc_drift", "shipped_without_commit", "ROADMAP.md", -1, "ANTS-9 no commit", false),
    };

    ASSERT_TRUE(dlg.debtDeferToRoadmap(deferred, heading));
    EXPECT_EQ(readFile(root + "/.roadmap-counter").trimmed().toStdString(), "3002");

    const QString rm = readFile(root + "/ROADMAP.md");
    EXPECT_TRUE(rm.contains(QStringLiteral("### 🧹 Debt-sweep fold-in (")))
        << rm.toStdString();
    EXPECT_TRUE(rm.contains(QStringLiteral("[ANTS-3001]"))) << rm.toStdString();
    EXPECT_TRUE(rm.contains(QStringLiteral("[ANTS-3002]"))) << rm.toStdString();

    // Wrong heading: false return, counter unchanged (no ID leak).
    EXPECT_FALSE(dlg.debtDeferToRoadmap(deferred, QStringLiteral("## nope")));
    EXPECT_EQ(readFile(root + "/.roadmap-counter").trimmed().toStdString(), "3002");
}

// INV-D4 — allow writes a matching allowlist entry; reload makes it match.
TEST(DebtSweepDialog, INVD4_DebtAllowRoundTrip) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    Config cfg;
    Dlg dlg(root, nullptr, &cfg);

    const auto d = mkDebt("code_drift", "added_todo", "src/b.cpp", 7,
                          "TODO: wire this up", false);
    ASSERT_TRUE(dlg.debtAllow(d, QStringLiteral("tracked elsewhere")));
    // appendAllowlistEntry reloads internally, so the match is live now.
    EXPECT_TRUE(dlg.allowlisted(Dlg::debtToAuditFinding(d)));
    // A different detector/file is not matched.
    EXPECT_FALSE(dlg.allowlisted(Dlg::debtToAuditFinding(
        mkDebt("code_drift", "added_todo", "src/other.cpp", 1, "x", false))));
}

// INV-D5 (ANTS-3346) — the bulk "Defer all to ROADMAP" button gates on the
// shared triage verdict before folding. Source-grep: the defer-all handler
// region references evaluateTriageGate (the modal confirmation itself is not
// unit-tested — button-lambda modals aren't, by precedent here; the gate
// LOGIC is covered behaviourally by DebtSweepEngine.TriageGate* in the
// debt_sweep_engine suite).
TEST(DebtSweepDialog, INVD5_BulkDeferGatedByTriageVerdict) {
    std::ifstream in(SRC_AUDITDIALOG_CPP_PATH);
    ASSERT_TRUE(in.good());
    std::stringstream ss; ss << in.rdbuf();
    const std::string src = ss.str();
    const auto btn = src.find("Defer all to ROADMAP");
    ASSERT_NE(btn, std::string::npos);
    // The connect() lambda's evaluateTriageGate call lands within the button's
    // construction/wiring region (well under 2000 chars of the label).
    const auto gate = src.find("evaluateTriageGate", btn);
    ASSERT_NE(gate, std::string::npos)
        << "Defer-all handler does not call evaluateTriageGate (ANTS-3346)";
    EXPECT_LT(gate - btn, static_cast<size_t>(2000));
}

// Smoke — the dialog constructs with the Debt Sweep tab present.
TEST(DebtSweepDialog, ConstructionSmoke) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    Config cfg;
    Dlg dlg(QFileInfo(tmp.path()).canonicalFilePath(), nullptr, &cfg);
    SUCCEED();
}
