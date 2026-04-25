// Audit regex-DoS watchdog — source-grep regression test.
// See spec.md. Fails non-zero if the shape-rejection helper disappears,
// the LIMIT_MATCH PCRE2 prefix is dropped, the user-pattern entry points
// (dropIfMatches, allowlist line_regex) bypass the helpers, or the
// rejection path stops emitting a qWarning.

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

// Brace counter skips '{' / '}' char literals + string + raw-string
// literals so source-grep extraction works on functions that contain
// `if (line.startsWith('{'))`-style code.
static std::string extractFnBody(const std::string &src, const char *qualName) {
    std::string pat = std::string("(?:void|int|bool|QString)\\s+") +
                      qualName + R"(\s*\([^)]*\)\s*(?:const\s*)?\{)";
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
    const std::string cpp = slurp(SRC_AUDIT_CPP_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1 — a shape-rejection helper exists. Match flexible naming:
    // any function whose name contains "Catastrophic", "Dangerous",
    // or "RegexShape" (case-insensitive) and returns bool.
    std::regex helperDef(
        R"(bool\s+(?:AuditDialog::)?\w*(?:[Cc]atastrophic|[Dd]angerous|[Rr]egex[Ss]hape)\w*\s*\()");
    std::smatch helperMatch;
    if (!std::regex_search(cpp, helperMatch, helperDef)) {
        fail("INV-1: no shape-rejection helper found "
             "(expected name like isCatastrophicRegex / isDangerousRegex). "
             "Without the helper, user patterns aren't shape-checked.");
    }

    // It must be invoked at the dropIfMatches site AND the allowlist site.
    // Use proximity: any helper-call within ~600 chars of "dropIfMatches"
    // and another within the loadAllowlist body.
    {
        // Find dropIfMatches mention; require helper invocation nearby.
        size_t pos = cpp.find("dropIfMatches");
        bool dropOk = false;
        while (pos != std::string::npos) {
            size_t winStart = pos > 600 ? pos - 600 : 0;
            size_t winEnd   = std::min(pos + 600, cpp.size());
            std::string win = cpp.substr(winStart, winEnd - winStart);
            std::regex helperCall(
                R"(\w*(?:[Cc]atastrophic|[Dd]angerous|[Rr]egex[Ss]hape)\w*\s*\()");
            if (std::regex_search(win, helperCall)) { dropOk = true; break; }
            pos = cpp.find("dropIfMatches", pos + 1);
        }
        if (!dropOk)
            fail("INV-1: shape-helper not invoked anywhere within 600 "
                 "chars of a `dropIfMatches` reference. The dropIfMatches "
                 "filter is the primary user-pattern entry point.");
    }
    {
        const std::string body = extractFnBody(cpp, "AuditDialog::loadAllowlist");
        if (body.empty()) {
            fail("precondition: loadAllowlist body not located.");
        } else {
            std::regex helperCall(
                R"(\w*(?:[Cc]atastrophic|[Dd]angerous|[Rr]egex[Ss]hape)\w*\s*\()");
            if (!std::regex_search(body, helperCall)) {
                fail("INV-1: shape-helper not invoked inside loadAllowlist. "
                     ".audit_allowlist.json's line_regex is the second "
                     "user-pattern entry point and must also be guarded.");
            }
        }
    }

    // INV-2 — the helper's body recognizes nested-quantifier shapes.
    // Find the helper definition's body and ensure it contains a regex
    // literal with both `+` and `*` (or `[+*]`) — the shape sentinel.
    if (helperMatch.size() > 0) {
        // Locate the helper's body via brace counting.
        const size_t defStart = static_cast<size_t>(helperMatch.position(0));
        const size_t openBrace = cpp.find('{', defStart);
        if (openBrace != std::string::npos) {
            int depth = 1;
            size_t i = openBrace + 1;
            while (i < cpp.size() && depth > 0) {
                if (cpp[i] == '{') ++depth;
                else if (cpp[i] == '}') --depth;
                ++i;
            }
            const std::string body =
                cpp.substr(openBrace + 1, i - openBrace - 2);
            // Look for a regex literal containing both '+' and '*'
            // (the `[+*]` token in our impl satisfies this trivially).
            // We check for `[+*]` directly OR the presence of both
            // `+` and `*` characters inside an R"(...)" raw string literal.
            std::regex shapeLiteral(R"(\[\s*\+\s*\*\s*\])");
            if (!std::regex_search(body, shapeLiteral)) {
                // Fallback: a raw regex literal that contains both `\+` and
                // `\*` as quantifier escapes (the alternative encoding).
                std::regex altLiteral(R"(\\\+[^"]*\\\*|\\\*[^"]*\\\+)");
                if (!std::regex_search(body, altLiteral)) {
                    fail("INV-2: shape-helper body lacks a regex literal "
                         "that detects nested-quantifier shapes "
                         "(expected `[+*]` inside the matcher pattern).");
                }
            }
        }
    }

    // INV-3 — PCRE2 LIMIT_MATCH=N applied. Look for the literal anywhere
    // in auditdialog.cpp, with N in [1000, 1000000].
    std::regex limitMatch(R"(LIMIT_MATCH=(\d+))");
    std::smatch lm;
    if (!std::regex_search(cpp, lm, limitMatch)) {
        fail("INV-3: no `(*LIMIT_MATCH=N)` PCRE2 cap found anywhere in "
             "auditdialog.cpp. The step-limit is the second-line defense "
             "for adversarial patterns that slip past shape rejection.");
    } else {
        const int n = std::atoi(lm[1].str().c_str());
        if (n < 1000 || n > 1000000) {
            std::fprintf(stderr, "LIMIT_MATCH=%d out of [1000, 1000000]\n", n);
            fail("INV-3: LIMIT_MATCH value outside the sane range. Too "
                 "low and legitimate patterns fail-soft; too high and "
                 "the cap doesn't bound match-time meaningfully.");
        }
    }

    // INV-4 — qWarning on rejection somewhere in the user-pattern entry path.
    // Search the loadAllowlist body specifically.
    {
        const std::string body = extractFnBody(cpp, "AuditDialog::loadAllowlist");
        if (!body.empty()) {
            std::regex warn(R"((?:qWarning|qCritical)\s*\()");
            if (!std::regex_search(body, warn)) {
                fail("INV-4: loadAllowlist body emits no qWarning. The "
                     "rejection path must surface bad patterns to the "
                     "user via the standard warning channel.");
            }
        }
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/audit_regex_dos_watchdog/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: regex-DoS watchdog (shape + step-limit) wired in\n");
    return 0;
}
