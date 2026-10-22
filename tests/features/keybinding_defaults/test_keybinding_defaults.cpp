// Feature-conformance test for spec.md (ANTS-6146) — one table of default
// keyboard shortcuts, read by the menus and by the Settings table.

#include "config.h"

#include <gtest/gtest.h>
#include <QSet>
#include <QString>

#include <regex>
#include <string>

#include "../../_support/srcgrep.h"

#ifndef SRC_SETTINGSDIALOG_CPP_PATH
#  error "SRC_SETTINGSDIALOG_CPP_PATH compile definition required"
#endif

namespace {

QString defaultFor(const QString &action) {
    for (const auto &[name, key] : Config::defaultKeybindings())
        if (name == action) return key;
    return {};
}

}  // namespace

// INV-1 — each action once, each default chord once.
TEST(KeybindingDefaults, Inv1ActionsAndChordsAreUnique) {
    QSet<QString> actions, chords;
    for (const auto &[name, key] : Config::defaultKeybindings()) {
        EXPECT_FALSE(actions.contains(name)) << "repeated action " << name.toStdString();
        EXPECT_FALSE(chords.contains(key)) << "repeated chord " << key.toStdString();
        actions.insert(name);
        chords.insert(key);
    }
}

// INV-2 — Ctrl+Shift+Up/Down belong to prompt navigation (ANTS-1165).
TEST(KeybindingDefaults, Inv2BookmarkJumpsAreCtrlAlt) {
    EXPECT_EQ(defaultFor("next_bookmark"), QStringLiteral("Ctrl+Alt+Down"));
    EXPECT_EQ(defaultFor("prev_bookmark"), QStringLiteral("Ctrl+Alt+Up"));
}

// INV-3 — the menus take every default from the table.
TEST(KeybindingDefaults, Inv3MenusBindFromTheTable) {
    const std::string mw = ants_test::slurpMainWindow();
    ASSERT_FALSE(mw.empty());
    const std::regex call(R"re(keybinding\(\s*"([a-z_]+)"\s*(,)?)re");
    int calls = 0;
    for (auto it = std::sregex_iterator(mw.begin(), mw.end(), call);
         it != std::sregex_iterator(); ++it) {
        ++calls;
        const std::string action = (*it)[1].str();
        EXPECT_FALSE(defaultFor(QString::fromStdString(action)).isEmpty())
            << action << " is bound in a menu but missing from the table";
        EXPECT_FALSE((*it)[2].matched)
            << action << " passes its own default; take it from the table";
    }
    EXPECT_GT(calls, 0);
}

// INV-4 — Settings lists and resets from the table.
TEST(KeybindingDefaults, Inv4SettingsReadsTheTable) {
    const std::string src = ants_test::slurpFile(SRC_SETTINGSDIALOG_CPP_PATH);
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("Config::defaultKeybindings()"), std::string::npos);
    EXPECT_EQ(src.find("{\"new_tab\""), std::string::npos)
        << "settingsdialog.cpp still holds its own shortcut table";
}
