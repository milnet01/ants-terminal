// ANTS-3565 — feature-conformance test: roadmap_log op:amend_body and
// op:flip_batch must locate an ants-v1 emoji bullet (`- 📋 [3D_E-0031] **...**`)
// living inside a GFM-majority (mixed-format) roadmap — the same blind spot
// ANTS-3561 fixed for op:flip / op:annotate. Before the fix, amend_body only
// tried the ants-v1 walker when the GFM walk was empty, and flip_batch picked
// one format for the whole locators array, so the appended emoji bullet was
// unreachable in a mixed file even though roadmap_query reads it fine.
//
// Drives the cmdRoadmapLog{AmendBody,FlipBatch}ForTest seams behaviourally
// (mirrors roadmap_log_flip_mixed_format).

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
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
    "minimum-parseable-size gate the write paths enforce before they will\n"
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

// Mixed-format roadmap: GFM task-list bullets (each with an indented body
// line, so amend_body has a body span to patch) + one ants-v1 emoji bullet
// with an em-dash + parenthetical + trailing-period headline and its own
// body line. 📋 = U+1F4CB (F0 9F 93 8B); — = U+2014 (E2 80 94).
QByteArray seed() {
    QByteArray b = "# Test Roadmap\n\n";
    b += kPad;
    b += "\n## Work Items\n\n"
         "- [ ] **G1.** \xE2\x80\x94 first GFM task-list bullet.\n"
         "  G1 body prose with REPLACE_ME_GFM token.\n"
         "- [ ] **G2.** \xE2\x80\x94 second GFM task-list bullet.\n"
         "- \xF0\x9F\x93\x8B [3D_E-0031] **Meadow realism A \xE2\x80\x94 real "
         "PBR ground textures on terrain (replace the flat-colour "
         "placeholder).**\n"
         "  Body prose with REPLACE_ME_V1 token.\n"
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

QString readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString s = QString::fromUtf8(f.readAll());
    f.close();
    return s;
}

}  // namespace

// INV-1 — amend_body patches the ants-v1 emoji bullet's body by id.
TEST(roadmap_log_mixed_amend_flipbatch, Inv1AmendEmojiBulletById) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("amend_body");
    req[QStringLiteral("id")]         = QStringLiteral("3D_E-0031");
    req[QStringLiteral("old_text")]   = QStringLiteral("REPLACE_ME_V1");
    req[QStringLiteral("new_text")]   = QStringLiteral("PATCHED_V1");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "amend_body must resolve the emoji bullet by id in a mixed file; "
           "got code="
        << resp.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("ants-v1"));
    const QString after = readAll(roadmapPath(tmp.path()));
    EXPECT_TRUE(after.contains(QStringLiteral("PATCHED_V1")));
    EXPECT_FALSE(after.contains(QStringLiteral("REPLACE_ME_V1")));
}

// INV-2 — amend_body resolves the emoji bullet by its verbatim headline.
TEST(roadmap_log_mixed_amend_flipbatch, Inv2AmendEmojiBulletByHeadline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("amend_body");
    req[QStringLiteral("headline")]   = emojiHeadline();
    req[QStringLiteral("old_text")]   = QStringLiteral("REPLACE_ME_V1");
    req[QStringLiteral("new_text")]   = QStringLiteral("PATCHED_V1");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "the verbatim headline locator must resolve the emoji bullet; "
           "got code="
        << resp.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("ants-v1"));
    EXPECT_TRUE(readAll(roadmapPath(tmp.path()))
                    .contains(QStringLiteral("PATCHED_V1")));
}

// INV-3 — flip_batch flips the emoji bullet by id; entry carries the format.
TEST(roadmap_log_mixed_amend_flipbatch, Inv3FlipBatchEmojiBulletById) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject loc;
    loc[QStringLiteral("id")] = QStringLiteral("3D_E-0031");
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip_batch");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("locators")]   = QJsonArray{ loc };
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 1)
        << "flip_batch must resolve the emoji bullet in a mixed file; skipped="
        << QString::fromUtf8(QJsonDocument(
               resp.value(QStringLiteral("skipped")).toArray())
               .toJson(QJsonDocument::Compact)).toStdString();
    const QJsonArray flipped = resp.value(QStringLiteral("flipped")).toArray();
    ASSERT_EQ(flipped.size(), 1);
    const QJsonObject e = flipped.first().toObject();
    EXPECT_EQ(e.value(QStringLiteral("format")).toString(),
              QStringLiteral("ants-v1"));
    EXPECT_EQ(e.value(QStringLiteral("to_status")).toString(),
              QStringLiteral("✅"));
}

// INV-4 — flip_batch flips a GFM bullet AND the emoji bullet in one commit.
TEST(roadmap_log_mixed_amend_flipbatch, Inv4FlipBatchMixedTargets) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject locG, locV;
    locG[QStringLiteral("id")] = QStringLiteral("G1");
    locV[QStringLiteral("id")] = QStringLiteral("3D_E-0031");
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip_batch");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("locators")]   = QJsonArray{ locG, locV };
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 2)
        << "both a GFM and an ants-v1 bullet must flip in one batch; skipped="
        << QString::fromUtf8(QJsonDocument(
               resp.value(QStringLiteral("skipped")).toArray())
               .toJson(QJsonDocument::Compact)).toStdString();
    // Exactly one entry carries the ants-v1 format tag (the emoji bullet);
    // the GFM entry carries none.
    const QJsonArray flipped = resp.value(QStringLiteral("flipped")).toArray();
    int v1Count = 0, gfmCount = 0;
    for (const QJsonValue &v : flipped) {
        if (v.toObject().value(QStringLiteral("format")).toString() ==
            QStringLiteral("ants-v1")) ++v1Count;
        else ++gfmCount;
    }
    EXPECT_EQ(v1Count, 1);
    EXPECT_EQ(gfmCount, 1);
    // Both bullets are now shipped in the file.
    const QString after = readAll(roadmapPath(tmp.path()));
    EXPECT_TRUE(after.contains(QStringLiteral("[x] **G1.**")));
    EXPECT_TRUE(after.contains(QString::fromUtf8("\xE2\x9C\x85 [3D_E-0031]")));
}

// INV-5 (regression) — the GFM paths are unchanged: amend_body still patches a
// GFM bullet's body, and the fallback never shadows a real GFM hit.
TEST(roadmap_log_mixed_amend_flipbatch, Inv5GfmPathsIntact) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("amend_body");
    req[QStringLiteral("id")]         = QStringLiteral("G1");
    req[QStringLiteral("old_text")]   = QStringLiteral("REPLACE_ME_GFM");
    req[QStringLiteral("new_text")]   = QStringLiteral("PATCHED_GFM");
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(req).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "a GFM bullet body must still patch; got code="
        << resp.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(),
              QStringLiteral("gfm"));
    EXPECT_TRUE(readAll(roadmapPath(tmp.path()))
                    .contains(QStringLiteral("PATCHED_GFM")));
}

// INV-6 (regression) — a genuinely-absent id manufactures no match: amend_body
// returns non-ok; flip_batch lands the locator in skipped[] as bullet_not_found.
TEST(roadmap_log_mixed_amend_flipbatch, Inv6AbsentIdFails) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject amendReq;
    amendReq[QStringLiteral("caller_cwd")] = tmp.path();
    amendReq[QStringLiteral("op")]         = QStringLiteral("amend_body");
    amendReq[QStringLiteral("id")]         = QStringLiteral("3D_E-9999");
    amendReq[QStringLiteral("old_text")]   = QStringLiteral("REPLACE_ME_V1");
    amendReq[QStringLiteral("new_text")]   = QStringLiteral("NOPE");
    const QJsonObject amendResp =
        rc.cmdRoadmapLogAmendBodyForTest(amendReq).object();
    EXPECT_FALSE(amendResp.value(QStringLiteral("ok")).toBool());

    QJsonObject loc;
    loc[QStringLiteral("id")] = QStringLiteral("3D_E-9999");
    QJsonObject fbReq;
    fbReq[QStringLiteral("caller_cwd")] = tmp.path();
    fbReq[QStringLiteral("op")]         = QStringLiteral("flip_batch");
    fbReq[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    fbReq[QStringLiteral("locators")]   = QJsonArray{ loc };
    const QJsonObject fbResp = rc.cmdRoadmapLogFlipBatchForTest(fbReq).object();
    EXPECT_EQ(fbResp.value(QStringLiteral("flipped_count")).toInt(), 0);
    const QJsonArray skipped = fbResp.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 1);
    EXPECT_EQ(skipped.first().toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
}
