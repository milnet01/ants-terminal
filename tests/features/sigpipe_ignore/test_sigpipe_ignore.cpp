// SIGPIPE ignore — source-grep regression test.
// See spec.md for the contract. Fails non-zero if main() no longer
// installs SIG_IGN for SIGPIPE, or installs it after QApplication is
// constructed.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_MAIN_PATH
#  error "SRC_MAIN_PATH compile definition required"
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

int main() {
    const std::string m = slurp(SRC_MAIN_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: a signal(SIGPIPE, SIG_IGN) call appears in main.cpp.
    // Accept `std::signal` or bare `signal`, tolerant of whitespace.
    std::regex signalCall(
        R"((?:std::)?signal\s*\(\s*SIGPIPE\s*,\s*SIG_IGN\s*\))");
    std::smatch signalMatch;
    if (!std::regex_search(m, signalMatch, signalCall)) {
        fail("INV-1: signal(SIGPIPE, SIG_IGN) not found in src/main.cpp — "
             "writing to a closed PTY FD now crashes the GUI on shell exit.");
    }

    // INV-2: the call is inside int main() and precedes any QApplication
    // construction. We locate int main(, then take everything from there
    // to the next QApplication constructor, and require the signal call
    // to appear in that window.
    std::regex mainStart(R"(int\s+main\s*\([^)]*\)\s*\{)");
    std::smatch mainMatch;
    if (!std::regex_search(m, mainMatch, mainStart)) {
        fail("INV-2 precondition: could not locate `int main(…) {` in main.cpp");
    } else {
        size_t mainPos = mainMatch.position(0) + mainMatch.length(0);
        // Find the first QApplication-like constructor after main()'s brace.
        std::regex qAppCtor(
            R"(Q(?:Core)?Application\s+\w+\s*\()");
        std::smatch qAppMatch;
        std::string afterMain = m.substr(mainPos);
        if (!std::regex_search(afterMain, qAppMatch, qAppCtor)) {
            fail("INV-2 precondition: no QApplication / QCoreApplication "
                 "constructor found after main() — unable to bound the "
                 "pre-QApplication window.");
        } else {
            std::string preQApp = afterMain.substr(0, qAppMatch.position(0));
            if (!std::regex_search(preQApp, signalCall)) {
                fail("INV-2: signal(SIGPIPE, SIG_IGN) call must appear inside "
                     "main() BEFORE the QApplication constructor. Installing "
                     "it after-the-fact leaves any write path triggered by "
                     "Qt resource loading exposed to termination.");
            }
        }
    }

    // INV-3: <csignal> is included (required for std::signal + SIG_IGN).
    std::regex csignalInclude(R"(#include\s*<csignal>)");
    if (!std::regex_search(m, csignalInclude)) {
        fail("INV-3: #include <csignal> not found — std::signal / SIG_IGN "
             "may not be declared on all libc implementations without it.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/sigpipe_ignore/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: SIGPIPE is ignored process-wide before QApplication\n");
    return 0;
}
