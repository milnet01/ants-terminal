// Feature-conformance test for ANTS-4556 + ANTS-4574 — roadmap_log refusals
// that name a route forward. Contract:
// tests/features/roadmap_log_refusal_candidates/spec.md
//
// Behavioural, against a real ROADMAP.md in a QTemporaryDir driven through the
// …ForTest seams. The subject is what the refusal ENVELOPE carries, so a grep
// over the message string would assert only that it looks like itself.

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "remotecontrol.h"

namespace {

// An ants-v1 roadmap whose sections include a NEAR MISS for "performance":
// the reported case is a caller who half-remembers the title and gets the
// trailing number wrong. `alpha-tooling` is the distractor that must not
// outrank it.
QString roadmapWithSections() {
    return QStringLiteral(
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Test Roadmap\n"
        "\n"
        "## Alpha Tooling\n"
        "\n"
        "- 📋 [ANTS-9001] **A bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n"
        "## Performance 2\n"
        "\n"
        "- 📋 [ANTS-9002] **Another bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n");
}

// A GFM task-list roadmap with TWO bullets leading on the same bold span, and
// no caret anchor on either — the shape ANTS-4574 measured. Both bullets carry
// the id `Photo mode`, so an id locator is the ambiguous one.
QString roadmapDuplicateBoldId() {
    return QStringLiteral(
        "# Test Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- [ ] **Photo mode** — free-camera capture.\n"
        "- [ ] **Photo mode** — the UI half.\n"
        "- [ ] **Audio pass** — unrelated, so the file is not all one id.\n");
}

void writeRoadmap(const QString &dir, const QString &content) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(content.toUtf8());
    f.close();
}

QString readRoadmap(const QString &dir) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QJsonObject appendReq(const QString &dir, const QString &section) {
    QJsonObject r;
    r["caller_cwd"] = dir;
    r["op"]         = QStringLiteral("append");
    r["section"]    = section;
    r["status"]     = QStringLiteral("planned");
    r["headline"]   = QStringLiteral("A newly filed bullet.");
    r["kind"]       = QStringLiteral("implement");
    r["source"]     = QStringLiteral("test");
    return r;
}

QStringList candidatesOf(const QJsonObject &env) {
    QStringList out;
    for (const auto v : env.value(QStringLiteral("candidates")).toArray())
        out << v.toString();
    return out;
}

}  // namespace

// ANTS-4556 — an unknown slug that is a NEAR MISS surfaces the real slug
// first. Word overlap is the primary key, so `performance` must rank
// `performance-2` above the unrelated section.
TEST(RoadmapLogRefusalCandidates, BadSectionCarriesRankedCandidates) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeRoadmap(dir.path(), roadmapWithSections());
    RemoteControl rc(nullptr);

    const auto env =
        rc.cmdRoadmapLogAppendForTest(appendReq(dir.path(), QStringLiteral("performance")))
            .object();
    ASSERT_EQ(env["code"].toString(), QStringLiteral("bad_section"));
    const QStringList cand = candidatesOf(env);
    ASSERT_FALSE(cand.isEmpty()) << "bad_section refused with no route forward";
    EXPECT_EQ(cand.first(), QStringLiteral("performance-2"))
        << "the near miss did not rank first: " << cand.join(QStringLiteral(", ")).toStdString();
}

// ANTS-4556 — the cap. A refusal body that dumps every slug costs more than
// the section_index call it is meant to save.
TEST(RoadmapLogRefusalCandidates, BadSectionCandidatesAreCapped) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString md = QStringLiteral("<!-- ants-roadmap-format: 1 -->\n\n# Test Roadmap\n\n");
    for (int i = 0; i < 40; ++i) {
        md += QStringLiteral("## Section %1\n\n"
                             "- 📋 [ANTS-90%2] **A bullet.**\n"
                             "  Kind: implement.\n"
                             "  Source: test.\n\n")
                  .arg(i).arg(i, 2, 10, QLatin1Char('0'));
    }
    writeRoadmap(dir.path(), md);
    RemoteControl rc(nullptr);

    const auto env =
        rc.cmdRoadmapLogAppendForTest(appendReq(dir.path(), QStringLiteral("nowhere-at-all")))
            .object();
    ASSERT_EQ(env["code"].toString(), QStringLiteral("bad_section"));
    EXPECT_LE(candidatesOf(env).size(), 10)
        << "the candidate list is uncapped, so a typo pays a full slug dump";
}

// ANTS-4556 — candidates are a HINT, never a resolution. Guessing which
// section the caller meant is how a 2.4 KB body lands in the wrong place.
TEST(RoadmapLogRefusalCandidates, BadSectionStillRefuses) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeRoadmap(dir.path(), roadmapWithSections());
    const QString before = readRoadmap(dir.path());
    RemoteControl rc(nullptr);

    const auto env =
        rc.cmdRoadmapLogAppendForTest(appendReq(dir.path(), QStringLiteral("performance")))
            .object();
    EXPECT_FALSE(env["ok"].toBool());
    EXPECT_EQ(readRoadmap(dir.path()), before)
        << "a near miss was resolved and written instead of refused";
}

// ANTS-4556 — a pure case mismatch has an EXACT answer (ANTS-1524), so it must
// keep returning bad_case + canonical_slug rather than being downgraded to a
// ranked guess by the new path.
TEST(RoadmapLogRefusalCandidates, BadCaseStillWinsOverCandidates) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeRoadmap(dir.path(), roadmapWithSections());
    RemoteControl rc(nullptr);

    const auto env =
        rc.cmdRoadmapLogAppendForTest(appendReq(dir.path(), QStringLiteral("Performance-2")))
            .object();
    EXPECT_EQ(env["code"].toString(), QStringLiteral("bad_case"));
    EXPECT_EQ(env["canonical_slug"].toString(), QStringLiteral("performance-2"));
}

// ANTS-4574 — when the AMBIGUOUS locator was the id, the advice must not be
// "narrow with anchor or id": the id is what failed, and these bullets carry
// no anchor, so both routes it names are closed.
TEST(RoadmapLogRefusalCandidates, AmbiguousIdNamesTheHeadlineRoute) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeRoadmap(dir.path(), roadmapDuplicateBoldId());
    RemoteControl rc(nullptr);

    QJsonObject req;
    req["caller_cwd"] = dir.path();
    req["op"]         = QStringLiteral("flip");
    req["id"]         = QStringLiteral("Photo mode");
    req["to_status"]  = QStringLiteral("shipped");

    const auto env = rc.cmdRoadmapLogFlipForTest(req).object();
    ASSERT_EQ(env["code"].toString(), QStringLiteral("bullet_ambiguous"));
    const QString err = env["error"].toString();
    EXPECT_FALSE(err.contains(QStringLiteral("narrow with anchor or id")))
        << "the refusal still names the locator that failed: " << err.toStdString();
    EXPECT_TRUE(err.contains(QStringLiteral("headline")))
        << "no open route named: " << err.toStdString();
    // The route it names has to be usable, so the headlines must be in hand.
    EXPECT_FALSE(env["suggestions"].toArray().isEmpty())
        << "headline route named with no headlines supplied";
}
