// Cell-decoding stream-status checks — source-grep regression test.
// See spec.md. Fails non-zero if readCell loses its bool return,
// any call site stops short-circuiting, or the combining-codepoint
// loop drops its status check.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_SESSION_CPP_PATH
#  error "SRC_SESSION_CPP_PATH compile definition required"
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
    std::string pat = std::string("(?:QByteArray|bool|void|QString)\\s+") +
                      qualName + R"(\s*\([\s\S]*?\)\s*(?:const\s*)?\{)";
    std::regex re(pat);
    std::smatch m;
    if (!std::regex_search(src, m, re)) return {};
    size_t start = m.position(0) + m.length(0);
    int depth = 1;
    size_t i = start;
    while (i < src.size() && depth > 0) {
        char c = src[i];
        if (c == '\'') {
            ++i;
            if (i < src.size() && src[i] == '\\') i += 2;
            else ++i;
            if (i < src.size() && src[i] == '\'') ++i;
            continue;
        }
        if (c == 'R' && i + 1 < src.size() && src[i + 1] == '"') {
            i += 2;
            std::string delim;
            while (i < src.size() && src[i] != '(') { delim += src[i]; ++i; }
            ++i;
            std::string close = ")" + delim + "\"";
            size_t end = src.find(close, i);
            i = (end == std::string::npos) ? src.size() : end + close.size();
            continue;
        }
        if (c == '"') {
            ++i;
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < src.size()) i += 2;
                else ++i;
            }
            if (i < src.size()) ++i;
            continue;
        }
        if (c == '{') ++depth;
        else if (c == '}') --depth;
        ++i;
    }
    if (depth != 0) return {};
    return src.substr(start, i - start - 1);
}

int main() {
    const std::string cpp = slurp(SRC_SESSION_CPP_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    const std::string resBody = extractFnBody(cpp, "SessionManager::restore");
    if (resBody.empty()) {
        fail("precondition: SessionManager::restore body not located.");
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/session_cell_loop_stream_status/spec.md\n",
            failures);
        return 1;
    }

    // INV-1 — readCell lambda returns bool.
    // Match `auto readCell = [&in](Cell &c) -> bool {`.
    std::regex readCellDecl(
        R"(auto\s+readCell\s*=\s*\[[^\]]*\]\s*\([^)]*\)\s*->\s*bool\s*\{)");
    if (!std::regex_search(resBody, readCellDecl))
        fail("INV-1: readCell lambda is not declared `-> bool`. Without "
             "a bool return, callers can't tell when the stream went bad "
             "mid-cell, and uninitialized fg/bg/flags flow into the grid.");

    // The body must include at least one `return false` (status check)
    // and a final `return true`. Locate the body via a coarse extract:
    // find `auto readCell` then the matching closing brace.
    std::smatch rcm;
    if (std::regex_search(resBody, rcm, readCellDecl)) {
        const size_t open = rcm.position(0) + rcm.length(0);
        // Brace-walk: simple here, the lambda body has no nested
        // strings or chars worth handling.
        int depth = 1;
        size_t j = open;
        while (j < resBody.size() && depth > 0) {
            if (resBody[j] == '{') ++depth;
            else if (resBody[j] == '}') --depth;
            ++j;
        }
        const std::string lambdaBody = resBody.substr(open, j - open - 1);
        if (lambdaBody.find("in.status()") == std::string::npos)
            fail("INV-1: readCell body does not check in.status() — the "
                 "bool return is decoration without a real failure path.");
        if (lambdaBody.find("return false") == std::string::npos)
            fail("INV-1: readCell body has no `return false` path. The "
                 "status check must short-circuit the lambda.");
        if (lambdaBody.find("return true") == std::string::npos)
            fail("INV-1: readCell body has no `return true` path. Without "
                 "an explicit success return, the lambda relies on falling "
                 "off the end — which is undefined for non-void lambdas.");
    }

    // INV-2 — every readCell call site is `if (!readCell(...))`.
    // Count total readCell( occurrences (excluding the lambda
    // declaration itself, which is `readCell\s*=`), then count the
    // guarded form `if (!readCell(`.
    int totalCalls = 0;
    {
        std::regex any(R"(\breadCell\s*\()");
        auto b = std::sregex_iterator(resBody.begin(), resBody.end(), any);
        auto e = std::sregex_iterator();
        for (auto it = b; it != e; ++it) ++totalCalls;
    }
    int guarded = 0;
    {
        std::regex guard(R"(if\s*\(\s*!\s*readCell\s*\()");
        auto b = std::sregex_iterator(resBody.begin(), resBody.end(), guard);
        auto e = std::sregex_iterator();
        for (auto it = b; it != e; ++it) ++guarded;
    }
    if (totalCalls < 4) {
        std::fprintf(stderr,
            "INV-2 readCell call count: %d (expected ≥ 4 — scrollback, "
            "screen-in-range, screen-skip-cols, screen-skip-rows)\n",
            totalCalls);
        fail("INV-2: fewer than four readCell call sites in restore. The "
             "decoder must touch every saved-cell category, including the "
             "skip paths used when grid dimensions shrink between save and "
             "restore.");
    }
    if (guarded != totalCalls) {
        std::fprintf(stderr,
            "INV-2 readCell guarded vs total: %d / %d\n",
            guarded, totalCalls);
        fail("INV-2: at least one readCell call site does not check the "
             "return value with `if (!readCell(...))`. Bare calls are the "
             "pre-fix shape that silently committed uninitialized cells.");
    }

    // INV-3 — combining-codepoint inner loop checks status.
    // Pattern: inside readCombining body, the `for (int k = 0; k < cpCount;`
    // loop must contain an `in.status() != QDataStream::Ok` check. The
    // simplest shape-check: extract the readCombining body and confirm
    // the substring `in >> cp` is followed (within a small window) by
    // `in.status()`.
    std::regex readCombDecl(
        R"(auto\s+readCombining\s*=\s*\[[^\]]*\]\s*\([^)]*\)\s*->\s*bool\s*\{)");
    std::smatch rcd;
    if (std::regex_search(resBody, rcd, readCombDecl)) {
        const size_t open = rcd.position(0) + rcd.length(0);
        int depth = 1;
        size_t j = open;
        while (j < resBody.size() && depth > 0) {
            if (resBody[j] == '{') ++depth;
            else if (resBody[j] == '}') --depth;
            ++j;
        }
        const std::string combBody = resBody.substr(open, j - open - 1);
        std::regex cpRead(
            R"(in\s*>>\s*cp\s*;[\s\S]{0,80}in\.status\(\)\s*!=\s*QDataStream::Ok)");
        if (!std::regex_search(combBody, cpRead))
            fail("INV-3: readCombining inner codepoint loop does not check "
                 "in.status() after `in >> cp;`. A truncated stream pushes "
                 "default-constructed (0) codepoints into the combining map.");
    } else {
        fail("INV-3: readCombining lambda not located — cannot verify "
             "inner codepoint status check.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/session_cell_loop_stream_status/spec.md\n",
            failures);
        return 1;
    }
    std::printf("OK: readCell + readCombining short-circuit on bad status\n");
    return 0;
}
