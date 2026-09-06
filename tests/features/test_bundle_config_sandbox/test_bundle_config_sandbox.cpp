// ANTS-4898 — a test process must not read the developer's real config.
// See tests/features/test_bundle_config_sandbox/spec.md.
//
// The sibling of ANTS-3856's XDG_DATA_HOME sandbox. That one was added
// because a test could open the live roadmap store; this one because a test
// could read the live config.json — and on 2026-09-06 that let twelve tests
// write into the user's real feedback corpus while going red for a reason
// that looked like a code defect.

#include "../../_support/xdg_guard.h"

#include "config.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

// INV-1 — the config location a test process resolves is not the user's own.
TEST(test_bundle_config_sandbox, Inv1ConfigLocationIsSandboxed) {
    const QString here =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    ASSERT_FALSE(here.isEmpty());

    // The real one, as it would resolve with no sandbox: $HOME/.config.
    const QString real =
        QDir(QDir::homePath()).filePath(QStringLiteral(".config"));
    EXPECT_NE(QDir(here).absolutePath(), QDir(real).absolutePath())
        << "the bundle main must redirect XDG_CONFIG_HOME before any test "
           "runs — a test that reads the developer's config asserts against "
           "their machine, and can write outside the repo";
}

// INV-2 — the specific property whose absence caused the damage: a Config
// built in a test carries none of the developer's settings.
TEST(test_bundle_config_sandbox, Inv2FreshConfigCarriesNoUserSettings) {
    Config cfg;
    EXPECT_TRUE(cfg.claudeMcpFeedbackRoot().isEmpty())
        << "a live claude.mcp_feedback_root redirects every derived feedback "
           "path — measured 2026-09-06, it sent twelve tests' writes into the "
           "real corpus";
}

// INV-3 — a per-test sandbox still wins over the per-process one, so the
// guards written before this change keep working.
TEST(test_bundle_config_sandbox, Inv3PerTestOverrideStillWins) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString mine = QDir(tmp.path()).filePath(QStringLiteral("cfg"));
    {
        ants_test::XdgGuard g;
        g.setEnv("XDG_CONFIG_HOME", mine.toUtf8());
        Config cfg;
        cfg.setClaudeMcpFeedbackRoot(QStringLiteral("/tmp/declared-corpus"));
        EXPECT_EQ(Config().claudeMcpFeedbackRoot(),
                  QStringLiteral("/tmp/declared-corpus"));
    }
    // ...and the process sandbox is intact once the guard unwinds.
    EXPECT_TRUE(Config().claudeMcpFeedbackRoot().isEmpty());
}
