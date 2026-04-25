// Feature-conformance test for spec.md —
//
// Source-grep test. Pty::~Pty must spawn a detached std::thread
// for the SIGTERM/SIGKILL escalation, retain the cheap pre-
// escalation reap, capture pid by value, and keep a synchronous
// fallback for thread-creation failure.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_PTYHANDLER_CPP_PATH
#  error "SRC_PTYHANDLER_CPP_PATH compile definition required"
#endif

namespace {

int g_failures = 0;

void expect(bool cond, const char *label, const std::string &detail = {}) {
    if (cond) {
        std::fprintf(stderr, "[PASS] %s\n", label);
    } else {
        std::fprintf(stderr, "[FAIL] %s  %s\n", label, detail.c_str());
        ++g_failures;
    }
}

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

// Slice from "Pty::~Pty()" up to the first top-level closing
// brace that ends the destructor.
std::string extractDtor(const std::string &src) {
    const size_t dtorAt = src.find("Pty::~Pty()");
    if (dtorAt == std::string::npos) return {};
    const size_t openBrace = src.find('{', dtorAt);
    if (openBrace == std::string::npos) return {};
    int depth = 1;
    size_t i = openBrace + 1;
    while (i < src.size() && depth > 0) {
        char c = src[i];
        if (c == '"') {
            ++i;
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\\') ++i;
                if (i < src.size()) ++i;
            }
            if (i < src.size()) ++i;
            continue;
        }
        if (c == '{') ++depth;
        else if (c == '}') --depth;
        ++i;
    }
    return src.substr(openBrace, i - openBrace);
}

}  // namespace

int main() {
    const std::string src = slurp(SRC_PTYHANDLER_CPP_PATH);

    // I4 — <thread> header included.
    expect(contains(src, "#include <thread>"),
           "I4/thread-header-included");

    const std::string dtor = extractDtor(src);
    expect(!dtor.empty(), "extract/dtor-body-found");

    // I1 — std::thread([ ... ]) lambda inside the destructor.
    expect(contains(dtor, "std::thread([pid]()"),
           "I1/dtor-spawns-std-thread-with-pid-capture");

    // I2 — detached, not joined.
    expect(contains(dtor, ".detach();"),
           "I2/dtor-detaches-the-worker-thread");

    // I3 — pid captured by value (the [pid] literal). Guards
    // against a regression that switches to [this] or [&] which
    // would dangle.
    {
        std::regex byValueCapture(R"(std::thread\(\[pid\]\(\))");
        expect(std::regex_search(dtor, byValueCapture),
               "I3/lambda-captures-pid-by-value-only");
    }
    expect(!contains(dtor, "std::thread([this]"),
           "I3/lambda-does-NOT-capture-this");
    expect(!contains(dtor, "std::thread([&"),
           "I3/lambda-does-NOT-capture-by-reference");

    // I5 — synchronous fallback retained inside a catch.
    expect(contains(dtor, "catch (const std::system_error &)"),
           "I5/system-error-catch-clause-present");
    {
        // After the catch keyword, the rest of the destructor body
        // must contain both SIGTERM and SIGKILL — that's the
        // synchronous fallback. We don't need to delimit the catch
        // body's exact end; the only later content in the dtor is
        // `m_childPid = -1;` which doesn't contain these signal
        // names.
        const size_t catchAt = dtor.find("catch (const std::system_error &)");
        const std::string after = (catchAt != std::string::npos)
            ? dtor.substr(catchAt) : std::string{};
        expect(contains(after, "SIGTERM") && contains(after, "SIGKILL"),
               "I5/fallback-keeps-SIGTERM-and-SIGKILL-escalation");
    }

    // I6 — pre-escalation cheap reap retained: SIGHUP, close, then
    // a WNOHANG check before any thread is spawned.
    expect(contains(dtor, "::kill(m_childPid, SIGHUP);"),
           "I6/SIGHUP-still-sent-up-front");
    expect(contains(dtor, "::waitpid(m_childPid, &status, WNOHANG)"),
           "I6/WNOHANG-cheap-reap-retained");

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
