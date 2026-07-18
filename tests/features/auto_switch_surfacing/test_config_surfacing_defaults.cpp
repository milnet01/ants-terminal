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
#include <QDir>
#include <QFile>
#include <QJsonObject>
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

// ANTS-1976 — claudeAutoModel() surfaces "debug" defaulting FALSE on a
// fresh config; writing claude.auto_model_debug:true reads through. Locks
// the config-key name the auto-switcher debug trace gates on.
TEST_F(ConfigSurfacingDefaults, DebugSurfacesAndDefaultsFalse) {
    {
        Config cfg;
        EXPECT_FALSE(cfg.claudeAutoModel().value("debug").toBool())
            << "debug must default false on a fresh config";
    }
    // Write config.json with the key set true under the isolated XDG dir.
    const QString cfgDir = m_dir->path() + QStringLiteral("/ants-terminal");
    ASSERT_TRUE(QDir().mkpath(cfgDir));
    QFile f(cfgDir + QStringLiteral("/config.json"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("{\"claude.auto_model_debug\": true}\n");
    f.close();

    Config cfg2;
    EXPECT_TRUE(cfg2.claudeAutoModel().value("debug").toBool())
        << "claude.auto_model_debug:true must surface as debug:true";
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

// ANTS-3572 — tokens-saved chip flag defaults TRUE (a token-saving surface
// ships enabled); the three data keys default empty/zero on a fresh config.
TEST_F(ConfigSurfacingDefaults, TokensSavedDefaults) {
    Config cfg;
    EXPECT_TRUE(cfg.claudeTokensSavedChipEnabled());
    EXPECT_TRUE(cfg.claudeTokensSavedMonthly().isEmpty());
    EXPECT_EQ(cfg.claudeTokensSavedLifetime(), 0);
    EXPECT_TRUE(cfg.claudeTokensSavedSince().isEmpty());
}

// ANTS-3572 — the chip flag saves on set (normal idiom); the three DATA
// setters are store-only, so they persist only after an explicit save()
// (INV-7 — the fold batches all three behind one write).
TEST_F(ConfigSurfacingDefaults, TokensSavedRoundTrip) {
    {
        Config cfg;
        cfg.setClaudeTokensSavedChipEnabled(false);  // saves immediately
        QJsonObject monthly;
        monthly["2026-07"] = 5000.0;
        cfg.setClaudeTokensSavedMonthly(monthly);    // store-only
        cfg.setClaudeTokensSavedLifetime(123456);    // store-only
        cfg.setClaudeTokensSavedSince("2026-07-18"); // store-only
        cfg.save();                                  // single write (INV-7)
    }
    Config cfg2;
    EXPECT_FALSE(cfg2.claudeTokensSavedChipEnabled());
    EXPECT_EQ(cfg2.claudeTokensSavedLifetime(), 123456);
    EXPECT_EQ(static_cast<qint64>(
                  cfg2.claudeTokensSavedMonthly().value("2026-07").toDouble()),
              5000);
    EXPECT_EQ(cfg2.claudeTokensSavedSince(), QString("2026-07-18"));
}

// ANTS-3572 — the store-only data setters do NOT persist without a save()
// (proves INV-7's "no per-call write" — a bare setter leaves disk untouched).
TEST_F(ConfigSurfacingDefaults, TokensSavedDataSettersAreStoreOnly) {
    {
        Config cfg;
        cfg.setClaudeTokensSavedLifetime(999);  // store-only, no save()
    }
    Config cfg2;
    EXPECT_EQ(cfg2.claudeTokensSavedLifetime(), 0)
        << "a store-only setter must not persist without an explicit save()";
}
