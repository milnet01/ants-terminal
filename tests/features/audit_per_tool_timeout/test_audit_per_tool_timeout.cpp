// Audit per-tool timeout — source-grep regression test.
// See spec.md. Fails non-zero if AuditCheck loses the timeoutMs field,
// runNextCheck reverts to a literal 30000, the timeout-handler message
// hard-codes "30s" again, or the calibration overrides for slow tools
// disappear.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_AUDIT_CPP_PATH
#  error "SRC_AUDIT_CPP_PATH compile definition required"
#endif
#ifndef SRC_AUDIT_H_PATH
#  error "SRC_AUDIT_H_PATH compile definition required"
#endif

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

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

int main() {
    const std::string cpp = slurp(SRC_AUDIT_CPP_PATH);
    const std::string hdr = slurp(SRC_AUDIT_H_PATH);
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

    // INV-4 — at least one slow-tool calibration override above 30000.
    // Look for any of the known-slow tool IDs adjacent to a `timeoutMs = `
    // assignment with a value > 30000. We check a conservative window
    // (±200 chars) because the loop body is short.
    static const char *slowTools[] = {
        "cppcheck", "semgrep", "osv_scanner", "trufflehog",
        "clang_tidy", "clazy",
    };
    bool foundOverride = false;
    for (const char *tool : slowTools) {
        std::string needle = std::string("\"") + tool + "\"";
        size_t pos = cpp.find(needle);
        while (pos != std::string::npos) {
            size_t windowStart = pos > 200 ? pos - 200 : 0;
            size_t windowEnd = std::min(pos + 200, cpp.size());
            std::string window = cpp.substr(windowStart, windowEnd - windowStart);
            std::regex assign(R"(timeoutMs\s*=\s*(\d+))");
            std::smatch m;
            if (std::regex_search(window, m, assign)) {
                int v = std::atoi(m[1].str().c_str());
                if (v > 30000) { foundOverride = true; break; }
            }
            pos = cpp.find(needle, pos + 1);
        }
        if (foundOverride) break;
    }
    if (!foundOverride) {
        fail("INV-4: no slow-tool ID (cppcheck / semgrep / osv_scanner / "
             "trufflehog / clang_tidy / clazy) found adjacent to a "
             "`timeoutMs = ` assignment with a value above 30000. The "
             "structural fix is incomplete without calibrated defaults.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/audit_per_tool_timeout/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: per-tool timeoutMs declared, used, and calibrated\n");
    return 0;
}
