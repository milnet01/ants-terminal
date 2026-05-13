// Feature-conformance test for ANTS-1111 AuditEngine::applyCorroborationShift.

#include <gtest/gtest.h>

#include <QList>
#include <QSet>
#include <QString>

#include "auditengine.h"

namespace {

Finding mk(const QString &checkId, const QString &file, int line,
           Severity sev) {
    Finding f;
    f.checkId = checkId;
    f.file = file;
    f.line = line;
    f.severity = sev;
    f.message = "test";
    return f;
}

}  // namespace

TEST(AuditCorroborationShift, Inv1PromotesOnTwoDistinctTools) {
    QList<Finding> findings = {
        mk("clazy-X", "src/foo.cpp", 10, Severity::Minor),
        mk("cppcheck-Y", "src/foo.cpp", 10, Severity::Major),
        mk("clazy-X", "src/bar.cpp", 20, Severity::Major),  // single tool, control
    };
    QSet<QString> noisy;
    AuditEngine::applyCorroborationShift(findings, noisy);

    EXPECT_EQ(int(findings[0].severity), int(Severity::Major));    // Minor +1
    EXPECT_EQ(int(findings[1].severity), int(Severity::Critical)); // Major +1
    EXPECT_EQ(int(findings[2].severity), int(Severity::Major));    // unchanged
}

TEST(AuditCorroborationShift, Inv1ClampsToBlocker) {
    QList<Finding> findings = {
        mk("clazy-X", "src/foo.cpp", 10, Severity::Blocker),
        mk("cppcheck-Y", "src/foo.cpp", 10, Severity::Blocker),
    };
    QSet<QString> noisy;
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Blocker));
    EXPECT_EQ(int(findings[1].severity), int(Severity::Blocker));
}

TEST(AuditCorroborationShift, Inv2DemotesNoisySingleTool) {
    QList<Finding> findings = {
        mk("noisy-rule", "src/foo.cpp", 10, Severity::Major),
    };
    QSet<QString> noisy = {"noisy-rule"};
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Minor));  // Major -1
}

TEST(AuditCorroborationShift, Inv2ClampsToInfo) {
    QList<Finding> findings = {
        mk("noisy-rule", "src/foo.cpp", 10, Severity::Info),
    };
    QSet<QString> noisy = {"noisy-rule"};
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Info));
}

TEST(AuditCorroborationShift, Inv3NoShiftForCleanSingleTool) {
    QList<Finding> findings = {
        mk("clean-rule", "src/foo.cpp", 10, Severity::Minor),
    };
    QSet<QString> noisy;  // empty
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Minor));
}

TEST(AuditCorroborationShift, SameCheckIdTwiceDoesNotPromote) {
    // Two findings from the SAME checkId at the same line — that's not
    // cross-tool corroboration; severity must stay.
    QList<Finding> findings = {
        mk("clazy-X", "src/foo.cpp", 10, Severity::Minor),
        mk("clazy-X", "src/foo.cpp", 10, Severity::Minor),
    };
    QSet<QString> noisy;
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Minor));
    EXPECT_EQ(int(findings[1].severity), int(Severity::Minor));
}

TEST(AuditCorroborationShift, FindingsWithoutFileLineSkipped) {
    Finding f;
    f.checkId = "rule";
    f.severity = Severity::Major;
    // file empty + line=-1
    QList<Finding> findings = {f};
    QSet<QString> noisy;
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_EQ(int(findings[0].severity), int(Severity::Major));
}

TEST(AuditCorroborationShift, EmptyInputNoOp) {
    QList<Finding> findings;
    QSet<QString> noisy = {"any"};
    AuditEngine::applyCorroborationShift(findings, noisy);
    EXPECT_TRUE(findings.isEmpty());
}
