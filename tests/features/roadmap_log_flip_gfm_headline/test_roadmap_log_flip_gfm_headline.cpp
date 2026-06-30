// ANTS-3378 — feature-conformance test for roadmap_log op:"flip" GFM
// headline locator. Behavioural: drives cmdRoadmapLogFlipForTest against
// seeded temp ROADMAPs and asserts the canonical headline roadmap_query
// reports is an accepted locator (pre-fix the raw-head match returned
// bullet_not_found).

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

#include <string>

ANTS_TEST_SCOPE();

namespace {

// GFM seed: a bold-ID + em-dash bullet (Vestige's shape), a bold-only
// bullet, and a plain (no-bold) bullet. \xE2\x80\x94 = em dash.
QByteArray seedGfm() {
    return QByteArray(
        "# Test Roadmap\n\n"
        "## Work Items\n\n"
        "- [ ] **AX11. Audio device hot-swap** \xE2\x80\x94 "
        "automatically re-route audio when devices change\n"
        "- [ ] **AX12. Bold only no dash**\n"
        "- [ ] Plain bullet no bold at all.\n"
        "\n");
}

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

QJsonObject flipReq(const QString &root, const QString &headline) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("op")]         = QStringLiteral("flip");
    o[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    o[QStringLiteral("headline")]   = headline;
    return o;
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — bold-ID + em-dash bullet flips by the post-em-dash prose
// (the canonical headline roadmap_query reports).
TEST(roadmap_log_flip_gfm_headline, Inv1EmDashHeadline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(
        flipReq(tmp.path(),
                QStringLiteral("automatically re-route audio when "
                               "devices change"))).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "em-dash headline should locate the bullet: "
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("flip"));
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "- [x] **AX11. Audio device hot-swap**"));
}

// INV-2 — bold-only bullet flips by the de-marked-up bold span; the `**`
// emphasis markers are not part of the locator token.
TEST(roadmap_log_flip_gfm_headline, Inv2BoldOnlyHeadline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(
        flipReq(tmp.path(),
                QStringLiteral("AX12. Bold only no dash"))).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "bold-only headline should locate the bullet: "
        << QJsonDocument(resp).toJson().toStdString();
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "- [x] **AX12. Bold only no dash**"));
}

// INV-3 — plain (no-bold) bullet still flips by its text (legacy path).
TEST(roadmap_log_flip_gfm_headline, Inv3PlainHeadlineLegacy) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(
        flipReq(tmp.path(),
                QStringLiteral("Plain bullet no bold at all."))).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "- [x] Plain bullet no bold at all."));
}

// INV-4 — a genuine miss returns bullet_not_found; suggestions rank by
// token overlap against the canonical headline and carry a `line`.
TEST(roadmap_log_flip_gfm_headline, Inv4MissSuggestionsCarryLine) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    // Shares the token "audio" with the AX11 canonical headline.
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(
        flipReq(tmp.path(),
                QStringLiteral("audio device unknownword zzz"))).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
    const QJsonArray sugg =
        resp.value(QStringLiteral("suggestions")).toArray();
    ASSERT_GT(sugg.size(), 0)
        << "a token-overlapping miss should suggest the nearest bullet";
    const QJsonObject first = sugg.first().toObject();
    EXPECT_TRUE(first.contains(QStringLiteral("line")))
        << "each suggestion carries the bullet line";
    // The displayed headline is the canonical form (no `**` markup).
    EXPECT_FALSE(first.value(QStringLiteral("headline"))
                     .toString().contains(QStringLiteral("**")));
}
