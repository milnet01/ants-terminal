// Audit incremental output drain — source-grep regression test.
// See spec.md. Fails non-zero if the audit runner reverts to the
// readAllStandardOutput-once pattern, drops the cap, or stops resetting
// per-check buffers.

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

    // INV-1 — readyReadStandardOutput / readyReadStandardError connections.
    std::regex readyReadOut(
        R"(connect\s*\([^;]*&QProcess::readyReadStandardOutput)");
    std::regex readyReadErr(
        R"(connect\s*\([^;]*&QProcess::readyReadStandardError)");
    if (!std::regex_search(cpp, readyReadOut)) {
        fail("INV-1: no connect(...&QProcess::readyReadStandardOutput) found. "
             "Without incremental drain, all output buffers in QProcess "
             "until finished() — pathological tools can buffer GB.");
    }
    if (!std::regex_search(cpp, readyReadErr)) {
        fail("INV-1: no connect(...&QProcess::readyReadStandardError) found. "
             "Stderr drain is required so error spew also gets bounded.");
    }

    // INV-2 — buffer members on the dialog. Accept either name.
    if (hdr.find("m_currentOutput") == std::string::npos) {
        fail("INV-2: no m_currentOutput buffer member in src/auditdialog.h.");
    }
    if (hdr.find("m_currentError") == std::string::npos) {
        fail("INV-2: no m_currentError buffer member in src/auditdialog.h.");
    }

    // INV-3 — MAX_TOOL_OUTPUT_BYTES cap declared (>= 4 MiB).
    // Accept the named constant; verify the literal cap is reasonable.
    if (cpp.find("MAX_TOOL_OUTPUT_BYTES") == std::string::npos &&
        hdr.find("MAX_TOOL_OUTPUT_BYTES") == std::string::npos) {
        fail("INV-3: MAX_TOOL_OUTPUT_BYTES cap symbol not found in "
             "auditdialog.{h,cpp}. Without the cap, runaway tools OOM "
             "the GUI.");
    }
    // Verify a defensible value (>= 4 MiB; pragmatic upper bound: < 1 GiB).
    std::regex capDecl(
        R"(MAX_TOOL_OUTPUT_BYTES\s*=\s*(\d+)\s*\*\s*(\d+)\s*\*\s*(\d+))");
    std::smatch capMatch;
    bool capOk = false;
    if (std::regex_search(hdr, capMatch, capDecl) ||
        std::regex_search(cpp, capMatch, capDecl)) {
        long long product = 1;
        for (int g = 1; g <= 3; ++g)
            product *= std::atoll(capMatch[g].str().c_str());
        if (product >= 4LL * 1024 * 1024 && product < (1LL << 30)) capOk = true;
    }
    if (!capOk) {
        fail("INV-3: MAX_TOOL_OUTPUT_BYTES value is missing or out of "
             "the 4 MiB ≤ cap < 1 GiB defensible range.");
    }

    // INV-4 — buffers reset at the start of each check.
    const std::string runNextBody = extractFnBody(cpp, "AuditDialog::runNextCheck");
    if (!runNextBody.empty()) {
        bool resetOk =
            runNextBody.find("m_currentOutput.clear") != std::string::npos &&
            runNextBody.find("m_currentError.clear") != std::string::npos;
        if (!resetOk) {
            fail("INV-4: runNextCheck does not clear m_currentOutput / "
                 "m_currentError before the next process start. Output "
                 "from check N+1 will concatenate with check N's tail.");
        }
    }

    // INV-5 — onCheckFinished reads from buffers, not the live process.
    const std::string finishedBody =
        extractFnBody(cpp, "AuditDialog::onCheckFinished");
    if (!finishedBody.empty()) {
        if (finishedBody.find("m_currentOutput") == std::string::npos) {
            fail("INV-5: onCheckFinished does not reference m_currentOutput. "
                 "Reverted to the live-process readAll pattern, defeating "
                 "the incremental drain.");
        }
        // Direct readAllStandardOutput on the process is allowed only as a
        // tail-drain (Qt may buffer between last readyRead and finished()),
        // but the *primary* read must come from m_currentOutput. We allow
        // up to one such call.
        std::regex liveReadAll(
            R"(m_process->readAllStandardOutput\s*\(\s*\))");
        auto begin = std::sregex_iterator(
            finishedBody.begin(), finishedBody.end(), liveReadAll);
        auto end = std::sregex_iterator();
        const auto count = std::distance(begin, end);
        if (count > 1) {
            fail("INV-5: more than one m_process->readAllStandardOutput() "
                 "call inside onCheckFinished — likely a regression to the "
                 "pre-fix one-shot read pattern.");
        }
    }

    // INV-6 — overflow path kills the process. Look for kill() inside one
    // of the drain slots (we accept either onCheckOutputReady or
    // onCheckErrorReady carrying the kill).
    bool killInDrain = false;
    for (const char *fn : {
        "AuditDialog::onCheckOutputReady",
        "AuditDialog::onCheckErrorReady",
    }) {
        const std::string body = extractFnBody(cpp, fn);
        if (body.empty()) continue;
        if (body.find("MAX_TOOL_OUTPUT_BYTES") != std::string::npos &&
            body.find("kill") != std::string::npos) {
            killInDrain = true;
            break;
        }
    }
    if (!killInDrain) {
        fail("INV-6: neither drain slot tests against MAX_TOOL_OUTPUT_BYTES "
             "and calls kill() on overflow. Without this, a runaway tool "
             "is bounded only by the timeout — and 30 s of unbounded "
             "buffering can still OOM the GUI.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/audit_incremental_output_drain/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: incremental drain wired with bounded buffer\n");
    return 0;
}
