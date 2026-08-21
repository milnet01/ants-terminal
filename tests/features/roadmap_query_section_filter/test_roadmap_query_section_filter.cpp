// Feature-conformance test for spec.md — ANTS-4610.
//
//   INV-1 query narrows section_index to the matching sections
//   INV-2 it matches a section with NO active work (the measured case)
//   INV-3 slug spelling resolves as well as headline spelling
//   INV-4 zero matches is distinguishable from an empty roadmap
//   INV-5 whole_word / regex compose, as they do on the bullets path
//
// Behavioural: drives RemoteControl::cmdRoadmapQuery. The defect is the
// SIZE of what the caller receives, so only a real envelope can show it.

#include "remotecontrol.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(bytes) == bytes.size();
}

// "Dev experience" holds only SHIPPED work. That is the measured case: the
// reporter's target section had active_count 0, so an active-only flag would
// not have answered it, and only a name filter does.
QByteArray fixture() {
    return QByteArrayLiteral(
        "<!-- ants-roadmap: 1 -->\n"
        "# Roadmap\n\n"
        "## Dev experience\n\n"
        "- \xE2\x9C\x85 [ANTS-0001] **First.**\n"
        "  Kind: fix.\n"
        "  **Layman:** A thing.\n\n"
        "## Packaging and release\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0002] **Second.**\n"
        "  Kind: fix.\n"
        "  **Layman:** A thing.\n\n"
        "## Terminal rendering\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0003] **Third.**\n"
        "  Kind: fix.\n"
        "  **Layman:** A thing.\n");
}

QJsonObject index(const QString &root, const QJsonObject &extra) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("mode")]       = QStringLiteral("section_index");
    req[QStringLiteral("status")]     = QStringLiteral("all");
    for (auto it = extra.begin(); it != extra.end(); ++it)
        req[it.key()] = it.value();
    return rc.cmdRoadmapQuery(req).object();
}

QStringList slugsOf(const QJsonObject &resp) {
    QStringList out;
    for (const QJsonValue &v : resp.value(QLatin1String("sections")).toArray())
        out << v.toObject().value(QStringLiteral("slug")).toString();
    return out;
}

QString seed(QTemporaryDir &tmp) {
    const QString root = tmp.path();
    writeFile(root + QStringLiteral("/ROADMAP.md"), fixture());
    return root;
}

}  // namespace

// INV-1 — the whole point. Measured cost of resolving one slug was 40 section
// objects / ~4.7k tokens; `fields` keeps the whole array (it operates on
// top-level keys) and `compact` drops empty VALUES, not rows.
TEST(roadmap_query_section_filter, Inv1QueryNarrowsTheIndex) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(tmp);

    const QJsonObject all = index(root, {});
    ASSERT_EQ(slugsOf(all).size(), 3) << "fixture should carry 3 sections";

    const QJsonObject one =
        index(root, {{QStringLiteral("query"), QStringLiteral("dev experience")}});
    ASSERT_TRUE(one.value(QStringLiteral("ok")).toBool())
        << "INV-1: query must be accepted, not refused as a bad mode combo";
    EXPECT_EQ(slugsOf(one), QStringList{QStringLiteral("dev-experience")});
}

// INV-2 — the target section holds no active work. An `active_only` flag is
// cheaper and would NOT have answered this; that is why the ask is a name
// filter.
TEST(roadmap_query_section_filter, Inv2MatchesASectionWithNoActiveWork) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(tmp);

    const QJsonObject resp =
        index(root, {{QStringLiteral("query"), QStringLiteral("dev experience")}});
    const QJsonArray secs = resp.value(QStringLiteral("sections")).toArray();
    ASSERT_EQ(secs.size(), 1);
    EXPECT_EQ(secs.at(0).toObject().value(QStringLiteral("active_count")).toInt(), 0)
        << "INV-2: the fixture section must have no active work";
}

// INV-3 — a caller who has seen a slug spells the slug. Both must resolve, or
// the filter answers only the half of callers who happened to guess right.
TEST(roadmap_query_section_filter, Inv3SlugSpellingResolvesToo) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(tmp);

    const QJsonObject resp =
        index(root, {{QStringLiteral("query"), QStringLiteral("dev-experience")}});
    EXPECT_EQ(slugsOf(resp), QStringList{QStringLiteral("dev-experience")});
}

// INV-4 — an unexplained empty list reads as "the roadmap has no sections".
// The counters are what separate "filtered everything out" from "nothing here".
TEST(roadmap_query_section_filter, Inv4ZeroMatchesIsLegible) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(tmp);

    const QJsonObject resp =
        index(root, {{QStringLiteral("query"), QStringLiteral("no such section")}});
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(slugsOf(resp).size(), 0);
    EXPECT_EQ(resp.value(QStringLiteral("query")).toString(),
              QStringLiteral("no such section"))
        << "INV-4: the applied filter must echo";
    EXPECT_EQ(resp.value(QStringLiteral("sections_considered")).toInt(), 3)
        << "INV-4: zero matches must be distinguishable from an empty roadmap";
    EXPECT_EQ(resp.value(QStringLiteral("sections_filtered_out")).toInt(), 3);
}

// INV-5 — the narrowing knobs are the bullets path's, not a second dialect.
// "Dev" appears inside no other heading here, so whole_word proves the
// boundary is applied rather than merely accepted.
TEST(roadmap_query_section_filter, Inv5WholeWordAndRegexCompose) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(tmp);

    const QJsonObject ww = index(root, {
        {QStringLiteral("query"), QStringLiteral("dev")},
        {QStringLiteral("whole_word"), true}});
    EXPECT_EQ(slugsOf(ww), QStringList{QStringLiteral("dev-experience")});

    const QJsonObject rx = index(root, {
        {QStringLiteral("query"), QStringLiteral("^(packaging|terminal)")},
        {QStringLiteral("regex"), true}});
    EXPECT_EQ(slugsOf(rx).size(), 2) << "INV-5: regex must apply to sections";
}
