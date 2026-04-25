// SARIF suppressions[] array — source-grep regression test.
// See spec.md. Fails non-zero if m_suppressionReasons is removed from
// the header, loadSuppressions/saveSuppression stop populating it, the
// SARIF export drops the `"suppressions"` key, or the export reverts
// to skipping suppressed findings.

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
    const std::string hdr = slurp(SRC_AUDIT_H_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1 — m_suppressionReasons declared in the header (QHash or QMap).
    std::regex decl(
        R"((?:QHash|QMap)\s*<\s*QString\s*,\s*QString\s*>\s+m_suppressionReasons)");
    if (!std::regex_search(hdr, decl)) {
        fail("INV-1: m_suppressionReasons (QHash<QString,QString> or "
             "QMap) not declared in src/auditdialog.h. The SARIF export "
             "needs the reasons map to populate "
             "result.suppressions[].justification.");
    }

    // INV-2 — loadSuppressions populates the reasons map.
    const std::string loadBody =
        extractFnBody(cpp, "AuditDialog::loadSuppressions");
    if (loadBody.empty()) {
        fail("precondition: loadSuppressions body not located.");
    } else {
        if (loadBody.find("m_suppressionReasons") == std::string::npos)
            fail("INV-2: loadSuppressions does not touch "
                 "m_suppressionReasons. The reasons map starts empty on "
                 "every load, so the SARIF export sees no justifications.");
        if (loadBody.find("m_suppressionReasons.clear") == std::string::npos)
            fail("INV-2: loadSuppressions does not call "
                 "m_suppressionReasons.clear() — stale reasons from prior "
                 "runs will leak into a fresh load.");
    }

    // INV-3 — saveSuppression updates the reasons map.
    const std::string saveBody =
        extractFnBody(cpp, "AuditDialog::saveSuppression");
    if (saveBody.empty()) {
        fail("precondition: saveSuppression body not located.");
    } else if (saveBody.find("m_suppressionReasons") == std::string::npos) {
        fail("INV-3: saveSuppression does not update "
             "m_suppressionReasons. New suppressions won't carry their "
             "reason into the SARIF export until the next reload.");
    }

    // INV-4 — exportSarif body includes `"suppressions"` and SARIF-spec
    // suppression fields.
    const std::string sarifBody =
        extractFnBody(cpp, "AuditDialog::exportSarif");
    if (sarifBody.empty()) {
        fail("precondition: exportSarif body not located.");
    } else {
        if (sarifBody.find("\"suppressions\"") == std::string::npos)
            fail("INV-4: exportSarif body does not write a "
                 "\"suppressions\" property. The SARIF v2.1.0 §3.34 "
                 "result.suppressions[] field is the entire point of "
                 "this feature.");
        if (sarifBody.find("\"external\"") == std::string::npos)
            fail("INV-4: exportSarif body does not emit `kind: \"external\"` — "
                 "required so SARIF consumers know the suppression came "
                 "from outside the source artifact.");
        const bool hasState =
            sarifBody.find("\"state\"") != std::string::npos ||
            sarifBody.find("\"accepted\"") != std::string::npos;
        if (!hasState)
            fail("INV-4: exportSarif body emits neither a `state` field "
                 "nor a value matching the SARIF state vocabulary "
                 "(`accepted`/`underReview`/`rejected`).");

        // INV-5 — exportSarif body does NOT skip findings unconditionally
        // on suppression — it must include them so the suppressions[]
        // array can attach.
        // Specifically, no `if (... isSuppressed(... )) continue;` line
        // should appear inside the exportSarif body.
        std::regex suppressContinue(
            R"(if\s*\([^)]*[Ss]uppress[^)]*\)\s*continue)");
        if (std::regex_search(sarifBody, suppressContinue)) {
            fail("INV-5: exportSarif body still drops suppressed findings "
                 "via a `continue` on isSuppressed/m_suppressedKeys check. "
                 "The whole point is to surface them with "
                 "result.suppressions[] instead of filtering them out.");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/audit_sarif_suppressions/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: SARIF suppressions[] array surfaced + reasons map wired\n");
    return 0;
}
