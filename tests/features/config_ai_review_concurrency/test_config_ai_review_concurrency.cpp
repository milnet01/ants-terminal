// ANTS-1727 — INV-14: ai_review_concurrency default 2, clamp [1, 4].

#include "config.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QTemporaryDir>

namespace {

// Sandbox XDG_CONFIG_HOME so Config reads/writes a temp dir, not the
// user's real config. Restores the prior value on teardown.
struct XdgConfigHomeGuard {
    QByteArray prior;
    bool hadPrior = false;
    XdgConfigHomeGuard() {
        hadPrior = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        if (hadPrior) prior = qgetenv("XDG_CONFIG_HOME");
    }
    void set(const QByteArray &v) { qputenv("XDG_CONFIG_HOME", v); }
    ~XdgConfigHomeGuard() {
        if (hadPrior) qputenv("XDG_CONFIG_HOME", prior);
        else qunsetenv("XDG_CONFIG_HOME");
    }
};

}  // namespace

TEST(ConfigAiReviewConcurrency, INV14_DefaultAndClamp) {
    XdgConfigHomeGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.set(tmp.path().toUtf8());

    Config cfg;
    EXPECT_EQ(cfg.aiReviewConcurrency(), 2);   // default when unset

    cfg.setAiReviewConcurrency(0);
    EXPECT_EQ(cfg.aiReviewConcurrency(), 1);    // clamp low
    cfg.setAiReviewConcurrency(-10);
    EXPECT_EQ(cfg.aiReviewConcurrency(), 1);

    cfg.setAiReviewConcurrency(1);
    EXPECT_EQ(cfg.aiReviewConcurrency(), 1);
    cfg.setAiReviewConcurrency(3);
    EXPECT_EQ(cfg.aiReviewConcurrency(), 3);    // in range
    cfg.setAiReviewConcurrency(4);
    EXPECT_EQ(cfg.aiReviewConcurrency(), 4);

    cfg.setAiReviewConcurrency(99);
    EXPECT_EQ(cfg.aiReviewConcurrency(), 4);     // clamp high
}
