// Why this exists: the auto-model nudge saved its answer through a fresh
// Config, which the config.json watcher took for an external edit — a
// reload, a closed Settings dialog and a toast, with m_config stale
// until then. See spec.md.

#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef ANTS_SOURCE_DIR
#  error "ANTS_SOURCE_DIR compile definition required"
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

TEST(AutoModelNudgeSharedConfig, WritesThroughMConfig) {
    const std::string src =
        ants_test::slurpFile(ANTS_SOURCE_DIR "/src/mainwindow_tabs.cpp");
    const std::string body =
        memberBody(src, "void MainWindow::showClaudeAutoModelNudge(");
    ASSERT_FALSE(body.empty())
        << "precondition: showClaudeAutoModelNudge body not found";

    // INV-1 — a fresh Config's save is not in m_config.lastWrittenBytes(),
    // so the watcher misreads it as an external edit (config-hot-reload C1).
    EXPECT_FALSE(
        std::regex_search(body, std::regex(R"(\bConfig\s*\()")) ||
        std::regex_search(body,
                          std::regex(R"(\bConfig\s+[a-z][A-Za-z0-9_]*\s*;)")))
        << "INV-1: showClaudeAutoModelNudge constructs a fresh Config";

    // INV-2 — both writes still happen, through m_config.
    EXPECT_NE(body.find("m_config.setClaudeAutoModelNudgeShown(true)"),
              std::string::npos)
        << "INV-2: the shown flag is not written through m_config";
    EXPECT_NE(body.find("m_config.setClaudeAutoModelSwitch(true)"),
              std::string::npos)
        << "INV-2: the switch is not written through m_config";
}
