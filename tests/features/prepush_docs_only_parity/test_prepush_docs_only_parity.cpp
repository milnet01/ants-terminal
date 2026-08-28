// ANTS-4726 — the pre-push hook's docs-only path set is a hand-maintained twin
// of ci.yml's push `paths-ignore`. Nothing checked they agree, and a twin that
// drifts makes the hook skip a gate CI will run. See spec.md.
//
// Static, and it has to be: locally the two files are only ever read by the
// hook at push time, so no test run can catch a disagreement between them.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_CI_WORKFLOW_PATH
#error "SRC_CI_WORKFLOW_PATH compile definition required"
#endif
#ifndef SRC_PREPUSH_HOOK_PATH
#error "SRC_PREPUSH_HOOK_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

std::string trim(std::string s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string unquote(std::string s) {
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"')
        && s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

// ci.yml's push `paths-ignore` block: the list items between that key and the
// next key at the same or shallower indent. Scoped to the FIRST occurrence,
// which is the push trigger — the pull_request trigger deliberately has none.
std::set<std::string> ciPathsIgnore(const std::string &yaml) {
    std::set<std::string> out;
    std::istringstream in(yaml);
    std::string line;
    bool inBlock = false;
    size_t keyIndent = 0;
    while (std::getline(in, line)) {
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        const std::string body = trim(line);
        if (body[0] == '#') continue;
        if (!inBlock) {
            if (body.rfind("paths-ignore:", 0) == 0) {
                inBlock   = true;
                keyIndent = first;
            }
            continue;
        }
        if (body[0] != '-' || first <= keyIndent) break;   // block ended
        out.insert(unquote(trim(body.substr(1))));
    }
    return out;
}

// The hook's docs_only_re, as the set of paths it names. `docs/**` and the
// regex's `docs/` are the same statement about a directory, so both normalise
// to the directory prefix and the comparison is about paths, not syntax.
std::set<std::string> hookDocsOnly(const std::string &sh, bool *anchored) {
    std::set<std::string> out;
    *anchored = false;
    const size_t k = sh.find("docs_only_re=");
    if (k == std::string::npos) return out;
    const size_t open  = sh.find('\'', k);
    const size_t close = open == std::string::npos ? open : sh.find('\'', open + 1);
    if (close == std::string::npos) return out;
    std::string re = sh.substr(open + 1, close - open - 1);

    if (re.rfind("^(", 0) == 0 && re.back() == ')') {
        *anchored = true;
        re = re.substr(2, re.size() - 3);
    }
    std::string cur;
    for (size_t i = 0; i <= re.size(); ++i) {
        if (i == re.size() || re[i] == '|') {
            if (!cur.empty()) out.insert(cur);
            cur.clear();
            continue;
        }
        if (re[i] == '\\') continue;          // `\.` is a literal dot here
        cur += re[i];
    }
    return out;
}

std::string join(const std::set<std::string> &s) {
    std::string out;
    for (const std::string &v : s) { if (!out.empty()) out += ", "; out += v; }
    return out.empty() ? std::string("<none>") : out;
}

// A ci.yml glob and a regex alternative naming the same directory must compare
// equal: `docs/**` and `docs/` both mean "everything under docs".
std::string normalise(std::string p) {
    if (p.size() > 2 && p.compare(p.size() - 2, 2, "**") == 0)
        p.erase(p.size() - 2);
    return p;
}

std::set<std::string> normaliseAll(const std::set<std::string> &in) {
    std::set<std::string> out;
    for (const std::string &v : in) out.insert(normalise(v));
    return out;
}

}  // namespace

// INV-1 — the two lists agree, in both directions.
TEST(PrepushDocsOnlyParity, Inv1HookAndWorkflowNameTheSamePaths) {
    expect_reset();
    const std::string yaml = ants_test::slurpFile(SRC_CI_WORKFLOW_PATH);
    const std::string sh   = ants_test::slurpFile(SRC_PREPUSH_HOOK_PATH);
    ASSERT_FALSE(yaml.empty());
    ASSERT_FALSE(sh.empty());

    bool anchored = false;
    const std::set<std::string> ci   = normaliseAll(ciPathsIgnore(yaml));
    const std::set<std::string> hook = normaliseAll(hookDocsOnly(sh, &anchored));

    expect(!ci.empty(), "4726/ci-paths-ignore-parsed",
           QString::fromStdString(join(ci)));
    expect(!hook.empty(), "4726/hook-regex-parsed",
           QString::fromStdString(join(hook)));

    std::set<std::string> ciOnly, hookOnly;
    std::set_difference(ci.begin(), ci.end(), hook.begin(), hook.end(),
                        std::inserter(ciOnly, ciOnly.begin()));
    std::set_difference(hook.begin(), hook.end(), ci.begin(), ci.end(),
                        std::inserter(hookOnly, hookOnly.begin()));

    // Only the hook: it skips a gate CI will run — the ANTS-4726 failure.
    expect(hookOnly.empty(), "4726/hook-skips-what-ci-runs",
           QString::fromStdString(join(hookOnly)));
    // Only ci.yml: merely wasteful, but it is still drift and it is still a
    // twin that has stopped agreeing.
    expect(ciOnly.empty(), "4726/ci-ignores-what-hook-gates",
           QString::fromStdString(join(ciOnly)));
    ASSERT_EQ(0, expect_finish());
}

// INV-2 — anchored at the start of the path. Unanchored, any path containing
// `docs/` reads as documentation.
TEST(PrepushDocsOnlyParity, Inv2RegexIsAnchoredAtPathStart) {
    expect_reset();
    const std::string sh = ants_test::slurpFile(SRC_PREPUSH_HOOK_PATH);
    ASSERT_FALSE(sh.empty());
    bool anchored = false;
    hookDocsOnly(sh, &anchored);
    expect(anchored, "4726/anchored", QString());
    ASSERT_EQ(0, expect_finish());
}
