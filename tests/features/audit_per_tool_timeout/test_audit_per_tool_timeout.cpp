// Audit per-tool timeout — source-grep regression test.
// See spec.md. Fails non-zero if AuditCheck loses the timeoutMs field,
// runNextCheck reverts to a literal 30000, the timeout-handler message
// hard-codes "30s" again, or the calibration overrides for slow tools
// disappear.

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef ANTS_AUDITDIALOG_SOURCES
#  error "ANTS_AUDITDIALOG_SOURCES compile definition required"
#endif
#ifndef SRC_AUDIT_H_PATH
#  error "SRC_AUDIT_H_PATH compile definition required"
#endif


// Locate the body of a free function (e.g. "void AuditDialog::runNextCheck").
// Returns "" if not found.
static std::string extractFnBody(const std::string &src, const char *qualName) {
    std::string pat = std::string("(?:void|int)\\s+") + qualName +
                      R"(\s*\([^)]*\)\s*\{)";
    std::regex re(pat);
    std::smatch m;
    if (!std::regex_search(src, m, re)) return {};
    size_t start = m.position(0) + m.length(0);
    int depth = 1;
    size_t i = start;
    while (i < src.size() && depth > 0) {
        char c = src[i];
        if (c == '{') ++depth;
        else if (c == '}') --depth;
        ++i;
    }
    if (depth != 0) return {};
    return src.substr(start, i - start - 1);
}

TEST(AuditPerToolTimeout, Main) {
    // ANTS-1119 v1: AuditCheck struct moved to auditengine.h. Search
    // both headers / both sources for the per-check `timeoutMs` field
    // and the populateChecks bumps.
    const std::string dialogCpp = ants_test::slurpAuditDialog();
    const std::string dialogHdr = ants_test::slurpFile(SRC_AUDIT_H_PATH);
#ifdef SRC_AUDIT_ENGINE_CPP_PATH
    const std::string engineCpp = ants_test::slurpFile(SRC_AUDIT_ENGINE_CPP_PATH);
#else
    const std::string engineCpp;
#endif
#ifdef SRC_AUDIT_ENGINE_H_PATH
    const std::string engineHdr = ants_test::slurpFile(SRC_AUDIT_ENGINE_H_PATH);
#else
    const std::string engineHdr;
#endif
    const std::string cpp = dialogCpp + engineCpp;
    const std::string hdr = dialogHdr + engineHdr;
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1 — AuditCheck declares int timeoutMs with a numeric default.
    // Match `int timeoutMs = <digits>;` inside the header (must be inside
    // a struct AuditCheck { ... } body in practice; we accept the looser
    // grep because the struct's locality is enforced by the compiler).
    std::regex timeoutMsField(
        R"(int\s+timeoutMs\s*=\s*\d+\s*;)");
    if (!std::regex_search(hdr, timeoutMsField)) {
        fail("INV-1: `int timeoutMs = <default>;` not found in "
             "src/auditdialog.h. Without the per-check field, every check "
             "falls back to whatever cap the runner happens to use.");
    }

    // INV-2 — runNextCheck starts the timer with check.timeoutMs.
    const std::string runNextBody = extractFnBody(cpp, "AuditDialog::runNextCheck");
    if (runNextBody.empty()) {
        fail("precondition: runNextCheck body not located in auditdialog.cpp.");
    } else {
        std::regex perCheckStart(
            R"(m_timeout->start\s*\(\s*check\.timeoutMs\s*\))");
        if (!std::regex_search(runNextBody, perCheckStart)) {
            fail("INV-2: runNextCheck does not call "
                 "m_timeout->start(check.timeoutMs). Hard-coded literals "
                 "force every check to share one cap.");
        }
        // Belt-and-suspenders: forbid the hard-coded 30000 inside the body.
        std::regex hardcoded(R"(m_timeout->start\s*\(\s*30000\s*\))");
        if (std::regex_search(runNextBody, hardcoded)) {
            fail("INV-2: literal `m_timeout->start(30000)` still inside "
                 "runNextCheck — the per-check field exists but the use "
                 "site bypasses it.");
        }
    }

    // INV-3 — timeout-handler warning message reflects the cap, not "30s".
    // The pre-fix string was the literal "Timed out (30s)". Forbid that
    // exact substring; allow any %1-style format that names the cap.
    if (cpp.find("\"Timed out (30s)\"") != std::string::npos) {
        fail("INV-3: literal \"Timed out (30s)\" string still present. The "
             "warning must be parameterized with the actual cap so checks "
             "with longer timeouts report correctly.");
    }
    // Additionally, ensure the timeout lambda references a per-check cap.
    // We accept the canonical pattern m_checks[m_currentCheck].timeoutMs
    // OR a precomputed local that includes the substring "timeoutMs".
    if (cpp.find("timeoutMs") == std::string::npos) {
        fail("INV-3: no `timeoutMs` token anywhere in auditdialog.cpp — "
             "the field exists in the header but the .cpp never reads it.");
    }

    // INV-4 — ANTS-6197: each known-slow tool sets a cap above 30000 on the
    // line after its registration, and no loop overrides caps by id.
    // ANTS-1677 — bounded to populateChecks()' body.
    const std::string checks =
        ants_test::slurpFunctionBody(dialogCpp, "void AuditDialog::populateChecks(");
    if (checks.empty())
        fail("precondition: populateChecks body not located in the AuditDialog sources.");
    static const char *slowTools[] = {
        "cppcheck", "cppcheck_unused", "clang_tidy", "clazy",
        "semgrep", "osv_scanner", "trufflehog", "compiler_warnings",
    };
    static const std::regex capLine(
        R"(^\}\);\s*m_checks\.last\(\)\.timeoutMs\s*=\s*([0-9][0-9 *]*);)");
    for (const char *tool : slowTools) {
        const size_t at = checks.find(std::string("\"") + tool + "\", \"");
        const size_t end = at == std::string::npos ? at : checks.find("});", at);
        std::smatch m;
        const std::string after = end == std::string::npos ? std::string()
                                                           : checks.substr(end, 160);
        long long ms = 0;
        if (std::regex_search(after, m, capLine)) {
            ms = 1;
            std::string expr = m[1].str();
            for (size_t p = 0; p <= expr.size();) {
                const size_t star = std::min(expr.find('*', p), expr.size());
                ms *= std::stoll(expr.substr(p, star - p));
                p = star + 1;
            }
        }
        if (ms <= 30000)
            fail((std::string("INV-4: ") + tool + " sets no cap above 30000 "
                  "on the line after its registration").c_str());
    }
    if (checks.find("c.timeoutMs =") != std::string::npos)
        fail("INV-4: populateChecks still overrides caps in an id-matching loop");

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/audit_per_tool_timeout/spec.md for context\n",
            failures);
        FAIL();
    }
}
