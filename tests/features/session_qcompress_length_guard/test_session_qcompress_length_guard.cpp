// qCompress length-prefix pre-flight — source-grep regression test.
// See spec.md. Fails non-zero if restore stops pre-validating the
// big-endian length prefix or reorders the check after qUncompress.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_SESSION_CPP_PATH
#  error "SRC_SESSION_CPP_PATH compile definition required"
#endif
#ifndef SRC_SESSION_H_PATH
#  error "SRC_SESSION_H_PATH compile definition required"
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
    const std::string hdr = slurp(SRC_SESSION_H_PATH);
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
            "tests/features/session_qcompress_length_guard/spec.md\n",
            failures);
        return 1;
    }

    // INV-2 — header declares MAX_UNCOMPRESSED at 500 MB.
    std::regex maxDecl(
        R"(static\s+constexpr\s+uint32_t\s+MAX_UNCOMPRESSED\s*=\s*([^;]+);)");
    std::smatch mx;
    if (!std::regex_search(hdr, mx, maxDecl)) {
        fail("INV-2: SessionManager::MAX_UNCOMPRESSED not declared in "
             "sessionmanager.h. The pre-flight threshold needs a named "
             "constant so it can't drift from the post-hoc cap.");
    } else {
        // The expression must evaluate to 500 * 1024 * 1024. We accept
        // any spelling that contains "500" and "1024" (e.g.
        // `500u * 1024u * 1024u`).
        const std::string expr = mx[1].str();
        if (expr.find("500") == std::string::npos ||
            expr.find("1024") == std::string::npos)
            fail("INV-2: MAX_UNCOMPRESSED expression doesn't look like the "
                 "expected 500 * 1024 * 1024. Mismatched threshold means "
                 "the pre-flight rejects too late or too early.");
    }

    // INV-1 + INV-3 — restore body has a pre-flight reconstruction
    // before qUncompress.
    // Find the position of the first qUncompress( call.
    const size_t uncompressPos = resBody.find("qUncompress(");
    if (uncompressPos == std::string::npos) {
        fail("precondition: restore body does not call qUncompress at all "
             "— this test is for a function that decompresses session data.");
    } else {
        // Look for an explicit shift-or pattern indicating big-endian
        // reconstruction. We allow either of:
        //   (uint32_t(b[0]) << 24) | ...
        //   bytes[0] << 24 | bytes[1] << 16 ...
        std::regex be32(R"(<<\s*24[\s\S]{0,200}?<<\s*16[\s\S]{0,200}?<<\s*8)");
        std::smatch bem;
        const std::string before = resBody.substr(0, uncompressPos);
        if (!std::regex_search(before, bem, be32)) {
            fail("INV-1/INV-3: no big-endian uint32 reconstruction "
                 "(<< 24 / << 16 / << 8 chain) found before qUncompress in "
                 "restore. The pre-flight check on qCompress's 4-byte "
                 "length prefix is missing or has been moved after the "
                 "qUncompress allocation.");
        }
        // INV-2 reference — pre-flight uses MAX_UNCOMPRESSED.
        if (before.find("MAX_UNCOMPRESSED") == std::string::npos)
            fail("INV-2: pre-flight check before qUncompress does not "
                 "reference MAX_UNCOMPRESSED. Inline numeric literals here "
                 "drift from the named cap.");
    }

    // INV-4 — short-payload guard.
    std::regex shortGuard(
        R"(compressed\.size\(\)\s*<\s*4[\s\S]{0,40}return\s+false)");
    if (!std::regex_search(resBody, shortGuard))
        fail("INV-4: no `if (compressed.size() < 4) return false` guard "
             "found in restore. A payload too short to contain a length "
             "prefix mustn't be passed to qUncompress.");

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/session_qcompress_length_guard/spec.md\n",
            failures);
        return 1;
    }
    std::printf("OK: qUncompress length-prefix pre-flight in place\n");
    return 0;
}
