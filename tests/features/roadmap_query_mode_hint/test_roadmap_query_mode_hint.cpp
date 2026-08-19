// ANTS-4511 — a bad_mode refusal must name the id/ids route it is
// withholding. Drives RemoteControl::cmdRoadmapQuery live against a seeded
// temp ROADMAP.md (path resolved from caller_cwd, so the null m_main is
// never dereferenced), mirroring roadmap_query_id_body_cap.
//
// Behavioural, NOT a source-scrape, and deliberately so: the sibling
// roadmap_query_id_body_cap exists because ANTS-3402's scrape-only coverage
// let an inert feature ship. A scrape for the hint string would pass on a
// hint built into a branch that never runs.
//
// See tests/features/roadmap_query_mode_hint/spec.md.

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

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

// Minimal ants-v1 roadmap. The mode check refuses before any bullet is
// needed, but seeding one keeps the refusal the only reason to fail.
QByteArray minimalRoadmap() {
    return QByteArray(
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-7777] **A bullet.**\n"
        "  Body.\n");
}

QJsonObject queryWithMode(const QString &root, const QString &mode) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("mode")]       = mode;
    return rc.cmdRoadmapQuery(req).object();
}

}  // namespace

// G1 — "by_id" still refuses, and the refusal now names the real route.
TEST(roadmap_query_mode_hint, Ants4511BadModeNamesTheIdRoute) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")),
                          minimalRoadmap()));

    const QJsonObject resp = queryWithMode(tmp.path(), QStringLiteral("by_id"));

    ASSERT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString().toStdString(),
              std::string("bad_mode"));

    // The accepted[] array is the pre-existing contract (ANTS-3617) and must
    // survive — it says what IS a mode.
    EXPECT_FALSE(resp.value(QStringLiteral("accepted")).toArray().isEmpty())
        << "accepted[] must still list the real modes";

    // The hint is the new half: none of those names says "fetch one item by
    // id", because the route is an ARGUMENT rather than a mode.
    const QString hint = resp.value(QStringLiteral("hint")).toString();
    EXPECT_FALSE(hint.isEmpty()) << "bad_mode must carry a hint";
    EXPECT_TRUE(hint.contains(QStringLiteral("id")))
        << "hint must name the id/ids route, got: " << hint.toStdString();
    EXPECT_TRUE(hint.contains(QStringLiteral("ids")))
        << "hint must name the plural selector too, got: " << hint.toStdString();
}

// G2 — the hint is a property of bad_mode, not of the string "by_id".
// A caller who guesses any other wrong name is in the same position.
TEST(roadmap_query_mode_hint, Ants4511HintIsNotSpecialCasedToByIdSpelling) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")),
                          minimalRoadmap()));

    for (const char *guess : {"by-id", "byId", "single", "item"}) {
        const QJsonObject resp =
            queryWithMode(tmp.path(), QString::fromUtf8(guess));
        ASSERT_FALSE(resp.value(QStringLiteral("ok")).toBool()) << guess;
        EXPECT_FALSE(resp.value(QStringLiteral("hint")).toString().isEmpty())
            << "every bad_mode refusal carries the hint, not just \"by_id\"; "
            << "missing for: " << guess;
    }
}

// G3 — a VALID mode is untouched. The hint must not leak onto a success
// envelope, where it would read as a warning about a call that was fine.
TEST(roadmap_query_mode_hint, Ants4511ValidModeCarriesNoHint) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")),
                          minimalRoadmap()));

    const QJsonObject resp =
        queryWithMode(tmp.path(), QStringLiteral("headline_only"));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("hint")).toString().isEmpty())
        << "a successful query must not carry the bad_mode hint";
}
