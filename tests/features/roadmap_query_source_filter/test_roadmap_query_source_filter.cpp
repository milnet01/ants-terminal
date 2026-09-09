// ANTS-4985 — roadmap_query `source` provenance filter. See spec.md.
//
// Drives RemoteControl::cmdRoadmapQuery live against a seeded temp roadmap,
// the same shape roadmap_query_kind_filter uses (the null m_main is never
// dereferenced on this path).

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
#include <QStringList>
#include <QTemporaryDir>

namespace {

// Deliberately mirrors the real corpus: two live spellings for one
// provenance, a dated suffix on each, and review-derived items whose KIND is
// not review-shaped — which is the whole reason this filter exists.
QByteArray roadmapWithSources() {
    return QByteArray(
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-9101] **Old-spelling review finding.**\n"
        "  Kind: review-fix.\n"
        "  Source: indie-review-2026-05-13.\n"
        "- \xF0\x9F\x93\x8B [ANTS-9102] **New-spelling review finding.**\n"
        "  Kind: fix.\n"
        "  Source: code-quality-review-2026-08-20.\n"
        "- \xF0\x9F\x93\x8B [ANTS-9103] **A review finding that is not a fix.**\n"
        "  Kind: perf.\n"
        "  Source: indie-review-2026-06-04.\n"
        "- \xF0\x9F\x93\x8B [ANTS-9106] **A perf item from elsewhere.**\n"
        "  Kind: perf.\n"
        "  Source: user-request-2026-07-01.\n"
        "- \xF0\x9F\x93\x8B [ANTS-9107] **A bullet with no Source line at all.**\n"
        "  Kind: implement.\n"
        "- \xF0\x9F\x93\x8B [ANTS-9104] **Planned work, no review.**\n"
        "  Kind: implement.\n"
        "  Source: planned.\n"
        "- \xE2\x9C\x85 [ANTS-9105] **A shipped review finding.**\n"
        "  Kind: review-fix.\n"
        "  Source: indie-review-2026-05-13.\n");
}

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QStringList idsOf(const QJsonObject &resp) {
    QStringList out;
    for (const auto &v : resp.value(QStringLiteral("bullets")).toArray())
        out << v.toObject().value(QStringLiteral("id")).toString();
    return out;
}

QJsonObject queryWith(const QString &root, const QJsonObject &extra) {
    RemoteControl rc(nullptr);
    QJsonObject req = extra;
    req[QStringLiteral("caller_cwd")] = root;
    return rc.cmdRoadmapQuery(req).object();
}

QString seed(QTemporaryDir &tmp) {
    EXPECT_TRUE(tmp.isValid());
    EXPECT_TRUE(writeFile(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")),
                          roadmapWithSources()));
    return tmp.path();
}

}  // namespace

// INV-1 — prefix match, case-insensitive, composing with status.
TEST(RoadmapQuerySourceFilter, Inv1PrefixNarrowsAndComposesWithStatus) {
    QTemporaryDir tmp; const QString root = seed(tmp);

    QJsonObject a;
    a[QStringLiteral("source")] = QStringLiteral("indie-review");
    const QJsonObject all = queryWith(root, a);
    ASSERT_TRUE(all.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(all).toJson().toStdString();
    QStringList ids = idsOf(all);
    ids.sort();
    EXPECT_EQ(ids, (QStringList{"ANTS-9101", "ANTS-9103", "ANTS-9105"}))
        << "prefix must match the dated suffixes; got "
        << ids.join(QStringLiteral(",")).toStdString();

    // Case-folded: the filter must not depend on how the caller typed it.
    QJsonObject upper;
    upper[QStringLiteral("source")] = QStringLiteral("INDIE-Review");
    EXPECT_EQ(idsOf(queryWith(root, upper)).size(), 3);

    // The question that motivated the item: which review items are OPEN.
    QJsonObject b;
    b[QStringLiteral("source")] = QStringLiteral("indie-review");
    b[QStringLiteral("status")] = QStringLiteral("active");
    QStringList open = idsOf(queryWith(root, b));
    open.sort();
    EXPECT_EQ(open, (QStringList{"ANTS-9101", "ANTS-9103"}))
        << "the shipped one must drop out";
}

// INV-2 — an array matches on ANY prefix. The standard keeps two live
// spellings for one provenance, so this is required, not convenience.
TEST(RoadmapQuerySourceFilter, Inv2ArrayMatchesAnyPrefix) {
    QTemporaryDir tmp; const QString root = seed(tmp);

    QJsonObject a;
    a[QStringLiteral("source")] = QJsonArray{QStringLiteral("indie-review"),
                                             QStringLiteral("code-quality-review")};
    QStringList ids = idsOf(queryWith(root, a));
    ids.sort();
    EXPECT_EQ(ids, (QStringList{"ANTS-9101", "ANTS-9102", "ANTS-9103", "ANTS-9105"}))
        << "both live review spellings must match in ONE call";
    EXPECT_FALSE(ids.contains(QStringLiteral("ANTS-9104")))
        << "planned work is not review-derived";
}

// INV-3 — provenance and kind are independent axes and intersect.
TEST(RoadmapQuerySourceFilter, Inv3ComposesWithKind) {
    QTemporaryDir tmp; const QString root = seed(tmp);

    // Guard against a vacuous pass: TWO items are Kind: perf, so `kind` alone
    // cannot produce this answer and the source axis has to do real work.
    QJsonObject kindOnly;
    kindOnly[QStringLiteral("kind")] = QStringLiteral("perf");
    QStringList byKind = idsOf(queryWith(root, kindOnly));
    byKind.sort();
    ASSERT_EQ(byKind, (QStringList{"ANTS-9103", "ANTS-9106"}))
        << "fixture must have two perf items or INV-3 proves nothing";

    QJsonObject a;
    a[QStringLiteral("source")] = QStringLiteral("indie-review");
    a[QStringLiteral("kind")]   = QStringLiteral("perf");
    EXPECT_EQ(idsOf(queryWith(root, a)), (QStringList{"ANTS-9103"}))
        << "a review-derived item whose kind is NOT review-shaped is exactly "
           "what kind alone cannot find — 30 of this project's 36 look like "
           "this";
}

// INV-4 — present but empty refuses. Returning everything would read as
// "it all came from there", which is the bad_kind failure mode.
TEST(RoadmapQuerySourceFilter, Inv4EmptyFilterRefusesRatherThanMatchingAll) {
    QTemporaryDir tmp; const QString root = seed(tmp);

    for (const QJsonValue &empty : {QJsonValue(QStringLiteral("")),
                                    QJsonValue(QStringLiteral("   ")),
                                    QJsonValue(QJsonArray{})}) {
        QJsonObject a;
        a[QStringLiteral("source")] = empty;
        const QJsonObject r = queryWith(root, a);
        EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool())
            << "an empty source filter must refuse, not return the full set";
        EXPECT_EQ(r.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_args"));
    }
}

// INV-5 — the echo, so a zero-row answer is readable.
TEST(RoadmapQuerySourceFilter, Inv5EnvelopeEchoesPrefixesAndDropCount) {
    QTemporaryDir tmp; const QString root = seed(tmp);

    QJsonObject a;
    a[QStringLiteral("source")] = QStringLiteral("no-such-provenance");
    const QJsonObject r = queryWith(root, a);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(idsOf(r).isEmpty());
    const QJsonArray echoed = r.value(QStringLiteral("source")).toArray();
    ASSERT_EQ(echoed.size(), 1);
    EXPECT_EQ(echoed.at(0).toString(), QStringLiteral("no-such-provenance"));
    EXPECT_GT(r.value(QStringLiteral("source_filtered_out")).toInt(), 0)
        << "zero rows with a positive drop count says the filter RAN and "
           "matched nothing — an empty list alone cannot say that";
}

// INV-6 — source is a first-class field, like kind and lanes.
TEST(RoadmapQuerySourceFilter, Inv6BulletsCarrySourceAsAField) {
    QTemporaryDir tmp; const QString root = seed(tmp);

    QJsonObject a;
    a[QStringLiteral("id")] = QStringLiteral("ANTS-9103");
    const QJsonObject r = queryWith(root, a);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    const QJsonArray bullets = r.value(QStringLiteral("bullets")).toArray();
    ASSERT_EQ(bullets.size(), 1);
    EXPECT_EQ(bullets.at(0).toObject().value(QStringLiteral("source")).toString(),
              QStringLiteral("indie-review-2026-06-04"))
        << "source must be readable without parsing it back out of `body`";
}

// ANTS-4985 — what a bullet with NO `Source:` line reports. The standard
// states two rules that both reach it: 3.5.3 says such a bullet "reads as"
// Source: planned, and 3.10.2 says missing fields come back as empty
// strings. They prescribe different values on the same read, so the document
// cannot name a winner without knowing which the code does. This is that
// answer, locked.
TEST(RoadmapQuerySourceFilter, Inv7AbsentSourceReportsEmptyNotTheDefault) {
    QTemporaryDir tmp; const QString root = seed(tmp);

    QJsonObject a;
    a[QStringLiteral("id")] = QStringLiteral("ANTS-9107");
    const QJsonObject r = queryWith(root, a);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    const QJsonArray bullets = r.value(QStringLiteral("bullets")).toArray();
    ASSERT_EQ(bullets.size(), 1);
    EXPECT_EQ(bullets.at(0).toObject().value(QStringLiteral("source")).toString(),
              QString())
        << "the envelope reports the ABSENCE; `planned` is a reader-side "
           "classification, not a value the parser invents. If this ever "
           "returns \"planned\", roadmap-format.md 3.10.2 is the passage to "
           "change, not this test.";

    // ...and therefore a source:"planned" filter does NOT collect it.
    QJsonObject f;
    f[QStringLiteral("source")] = QStringLiteral("planned");
    EXPECT_FALSE(idsOf(queryWith(root, f)).contains(QStringLiteral("ANTS-9107")))
        << "a bullet with no Source line must not be swept up by a filter for "
           "the default value — that would make the filter answer a question "
           "about classification rather than about what is written.";
}
