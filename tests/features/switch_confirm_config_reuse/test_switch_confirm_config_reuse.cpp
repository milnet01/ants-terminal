// Why this exists: the user-typed /model confirm path constructed a fresh
// Config on the 2 s tick and again on every poll of the burst that tick
// arms — seventeen open-read-parse cycles per two seconds, on the default
// configuration, in the same file whose cachedConfig() exists to stop
// exactly that. See spec.md.

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#  error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif

namespace {

std::string memberBody(const std::string &src, const char *decl) {
    const size_t sig = src.find(decl);
    if (sig == std::string::npos) return {};
    const size_t brace = src.find('{', sig);
    if (brace == std::string::npos) return {};
    size_t i = brace + 1;
    int depth = 1;
    while (i < src.size() && depth > 0) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') --depth;
        ++i;
    }
    if (depth != 0) return {};
    return src.substr(brace + 1, i - brace - 2);
}

// A fresh Config: `Config()` or a `Config name;` declaration.
bool constructsConfig(const std::string &body) {
    return std::regex_search(body, std::regex(R"(\bConfig\s*\()")) ||
           std::regex_search(body, std::regex(R"(\bConfig\s+[a-z][A-Za-z0-9_]*\s*;)"));
}

}  // namespace

TEST(SwitchConfirmConfigReuse, Main) {
    const std::string src =
        ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    struct Case { const char *decl; const char *inv; const char *why; };
    const Case cases[] = {
        {"void ClaudeStatusBarController::maybeAutoConfirmUserModelSwitch(",
         "INV-1",
         "runs on the 2 s status tick and arms the poll burst below"},
        {"void ClaudeStatusBarController::pollUnarmedSwitchConfirm(",
         "INV-2",
         "runs kSwitchConfirmMaxPolls times per burst, every "
         "kSwitchConfirmPollMs"},
    };

    for (const Case &c : cases) {
        const std::string body = memberBody(src, c.decl);
        if (body.empty()) {
            std::fprintf(stderr, "FAIL: precondition: %s body not found\n",
                         c.decl);
            ++failures;
            continue;
        }
        // INV-1 / INV-2 — no fresh Config on the looping path.
        if (constructsConfig(body)) {
            std::fprintf(stderr,
                "FAIL: %s: this function constructs a fresh Config, and it "
                "%s. Each ctor is an open + readAll + JSON parse; together "
                "the tick and one full burst are seventeen of them every "
                "two seconds, on the default configuration. cachedConfig() "
                "exists in this file for exactly this (ANTS-2116).\n",
                c.inv, c.why);
            ++failures;
        }
        // INV-3 — the read must still happen.
        if (body.find("claudeAutoModelConfirmUserSwitch") == std::string::npos) {
            std::fprintf(stderr,
                "FAIL: INV-3: %s no longer reads "
                "claudeAutoModelConfirmUserSwitch at all — the cost went "
                "away by deleting the behaviour.\n", c.decl);
            ++failures;
        }
    }

    // INV-4 — the helper being routed to is still the caching one.
    const std::string cached = memberBody(src, "const Config &cachedConfig(");
    if (cached.empty()) {
        fail("INV-4: cachedConfig() not found in claudestatuswidgets.cpp.");
    } else if (cached.find("lastModified") == std::string::npos ||
               cached.find("size") == std::string::npos) {
        fail("INV-4: cachedConfig() no longer guards on both mtime and "
             "size, so routing through it is not the reuse this claims.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/switch_confirm_config_reuse/spec.md\n", failures);
    }
    ASSERT_EQ(0, failures);
}
