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

// ANTS-3736 fixture — a body whose FIRST and LAST prose lines are distinct
// sentinels, so a test can prove which end(s) of the body survived the cap.
// `lines` sets the size: 60 clears the 2000 list cap, 400 clears the 16384
// store cap (the case max_body_bytes could never reach before the fix).
QByteArray roadmapWithSentinelBody(int lines) {
    QByteArray md =
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-7777] **Long-body epic.**\n"
        "  HEADSENTINEL this epic converts the level mesh.\n";
    for (int i = 0; i < lines; ++i) {
        md += "  Lorem ipsum dolor sit amet consectetur adipiscing.\n";
    }
    md += "  TAILSENTINEL current status phase L1d has shipped.\n";
    md += "  Kind: feature.\n";
    md += "  Source: test.\n";
    return md;
}

// ANTS-4091 fixture — N long-bodied bullets (ANTS-7800…), so a wide ids[]
// fetch can prove the scaled default still falls back to the 2000 floor.
QByteArray roadmapWithNLongBullets(int n) {
    QByteArray md = "# Roadmap\n\n## Work\n\n";
    for (int b = 0; b < n; ++b) {
        md += "- \xF0\x9F\x93\x8B [ANTS-";
        md += QByteArray::number(7800 + b);
        md += "] **Long-body epic.**\n";
        for (int i = 0; i < 60; ++i) {
            md += "  Lorem ipsum dolor sit amet consectetur adipiscing.\n";
        }
        md += "  Kind: feature.\n";
        md += "  Source: test.\n";
    }
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

// INV-4 (ANTS-3736) — a body truncated at the 2000 list cap keeps BOTH ends.
// On an append-only progress-log body the head says what the item is and the
// tail says where it currently stands; head-only truncation silently served
// the OLDEST text as the answer to "what is the state of this?".
TEST(roadmap_query_id_body_cap, Inv4TruncatedBodyKeepsHeadAndTail) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapWithSentinelBody(60)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]   = tmp.path();
    req[QStringLiteral("include_body")] = true;
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const QString body =
        bodyForId(resp.value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));
    EXPECT_EQ(body.size(), 2000) << "elided body must land exactly on the cap";
    EXPECT_TRUE(body.contains(QStringLiteral("HEADSENTINEL")))
        << "head must survive truncation";
    EXPECT_TRUE(body.contains(QStringLiteral("TAILSENTINEL")))
        << "tail must survive truncation — it carries the CURRENT status";
    EXPECT_TRUE(body.contains(QStringLiteral("[body elided")))
        << "the elision must be explicit, not a silent join";
}

// INV-5 (ANTS-3736) — the fix must apply at the CACHE truncation site too.
// A body past the 16384 store cap lost its tail before any emission cap ran,
// so no max_body_bytes setting could reach it — the reporter's "unreachable
// at any setting". Ceiling fetch on an oversized body must still show the tail.
TEST(roadmap_query_id_body_cap, Inv5TailSurvivesTheStoreCap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapWithSentinelBody(400)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]     = tmp.path();
    req[QStringLiteral("id")]             = QStringLiteral("ANTS-7777");
    req[QStringLiteral("include_body")]   = true;
    req[QStringLiteral("max_body_bytes")] = 16384;
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const QString body =
        bodyForId(resp.value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));
    EXPECT_GT(body.size(), 2000);
    EXPECT_TRUE(body.contains(QStringLiteral("TAILSENTINEL")))
        << "a body over the store cap must still surface its tail";
}

// INV-6 (ANTS-4091) — a targeted fetch's DEFAULT cap scales with the id
// count, so a single-id fetch of a body under the store cap returns it
// WHOLE. Head+tail elision keeps both ends and drops the MIDDLE — where a
// bullet's resume plan sits — and on a targeted fetch that cost is
// avoidable: the caller has already narrowed the payload to the ids named.
TEST(roadmap_query_id_body_cap, Inv6TargetedDefaultReturnsWholeBody) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapWithSentinelBody(60)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]   = tmp.path();
    req[QStringLiteral("id")]           = QStringLiteral("ANTS-7777");
    req[QStringLiteral("include_body")] = true;   // NO max_body_bytes
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const QString body =
        bodyForId(resp.value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));
    EXPECT_GT(body.size(), 2000)
        << "an unqualified id fetch must not fall back to the 2000 list cap";
    EXPECT_FALSE(body.contains(QStringLiteral("[body elided")))
        << "a ~3 K body is under the store cap — nothing should be elided";
    EXPECT_TRUE(body.contains(QStringLiteral("HEADSENTINEL")));
    EXPECT_TRUE(body.contains(QStringLiteral("TAILSENTINEL")));

    // A one-element ids[] is the same targeted fetch (n == 1).
    QJsonObject req2;
    req2[QStringLiteral("caller_cwd")]   = tmp.path();
    QJsonArray ids;
    ids.append(QStringLiteral("ANTS-7777"));
    req2[QStringLiteral("ids")]          = ids;
    req2[QStringLiteral("include_body")] = true;
    const QJsonObject resp2 = rc.cmdRoadmapQuery(req2).object();
    ASSERT_TRUE(resp2.value(QStringLiteral("ok")).toBool());
    const QString body2 =
        bodyForId(resp2.value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));
    EXPECT_EQ(body2, body) << "ids:[X] must behave as id:X";
}

// INV-7 (ANTS-4091) — the `/n` divisor is what makes INV-6 safe: a 9-id
// fetch (16384 / 9 < 2000) falls to the 2000 floor, so the targeted path's
// total body payload stays bounded however many ids are named.
TEST(roadmap_query_id_body_cap, Inv7ScaledDefaultKeepsPayloadBounded) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapWithNLongBullets(9)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    QJsonArray ids;
    for (int i = 0; i < 9; ++i) {
        ids.append(QStringLiteral("ANTS-%1").arg(7800 + i));
    }
    req[QStringLiteral("ids")]          = ids;
    req[QStringLiteral("include_body")] = true;   // NO max_body_bytes
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const QJsonArray bullets = resp.value(QStringLiteral("bullets")).toArray();
    ASSERT_EQ(bullets.size(), 9);
    for (const auto &v : bullets) {
        const QJsonObject o = v.toObject();
        EXPECT_EQ(o.value(QStringLiteral("body")).toString().size(), 2000)
            << "a 9-id fetch must emit at the 2000 floor, not the store cap";
        EXPECT_TRUE(o.value(QStringLiteral("body_truncated")).toBool());
    }
}

// INV-8 (ANTS-4091) — the elision marker names the remedy. ANTS-3736's
// marker said text was elided but not how to read it; it must stay
// count-free so it is byte-stable across calls.
TEST(roadmap_query_id_body_cap, Inv8ElisionMarkerNamesTheRemedy) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapWithSentinelBody(60)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]   = tmp.path();
    req[QStringLiteral("include_body")] = true;   // list path — elides at 2000
    const QString body =
        bodyForId(rc.cmdRoadmapQuery(req).object()
                      .value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));

    ASSERT_TRUE(body.contains(QStringLiteral("[body elided")));
    EXPECT_TRUE(body.contains(QStringLiteral("max_body_bytes")))
        << "the marker must name how to read the elided span";

    RemoteControl rc2(nullptr);
    const QString body2 =
        bodyForId(rc2.cmdRoadmapQuery(req).object()
                      .value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));
    EXPECT_EQ(body, body2)
        << "the marker must carry no counts — byte-stable across calls";
}
