// Audit dedup-key width — source-grep regression test.
// See spec.md. Fails non-zero if computeDedup reverts to .left(16),
// the isSuppressed helper disappears, render sites stop using it,
// or saveSuppression stops updating m_suppressionReasons.

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

// Locate the body of a free function (e.g. "static QString computeDedup").
// Brace counter skips char literals ('{', '}'), string literals ("..."),
// and raw string literals (R"delim(...)delim") so braces inside them
// aren't miscounted. Comments are not stripped — auditdialog.cpp has no
// `{` or `}` in line comments inside the bodies we extract.
static std::string extractFnBody(const std::string &src, const char *qualName) {
    std::string pat = std::string("(?:static\\s+)?(?:void|int|bool|QString)\\s+") +
                      qualName + R"(\s*\([^)]*\)\s*(?:const\s*)?\{)";
    std::regex re(pat);
    std::smatch m;
    if (!std::regex_search(src, m, re)) return {};
    size_t start = m.position(0) + m.length(0);
    int depth = 1;
    size_t i = start;
    while (i < src.size() && depth > 0) {
        char c = src[i];
        // Char literal: '{' or '}' or '\'' etc.
        if (c == '\'') {
            ++i;
            if (i < src.size() && src[i] == '\\') i += 2;  // escape: '\n'
            else ++i;                                       // single char
            if (i < src.size() && src[i] == '\'') ++i;      // closing quote
            continue;
        }
        // Raw string literal: R"delim( ... )delim"
        if (c == 'R' && i + 1 < src.size() && src[i + 1] == '"') {
            i += 2;
            // Capture delimiter up to '('
            std::string delim;
            while (i < src.size() && src[i] != '(') { delim += src[i]; ++i; }
            ++i;  // skip '('
            std::string close = ")" + delim + "\"";
            size_t end = src.find(close, i);
            i = (end == std::string::npos) ? src.size() : end + close.size();
            continue;
        }
        // Plain string literal: "..."
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
    // ANTS-1119 v1 split: computeDedup moved to auditengine.cpp; the
    // dialog still owns isSuppressed. Slurp both and search the union
    // for the engine-side INVs, the dialog-side for the helper.
    const std::string dialogCpp = slurp(SRC_AUDIT_CPP_PATH);
    const std::string hdr       = slurp(SRC_AUDIT_H_PATH);
#ifdef SRC_AUDIT_ENGINE_CPP_PATH
    const std::string engineCpp = slurp(SRC_AUDIT_ENGINE_CPP_PATH);
#else
    const std::string engineCpp;
#endif
    const std::string cpp = dialogCpp + engineCpp;
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1 — computeDedup body has .left(24) (or wider).
    const std::string ddBody = extractFnBody(cpp, "computeDedup");
    if (ddBody.empty()) {
        fail("precondition: computeDedup body not located in auditdialog.cpp "
             "or auditengine.cpp (ANTS-1119 v1 moved it to the engine).");
    } else {
        std::regex wide(R"(\.left\(\s*(\d+)\s*\))");
        std::smatch m;
        if (!std::regex_search(ddBody, m, wide)) {
            fail("INV-1: no `.left(N)` truncation found in computeDedup. "
                 "The function must keep a fixed-width prefix of the "
                 "SHA-256 digest.");
        } else {
            const int width = std::atoi(m[1].str().c_str());
            if (width < 24) {
                fail("INV-1: computeDedup truncates to fewer than 24 hex "
                     "chars — must be at least 24 (96 bits) for the widened "
                     "collision threshold.");
            }
        }
    }

    // INV-2 — backward-compat helper exists. Try the canonical name
    // first (isSuppressed); fall back to any *Suppressed* helper. Body
    // must reference both m_suppressedKeys AND a `.left(16)` legacy
    // prefix lookup.
    const std::string helperBody =
        extractFnBody(cpp, "AuditDialog::isSuppressed");
    if (helperBody.empty()) {
        fail("INV-2: no `bool AuditDialog::isSuppressed(...) const` "
             "helper found. Render sites need a single point that "
             "encapsulates the legacy-key backward-compat lookup.");
    } else {
        if (helperBody.find("m_suppressedKeys") == std::string::npos)
            fail("INV-2: isSuppressed body does not reference "
                 "m_suppressedKeys — the helper isn't the suppression "
                 "lookup it claims to be.");
        if (helperBody.find(".left(16)") == std::string::npos)
            fail("INV-2: isSuppressed body lacks a `.left(16)` legacy-prefix "
                 "fallback path. Without it, existing user "
                 "~/.audit_suppress 16-char keys won't match new 24-char "
                 "findings.");
    }

    // INV-3 — render sites use the helper, not raw m_suppressedKeys.contains
    // on a Finding's dedupKey. Tighten by counting only `f.dedupKey` field
    // accesses, which excludes bare `dedupKey` parameter accesses inside
    // isSuppressed itself and saveSuppression's de-dup-on-insert.
    std::regex rawFieldContains(
        R"(m_suppressedKeys\.contains\(\s*f\.dedupKey)");
    int rawCount = 0;
    auto rb = std::sregex_iterator(cpp.begin(), cpp.end(), rawFieldContains);
    auto re_ = std::sregex_iterator();
    for (auto it = rb; it != re_; ++it) ++rawCount;
    if (rawCount > 0) {
        std::fprintf(stderr, "INV-3 raw m_suppressedKeys.contains(f.dedupKey) "
                     "sites: %d (expected 0)\n", rawCount);
        fail("INV-3: render sites still call m_suppressedKeys.contains "
             "on f.dedupKey directly instead of going through the "
             "backward-compat helper. Legacy 16-char keys won't match "
             "new 24-char findings.");
    }

    // Ensure isSuppressed (or the helper) IS called from at least four sites.
    std::regex helperCall(R"(isSuppressed\s*\(\s*[^)]*dedupKey)");
    int helperCount = 0;
    auto hb = std::sregex_iterator(cpp.begin(), cpp.end(), helperCall);
    auto he = std::sregex_iterator();
    for (auto it = hb; it != he; ++it) ++helperCount;
    if (helperCount < 4) {
        std::fprintf(stderr, "INV-3 helper-call count: %d (expected ≥ 4)\n",
                     helperCount);
        fail("INV-3: fewer than four render sites use the isSuppressed "
             "helper — render pipeline likely still has direct contains "
             "calls or the helper isn't named isSuppressed.");
    }

    // INV-4 — saveSuppression updates m_suppressionReasons.
    const std::string saveBody = extractFnBody(cpp, "AuditDialog::saveSuppression");
    if (saveBody.empty()) {
        fail("precondition: saveSuppression body not located.");
    } else if (saveBody.find("m_suppressionReasons") == std::string::npos) {
        fail("INV-4: saveSuppression body does not touch "
             "m_suppressionReasons. Without the reason mirror, the SARIF "
             "suppressions[].justification field can't surface the user's "
             "reason text.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/audit_dedup_96bit/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: dedup widened to 96 bits with backward-compat helper\n");
    return 0;
}
