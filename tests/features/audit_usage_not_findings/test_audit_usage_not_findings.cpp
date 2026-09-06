// Feature-conformance test for ANTS-3846 — a tool that printed its usage did
// not find anything. See spec.md.
//
// The fixture is clang-tidy's REAL output, captured 2026-09-06 from the
// installed binary with `clang-tidy --checks=-* a.cpp --`. It matters that
// this is the tool's own bytes rather than a paraphrase: the whole defect is
// that the runner read this text as findings, so a fixture that differed
// from it would prove nothing about the real stream.
//
// Note the exit status in that run: 0. The runner cannot detect this from
// the exit code, which is why it needs a marker at all.

#include "auditrunner.h"

#include <gtest/gtest.h>

#include <QSet>
#include <QString>
#include <QStringLiteral>

namespace {

// Captured verbatim. Truncated after the first few option lines — the banner
// runs to ~90 lines, and it is the HEAD that identifies it.
const char *kClangTidyUsage =
    "Error: no checks enabled.\n"
    "USAGE: clang-tidy [options] <source0> [... <sourceN>]\n"
    "\n"
    "OPTIONS:\n"
    "\n"
    "Generic Options:\n"
    "\n"
    "  --help                           - Display available options (--help-hidden for more)\n"
    "  --version                        - Display the version of this program\n"
    "\n"
    "clang-tidy options:\n"
    "\n"
    "  --allow-enabling-analyzer-alpha-checkers - Allow enabling alpha checkers.\n"
    "  --checks=<string>                - Comma-separated list of globs\n";

// A normal clang-tidy findings stream, for the negative case.
const char *kRealFindings =
    "/src/widget.cpp:12:5: warning: use auto [modernize-use-auto]\n"
    "/src/widget.cpp:40:9: warning: prefer make_unique [modernize-make-unique]\n";

}  // namespace

// INV-1 — the usage banner is an abort, not 92 findings.
TEST(AuditUsageNotFindings, Inv1UsageBannerIsAnAbort) {
    const auto c = AuditRunner::internal::parseWithSuppression(
        QStringLiteral("clang-tidy"), QString::fromUtf8(kClangTidyUsage), 10,
        {});

    EXPECT_TRUE(c.aborted)
        << "a tool that printed its usage analysed nothing — it must reach "
           "incomplete_tools[], not the findings list";
    EXPECT_EQ(c.findingsCount, 0)
        << "every one of these would be a phantom finding: the report said "
           "92, and their contents were option help lines";
    EXPECT_EQ(c.rawCount, 0);
}

// INV-1 — the `no checks enabled` line alone is enough, without the banner.
TEST(AuditUsageNotFindings, Inv1NoChecksEnabledAlone) {
    const auto c = AuditRunner::internal::parseWithSuppression(
        QStringLiteral("clang-tidy"),
        QStringLiteral("Error: no checks enabled.\n"), 10, {});
    EXPECT_TRUE(c.aborted);
    EXPECT_EQ(c.findingsCount, 0);
}

// INV-2 — a normal findings stream is untouched.
TEST(AuditUsageNotFindings, Inv2RealFindingsUnaffected) {
    const auto c = AuditRunner::internal::parseWithSuppression(
        QStringLiteral("clang-tidy"), QString::fromUtf8(kRealFindings), 10, {});
    EXPECT_FALSE(c.aborted)
        << "the guard must not fire on a tool that actually ran";
    EXPECT_EQ(c.findingsCount, 2);
}

// INV-2 — conservative: a finding whose MESSAGE mentions usage is a finding.
// The marker is anchored at line start and only near the head of the stream,
// so this must not be swept up.
TEST(AuditUsageNotFindings, Inv2MessageMentioningUsageIsStillAFinding) {
    const QString raw = QStringLiteral(
        "/src/a.cpp:3:1: warning: document the USAGE: of this helper [x]\n"
        "/src/b.cpp:9:1: warning: usage: prefer the span overload [y]\n");
    const auto c = AuditRunner::internal::parseWithSuppression(
        QStringLiteral("clang-tidy"), raw, 10, {});
    EXPECT_FALSE(c.aborted)
        << "a diagnostic that talks about usage is not a usage banner";
    EXPECT_EQ(c.findingsCount, 2);
}

// ANTS-3846 — the sibling plain-text tools share the guard, since the same
// shape is reachable for any of them (mypy and cppcheck both print usage on a
// bad invocation).
TEST(AuditUsageNotFindings, Inv1AppliesToTheOtherPlainTextTools) {
    for (const char *tool : {"cppcheck", "clazy", "mypy"}) {
        const auto c = AuditRunner::internal::parseWithSuppression(
            QString::fromUtf8(tool),
            QStringLiteral("usage: %1 [options] <files>\n"
                           "\n"
                           "  -h, --help   show this help message and exit\n")
                .arg(QString::fromUtf8(tool)),
            10, {});
        EXPECT_TRUE(c.aborted) << tool << " usage banner must abort";
        EXPECT_EQ(c.findingsCount, 0) << tool;
    }
}
