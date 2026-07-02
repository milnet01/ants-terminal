// ANTS-3425 — end-to-end regression: roadmap_query by-id (and ids[])
// must honour max_body_bytes. Drives RemoteControl::cmdRoadmapQuery live
// against a seeded temp ROADMAP.md (path resolved from caller_cwd, so the
// null m_main is never dereferenced), mirroring the changelog_log_writer
// live-drive pattern. ANTS-3402 shipped max_body_bytes but its coverage was
// source-scrape only; the cached body stayed pre-truncated to the 2000
// default, making the raised cap inert. This test locks the behaviour.

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

// ants-v1 roadmap with one planned bullet whose body runs well past the
// 2000-char list cap (~3000 chars of prose across indented continuation
// lines), so a 6000-byte id fetch can return a body strictly longer than
// 2000 only when the cache stores it above the default cap.
QByteArray roadmapWithLongBody() {
    QByteArray md =
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-7777] **Long-body epic.**\n";
    // 60 continuation lines × ~50 chars ≈ 3000-char body.
    for (int i = 0; i < 60; ++i) {
        md += "  Lorem ipsum dolor sit amet consectetur adipiscing.\n";
    }
    md += "  Kind: feature.\n";
    md += "  Source: test.\n";
    return md;
}

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QString rmPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

// Pull the `body` string for a given id out of a bullets[] array.
QString bodyForId(const QJsonArray &bullets, const QString &id) {
    for (const auto &v : bullets) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("id")).toString() == id) {
            return o.value(QStringLiteral("body")).toString();
        }
    }
    return {};
}

}  // namespace

// INV-1 — a single-id fetch with max_body_bytes:6000 returns the fuller body.
TEST(roadmap_query_id_body_cap, Inv1IdHonoursMaxBodyBytes) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapWithLongBody()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]     = tmp.path();
    req[QStringLiteral("id")]             = QStringLiteral("ANTS-7777");
    req[QStringLiteral("max_body_bytes")] = 6000;
    req[QStringLiteral("include_body")]   = true;
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    ASSERT_TRUE(resp.value(QStringLiteral("found")).toBool());
    const QString body =
        bodyForId(resp.value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));
    // Pre-fix: cached body pre-truncated to 2000 → left(6000) is a no-op,
    // so body.size() == 2000. Post-fix: cache stores up to 16384, the id
    // branch emits up to the 6000 cap → the full ~3000-char body.
    EXPECT_GT(body.size(), 2000)
        << "id fetch must honour max_body_bytes (got " << body.size()
        << " chars — the 2000 default cap leaked through)";
}

// INV-2 — the plural ids[] path applies the same raised cap.
TEST(roadmap_query_id_body_cap, Inv2IdsHonoursMaxBodyBytes) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapWithLongBody()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]     = tmp.path();
    QJsonArray ids;
    ids.append(QStringLiteral("ANTS-7777"));
    req[QStringLiteral("ids")]            = ids;
    req[QStringLiteral("max_body_bytes")] = 6000;
    req[QStringLiteral("include_body")]   = true;
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const QString body =
        bodyForId(resp.value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));
    EXPECT_GT(body.size(), 2000)
        << "ids[] fetch must honour max_body_bytes (got " << body.size()
        << " chars)";
}

// INV-3 — a plain list fetch (no id/ids/max_body_bytes) still caps at 2000.
TEST(roadmap_query_id_body_cap, Inv3ListStaysCappedAt2000) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapWithLongBody()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]   = tmp.path();
    req[QStringLiteral("include_body")] = true;
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const QJsonArray bullets = resp.value(QStringLiteral("bullets")).toArray();
    const QString body = bodyForId(bullets, QStringLiteral("ANTS-7777"));
    // List emission re-truncates to the 2000 list cap; the raised store
    // cap is reserved for the targeted id/ids fetch.
    EXPECT_EQ(body.size(), 2000)
        << "list emission must stay at the 2000 cap (got " << body.size()
        << ")";
    for (const auto &v : bullets) {
        if (v.toObject().value(QStringLiteral("id")).toString() ==
            QLatin1String("ANTS-7777")) {
            EXPECT_TRUE(
                v.toObject().value(QStringLiteral("body_truncated")).toBool())
                << "the long-body bullet must be flagged body_truncated in a "
                   "list fetch";
        }
    }
}
