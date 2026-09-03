// Feature-conformance test for ANTS-4849 — `stable_id` must accept the ID
// dialects the rest of the verb already accepts. See spec.md.
//
// The defect is an ASYMMETRY, not a regex opinion. `stable_id` demanded a
// leading LETTER, while on the same verb the `id` locator accepts "3D_E-0629"
// (op:"flip" uses it) and `id_prefix` deliberately allows a leading digit so
// long as a letter appears somewhere — its own refusal names "3D_E" as a
// valid example. So one argument was stricter than the two either side of it
// for no reason visible from the schema, and the project it locked out has
// used that dialect for hundreds of items.
//
// The workaround left open was the counter strategy, which silently depends
// on .roadmap-counter being in sync — where `stable_id` is the argument that
// exists precisely to let the caller name the ID.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

// Padded past the 1 KiB minimum-parseable gate, and seeded with the reporting
// project's own dialect so the sniffer has something real to find.
QString seedRoadmap() {
    QString s = QStringLiteral("# Vestige Roadmap\n\n");
    for (int i = 0; i < 14; ++i) {
        s += QStringLiteral(
            "Intro paragraph padding this file past the minimum-parseable "
            "gate the append path enforces before it will trust a walk of "
            "the bullets below. Lorem ipsum dolor sit amet.\n");
    }
    s += QString::fromUtf8(
        "\n## Backlog\n"
        "\n"
        "- \xF0\x9F\x93\x8B [3D_E-0629] **An existing digit-led bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n");
    return s;
}

bool seed(const QTemporaryDir &tmp) {
    QFile f(tmp.path() + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(seedRoadmap().toUtf8());
    f.close();
    return true;
}

QJsonObject appendReq(const QString &root, const QString &stableId) {
    QJsonObject req;
    req["caller_cwd"]  = root;
    req["op"]          = QStringLiteral("append");
    req["section"]     = QStringLiteral("backlog");
    req["status"]      = QStringLiteral("planned");
    req["headline"]    = QStringLiteral("A newly filed digit-led item.");
    req["kind"]        = QStringLiteral("fix");
    req["source"]      = QStringLiteral("test");
    req["id_strategy"] = QStringLiteral("stable_prefix");
    req["stable_id"]   = stableId;
    return req;
}

}  // namespace

// INV-1 — op:"append" accepts a digit-led stable_id. Breaks when the shape
// still demands a leading letter: bad_args, and the project cannot name its
// own IDs.
TEST(roadmap_log_stable_id_digit_prefix, Inv1AppendAcceptsADigitLedId) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp));

    RemoteControl rc(nullptr);
    const QJsonObject out =
        rc.cmdRoadmapLogAppendForTest(appendReq(tmp.path(),
                                                QStringLiteral("3D_E-0682")))
            .object();
    expect(out.value(QStringLiteral("ok")).toBool(),
           "INV-1: a digit-led stable_id is accepted by op:\"append\"");
    expect(out.value(QStringLiteral("id")).toString() ==
               QStringLiteral("3D_E-0682"),
           "INV-1: the id the caller named is the id that lands");
    EXPECT_EQ(0, expect_finish())
        << QJsonDocument(out).toJson().toStdString();
}

// INV-2 — the guard's REAL purpose survives. A bare number is still refused,
// because no locator could tell it from a counter ID. This is the control:
// without it, "accept everything" would pass INV-1.
TEST(roadmap_log_stable_id_digit_prefix, Inv2BareNumberStillRefused) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp));

    RemoteControl rc(nullptr);
    const QJsonObject out =
        rc.cmdRoadmapLogAppendForTest(appendReq(tmp.path(),
                                                QStringLiteral("0682")))
            .object();
    expect(!out.value(QStringLiteral("ok")).toBool(),
           "INV-2: a stable_id with no letter at all is still refused");
    expect(out.value(QStringLiteral("code")).toString() ==
               QStringLiteral("bad_args"),
           "INV-2: refused as bad_args, not by some later accident");
    EXPECT_EQ(0, expect_finish())
        << QJsonDocument(out).toJson().toStdString();
}

// INV-3 — op:"append_batch" agrees with op:"append". The two carry separate
// copies of the shape, which is how they drift.
TEST(roadmap_log_stable_id_digit_prefix, Inv3BatchAgreesWithAppend) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp));

    QJsonObject good;
    good["status"]    = QStringLiteral("planned");
    good["headline"]  = QStringLiteral("A batched digit-led item.");
    good["kind"]      = QStringLiteral("fix");
    good["source"]    = QStringLiteral("test");
    good["stable_id"] = QStringLiteral("3D_E-0683");
    QJsonObject bare = good;
    bare["headline"]  = QStringLiteral("A batched bare-number item.");
    bare["stable_id"] = QStringLiteral("0683");

    QJsonObject req;
    req["caller_cwd"]  = tmp.path();
    req["op"]          = QStringLiteral("append_batch");
    req["section"]     = QStringLiteral("backlog");
    req["id_strategy"] = QStringLiteral("stable_prefix");
    req["bullets"]     = QJsonArray{ good, bare };

    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    expect(out.value(QStringLiteral("applied_count")).toInt() == 1,
           "INV-3: the digit-led bullet applies");
    expect(out.value(QStringLiteral("skipped_count")).toInt() == 1,
           "INV-3: the bare-number bullet is still skipped, not the other way "
           "round");
    EXPECT_EQ(0, expect_finish())
        << QJsonDocument(out).toJson().toStdString();
}

// INV-4 — the sniffer sees the same dialect, checked through the behaviour it
// exists to produce rather than by calling it. It fires when .roadmap-counter
// is missing AND the project uses stable IDs, to point the caller at the
// stable_prefix strategy — which is exactly this project's situation, so a
// shape that could not see 3D_E-0629 withheld the hint from the projects it
// was written for.
//
// Driven through op:"append" on the COUNTER strategy with no counter file.
// remotecontrol_internal.h is off limits to a test (RcTuSplit INV-5 keeps it
// to the ANTS_RC_SOURCES list), and the public path is the better subject
// anyway: it is what a caller actually meets.
TEST(roadmap_log_stable_id_digit_prefix, Inv4SnifferSeesADigitLedId) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp));   // seeds ROADMAP.md and NO .roadmap-counter

    QJsonObject req = appendReq(tmp.path(), QString());
    req.remove(QStringLiteral("stable_id"));
    req["id_strategy"] = QStringLiteral("counter");

    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();
    const QString body = QString::fromUtf8(QJsonDocument(out).toJson());

    expect(body.contains(QStringLiteral("3D_E-0629")),
           "INV-4: the missing-counter refusal names the digit-led id it "
           "found, so the caller is pointed at the stable_prefix strategy");
    EXPECT_EQ(0, expect_finish()) << body.toStdString();
}
