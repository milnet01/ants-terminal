// ANTS-1727 — INV-14: ai_review_concurrency default 2, clamp [1, 4].

#include "config.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QTemporaryDir>
#include "../../_support/xdg_guard.h"

TEST(ConfigAiReviewConcurrency, INV14_DefaultAndClamp) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_CONFIG_HOME", tmp.path().toUtf8());

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
