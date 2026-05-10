// Feature-conformance test for spec.md (ANTS-1198) — per-style
// font setters must disable kerning. Source-grep test, no Qt
// runtime needed.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <gtest/gtest.h>

#ifndef SRC_TERMINALWIDGET_CPP_PATH
#  error "SRC_TERMINALWIDGET_CPP_PATH compile definition required"
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

// Slice from "void TerminalWidget::<setter>(" up to the first
// top-level closing brace.
std::string extractSetterBody(const std::string &src, const std::string &setter) {
    const std::string sig = "void TerminalWidget::" + setter + "(";
    const size_t at = src.find(sig);
    if (at == std::string::npos) return {};
    const size_t openBrace = src.find('{', at);
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

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

TEST(StyledFontKerningOff, Main) {
    const std::string src = slurp(SRC_TERMINALWIDGET_CPP_PATH);

    {
        const std::string body = extractSetterBody(src, "setBoldFontFamily");
        expect(!body.empty(), "extract/setBoldFontFamily-body-found");
        expect(contains(body, "m_fontBold.setKerning(false)"),
               "I1/setBoldFontFamily-disables-kerning");
    }
    {
        const std::string body = extractSetterBody(src, "setItalicFontFamily");
        expect(!body.empty(), "extract/setItalicFontFamily-body-found");
        expect(contains(body, "m_fontItalic.setKerning(false)"),
               "I2/setItalicFontFamily-disables-kerning");
    }
    {
        const std::string body = extractSetterBody(src, "setBoldItalicFontFamily");
        expect(!body.empty(), "extract/setBoldItalicFontFamily-body-found");
        expect(contains(body, "m_fontBoldItalic.setKerning(false)"),
               "I3/setBoldItalicFontFamily-disables-kerning");
    }

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        FAIL();
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return;
}

