// User themes load safely — see spec.md. ANTS-5082.

#include "themes.h"

#include "../../_support/xdg_guard.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

bool writeTheme(const QString &dir, const QString &name, const QByteArray &body) {
    QFile f(dir + QLatin1Char('/') + name);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(body) == body.size();
}

const Theme *findTheme(const std::vector<Theme> &themes, const QString &name) {
    for (const Theme &t : themes)
        if (t.name == name) return &t;
    return nullptr;
}

}  // namespace

TEST(UserThemeLoading, MistypedColourAndOversizedFile) {
    QTemporaryDir cfg;
    ASSERT_TRUE(cfg.isValid());
    ants_test::XdgGuard g;
    g.setTestMode(false);  // test mode ignores XDG_CONFIG_HOME
    g.setEnv("XDG_CONFIG_HOME", cfg.path().toUtf8());

    const QString themesDir = cfg.path() + QStringLiteral("/ants-terminal/themes");
    ASSERT_TRUE(QDir().mkpath(themesDir));

    // INV-1
    ASSERT_TRUE(writeTheme(themesDir, QStringLiteral("bad-colour.json"),
        R"({"name":"BadColour","bg_primary":"not-a-colour","text_primary":"#ffffff"})"));

    // INV-2 — valid JSON, but past the cap.
    QByteArray huge = R"({"name":"Huge","pad":")";
    huge.append(QByteArray(2 * 1024 * 1024, 'x'));
    huge.append(R"("})");
    ASSERT_TRUE(writeTheme(themesDir, QStringLiteral("huge.json"), huge));

    const std::vector<Theme> themes = Themes::loadUserThemes();

    const Theme *bad = findTheme(themes, QStringLiteral("BadColour"));
    ASSERT_NE(bad, nullptr) << "the theme with a mistyped colour did not load";
    EXPECT_TRUE(bad->bgPrimary.isValid()) << "a mistyped colour loaded as an invalid QColor";
    EXPECT_EQ(bad->bgPrimary, QColor(0x1E, 0x1E, 0x2E));

    EXPECT_EQ(findTheme(themes, QStringLiteral("Huge")), nullptr)
        << "an oversized theme file was read and loaded";
}

// RC-36 (audit 2026-09-26) — a user theme named like a built-in replaces it:
// listed once, and byName() returns the user's copy.
TEST(UserThemeLoading, UserThemeReplacesSameNamedBuiltIn) {
    QTemporaryDir cfg;
    ASSERT_TRUE(cfg.isValid());
    ants_test::XdgGuard g;
    g.setTestMode(false);
    g.setEnv("XDG_CONFIG_HOME", cfg.path().toUtf8());
    const QString themesDir = cfg.path() + QStringLiteral("/ants-terminal/themes");
    ASSERT_TRUE(QDir().mkpath(themesDir));
    ASSERT_TRUE(writeTheme(themesDir, QStringLiteral("dark.json"),
        R"({"name":"Dark","bg_primary":"#123456","text_primary":"#ffffff"})"));

    Themes::reload();
    const QColor got = Themes::byName(QStringLiteral("Dark")).bgPrimary;
    const qsizetype listed = Themes::names().count(QStringLiteral("Dark"));

    // Leave the process-wide list as the other tests expect it.
    QFile::remove(themesDir + QStringLiteral("/dark.json"));
    Themes::reload();

    EXPECT_EQ(got, QColor(0x12, 0x34, 0x56)) << "byName returned the built-in";
    EXPECT_EQ(listed, 1) << "the name is listed more than once";
}
