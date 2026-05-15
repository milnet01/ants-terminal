// Tiny helpers for source-grep tests that need to assert invariants
// over a specific function body without relying on a fixed-size
// `substr(pos, N)` window (which is brittle to body growth — caught
// during ANTS-1348 when cmdGetText's body grew past the 2500-char
// window that the existing `remote_control_get_text` test slurped).
//
// Header-only; no link-time dependency. Designed to be included from
// any test_*.cpp that already includes `<fstream>` / `<string>`.

#pragma once

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

namespace ants_test {

// Read the entire file at `path`. Returns empty string on open failure
// (callers can detect via `result.empty()`); errors don't print here so
// the test harness controls the failure message.
inline std::string slurpFile(const char *path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Return the body of the function whose signature starts with the
// `signatureAnchor` substring (e.g. "RemoteControl::cmdGetText"). The
// scan finds the first '{' at or after the anchor, then walks forward
// counting braces with awareness of:
//   - string literals "..." (handles backslash-escaped quotes)
//   - character literals '...' (single-char; handles escapes)
//   - line comments  // ...EOL
//   - block comments /* ... */
//
// Returns the substring between (and including) the opening `{` and
// the matching closing `}`. On any failure (signature not found,
// unbalanced braces) returns the empty string.
//
// This is deliberately conservative — it doesn't try to parse
// raw-string literals, trigraphs, or weird preprocessor games. The
// goal is robustness against ordinary function-body growth, not a
// full C++ tokeniser.
inline std::string slurpFunctionBody(const std::string &src,
                                     const std::string &signatureAnchor) {
    const std::size_t sigPos = src.find(signatureAnchor);
    if (sigPos == std::string::npos) return {};

    const std::size_t openBrace = src.find('{', sigPos);
    if (openBrace == std::string::npos) return {};

    int depth = 0;
    const std::size_t n = src.size();
    for (std::size_t i = openBrace; i < n; ++i) {
        const char c = src[i];

        // String literal: skip to closing quote, honouring backslash
        // escapes (so `"\""` is one literal, not two).
        if (c == '"') {
            ++i;
            while (i < n && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < n) ++i;  // skip escaped char
                ++i;
            }
            continue;
        }
        // Character literal: same idea, single-quote.
        if (c == '\'') {
            ++i;
            while (i < n && src[i] != '\'') {
                if (src[i] == '\\' && i + 1 < n) ++i;
                ++i;
            }
            continue;
        }
        // Line comment.
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            i += 2;
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        // Block comment.
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            ++i;  // skip the '/' of the close
            continue;
        }

        if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) {
                return src.substr(openBrace, i - openBrace + 1);
            }
        }
    }
    return {};  // unbalanced
}

// Convenience overload: combines slurpFile + slurpFunctionBody.
inline std::string slurpFunctionBody(const char *path,
                                     const std::string &signatureAnchor) {
    return slurpFunctionBody(slurpFile(path), signatureAnchor);
}

// Count non-overlapping occurrences of `needle` in `hay`.
inline std::size_t countOccurrences(const std::string &hay,
                                    const std::string &needle) {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

}  // namespace ants_test
