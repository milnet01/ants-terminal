// Why this exists: the shared caller_cwd property description promised a
// focused-tab fallback that ANTS-1415 Phase 3b removed and replaced with a
// tab_or_cwd_required refusal, and enumerated five of the seven verbs the
// classifier marks TabSpecific. See spec.md.

#include <cstdio>
#include <regex>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#  error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

namespace {

// The lambda that builds the shared property, plus the comment block
// immediately above it: from the ANTS-1520 comment anchor to the end of
// the lambda body.
std::string sharedPropRegion(const std::string &src) {
    const size_t anchor = src.find("makeCallerCwdReadProp");
    if (anchor == std::string::npos) return {};
    // Walk back to the start of the comment block that introduces it.
    const size_t commentStart = src.rfind("// ANTS-1520", anchor);
    const size_t begin = (commentStart == std::string::npos) ? anchor : commentStart;
    const size_t end = src.find("};", anchor);
    if (end == std::string::npos) return {};
    return src.substr(begin, end - begin);
}

// Verbs the classifier returns TabSpecific for.
std::set<std::string> tabSpecificVerbs(const std::string &src) {
    std::set<std::string> out;
    const std::regex re(
        R"RX(toolName\s*==\s*QStringLiteral\s*\(\s*"([a-z_]+)"\s*\)\s*\)\s*return\s+C::TabSpecific)RX");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

// Join adjacent C++ string literals: `"a " \n "b"` becomes `"a b"`. Without
// this every phrase check below is defeated by where the author happened to
// wrap the line — INV-1's phrase is split across two literals in the very
// code it exists to forbid, so the check passed against the defect.
std::string joinAdjacentLiterals(const std::string &in) {
    static const std::regex seam(R"RX("\s*")RX");
    return std::regex_replace(in, seam, "");
}

}  // namespace

TEST(CallerCwdSchemaTruth, Main) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string region = joinAdjacentLiterals(sharedPropRegion(src));
    ASSERT_FALSE(region.empty())
        << "precondition: the makeCallerCwdReadProp helper and its comment "
           "block were not found in src/claudeintegration.cpp";

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1 — no promise of a fallback that the dispatcher refuses.
    if (region.find("fall back to the focused") != std::string::npos ||
        region.find("falls back to the focused") != std::string::npos) {
        fail("INV-1: the shared caller_cwd description still promises a "
             "focused-tab fallback. ANTS-1415 Phase 3b removed it and "
             "refuses tab_or_cwd_required instead, so a session that "
             "believes this omits caller_cwd and meets an error it was "
             "told could not happen.");
    }

    // INV-2 — it names what a caller actually meets.
    if (region.find("tab_or_cwd_required") == std::string::npos) {
        fail("INV-2: the shared caller_cwd description does not name the "
             "tab_or_cwd_required refusal, which is what a per-tab verb "
             "returns when neither routing key is present.");
    }

    // INV-3 — `get_*` does not describe the classified set.
    if (region.find("`get_*`") != std::string::npos) {
        fail("INV-3: the description still calls the per-tab set `get_*`. "
             "recent_errors and last_selection are TabSpecific and neither "
             "carries that prefix.");
    }

    // INV-5 first — an empty extraction would make INV-4 vacuous.
    const std::set<std::string> verbs = tabSpecificVerbs(src);
    if (verbs.empty()) {
        fail("INV-5: parsed no TabSpecific verbs out of the classifier, so "
             "the enumeration check below would pass for the wrong reason.");
    } else {
        // INV-4 — the comment enumerates every one of them.
        for (const std::string &v : verbs) {
            if (region.find(v) == std::string::npos) {
                std::fprintf(stderr,
                    "FAIL: INV-4: \"%s\" is classified TabSpecific but is "
                    "not named in the caller_cwd helper's comment, which is "
                    "where the enumeration lives. The comment listed five "
                    "of seven before this was pinned.\n", v.c_str());
                ++failures;
            }
        }
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/caller_cwd_schema_truth/spec.md\n", failures);
    }
    ASSERT_EQ(0, failures);
}
