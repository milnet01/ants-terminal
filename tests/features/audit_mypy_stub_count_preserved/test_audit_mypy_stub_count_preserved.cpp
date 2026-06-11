// Feature-conformance test for tests/features/audit_mypy_stub_count_preserved/spec.md.
//
// Verifies ANTS-1343 — pre-collapse findingCount preserved across the
// AuditEngine::consolidateMypyStubHints() + capFindings() pipeline.

#include "auditengine.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"


#include <fstream>
#include <sstream>
#include <string>

namespace {


Finding makeStubFinding(const QString &pkg) {
    Finding f;
    f.checkId   = QStringLiteral("mypy");
    f.checkName = QStringLiteral("mypy");
    f.category  = QStringLiteral("python");
    f.type      = CheckType::CodeSmell;
    f.severity  = Severity::Minor;
    f.source    = QStringLiteral("mypy");
    f.message   = QStringLiteral(
        R"(Library stubs not installed for "%1")").arg(pkg);
    f.dedupKey  = AuditEngine::computeDedup(
        QString(), 0, f.checkId, f.message);
    return f;
}

CheckResult makeMypyResult(int packageCount) {
    CheckResult r;
    r.checkId   = QStringLiteral("mypy");
    r.checkName = QStringLiteral("mypy");
    r.category  = QStringLiteral("python");
    r.type      = CheckType::CodeSmell;
    r.severity  = Severity::Minor;
    r.source    = QStringLiteral("mypy");
    for (int i = 0; i < packageCount; ++i) {
        r.findings.append(
            makeStubFinding(QStringLiteral("pkg%1").arg(i)));
    }
    return r;
}

}  // namespace

// INV-1 — header declares the flag, default-false.
TEST(AuditMypyStubCountPreserved, Inv1FlagInHeader) {
    const std::string hdr = ants_test::slurpFile(SRC_AUDIT_ENGINE_H);
    ASSERT_FALSE(hdr.empty()) << "auditengine.h not readable";
    EXPECT_NE(hdr.find("findingCountAuthored"), std::string::npos)
        << "CheckResult.findingCountAuthored not declared in header";
}

// INV-2 — non-mypy consolidator no-op.
TEST(AuditMypyStubCountPreserved, Inv2NonMypyNoOp) {
    CheckResult r;
    r.checkId = QStringLiteral("cppcheck");
    r.findings.append(makeStubFinding(QStringLiteral("requests")));
    r.findings.append(makeStubFinding(QStringLiteral("yaml")));
    AuditEngine::consolidateMypyStubHints(r);
    EXPECT_FALSE(r.findingCountAuthored);
    EXPECT_EQ(r.findings.size(), 2);
}

// INV-3 — single mypy stub package: no fold, no flag.
TEST(AuditMypyStubCountPreserved, Inv3SinglePackageNoFold) {
    CheckResult r = makeMypyResult(1);
    // Need ≥ 2 entries for the early-exit path; pad with a non-stub.
    Finding plain;
    plain.checkId   = QStringLiteral("mypy");
    plain.message   = QStringLiteral("some unrelated mypy warning");
    plain.dedupKey  = AuditEngine::computeDedup(
        QString(), 0, plain.checkId, plain.message);
    r.findings.append(plain);

    AuditEngine::consolidateMypyStubHints(r);
    EXPECT_FALSE(r.findingCountAuthored);
    EXPECT_EQ(r.findings.size(), 2);
}

// INV-4 — multi-package collapse stamps count + flag.
TEST(AuditMypyStubCountPreserved, Inv4MultiPackageCollapse) {
    CheckResult r = makeMypyResult(23);  // 23 stub packages
    const int preCollapse = r.findings.size();

    AuditEngine::consolidateMypyStubHints(r);

    EXPECT_TRUE(r.findingCountAuthored);
    EXPECT_EQ(r.findings.size(), 1)
        << "synthetic Info hint should be the only entry";
    EXPECT_EQ(r.findingCount, preCollapse + r.omittedCount)
        << "findingCount should reflect pre-collapse 23, not post-collapse 1";
    EXPECT_EQ(r.findingCount, 23);

    // The surviving entry is the synthetic Info hint.
    const Finding &hint = r.findings.first();
    EXPECT_EQ(hint.type, CheckType::Info);
    EXPECT_TRUE(hint.message.contains(QStringLiteral("23 missing")));
}

// INV-5 — dispatcher source-grep: post-cap line gates on the flag.
TEST(AuditMypyStubCountPreserved, Inv5DispatcherGate) {
    const std::string src = ants_test::slurpFile(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty()) << "auditdialog.cpp not readable";
    // The full sequence is the consolidate → capFindings → gated
    // overwrite. We search for the gating predicate by name to keep the
    // grep robust to surrounding formatting.
    EXPECT_NE(src.find("!r.findingCountAuthored"), std::string::npos)
        << "auditdialog.cpp dispatcher no longer gates the findingCount "
           "overwrite on findingCountAuthored (ANTS-1343 INV-5)";
}
