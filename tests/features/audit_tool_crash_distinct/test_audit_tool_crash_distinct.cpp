// Audit tool-crash distinct-from-empty — source-grep regression test.
// See spec.md. Fails non-zero if onCheckFinished reverts to ignoring its
// parameters or stops branching on QProcess::CrashExit.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_AUDIT_CPP_PATH
#  error "SRC_AUDIT_CPP_PATH compile definition required"
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
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1 — onCheckFinished parameters are named, not /*…*/'d out.
    // Match the function signature in the .cpp file. The pre-fix shape
    // was `int /*exitCode*/, QProcess::ExitStatus /*status*/`; we forbid
    // EITHER /* */ comment on the parameters.
    std::regex preFixSig(
        R"(void\s+AuditDialog::onCheckFinished\s*\(\s*int\s*/\*[^*]*\*/\s*,)");
    if (std::regex_search(cpp, preFixSig)) {
        fail("INV-1: onCheckFinished still uses `int /*exitCode*/` — the "
             "parameter is being discarded as a comment, so the function "
             "cannot branch on the exit code.");
    }
    std::regex preFixStatus(
        R"(QProcess::ExitStatus\s*/\*[^*]*\*/\s*\))");
    if (std::regex_search(cpp, preFixStatus)) {
        fail("INV-1: onCheckFinished still uses `QProcess::ExitStatus "
             "/*status*/` — the status parameter is being discarded so "
             "CrashExit cannot be detected.");
    }
    // Positive form: signature must use named parameters.
    std::regex namedSig(
        R"(void\s+AuditDialog::onCheckFinished\s*\(\s*int\s+\w+\s*,\s*QProcess::ExitStatus\s+\w+\s*\))");
    if (!std::regex_search(cpp, namedSig)) {
        fail("INV-1: onCheckFinished signature does not use named "
             "parameters of the expected shape `(int <name>, "
             "QProcess::ExitStatus <name>)`.");
    }

    // INV-2 — QProcess::CrashExit referenced inside the function body.
    const std::string body = extractFnBody(cpp, "AuditDialog::onCheckFinished");
    if (body.empty()) {
        fail("precondition: onCheckFinished body could not be extracted.");
    } else {
        if (body.find("CrashExit") == std::string::npos) {
            fail("INV-2: QProcess::CrashExit not referenced inside "
                 "onCheckFinished. Without this branch, a tool that "
                 "segfaults is indistinguishable from a clean run.");
        }
    }

    // INV-3 — warning emission for the crash path. Accept either form:
    //   (a) direct `r.warning = true` inside the function body, OR
    //   (b) a call to a helper (e.g. `makeToolHealthWarning(...)`) and
    //       the helper itself contains the assignment.
    if (!body.empty()) {
        bool sawWarningEmission = false;
        if (body.find("warning = true") != std::string::npos ||
            body.find("warning   = true") != std::string::npos) {
            sawWarningEmission = true;
        } else {
            // Helper-extracted form. Look for any `Warning(` / `Health(`
            // helper call inside the body, and verify the helper sets
            // .warning = true at file scope.
            std::regex helperCall(
                R"((make[A-Za-z]*Warning|emit[A-Za-z]*Warning|Health[A-Za-z]*)\s*\()");
            if (std::regex_search(body, helperCall)) {
                std::regex helperBody(
                    R"((make[A-Za-z]*Warning|emit[A-Za-z]*Warning|Health[A-Za-z]*)[\s\S]{0,800}warning\s*=\s*true)");
                if (std::regex_search(cpp, helperBody))
                    sawWarningEmission = true;
            }
        }
        if (!sawWarningEmission) {
            fail("INV-3: no `warning = true` emission (direct or via a "
                 "helper) reachable from onCheckFinished. The crash "
                 "branch must mark the row as a warning so the renderer "
                 "styles it distinctly.");
        }
        bool sawCrashWord =
            body.find("crashed") != std::string::npos ||
            body.find("crash") != std::string::npos ||
            body.find("Tool exited") != std::string::npos;
        if (!sawCrashWord) {
            fail("INV-3: no string containing 'crash' or 'Tool exited' "
                 "inside onCheckFinished — the warning message must "
                 "name the failure mode for the user.");
        }
    }

    // INV-4 — Severity::Info on tool-health warnings. The crash and
    // non-zero-exit branches must demote severity so they don't sort
    // to the top of the report. Look for `Severity::Info` inside the
    // body.
    if (!body.empty() && body.find("Severity::Info") == std::string::npos) {
        // Helper-extracted form is also acceptable — accept any of:
        //   - direct r.severity = Severity::Info inside the function body, OR
        //   - call to a helper that contains the assignment (most
        //     auditdialog.cpp helpers will be visible at file scope, so
        //     just grep the whole .cpp for the helper definition).
        std::regex helperWithInfo(
            R"(makeToolHealthWarning[\s\S]{0,500}Severity::Info)");
        if (!std::regex_search(cpp, helperWithInfo)) {
            fail("INV-4: tool-health warning rows are not demoted to "
                 "Severity::Info. Without this, a Major-severity check "
                 "that crashed sorts to the top of the report next to "
                 "real findings.");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/audit_tool_crash_distinct/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: tool-crash branch produces a distinct Info warning\n");
    return 0;
}
