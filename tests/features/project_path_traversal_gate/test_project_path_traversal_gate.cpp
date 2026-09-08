// Why this exists: discoverProjects resolves a project path from three
// sources and only the middle one was traversal-checked — while the
// UNCHECKED session-metadata value is tried FIRST, so the checked one
// never ran when both were present. See spec.md.
//
// decodeProjectPath is a public static, so INV-1..3 are real behaviour
// tests. The two discoverProjects gates need a populated ~/.claude tree
// and an instance, so those are source-greps.

#include "claudeintegration.h"

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#  error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
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

}  // namespace

TEST(ProjectPathTraversalGate, Inv1RejectsTraversal) {
    // `-..-..-etc` splits to {"..", "..", "etc"} and joins to a path that
    // climbs out of the tree. The caller uses the result as a project root.
    EXPECT_TRUE(ClaudeIntegration::decodeProjectPath("-..-..-etc").isEmpty())
        << "decodeProjectPath returned a path with a .. component; the "
           "caller publishes it as a project root";
    EXPECT_TRUE(ClaudeIntegration::decodeProjectPath("-home-user-..-..-etc")
                    .isEmpty());
}

TEST(ProjectPathTraversalGate, Inv2DecodesOrdinaryName) {
    // The gate must not cost the normal case. `/mnt` exists on this host
    // and the remaining components need not, since decode falls back to
    // the separator form when neither candidate is on disk.
    const QString got = ClaudeIntegration::decodeProjectPath("-mnt-nonexistent-proj");
    EXPECT_FALSE(got.isEmpty()) << "an ordinary encoded name must still decode";
    EXPECT_TRUE(got.startsWith(QLatin1String("/mnt")))
        << "decoded as: " << got.toStdString();
    EXPECT_FALSE(got.contains(QLatin1String("..")));
}

TEST(ProjectPathTraversalGate, Inv3RejectsNonAbsoluteEncoding) {
    // Claude Code encodes absolute paths, so the name always starts with
    // '-'. Anything else is foreign and must not be handed back as a
    // relative path the caller would use as a project root.
    EXPECT_TRUE(ClaudeIntegration::decodeProjectPath("relative-name").isEmpty());
    EXPECT_TRUE(ClaudeIntegration::decodeProjectPath("").isEmpty());
}

TEST(ProjectPathTraversalGate, Inv4And5DiscoverProjectsGates) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string body =
        memberBody(src, "QList<ClaudeProject> ClaudeIntegration::discoverProjects()");
    ASSERT_FALSE(body.empty())
        << "precondition: discoverProjects body not found";

    int failures = 0;

    // INV-4 — the session-metadata cwd is checked where it is stored, so
    // the map holds only safe values and an unsafe one falls through to
    // the next source instead of shadowing it.
    const std::regex storeGate(
        R"(isSafeAbsolutePath\s*\(\s*cwd\s*\))");
    if (!std::regex_search(body, storeGate)) {
        std::fprintf(stderr,
            "FAIL: INV-4: the session-metadata cwd is stored without an "
            "isSafeAbsolutePath check. It is the FIRST source tried, so "
            "the gated transcript cwd never runs when both are present — "
            "the checked source is the one that loses.\n");
        ++failures;
    }

    // INV-5 — one gate on the resolved value covers all three sources.
    const std::regex publishGate(
        R"(isSafeAbsolutePath\s*\(\s*realPath\s*\))");
    if (!std::regex_search(body, publishGate)) {
        std::fprintf(stderr,
            "FAIL: INV-5: the resolved project path is published without a "
            "final isSafeAbsolutePath check, so a source added later "
            "inherits no gate.\n");
        ++failures;
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/project_path_traversal_gate/spec.md\n",
            failures);
    }
    ASSERT_EQ(0, failures);
}
