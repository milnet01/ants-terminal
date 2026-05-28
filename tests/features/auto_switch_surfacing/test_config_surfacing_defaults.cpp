// ANTS-1893 INV-11 — the three surfacing mute toggles default TRUE on
// a fresh config (a user upgrading from pre-1893 sees all three
// surfaces ON by default). Setter round-trips persist FALSE so a
// muted toggle stays muted across process restarts.
//
// XDG_CONFIG_HOME isolation: the test sets the env var to a
// QTemporaryDir scope so the user's real config.json is never read or
// written. The override restores on dtor (QTemporaryDir cleanup) but
// we explicitly unsetenv at TearDown for belt-and-braces hygiene.
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include "config.h"

namespace {

class ConfigSurfacingDefaults : public ::testing::Test {
protected:
    void SetUp() override {
        m_dir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(m_dir->isValid());
        m_prevXdg = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", m_dir->path().toUtf8());
    }
    void TearDown() override {
        if (m_prevXdg.isEmpty()) qunsetenv("XDG_CONFIG_HOME");
        else                     qputenv("XDG_CONFIG_HOME", m_prevXdg);
        m_dir.reset();
    }
    std::unique_ptr<QTemporaryDir> m_dir;
    QByteArray m_prevXdg;
};

}  // namespace

// INV-11 — three surfacing toggles default TRUE on a stripped-down
// JSON missing the three keys.
TEST_F(ConfigSurfacingDefaults, AllThreeDefaultTrue) {
    Config cfg;
    EXPECT_TRUE(cfg.claudeAutoModelToastEnabled());
    EXPECT_TRUE(cfg.claudeAutoModelChipPulseEnabled());
    EXPECT_TRUE(cfg.claudeAutoModelUndoEnabled());
}

// INV-11 round-trip — each setter persists FALSE; re-read sees FALSE.
TEST_F(ConfigSurfacingDefaults, RoundTripToFalsePersists) {
    {
        Config cfg;
        cfg.setClaudeAutoModelToastEnabled(false);
        cfg.setClaudeAutoModelChipPulseEnabled(false);
        cfg.setClaudeAutoModelUndoEnabled(false);
    }
    // Re-construct Config — should read the persisted false values.
    Config cfg2;
    EXPECT_FALSE(cfg2.claudeAutoModelToastEnabled());
    EXPECT_FALSE(cfg2.claudeAutoModelChipPulseEnabled());
    EXPECT_FALSE(cfg2.claudeAutoModelUndoEnabled());
}

// INV-11 each-toggle-independent — flipping one off leaves the other
// two TRUE.
TEST_F(ConfigSurfacingDefaults, IndependentTogglesPersistOrthogonally) {
    {
        Config cfg;
        cfg.setClaudeAutoModelToastEnabled(false);
        // Other two left at default (TRUE).
    }
    Config cfg2;
    EXPECT_FALSE(cfg2.claudeAutoModelToastEnabled());
    EXPECT_TRUE(cfg2.claudeAutoModelChipPulseEnabled());
    EXPECT_TRUE(cfg2.claudeAutoModelUndoEnabled());
}
