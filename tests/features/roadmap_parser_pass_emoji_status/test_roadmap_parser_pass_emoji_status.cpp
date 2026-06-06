// ANTS-2039 — feature-conformance test for pass-heading status
// classification of the emoji-prefixed `- **Status**: ✅ Done` form.
// Behavioural test against RoadmapDialog::parseBullets's pass-headings
// dispatch (no real ROADMAP.md needed). Reproduces the RetroDB
// session-5 symptom (✅ Done lines reading as 📋) and locks in the
// leading-emoji skip.

#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringLiteral>

namespace {

// The 📋/🚧/✅/💭 status emojis as UTF-8 byte literals (matching the
// codepoints parsePassHeadingBullets writes into BulletRecord::status).
const QString kPlanned    = QString::fromUtf8("\xF0\x9F\x93\x8B"); // 📋
const QString kInProgress = QString::fromUtf8("\xF0\x9F\x9A\xA7"); // 🚧
const QString kShipped    = QString::fromUtf8("\xE2\x9C\x85");     // ✅
const QString kConsidered = QString::fromUtf8("\xF0\x9F\x92\xAD"); // 💭

// A pass-headings doc whose Status lines carry an emoji prefix before
// the keyword, plus the bare-keyword and bare-emoji edge forms. The
// sniffer needs ≥2 `#### Pass` headings + ≥2 `- **Status**:` markers
// and no ants-v1 emoji bullets, which this satisfies.
QString emojiStatusDoc() {
    // fromUtf8 (not QStringLiteral): the \xNN escapes below are UTF-8
    // bytes for the status emojis. QStringLiteral would build a u"…"
    // (char16_t) literal and turn each byte into a wrong code unit.
    return QString::fromUtf8(
        "## Active\n"
        "\n"
        "#### Pass 48.1 (CRITICAL, S) Emoji-prefixed done\n"
        "- **Status**: \xE2\x9C\x85 Done (v3.6.3x)\n"
        "- **Finding**: shipped.\n"
        "\n"
        "#### Pass 48.2 (HIGH, M) Emoji-prefixed in-progress\n"
        "- **Status**: \xF0\x9F\x9A\xA7 In-progress\n"
        "- **Finding**: doing.\n"
        "\n"
        "#### Pass 48.3 (LOW, S) Emoji-prefixed deferred\n"
        "- **Status**: \xF0\x9F\x92\xAD Deferred\n"
        "- **Finding**: later.\n"
        "\n"
        "#### Pass 48.4 (LOW, S) Bare emoji, no keyword\n"
        "- **Status**: \xE2\x9C\x85\n"
        "- **Finding**: emoji only.\n"
        "\n"
        "#### Pass 48.5 (LOW, S) Bare keyword, no emoji\n"
        "- **Status**: done\n"
        "- **Finding**: keyword only.\n"
        "\n"
        "#### Pass 48.6 (LOW, S) Bare keyword in-progress\n"
        "- **Status**: in-progress\n"
        "- **Finding**: keyword only.\n");
}

}  // namespace

// INV-1 — `✅ Done (v3.6.3x)` reads as shipped, not planned.
TEST(roadmap_parser_pass_emoji_status, Inv1EmojiDoneIsShipped) {
    const auto bullets = RoadmapDialog::parseBullets(emojiStatusDoc());
    ASSERT_EQ(bullets.size(), 6)
        << "precondition: doc must parse as 6 pass-heading bullets";
    EXPECT_EQ(bullets[0].status, kShipped)
        << "INV-1: `- **Status**: ✅ Done (v3.6.3x)` must read ✅ "
           "(the RetroDB session-5 bug read it as 📋)";
}

// INV-2 — every status emoji is skipped before the keyword.
TEST(roadmap_parser_pass_emoji_status, Inv2AllEmojisSkipped) {
    const auto bullets = RoadmapDialog::parseBullets(emojiStatusDoc());
    ASSERT_EQ(bullets.size(), 6);
    EXPECT_EQ(bullets[1].status, kInProgress)
        << "INV-2: `🚧 In-progress` must read 🚧";
    EXPECT_EQ(bullets[2].status, kConsidered)
        << "INV-2: `💭 Deferred` must read 💭";
}

// INV-3 — a bare emoji with no trailing keyword is authoritative.
TEST(roadmap_parser_pass_emoji_status, Inv3BareEmojiAuthoritative) {
    const auto bullets = RoadmapDialog::parseBullets(emojiStatusDoc());
    ASSERT_EQ(bullets.size(), 6);
    EXPECT_EQ(bullets[3].status, kShipped)
        << "INV-3: `- **Status**: ✅` (no keyword) must read ✅";
}

// INV-4 — the bare-keyword forms are unchanged.
TEST(roadmap_parser_pass_emoji_status, Inv4BareKeywordUnchanged) {
    const auto bullets = RoadmapDialog::parseBullets(emojiStatusDoc());
    ASSERT_EQ(bullets.size(), 6);
    EXPECT_EQ(bullets[4].status, kShipped)
        << "INV-4: bare `done` (no emoji) still reads ✅";
    EXPECT_EQ(bullets[5].status, kInProgress)
        << "INV-4: bare `in-progress` (no emoji) still reads 🚧";
    // And planned remains the default for an absent/unknown status —
    // guarded indirectly: none of the six fixtures should be 📋.
    for (const auto &b : bullets) {
        EXPECT_NE(b.status, kPlanned)
            << "INV-4: no fixture status should fall through to 📋";
    }
}
