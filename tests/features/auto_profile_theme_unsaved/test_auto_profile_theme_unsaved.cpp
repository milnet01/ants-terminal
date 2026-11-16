// ANTS-6489 — an auto-profile theme is shown, not saved.
// See tests/features/auto_profile_theme_unsaved/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <regex>
#include <string>

namespace {

std::string bodyOf(const char *signature) {
    return ants_test::slurpFunctionBody(ants_test::slurpMainWindow(), signature);
}

}  // namespace

// INV-1 — the auto-profile path applies its theme unsaved.
TEST(AutoProfileThemeUnsaved, Inv1ProfileThemeIsNotSaved) {
    const std::string body = bodyOf("void MainWindow::checkAutoProfileRules(");
    ASSERT_FALSE(body.empty()) << "checkAutoProfileRules body not found";
    EXPECT_TRUE(std::regex_search(
        body, std::regex(R"(applyTheme\([^;]*ThemeSave::Unsaved\s*\))")))
        << "the profile's theme must be applied with ThemeSave::Unsaved";
    EXPECT_EQ(body.find("m_config.setTheme("), std::string::npos)
        << "checkAutoProfileRules must not save a theme";
}

// INV-2 — applyTheme saves only when asked, on every path that saves.
TEST(AutoProfileThemeUnsaved, Inv2ApplyThemeSavesOnlyWhenAsked) {
    const std::string body = bodyOf("void MainWindow::applyTheme(");
    ASSERT_FALSE(body.empty()) << "applyTheme body not found";
    const std::regex gatedSave(
        R"(if\s*\(\s*save\s*==\s*ThemeSave::Saved\s*\)\s*m_config\.setTheme\(name\))");
    const auto saves = std::distance(
        std::sregex_iterator(body.begin(), body.end(), gatedSave),
        std::sregex_iterator());
    EXPECT_EQ(saves, 2)
        << "the early return and the full apply must each save only for "
           "ThemeSave::Saved";
    std::size_t setThemes = 0;
    for (std::size_t p = body.find("m_config.setTheme("); p != std::string::npos;
         p = body.find("m_config.setTheme(", p + 1))
        ++setThemes;
    EXPECT_EQ(setThemes, 2u) << "an ungated m_config.setTheme is left";
    EXPECT_TRUE(std::regex_search(body, std::regex(R"(applyTheme\(deferred,\s*save\))")))
        << "the popup deferral must carry the save flag";
}

// INV-3 — a reload re-applies the theme only when the saved one changed.
TEST(AutoProfileThemeUnsaved, Inv3ReloadComparesTheSavedTheme) {
    const std::string body = bodyOf("void MainWindow::onConfigFileChanged(");
    ASSERT_FALSE(body.empty()) << "onConfigFileChanged body not found";
    const auto saved = body.find("const QString savedTheme = m_config.theme();");
    const auto reload = body.find("m_config = Config();");
    ASSERT_NE(saved, std::string::npos)
        << "the theme saved before the reload must be kept";
    ASSERT_NE(reload, std::string::npos);
    EXPECT_LT(saved, reload) << "savedTheme must be read before the reload";
    EXPECT_NE(body.find("newTheme != savedTheme"), std::string::npos)
        << "the reload must compare against the theme saved before it";
}
