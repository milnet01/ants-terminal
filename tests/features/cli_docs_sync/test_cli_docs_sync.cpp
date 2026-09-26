// The CLI's documentation lists every option — see spec.md. RC-51.

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <cctype>
#include <regex>
#include <set>
#include <string>

namespace {

std::string root() { return ANTS_CLI_DOCS_ROOT; }

// Long option names main.cpp defines: the first name of each
// QCommandLineOption("name" / {"name", ...}), plus the pre-parse flag.
std::set<std::string> optionsFromMain() {
    const std::string src = ants_test::slurpFile(root() + "/src/main.cpp");
    std::set<std::string> out;
    const std::regex opt(R"re(QCommandLineOption\s+\w+\(\s*\{?\s*"([a-z][a-z0-9-]+)"(?:\s*,\s*"([a-z][a-z0-9-]+)")?)re");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), opt);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
        if ((*it)[2].matched) out.insert((*it)[2].str());
    }
    if (src.find("\"--export-roadmaps\"") != std::string::npos)
        out.insert("export-roadmaps");
    return out;
}

std::string unescapeMan(std::string s) {
    for (std::size_t i = 0; (i = s.find("\\-", i)) != std::string::npos;)
        s.replace(i, 2, "-");
    return s;
}

// `token` present as a whole option, so "--remote" is not satisfied by
// "--remote-socket" alone.
bool hasWhole(const std::string &text, const std::string &token) {
    for (std::size_t i = 0; (i = text.find(token, i)) != std::string::npos; ++i) {
        const std::size_t end = i + token.size();
        const char next = end < text.size() ? text[end] : ' ';
        if (!(std::isalnum(static_cast<unsigned char>(next)) || next == '-'))
            return true;
    }
    return false;
}

}  // namespace

TEST(CliDocsSync, EveryLongOptionIsDocumentedEverywhere) {
    const std::set<std::string> opts = optionsFromMain();
    ASSERT_GE(opts.size(), 5u) << "the option scrape found almost nothing";
    ASSERT_TRUE(opts.count("remote-json")) << "the scrape missed a known option";

    const std::string man =
        unescapeMan(ants_test::slurpFile(root() + "/packaging/linux/ants-terminal.1"));
    const std::string bash =
        ants_test::slurpFile(root() + "/packaging/completions/ants-terminal.bash");
    const std::string zsh =
        ants_test::slurpFile(root() + "/packaging/completions/_ants-terminal");
    const std::string fish =
        ants_test::slurpFile(root() + "/packaging/completions/ants-terminal.fish");
    ASSERT_FALSE(man.empty());
    ASSERT_FALSE(bash.empty());
    ASSERT_FALSE(zsh.empty());
    ASSERT_FALSE(fish.empty());

    for (const std::string &o : opts) {
        const std::string dashed = "--" + o;
        EXPECT_TRUE(hasWhole(man, dashed)) << dashed << " missing from the man page";
        EXPECT_TRUE(hasWhole(bash, dashed)) << dashed << " missing from bash completion";
        EXPECT_TRUE(hasWhole(zsh, dashed)) << dashed << " missing from zsh completion";
        EXPECT_TRUE(hasWhole(fish, "-l " + o)) << o << " missing from fish completion";
    }
}
