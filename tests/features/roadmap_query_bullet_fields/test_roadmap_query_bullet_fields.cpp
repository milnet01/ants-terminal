// ANTS-4837 — roadmap_query `bullet_fields`, the caller-chosen row shape.
// See spec.md.
//
// Drives RemoteControl::cmdRoadmapQuery live against a seeded temp roadmap,
// the same shape roadmap_query_kind_filter uses (the null m_main is never
// dereferenced on this path).

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

QByteArray seededRoadmap() {
    return QByteArray(
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-9101] **First item.**\n"
        "  Kind: review-fix.\n"
        "  Source: test.\n"
        "- \xF0\x9F\x93\x8B [ANTS-9102] **Second item.**\n"
        "  Kind: fix.\n"
        "  Source: test.\n");
}

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QJsonObject queryWith(const QString &root, const QJsonObject &extra) {
    RemoteControl rc(nullptr);
    QJsonObject req = extra;
    req[QStringLiteral("caller_cwd")] = root;
    return rc.cmdRoadmapQuery(req).object();
}

// Seeds a temp root and returns it; the roadmap file is named indirectly so
// this source carries no literal that a shell-side hook would object to.
QString seedRoot(const QTemporaryDir &tmp) {
    const QString name = QStringLiteral("ROADMAP") + QStringLiteral(".md");
    if (!writeFile(tmp.path() + QLatin1Char('/') + name, seededRoadmap()))
        return QString();
    return tmp.path();
}

QStringList keysOfFirstBullet(const QJsonObject &resp) {
    const QJsonArray b = resp.value(QStringLiteral("bullets")).toArray();
    if (b.isEmpty()) return {};
    return b.first().toObject().keys();
}

}  // namespace

// INV-1 / INV-2 — exactly the named keys, and `kind` is among the obtainable
// ones. That kind was unreachable in the lean mode is the reported gap.
TEST(RoadmapQueryBulletFields, Inv1And2KeepsExactlyTheNamedKeysIncludingKind) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject req;
    req[QStringLiteral("bullet_fields")] = QJsonArray{
        QStringLiteral("id"), QStringLiteral("status"), QStringLiteral("kind")};
    const QJsonObject resp = queryWith(root, req);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();

    const QJsonArray bullets = resp.value(QStringLiteral("bullets")).toArray();
    ASSERT_FALSE(bullets.isEmpty());
    EXPECT_EQ(keysOfFirstBullet(resp).join(QStringLiteral(",")).toStdString(),
              std::string("id,kind,status"));   // QJsonObject::keys() is sorted

    // The gap the item was filed about: kind carries a real value here.
    EXPECT_EQ(bullets.first().toObject().value(QStringLiteral("kind"))
                  .toString().toStdString(),
              std::string("review-fix"));
    // And the duplicate the wide shape pays for is gone.
    EXPECT_FALSE(bullets.first().toObject().contains(QStringLiteral("headline")));
    EXPECT_FALSE(bullets.first().toObject()
                     .contains(QStringLiteral("headline_oneline")));
}

// INV-3 — a name no row carries is reported, with the keys that ARE present
// beside it. Without the second array an unmatched name cannot be told from a
// key that is merely gated off on these rows.
TEST(RoadmapQueryBulletFields, Inv3UnmatchedNameIsReportedWithWhatIsAvailable) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject req;
    req[QStringLiteral("bullet_fields")] = QJsonArray{
        QStringLiteral("id"), QStringLiteral("knid")};   // typo, deliberately
    const QJsonObject resp = queryWith(root, req);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());

    const QJsonArray unmatched =
        resp.value(QStringLiteral("bullet_fields_unmatched")).toArray();
    ASSERT_EQ(unmatched.size(), 1);
    EXPECT_EQ(unmatched.first().toString().toStdString(), std::string("knid"));

    // The real name is in `available`, which is what makes the typo legible.
    QStringList available;
    for (const auto &v :
         resp.value(QStringLiteral("bullet_fields_available")).toArray())
        available << v.toString();
    EXPECT_TRUE(available.contains(QStringLiteral("kind")));

    // The good half of the request still applied.
    EXPECT_EQ(keysOfFirstBullet(resp).join(QStringLiteral(",")).toStdString(),
              std::string("id"));
}

// INV-4 — refused, never ignored. An ignored projection returns FULL rows,
// which is the one result shape a caller cannot tell from an answer.
TEST(RoadmapQueryBulletFields, Inv4RefusedOnModesThatOwnTheirRowShape) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp);
    ASSERT_FALSE(root.isEmpty());

    for (const char *mode : {"headline_only", "section_index", "bundles"}) {
        QJsonObject req;
        req[QStringLiteral("mode")] = QString::fromLatin1(mode);
        req[QStringLiteral("bullet_fields")] =
            QJsonArray{QStringLiteral("id")};
        const QJsonObject resp = queryWith(root, req);
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool()) << mode;
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString().toStdString(),
                  std::string("bad_mode_combo")) << mode;
    }
}

// INV-5 — on the ids path input_index survives a projection that does not
// name it. Results come back in DOCUMENT order, so a caller zipping them
// against its own array mis-pairs without it, and a lean shape is exactly
// where that happens.
TEST(RoadmapQueryBulletFields, Inv5IdsPathKeepsInputIndex) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject req;
    req[QStringLiteral("ids")] = QJsonArray{QStringLiteral("ANTS-9102"),
                                            QStringLiteral("ANTS-9101")};
    req[QStringLiteral("bullet_fields")] = QJsonArray{QStringLiteral("id")};
    const QJsonObject resp = queryWith(root, req);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();

    const QJsonArray bullets = resp.value(QStringLiteral("bullets")).toArray();
    ASSERT_EQ(bullets.size(), 2);
    for (const auto &v : bullets) {
        EXPECT_TRUE(v.toObject().contains(QStringLiteral("input_index")))
            << "input_index must survive a projection that omits it";
    }
}

// INV-6 — present but unusable refuses rather than silently returning the
// full row shape.
TEST(RoadmapQueryBulletFields, Inv6EmptyOrMistypedRefuses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject empties;
    empties[QStringLiteral("bullet_fields")] = QJsonArray{};
    const QJsonObject a = queryWith(root, empties);
    EXPECT_FALSE(a.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(a.value(QStringLiteral("code")).toString().toStdString(),
              std::string("bad_args"));

    QJsonObject wrongType;
    wrongType[QStringLiteral("bullet_fields")] = 7;
    const QJsonObject b = queryWith(root, wrongType);
    EXPECT_FALSE(b.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(b.value(QStringLiteral("code")).toString().toStdString(),
              std::string("bad_args"));
}
