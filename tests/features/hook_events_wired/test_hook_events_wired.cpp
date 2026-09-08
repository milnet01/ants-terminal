// Why this exists: processHookEvent handled PermissionRequest and
// PostToolUseFailure, and the installer wired neither, so both branches —
// each with a live consumer, one with a hardening pass of its own — could
// never receive an event. Nothing checked the two lists against each
// other. See spec.md.

#include <cstdio>
#include <regex>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#  error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_SETTINGSDIALOG_CPP
#  error "SRC_SETTINGSDIALOG_CPP compile definition required"
#endif

namespace {

std::string bodyAfter(const std::string &src, const char *marker) {
    const size_t sig = src.find(marker);
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

// Event names the installer writes: the QStringLiteral entries inside
// claudeHookEvents().
std::set<std::string> installedEvents(const std::string &settingsSrc) {
    const std::string body = bodyAfter(settingsSrc, "claudeHookEvents()");
    std::set<std::string> out;
    const std::regex lit(R"RX(QStringLiteral\s*\(\s*"([A-Za-z]+)"\s*\))RX");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), lit);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

// Event names the handler compares against, in either spelling the file
// uses: `hookName == "X"` and `hookName == QLatin1String("X")`.
std::set<std::string> handledEvents(const std::string &ciSrc) {
    const std::string body =
        bodyAfter(ciSrc, "void ClaudeIntegration::processHookEvent(");
    std::set<std::string> out;
    const std::regex cmp(
        R"RX(hookName\s*==\s*(?:QLatin1String\s*\(\s*)?"([A-Za-z]+)")RX");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), cmp);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

std::string join(const std::set<std::string> &s) {
    std::string out;
    for (const std::string &e : s) { if (!out.empty()) out += ", "; out += e; }
    return out.empty() ? std::string("(none)") : out;
}

}  // namespace

TEST(HookEventsWired, Main) {
    const std::string settings = ants_test::slurpFile(SRC_SETTINGSDIALOG_CPP);
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    const std::set<std::string> installed = installedEvents(settings);
    const std::set<std::string> handled = handledEvents(ci);

    int failures = 0;

    // INV-3 first: an empty set would satisfy the comparisons vacuously.
    if (installed.empty()) {
        std::fprintf(stderr,
            "FAIL: INV-3: parsed no event names out of claudeHookEvents() "
            "in src/settingsdialog.cpp — the comparisons below would pass "
            "for the wrong reason.\n");
        ++failures;
    }
    if (handled.empty()) {
        std::fprintf(stderr,
            "FAIL: INV-3: parsed no `hookName == \"...\"` comparisons out "
            "of processHookEvent in src/claudeintegration.cpp.\n");
        ++failures;
    }

    if (failures == 0) {
        // INV-1 — a handled event nobody installs is a feature that never
        // runs, and nothing reports it: no log fires for an event that
        // never arrives, and the hooks status only verifies what the
        // installer writes.
        for (const std::string &e : handled) {
            if (!installed.count(e)) {
                std::fprintf(stderr,
                    "FAIL: INV-1: processHookEvent handles \"%s\" but "
                    "claudeHookEvents() does not install it, so that branch "
                    "can never receive an event.\n", e.c_str());
                ++failures;
            }
        }
        // INV-2 — an installed event nobody handles spawns the forwarder
        // script, a Python interpreter and a socket connection every time
        // it fires, to reach a branch that does nothing.
        for (const std::string &e : installed) {
            if (!handled.count(e)) {
                std::fprintf(stderr,
                    "FAIL: INV-2: claudeHookEvents() installs \"%s\" but "
                    "processHookEvent has no branch for it — every "
                    "occurrence pays a process spawn for nothing.\n",
                    e.c_str());
                ++failures;
            }
        }
    }

    // INV-4 — the two the sweep found missing, named so a revert is caught
    // by name and not only by the set comparison above.
    for (const char *e : {"PermissionRequest", "PostToolUseFailure"}) {
        if (!installed.count(e)) {
            std::fprintf(stderr,
                "FAIL: INV-4: \"%s\" is not installed. It is a documented "
                "Claude Code hook event with a live consumer in the status "
                "widgets.\n", e);
            ++failures;
        }
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\ninstalled: %s\nhandled:   %s\n"
            "%d invariant(s) failed — see "
            "tests/features/hook_events_wired/spec.md\n",
            join(installed).c_str(), join(handled).c_str(), failures);
    }
    ASSERT_EQ(0, failures);
}
