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
