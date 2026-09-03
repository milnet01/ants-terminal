// ANTS-4404 — feature-conformance test for the roadmap_log write path's
// fence-extent rule. Behavioural: drives cmdRoadmapLogAmendBodyForTest
// against seeded temp roadmaps whose prose QUOTES fence syntax, and asserts
// the bullets below stay editable. See spec.md.
//
// The measured defect: ROADMAP.md:31099 carries ```` ```python ```` — a
// four-backtick span quoting a three-backtick literal, which CommonMark
// § 4.5 keeps as a paragraph. The pre-fix walkers read it as a fence opener
// that nothing closes, so every bullet below it refused
// anchor_unsafe_context and the tail of the roadmap was unwritable.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(bytes) == bytes.size();
}

QJsonObject req(const QString &root) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = QStringLiteral("amend_body");
    return r;
}

// Padding past kRoadmapMinParseableSize (1024 B), which the amend path
// requires before it will trust an ants-v1 walk. Mirrors seedV1() in
// tests/features/roadmap_log_amend_body/.
const char *kPad =
    "Intro paragraph that exists purely to pad the file past the\n"
    "1 KiB minimum-parseable-size gate the amend path enforces\n"
    "before it will trust an ants-v1 walk. Lorem ipsum dolor sit\n"
    "amet, consectetur adipiscing elit, sed do eiusmod tempor\n"
    "incididunt ut labore et dolore magna aliqua. Ut enim ad minim\n"
    "veniam, quis nostrud exercitation ullamco laboris nisi ut\n"
    "aliquip ex ea commodo consequat. Duis aute irure dolor in\n"
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat\n"
    "nulla pariatur. Excepteur sint occaecat cupidatat non\n"
    "proident, sunt in culpa qui officia deserunt mollit anim id\n"
    "est laborum. More padding to be safe and clear the gate with\n"
    "comfortable headroom for the parser and the size check above.\n"
    "Sed ut perspiciatis unde omnis iste natus error sit\n"
    "voluptatem accusantium doloremque laudantium, totam rem\n"
    "aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo.\n";

// 📋 = U+1F4CB, as the ants-v1 status glyph.
const char *kBullet =
    "- \xF0\x9F\x93\x8B [ANTS-0042] **Seed bullet headline.**\n"
    "  Layman: the value is pinned in the spec here.\n"
    "  Kind: feature.\n"
    "  Source: seed.\n";

QByteArray seed(const QByteArray &beforeBullet) {
    return QByteArray("# Test Roadmap\n\n") + kPad
         + QByteArray("\n## Work Items\n\n") + beforeBullet
         + QByteArray(kBullet) + QByteArray("\n");
}

QJsonObject amend(const QString &root) {
    RemoteControl rc(nullptr);
    QJsonObject r = req(root);
    r[QStringLiteral("id")]       = QStringLiteral("ANTS-0042");
    r[QStringLiteral("old_text")] = QStringLiteral("pinned in the spec");
    r[QStringLiteral("new_text")] = QStringLiteral("pinned in INV-2");
    return rc.cmdRoadmapLogAmendBodyForTest(r).object();
}

}  // namespace

// INV-1 — the real-world shape from ROADMAP.md:31099. A four-backtick span
// quoting a three-backtick literal is a paragraph, not a fence opener, so the
// bullet below it stays editable.
TEST(roadmap_log_fence_span, Inv1QuotedFenceDoesNotMaskFollowingBullet) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Wrapped exactly as ROADMAP.md:31098-31100 wraps it: the span lands at
    // the START of its line, past a two-space continuation indent, which is
    // what makes trimmed().startsWith("```") fire on it.
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed(
        "Why their specs are not simply retagged: every spec there writes\n"
        "patterns in\n"
        "  ```` ```python ```` because that is what the formatter formats\n"
        "  and CI gates; a ```` ```regex ```` fence is invisible to it.\n"
        "\n")));

    const QJsonObject resp = amend(tmp.path());

    EXPECT_NE(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("anchor_unsafe_context"))
        << "a quoted fence must not mask the bullet below it — this is the "
           "ROADMAP.md:31099 shape that made the roadmap tail unwritable";
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "amend_body should succeed below a quoted fence";
    EXPECT_TRUE(resp.value(QStringLiteral("amended")).toBool());
}

// INV-2 — masking is not weakened to buy INV-1: a genuine unterminated fence
// still makes the bullet below it refuse.
TEST(roadmap_log_fence_span, Inv2RealFenceStillMasks) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed(
        "```\n"
        "an unterminated code block swallowing what follows\n")));

    const QJsonObject resp = amend(tmp.path());

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << "a bullet inside a real unterminated fence must refuse";
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("anchor_unsafe_context"));
}

// INV-3 — a properly closed block releases the bullet after it.
TEST(roadmap_log_fence_span, Inv3ClosedFenceReleasesFollowingBullet) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed(
        "```python\n"
        "print('a properly closed block')\n"
        "```\n"
        "\n")));

    const QJsonObject resp = amend(tmp.path());

    EXPECT_NE(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("anchor_unsafe_context"))
        << "a closed fence must not leak past its closer";
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
}

// INV-4 — the regression a naive indent tightening would cause, in both
// directions. A fence indented past the top-level three-space allowance but
// within its list item's content column still masks (ANTS-3638) — ROADMAP.md
// carries fences at indent 5 under bullets — and its closer still releases
// what follows. Same shape ANTS-4403 pinned for the migration walk.
TEST(roadmap_log_fence_span, Inv4IndentedFenceUnderBulletStillMasks) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed(
        "- \xF0\x9F\x93\x8B [ANTS-0041] **Carries an indented sample.**\n"
        "  Body.\n"
        "\n"
        "     ```\n"
        "     - \xF0\x9F\x93\x8B [ANTS-0043] **Sample text, not a bullet.**\n"
        "     ```\n"
        "\n")));

    // The decoy inside the fence is seen as a bullet but flagged fenced, so a
    // write against it refuses rather than editing sample text.
    RemoteControl rc(nullptr);
    QJsonObject inner = req(tmp.path());
    inner[QStringLiteral("id")]       = QStringLiteral("ANTS-0043");
    inner[QStringLiteral("old_text")] = QStringLiteral("Sample");
    inner[QStringLiteral("new_text")] = QStringLiteral("Edited");
    const QJsonObject innerResp =
        rc.cmdRoadmapLogAmendBodyForTest(inner).object();
    EXPECT_FALSE(innerResp.value(QStringLiteral("ok")).toBool())
        << "an indented fence under a list item must still mask its body";

    // …and the closer releases the real bullet after it.
    const QJsonObject resp = amend(tmp.path());
    EXPECT_NE(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("anchor_unsafe_context"))
        << "an indented fence's closer must release what follows";
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
}

// INV-5 (ANTS-4823 repair 2) — a fence opened inside one bullet's body ends
// with that body and cannot reach a later bullet. The write-side escape only
// covers text written THROUGH the verb, so a hand-written or legacy body can
// still open a fence nothing closes; before this rule that body took every
// bullet under it down with it, and the refusal named an innocent one.
TEST(roadmap_log_fence_span, Inv5BulletBodyFenceDoesNotEscapeItsBullet) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed(
        "- \xF0\x9F\x93\x8B [ANTS-0041] **Body opens a fence it never closes.**\n"
        "  Body prose before the opener.\n"
        "  ```\n"
        "  an opener with no closer, inside this bullet's body\n"
        "\n")));

    const QJsonObject resp = amend(tmp.path());

    EXPECT_NE(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("anchor_unsafe_context"))
        << "a fence opened in a PREVIOUS bullet's body must not mask this "
           "bullet — one bad body made most of the roadmap unwritable";
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "amend_body should succeed below a bullet-scoped fence";
    EXPECT_TRUE(resp.value(QStringLiteral("amended")).toBool());
}
