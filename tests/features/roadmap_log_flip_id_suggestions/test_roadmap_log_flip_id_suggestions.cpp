// ANTS-3566 — feature-conformance test: on a bullet_not_found for an ID
// locator, roadmap_log flip/annotate must rank `suggestions[]` by sibling-id
// shared prefix, NOT by headline overlap with the id string. Before the fix an
// id like "3D_E-0031" surfaced every bullet whose HEADLINE merely started with
// "3", while the real same-project siblings (which share no headline text) were
// never suggested.
//
// Drives the cmdRoadmapLogFlipForTest seam behaviourally.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

// >1 KiB pad so a pure ants-v1 file clears kRoadmapMinParseableSize (1024 B)
// — below that gate the flip path never engages the ants-v1 walker.
const char *kPad =
    "Intro paragraph that exists purely to pad the file past the 1 KiB\n"
    "minimum-parseable-size gate. Lorem ipsum dolor sit amet, consectetur\n"
    "adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore\n"
    "magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation\n"
    "ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute\n"
    "irure dolor in reprehenderit in voluptate velit esse cillum dolore eu\n"
    "fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident,\n"
    "sunt in culpa qui officia deserunt mollit anim id est laborum. Sed ut\n"
    "perspiciatis unde omnis iste natus error sit voluptatem accusantium\n"
    "doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo\n"
    "inventore veritatis et quasi architecto beatae vitae dicta explicabo.\n"
    "Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut odit aut\n"
    "fugit, sed quia consequuntur magni dolores eos qui ratione voluptatem\n"
    "sequi nesciunt. Neque porro quisquam est, qui dolorem ipsum quia dolor\n"
    "sit amet consectetur adipisci velit, sed quia non numquam eius modi\n"
    "tempora incidunt ut labore et dolore magnam aliquam quaerat voluptatem.\n";

// Pure ants-v1 file: two `3D_E-` siblings + an unrelated `OTHER-0001` whose
// HEADLINE starts with "3" (the pre-fix false positive). 📋 = U+1F4CB.
QByteArray seedV1() {
    QByteArray b = "# Test Roadmap\n\n";
    b += kPad;
    b += "\n## Work Items\n\n"
         "- \xF0\x9F\x93\x8B [3D_E-0010] **Alpha meadow work.**\n"
         "- \xF0\x9F\x93\x8B [3D_E-0011] **Beta meadow work.**\n"
         "- \xF0\x9F\x93\x8B [OTHER-0001] **3 unrelated decoy task.**\n\n";
    return b;
}

// GFM file: two `AX` bold-ID siblings + an unrelated `ZZ01` whose headline
// starts "ax" (the pre-fix false positive via the shared-prefix tie-breaker).
QByteArray seedGfm() {
    QByteArray b = "# Test Roadmap\n\n";
    b += kPad;
    b += "\n## Work Items\n\n"
         "- [ ] **AX10.** Alpha device work.\n"
         "- [ ] **AX11.** Beta device work.\n"
         "- [ ] **ZZ01.** ax unrelated decoy.\n\n";
    return b;
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

QJsonObject flipById(RemoteControl &rc, const QString &root, const QString &id) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("id")]         = id;
    return rc.cmdRoadmapLogFlipForTest(req).object();
}

}  // namespace

// INV-1 — ants-v1: id-locator miss suggests only same-prefix siblings.
TEST(roadmap_log_flip_id_suggestions, Inv1AntsV1SiblingIds) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        flipById(rc, tmp.path(), QStringLiteral("3D_E-0031"));

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
    const QJsonArray sugg = resp.value(QStringLiteral("suggestions")).toArray();
    ASSERT_GT(sugg.size(), 0)
        << "same-prefix siblings should be suggested for an id miss";
    for (const QJsonValue &v : sugg) {
        const QString sid = v.toObject().value(QStringLiteral("id")).toString();
        EXPECT_TRUE(sid.startsWith(QStringLiteral("3D_E-")))
            << "suggestion id \"" << sid.toStdString()
            << "\" is not a 3D_E- sibling — headline-overlap leak";
        EXPECT_NE(sid, QStringLiteral("OTHER-0001"))
            << "the headline-starts-with-3 decoy must not be suggested";
    }
}

// INV-2 — GFM: id-locator miss suggests only same-prefix bold-ID siblings.
TEST(roadmap_log_flip_id_suggestions, Inv2GfmSiblingIds) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        flipById(rc, tmp.path(), QStringLiteral("AX99"));

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
    const QJsonArray sugg = resp.value(QStringLiteral("suggestions")).toArray();
    ASSERT_GT(sugg.size(), 0)
        << "same-prefix bold-ID siblings should be suggested for an id miss";
    for (const QJsonValue &v : sugg) {
        const QString sid = v.toObject().value(QStringLiteral("id")).toString();
        EXPECT_TRUE(sid.startsWith(QStringLiteral("AX")))
            << "suggestion id \"" << sid.toStdString()
            << "\" is not an AX sibling — headline-overlap leak";
        EXPECT_NE(sid, QStringLiteral("ZZ01"))
            << "the headline-starts-with-ax decoy must not be suggested";
    }
}

// INV-3 (regression) — a headline-locator miss still ranks by token overlap.
TEST(roadmap_log_flip_id_suggestions, Inv3HeadlineRankingUnchanged) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    // Shares the token "alpha" with the AX10 canonical headline.
    req[QStringLiteral("headline")]   =
        QStringLiteral("alpha unknownword zzz");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
    const QJsonArray sugg = resp.value(QStringLiteral("suggestions")).toArray();
    ASSERT_GT(sugg.size(), 0)
        << "a token-overlapping headline miss should still suggest a bullet";
    bool sawAlpha = false;
    for (const QJsonValue &v : sugg) {
        if (v.toObject().value(QStringLiteral("id")).toString() ==
            QStringLiteral("AX10")) sawAlpha = true;
    }
    EXPECT_TRUE(sawAlpha)
        << "the token-overlapping bullet (AX10 / 'Alpha device work') should "
           "still be suggested for a headline locator";
}
