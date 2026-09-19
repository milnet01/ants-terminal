// ANTS-5094 — the roadmap section cache holds bullets without bodies.
// See tests/features/roadmap_section_cache_bodies/spec.md.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "remotecontrol.h"

namespace {

// An ants-v1 roadmap, padded past RemoteControl::kRoadmapMinParseableSize.
QByteArray doc() {
    QByteArray b = "# Roadmap\n\n";
    for (int i = 0; i < 30; ++i)
        b += "Padding to clear the minimum-parseable-size gate. \n";
    b += "\n## Section One\n\n"
         "- \xF0\x9F\x93\x8B [TEST-0001] **First headline.**\n"
         "  The body holds zebraword and nothing else.\n"
         "- \xE2\x9C\x85 [TEST-0002] **Second headline.**\n"
         "  Another body.\n";
    return b;
}

struct Fixture {
    QTemporaryDir tmp;
    QString root;
    bool ok = false;
    Fixture() {
        if (!tmp.isValid()) return;
        root = QFileInfo(tmp.path()).canonicalFilePath();
        QFile f(root + QStringLiteral("/ROADMAP.md"));
        if (!f.open(QIODevice::WriteOnly)) return;
        const QByteArray d = doc();
        ok = f.write(d) == d.size();
    }
    QJsonObject req(bool includeBody, const QString &query = {}) const {
        QJsonObject r;
        r[QStringLiteral("caller_cwd")] = root;
        r[QStringLiteral("section")] = QStringLiteral("section-one");
        r[QStringLiteral("status")] = QStringLiteral("all");
        if (includeBody) r[QStringLiteral("include_body")] = true;
        if (!query.isEmpty()) r[QStringLiteral("query")] = query;
        return r;
    }
};

QString bodyOf(const QJsonObject &resp, const QString &id) {
    for (const auto &v : resp.value(QStringLiteral("bullets")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("id")).toString() == id)
            return o.value(QStringLiteral("body")).toString();
    }
    return {};
}

}  // namespace

// INV-1
TEST(RoadmapSectionCacheBodies, Inv1CacheHoldsNoBodies) {
    Fixture fx;
    ASSERT_TRUE(fx.ok);
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapQuery(fx.req(true)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(bodyOf(resp, QStringLiteral("TEST-0001")).contains(
        QStringLiteral("zebraword")))
        << "INV-1: the reply lost its body";
    const QJsonArray cached =
        rc.sectionCacheEntryForTest(QStringLiteral("section-one"));
    ASSERT_FALSE(cached.isEmpty()) << "INV-1: the section was not cached";
    for (const auto &v : cached)
        EXPECT_FALSE(v.toObject().contains(QStringLiteral("body")))
            << "INV-1: a cached bullet carries its body";
}

// INV-2
TEST(RoadmapSectionCacheBodies, Inv2BodiesOnWarmCache) {
    Fixture fx;
    ASSERT_TRUE(fx.ok);
    RemoteControl rc(nullptr);
    rc.cmdRoadmapQuery(fx.req(false));
    const QJsonObject resp = rc.cmdRoadmapQuery(fx.req(true)).object();
    EXPECT_TRUE(bodyOf(resp, QStringLiteral("TEST-0001")).contains(
        QStringLiteral("zebraword")))
        << "INV-2: include_body on a warm cache lost the body";
}

// INV-3
TEST(RoadmapSectionCacheBodies, Inv3BodyQueryOnWarmCache) {
    Fixture fx;
    ASSERT_TRUE(fx.ok);
    RemoteControl rc(nullptr);
    rc.cmdRoadmapQuery(fx.req(false));
    const QJsonObject resp =
        rc.cmdRoadmapQuery(fx.req(false, QStringLiteral("zebraword"))).object();
    const QJsonArray bullets = resp.value(QStringLiteral("bullets")).toArray();
    ASSERT_EQ(bullets.size(), 1) << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(bullets.at(0).toObject().value(QStringLiteral("id")).toString(),
              QStringLiteral("TEST-0001"))
        << "INV-3: a body-text query missed on a warm cache";
}
