// SHA-256 envelope on session files — source-grep regression test.
// See spec.md. Fails non-zero if serialize stops writing the envelope,
// restore stops verifying it, or the envelope constants drift.

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

// Locate the body of a member function by qualified name (e.g.
// "SessionManager::serialize"). Brace counter skips char literals,
// plain string literals, and raw string literals so braces inside
// them don't shift depth.
static std::string extractFnBody(const std::string &src, const char *qualName) {
    // Match common signatures we care about. Return type can be
    // QByteArray, bool, or void; the function may have parameters
    // spread across multiple lines (so consume non-greedy to the
    // first `{`).
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

    // INV-1 — serialize writes the envelope.
    const std::string serBody = extractFnBody(cpp, "SessionManager::serialize");
    if (serBody.empty()) {
        fail("precondition: SessionManager::serialize body not located.");
    } else {
        if (serBody.find("ENVELOPE_MAGIC") == std::string::npos)
            fail("INV-1: serialize body does not reference ENVELOPE_MAGIC. "
                 "The qCompress output must be wrapped in the V4 envelope, "
                 "not returned raw.");
        if (serBody.find("ENVELOPE_VERSION") == std::string::npos)
            fail("INV-1: serialize body does not reference ENVELOPE_VERSION. "
                 "The envelope header must include the version field so "
                 "future format bumps can be detected.");
        std::regex shaCall(
            R"(QCryptographicHash::hash\s*\([^)]*QCryptographicHash::Sha256)");
        if (!std::regex_search(serBody, shaCall))
            fail("INV-1: serialize body lacks a "
                 "QCryptographicHash::hash(..., QCryptographicHash::Sha256) "
                 "call. The envelope must include a SHA-256 over the "
                 "compressed payload.");
    }

    // INV-2 — restore peeks magic and verifies the hash.
    const std::string resBody = extractFnBody(cpp, "SessionManager::restore");
    if (resBody.empty()) {
        fail("precondition: SessionManager::restore body not located.");
    } else {
        if (resBody.find("ENVELOPE_MAGIC") == std::string::npos)
            fail("INV-2: restore body does not reference ENVELOPE_MAGIC. "
                 "Without the magic peek, V4 envelopes look like garbage to "
                 "the legacy decoder.");
        std::regex shaCall(
            R"(QCryptographicHash::hash\s*\([^)]*QCryptographicHash::Sha256)");
        if (!std::regex_search(resBody, shaCall))
            fail("INV-2: restore body does not recompute the SHA-256. "
                 "The hash field must be verified before decompression.");
        // Tolerate either `actual != sha` or `sha != actual`; require a
        // mismatch-→-return-false somewhere in the body.
        std::regex hashCmpReturn(
            R"((?:!=|==)[^;]*(?:sha|hash)[\s\S]{0,80}return\s+false)",
            std::regex::icase);
        if (!std::regex_search(resBody, hashCmpReturn))
            fail("INV-2: restore body has the SHA-256 recomputation but "
                 "no `return false` gated on the comparison. A hash that "
                 "is computed and not checked is decoration, not defense.");
    }

    // INV-3 — envelope constants exist on SessionManager.
    std::regex magicDecl(
        R"(static\s+constexpr\s+uint32_t\s+ENVELOPE_MAGIC\s*=\s*0x53484543)");
    if (!std::regex_search(hdr, magicDecl))
        fail("INV-3: SessionManager::ENVELOPE_MAGIC missing from "
             "sessionmanager.h, or value drifted from 0x53484543. The "
             "magic must be unique enough to disambiguate from qCompress's "
             "uncompressed-length prefix.");
    std::regex versionDecl(
        R"(static\s+constexpr\s+uint32_t\s+ENVELOPE_VERSION\s*=\s*(\d+))");
    std::smatch m;
    if (!std::regex_search(hdr, m, versionDecl)) {
        fail("INV-3: SessionManager::ENVELOPE_VERSION missing from "
             "sessionmanager.h.");
    } else if (m[1].str() != "1") {
        fail("INV-4: ENVELOPE_VERSION drifted from 1. Bumping it requires "
             "updating this test deliberately and writing a migration "
             "story for existing V4 files.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/session_sha256_checksum/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: V4 envelope writes & verifies SHA-256 over payload\n");
    return 0;
}
