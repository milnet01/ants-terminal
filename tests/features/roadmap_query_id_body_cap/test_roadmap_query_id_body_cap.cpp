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

// ANTS-4362 — the id/ids paths return bodies WITHOUT include_body while a
// status- or section-filtered query strips them unless asked. That asymmetry
// is deliberate (a targeted fetch is a handful of bullets, a status filter can
// match the whole roadmap) but it was SILENT, so a filtered reply read as
// though those bullets simply had no bodies — and orienting on a queue is
// exactly the filtered shape. Games_Hub picked between two candidates on
// headlines alone because of it.
TEST(roadmap_query_id_body_cap, Ants4362FilteredQuerySaysBodiesWereWithheld) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapWithLongBody()));
    RemoteControl rc(nullptr);

    // (a) filtered, no include_body → bodies withheld, and it SAYS so.
    QJsonObject filtered;
    filtered[QStringLiteral("caller_cwd")] = tmp.path();
    filtered[QStringLiteral("status")]     = QStringLiteral("all");
    const QJsonObject fr = rc.cmdRoadmapQuery(filtered).object();
    ASSERT_TRUE(fr.value(QStringLiteral("ok")).toBool());
    ASSERT_FALSE(fr.value(QStringLiteral("bullets")).toArray().isEmpty());
    EXPECT_TRUE(fr.value(QStringLiteral("bodies_omitted")).toBool())
        << "a filtered query that withheld bodies must say so";
    EXPECT_TRUE(fr.value(QStringLiteral("bodies_omitted_reason"))
                    .toString().contains(QStringLiteral("include_body")))
        << "and name the argument that returns them";

    // (b) the same filter WITH include_body → no marker, bodies present.
    filtered[QStringLiteral("include_body")] = true;
    const QJsonObject wr = rc.cmdRoadmapQuery(filtered).object();
    EXPECT_FALSE(wr.contains(QStringLiteral("bodies_omitted")))
        << "nothing was withheld, so nothing should be claimed";

    // (c) an id fetch never withholds, so it must not carry the marker
    // either — the marker means "withheld", not "this is a list".
    QJsonObject byId;
    byId[QStringLiteral("caller_cwd")] = tmp.path();
    byId[QStringLiteral("id")]         = QStringLiteral("ANTS-7777");
    EXPECT_FALSE(rc.cmdRoadmapQuery(byId).object()
                     .contains(QStringLiteral("bodies_omitted")));
}

// ANTS-4400 — each bullet carries `input_index`, its position in the `ids`
// array the caller sent.
//
// Results are in DOCUMENT order, which is documented and correct — but the
// natural use of `ids` is "compare these two, in the order I care about", and
// a caller who zips the result against its own input array silently
// mis-pairs. Fetching FIBR-0249 and FIBR-0248 returned 0248 first because it
// sits earlier in the file.
//
// The cheaper of the two offered fixes: no `order:"input"` mode, just the
// position, so a caller restores its ordering without a dict comprehension.
TEST(roadmap_query_id_body_cap, Ants4400IdsCarryInputIndex) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QByteArray md =
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0001] **Earlier in the file.**\n"
        "  Kind: feature.\n"
        "  Source: test.\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0002] **Later in the file.**\n"
        "  Kind: feature.\n"
        "  Source: test.\n";
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), md));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    // Asked for in REVERSE document order — the shape that mis-pairs.
    QJsonArray ids;
    ids.append(QStringLiteral("ANTS-0002"));
    ids.append(QStringLiteral("ANTS-0001"));
    req[QStringLiteral("ids")] = ids;
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();

    const QJsonArray bullets = resp.value(QStringLiteral("bullets")).toArray();
    ASSERT_EQ(bullets.size(), 2);
    // Document order is unchanged — the fix adds a field, it does not reorder.
    EXPECT_EQ(bullets.at(0).toObject().value(QStringLiteral("id")).toString(),
              QStringLiteral("ANTS-0001"))
        << "results stay in DOCUMENT order; that behaviour is deliberate";
    // …and each bullet says where the CALLER asked for it.
    EXPECT_EQ(bullets.at(0).toObject().value(QStringLiteral("input_index")).toInt(-1), 1);
    EXPECT_EQ(bullets.at(1).toObject().value(QStringLiteral("input_index")).toInt(-1), 0)
        << "without this a caller zipping bullets[] against its own ids[] "
           "pairs each bullet with the wrong request";
}

// ---------------------------------------------------------------------------
// ANTS-4630 — the MIDDLE of a body past the store cap must be reachable.
//
// ANTS-3736 keeps the head and the tail and drops the middle; ANTS-4091 gave
// the marker a remedy to name. But the elision fires at CACHE BUILD, at the
// 16 KiB store cap, and `max_body_bytes` clamped to that same 16 KiB — so for
// any body over the cap the named remedy could not reach past the wall that
// produced it. The spill did not rescue it either: the body is elided before
// the response is serialised, so read_spill pages already-elided text (the
// marker itself was measured sitting INSIDE the spilled payload).
//
// A body whose middle is a sentinel proves the whole span is reachable, where
// the head/tail sentinels only ever proved the two ends were.

namespace {

// ~51 KiB body carrying three positional sentinels. MID sits past the head
// the elision retains and before its 1 KiB tail — inside the elided span.
QByteArray roadmapWithMiddleSentinelBody(int lines) {
    QByteArray md =
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-7777] **Long-body epic.**\n"
        "  HEADSENTINEL this epic converts the level mesh.\n";
    for (int i = 0; i < lines; ++i) {
        if (i == lines / 2)
            md += "  MIDSENTINEL the resume plan for phase L1c lives here.\n";
        md += "  Lorem ipsum dolor sit amet consectetur adipiscing.\n";
    }
    md += "  TAILSENTINEL current status phase L1d has shipped.\n";
    md += "  Kind: feature.\n";
    md += "  Source: test.\n";
    return md;
}

}  // namespace

// INV-9 (ANTS-4630) — a single-id fetch may raise max_body_bytes past the
// 16 KiB store cap, and the middle of an oversized body comes back with it.
TEST(roadmap_query_id_body_cap, Inv9SingleIdReachesTheMiddleOfALongBody) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()),
                          roadmapWithMiddleSentinelBody(1000)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")]     = tmp.path();
    req[QStringLiteral("id")]             = QStringLiteral("ANTS-7777");
    req[QStringLiteral("include_body")]   = true;
    req[QStringLiteral("max_body_bytes")] = 200000;   // past the old ceiling
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const QString body =
        bodyForId(resp.value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));

    EXPECT_GT(body.size(), 16384)
        << "a single-id fetch must be able to exceed the 16 KiB store cap";
    EXPECT_TRUE(body.contains(QStringLiteral("HEADSENTINEL")));
    EXPECT_TRUE(body.contains(QStringLiteral("MIDSENTINEL")))
        << "the elided MIDDLE is the whole defect — it must be reachable";
    EXPECT_TRUE(body.contains(QStringLiteral("TAILSENTINEL")));
    EXPECT_FALSE(body.contains(QStringLiteral("[body elided")))
        << "a cap above the body length must not elide at all";
}

// INV-10 (ANTS-4630) — the lift is scoped to a SINGLE id. A wide ids[] fetch
// keeps its ceiling, so one call cannot pull N oversized bodies inline.
TEST(roadmap_query_id_body_cap, Inv10WideIdsFetchKeepsItsCeiling) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()), roadmapWithNLongBullets(3)));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    QJsonArray ids;
    for (int i = 0; i < 3; ++i) ids.append(QStringLiteral("ANTS-%1").arg(7800 + i));
    req[QStringLiteral("ids")]            = ids;
    req[QStringLiteral("include_body")]   = true;
    req[QStringLiteral("max_body_bytes")] = 200000;
    const QJsonObject resp = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    for (const auto &v : resp.value(QStringLiteral("bullets")).toArray()) {
        EXPECT_LE(v.toObject().value(QStringLiteral("body")).toString().size(),
                  16384)
            << "the raised ceiling is single-id only";
    }
}

// INV-11 (ANTS-4630) — the marker must name a remedy that works AT THE SIZE
// THAT TRIGGERED IT. Naming max_body_bytes was true only below the store cap;
// above it the argument was clamped to less than the body.
TEST(roadmap_query_id_body_cap, Inv11MarkerRemedyWorksAtTheTriggeringSize) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(rmPath(tmp.path()),
                          roadmapWithMiddleSentinelBody(1000)));
    RemoteControl rc(nullptr);

    // The list path elides and names a remedy.
    QJsonObject list;
    list[QStringLiteral("caller_cwd")]   = tmp.path();
    list[QStringLiteral("include_body")] = true;
    const QJsonObject listResp = rc.cmdRoadmapQuery(list).object();
    const QString elided =
        bodyForId(listResp.value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));
    ASSERT_TRUE(elided.contains(QStringLiteral("[body elided")))
        << "a body this size must still elide on the list path";

    // Following that remedy on this very bullet must return the whole body.
    QJsonObject targeted;
    targeted[QStringLiteral("caller_cwd")]     = tmp.path();
    targeted[QStringLiteral("id")]             = QStringLiteral("ANTS-7777");
    targeted[QStringLiteral("include_body")]   = true;
    targeted[QStringLiteral("max_body_bytes")] = 200000;
    const QString full =
        bodyForId(rc.cmdRoadmapQuery(targeted).object()
                      .value(QStringLiteral("bullets")).toArray(),
                  QStringLiteral("ANTS-7777"));
    EXPECT_TRUE(full.contains(QStringLiteral("MIDSENTINEL")))
        << "the marker's own advice must reach the span the marker replaced";
}
