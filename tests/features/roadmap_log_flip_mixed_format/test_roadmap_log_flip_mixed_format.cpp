// ANTS-3561 — feature-conformance test: roadmap_log op:flip / op:annotate must
// locate an ants-v1 emoji bullet (`- 📋 [3D_E-0031] **...**`) living inside a
// GFM-majority (mixed-format) roadmap. Before the fix, cmdRoadmapLogFlip only
// tried the ants-v1 walker when the GFM walk found ZERO bullets; a mixed file
// (994 GFM checkboxes + a handful of appended emoji bullets, the real Vestige
// shape) has a non-empty GFM walk, so the emoji bullet was unreachable by any
// locator — bullet_not_found — even though roadmap_query reads it fine.
//
// Drives the cmdRoadmapLogFlipForTest seam behaviourally (mirrors
// roadmap_id_format_guard / roadmap_log_flip_idless_antsv1).

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

// ~1 KiB intro padding so the file clears kRoadmapMinParseableSize (1024 B)
// and the ants-v1 walker fallback is eligible.
const char *kPad =
    "Intro paragraph that exists purely to pad the file past the 1 KiB\n"
    "minimum-parseable-size gate the flip path enforces before it will\n"
    "trust an ants-v1 walk. Lorem ipsum dolor sit amet, consectetur\n"
    "adipiscing elit, sed do eiusmod tempor incididunt ut labore et\n"
    "dolore magna aliqua. Ut enim ad minim veniam, quis nostrud\n"
    "exercitation ullamco laboris nisi ut aliquip ex ea commodo\n"
    "consequat. Duis aute irure dolor in reprehenderit in voluptate\n"
    "velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint\n"
    "occaecat cupidatat non proident, sunt in culpa qui officia deserunt\n"
    "mollit anim id est laborum. Sed ut perspiciatis unde omnis iste\n"
    "natus error sit voluptatem accusantium doloremque laudantium, totam\n"
    "rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam\n"
    "voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed\n"
    "quia consequuntur magni dolores eos qui ratione voluptatem sequi.\n";

// A mixed-format roadmap: GFM task-list bullets (so walkGfmBullets is
// non-empty) + one ants-v1 emoji bullet with an em-dash + parenthetical +
// trailing-period headline (the exact Vestige 3D_E-0031 shape).
// 📋 = U+1F4CB (F0 9F 93 8B); — = U+2014 (E2 80 94).
QByteArray seed() {
    QByteArray b = "# Test Roadmap\n\n";
    b += kPad;
    b += "\n## Work Items\n\n"
         "- [ ] **G1.** \xE2\x80\x94 first GFM task-list bullet.\n"
         "- [ ] **G2.** \xE2\x80\x94 second GFM task-list bullet.\n"
         "- \xF0\x9F\x93\x8B [3D_E-0031] **Meadow realism A \xE2\x80\x94 real "
         "PBR ground textures on terrain (replace the flat-colour "
         "placeholder).**\n"
         "  Source: seed.\n\n";
    return b;
}

// The verbatim headline roadmap_query reports for the emoji bullet.
QString emojiHeadline() {
    return QString::fromUtf8(
        "Meadow realism A \xE2\x80\x94 real PBR ground textures on terrain "
        "(replace the flat-colour placeholder).");
}

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

}  // namespace

// INV-1 — flip the ants-v1 emoji bullet by id inside a GFM-majority file.
TEST(roadmap_log_flip_mixed_format, Inv1FlipEmojiBulletById) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("id")]         = QStringLiteral("3D_E-0031");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "an ants-v1 emoji bullet in a mixed GFM file must resolve by id; "
           "got code="
        << resp.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("ants-v1"));
    EXPECT_EQ(resp.value(QStringLiteral("to_status")).toString(),
              QStringLiteral("✅"));
}

// INV-2 — annotate the emoji bullet by id (append a note, status untouched).
TEST(roadmap_log_flip_mixed_format, Inv2AnnotateEmojiBulletById) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("annotate");
    req[QStringLiteral("id")]         = QStringLiteral("3D_E-0031");
    req[QStringLiteral("note")]       = QStringLiteral("Progress note.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "annotate must resolve the emoji bullet by id; got code="
        << resp.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("ants-v1"));

    // The note landed in the file under the emoji bullet.
    QFile f(roadmapPath(tmp.path()));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString after = QString::fromUtf8(f.readAll());
    f.close();
    EXPECT_TRUE(after.contains(QStringLiteral("Progress note.")));
}

// INV-3 — flip the emoji bullet by its verbatim headline (em-dash +
// parenthetical + trailing period), the fallback the field report found broken.
TEST(roadmap_log_flip_mixed_format, Inv3FlipEmojiBulletByHeadline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("in-progress");
    req[QStringLiteral("headline")]   = emojiHeadline();
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "the verbatim headline locator must resolve the emoji bullet; "
           "got code="
        << resp.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("ants-v1"));
}

// INV-4 (regression) — a GFM bullet in the same mixed file still flips by its
// bold-ID; the ants-v1 fallback never shadows a real GFM match.
TEST(roadmap_log_flip_mixed_format, Inv4GfmBulletStillFlips) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("id")]         = QStringLiteral("G1");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "a GFM bullet must still flip in a mixed file; got code="
        << resp.value(QStringLiteral("code")).toString().toStdString();
    // The GFM flip path carries no `format` field (only the ants-v1 path
    // stamps format:"ants-v1"); assert the GFM identity via its bold-ID echo.
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(),
              QStringLiteral("G1"));
    EXPECT_EQ(resp.value(QStringLiteral("to_status")).toString(),
              QStringLiteral("✅"));
}

// INV-5 (regression) — a canonical but genuinely-absent id still returns
// bullet_not_found; the fallback resolves real bullets only.
TEST(roadmap_log_flip_mixed_format, Inv5AbsentIdStillNotFound) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("id")]         = QStringLiteral("3D_E-9999");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
}

// ANTS-4604 — roadmap_query must say the file is MIXED, not just that it is
// github-task-list.
//
// This suite's own INV-1 asserts that a flip on the emoji bullet answers
// `format: "ants-v1"`, and that is correct — it describes the bullet the op
// touched. roadmap_query answers `github-task-list`, and that is correct too —
// it means "the GFM adapter engaged, so kind/lanes/layman are degraded". One
// field name, two questions. Vestige read the pair as a contradiction and, from
// it, inferred that the disagreement decides whether the store may serve their
// project. It does not: migratedProject() runs its own detector over the
// detection prefix and reads neither envelope field.
//
// What was actually missing is the thing both answers hide: whether the file is
// uniform. The old loop broke on the first GFM bullet, so a file with one GFM
// bullet and a file with nothing but GFM bullets were indistinguishable.
TEST(roadmap_log_flip_mixed_format, Ants4604QueryReportsMixedWithCounts) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("github-task-list"))
        << "unchanged: the adapter-mode echo still fires";
    EXPECT_TRUE(resp.value(QStringLiteral("mixed")).toBool())
        << "the seed is 2 GFM bullets + 1 ants-v1 emoji bullet: "
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("gfm_bullets")).toInt(), 2);
    EXPECT_EQ(resp.value(QStringLiteral("non_gfm_bullets")).toInt(), 1);
}

// ANTS-4604 — and a UNIFORM file's envelope is byte-identical to what it was.
// This is the half that keeps the change additive: emitting `mixed:false` on
// every uniform roadmap would be a new field on every existing caller's
// envelope, which is what "additive" was chosen to avoid.
TEST(roadmap_log_flip_mixed_format, Ants4604UniformGfmOmitsTheMixedFields) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QByteArray uniform = "# Test Roadmap\n\n";
    uniform += kPad;
    uniform += "\n## Work Items\n\n"
               "- [ ] **G1.** \xE2\x80\x94 first GFM task-list bullet.\n"
               "- [ ] **G2.** \xE2\x80\x94 second GFM task-list bullet.\n"
               "- [x] **G3.** \xE2\x80\x94 third GFM task-list bullet.\n\n";
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), uniform));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("github-task-list"));
    EXPECT_FALSE(resp.contains(QStringLiteral("mixed")))
        << "a uniform file must not gain the field: "
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_FALSE(resp.contains(QStringLiteral("gfm_bullets")));
    EXPECT_FALSE(resp.contains(QStringLiteral("non_gfm_bullets")));
}
