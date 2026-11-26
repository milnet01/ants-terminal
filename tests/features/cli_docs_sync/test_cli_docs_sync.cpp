// The CLI's documentation lists every option — see spec.md. RC-51.

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <set>
#include <string>

namespace {

std::string root() { return ANTS_CLI_DOCS_ROOT; }

// Long option names main.cpp defines: the first name of each
// QCommandLineOption, by `name(` or `name{` and then "name" or {"name", ...}, plus each pre-parse flag that
// main.cpp tests with qstrcmp(argv[i], "--name") before QApplication exists.
std::set<std::string> optionsFromMain() {
    const std::string src = ants_test::slurpFile(root() + "/src/main.cpp");
    std::set<std::string> out;
    const std::regex opt(R"re(QCommandLineOption\s+\w+\s*[({]\s*\{?\s*"([a-z][a-z0-9-]+)"(?:\s*,\s*"([a-z][a-z0-9-]+)")?)re");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), opt);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
        if ((*it)[2].matched) out.insert((*it)[2].str());
    }
    const std::regex pre(R"re(qstrcmp\(\s*argv\[\w+\]\s*,\s*"--([a-z][a-z0-9-]+)"\s*\))re");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), pre);
         it != std::sregex_iterator(); ++it)
        out.insert((*it)[1].str());
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

// ANTS-6599 row 1293 — `--import-roadmap <export.jsonl> <project-root>`: the
// second argument completes to a folder in bash and fish, as it does in zsh.
// Each runs in a scratch folder holding one folder and one file.
namespace {
std::string runShell(const std::string &script) {
    const std::string cmd = "bash -c '" + script + "' '" + root() + "/packaging/completions'";
    std::string out;
    if (FILE *p = popen(cmd.c_str(), "r")) {
        char buf[256];
        while (fgets(buf, sizeof buf, p)) out += buf;
        pclose(p);
    }
    return out;
}
const char *kScratch =
    R"(d=$(mktemp -d) && mkdir "$d/proj" && touch "$d/x.jsonl" && cd "$d" && )";
}  // namespace

TEST(CliDocsSync, Ants6599BashImportRoadmapSecondArgIsAFolder) {
    EXPECT_EQ("proj\n", runShell(std::string(kScratch) + R"(source "$0/ants-terminal.bash"; )"
        R"(COMP_WORDS=(ants-terminal --import-roadmap x.jsonl ""); COMP_CWORD=3; )"
        R"(_ants_terminal; printf "%s\n" "${COMPREPLY[@]}"; rm -rf "$d")"));
}

TEST(CliDocsSync, Ants6599FishImportRoadmapSecondArgIsAFolder) {
    if (std::system("command -v fish >/dev/null 2>&1") != 0) GTEST_SKIP() << "fish not installed";
    // fish may append a tab and a description to each candidate.
    const std::string out = runShell(std::string(kScratch) +
        R"(fish -c "source $0/ants-terminal.fish; complete -C \"ants-terminal --import-roadmap x.jsonl \""; )"
        R"(rm -rf "$d")");
    EXPECT_EQ(0u, out.rfind("proj/", 0)) << out;
    EXPECT_EQ(std::string::npos, out.find("x.jsonl")) << out;
}
