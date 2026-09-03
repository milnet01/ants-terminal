// ANTS-3382 — feature-conformance test for roadmap_log evidence:[paths].
// Behavioural: drives cmdRoadmapLogAppendForTest against a seeded temp
// ROADMAP, then round-trips through RoadmapDialog::parseBullets.

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "remotecontrol.h"
#include "roadmapdialog.h"

namespace {

QString freshRoadmap() {
    return QString::fromUtf8(
        "# Fresh Roadmap\n"
        "\n"
        "## Backlog\n"
        "\n"
        "- \xF0\x9F\x93\x8B [ANTS-9001] **An existing bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n");
}

bool writeRoadmap(const QString &dir, const QString &content) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(content.toUtf8());
    f.close();
    return true;
}

bool writeCounter(const QString &dir, qint64 value) {
    QFile f(dir + QStringLiteral("/.roadmap-counter"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write((QString::number(value) + QChar('\n')).toUtf8());
    f.close();
    return true;
}

QString readRoadmap(const QString &dir) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll());
}

QJsonObject appendReq(const QString &dir, const QString &headline) {
    QJsonObject r;
    r["caller_cwd"] = dir;
    r["op"]         = QStringLiteral("append");
    r["section"]    = QStringLiteral("backlog");
    r["status"]     = QStringLiteral("planned");
    r["headline"]   = headline;
    r["kind"]       = QStringLiteral("fix");
    r["source"]     = QStringLiteral("test");
    return r;
}

// Find the bullet whose headline contains `needle`.
RoadmapDialog::BulletRecord findBullet(const QString &markdown,
                                       const QString &needle) {
    const auto bullets = RoadmapDialog::parseBullets(markdown);
    for (const auto &b : bullets) {
        if (b.headline.contains(needle)) return b;
    }
    return {};
}

}  // namespace

// INV-1 / INV-2 — append with evidence writes an Evidence: line (no
// trailing period) and parseBullets round-trips the paths, dots intact.
TEST(roadmap_log_evidence, Inv1Inv2AppendAndRoundTrip) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeRoadmap(tmp.path(), freshRoadmap()));
    ASSERT_TRUE(writeCounter(tmp.path(), 9001));

    RemoteControl rc(nullptr);
    QJsonObject req = appendReq(tmp.path(), QStringLiteral("Crash on resume."));
    QJsonArray ev;
    ev.append(QStringLiteral("photos/IMG_2031.jpg"));
    ev.append(QStringLiteral("logs/run.txt"));
    req["evidence"] = ev;
    const QJsonObject out =
        rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(out).toJson().toStdString();

    const QString md = readRoadmap(tmp.path());
    // INV-1 — the rendered line, no trailing sentence period.
    EXPECT_TRUE(md.contains(QStringLiteral(
        "  Evidence: photos/IMG_2031.jpg, logs/run.txt\n")))
        << md.toStdString();

    // INV-2 — parseBullets round-trip; the .jpg dot is not truncated.
    const auto b = findBullet(md, QStringLiteral("Crash on resume"));
    ASSERT_EQ(b.evidence.size(), 2);
    EXPECT_EQ(b.evidence.at(0), QStringLiteral("photos/IMG_2031.jpg"));
    EXPECT_EQ(b.evidence.at(1), QStringLiteral("logs/run.txt"));
}

// INV-3 — no evidence arg → no Evidence: line, empty evidence field.
TEST(roadmap_log_evidence, Inv3NoEvidenceIsAdditive) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeRoadmap(tmp.path(), freshRoadmap()));
    ASSERT_TRUE(writeCounter(tmp.path(), 9001));

    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(
        appendReq(tmp.path(), QStringLiteral("Plain item."))).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool());

    const QString md = readRoadmap(tmp.path());
    EXPECT_FALSE(md.contains(QStringLiteral("Evidence:")));
    const auto b = findBullet(md, QStringLiteral("Plain item"));
    EXPECT_TRUE(b.evidence.isEmpty());
}

// INV-4 — a path with an embedded comma/newline is folded to spaces so
// the single-line Evidence: field shape survives (no spurious 3rd path).
TEST(roadmap_log_evidence, Inv4CommaInPathFolded) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeRoadmap(tmp.path(), freshRoadmap()));
    ASSERT_TRUE(writeCounter(tmp.path(), 9001));

    RemoteControl rc(nullptr);
    QJsonObject req = appendReq(tmp.path(), QStringLiteral("Weird path."));
    QJsonArray ev;
    ev.append(QStringLiteral("a/x,y.png"));   // comma inside one path
    req["evidence"] = ev;
    const QJsonObject out =
        rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool());

    const QString md = readRoadmap(tmp.path());
    const auto b = findBullet(md, QStringLiteral("Weird path"));
    // The comma was folded to a space, so it stays ONE evidence entry.
    ASSERT_EQ(b.evidence.size(), 1);
    EXPECT_EQ(b.evidence.at(0), QStringLiteral("a/x y.png"));
}

// INV-5 (ANTS-3407) — a hand-edited lowercase `evidence:` label parses
// case-insensitively (parity with the long-standing `Layman:` tolerance),
// while the UN-ANCHORED labels stay case-SENSITIVE so they can't mis-capture a
// lowercase "lanes:" / "kind:" occurring mid-prose. Drives parseBullets
// directly on hand-typed markdown (no writer round-trip).
//
// ANTS-4065 § 2.2 moved `kind:` from the first group to the second, and this
// test moved with it. That spec un-anchors rxKind() — 99 bullets in this
// project write the trailer inline, and the anchor lost every one of them — and
// case tolerance is only safe while the anchor holds it to a label position:
// un-anchored, "…changed the kind: of work we do…" parses as a declaration.
// rxLanes() has never had the option for exactly this reason. The cost is that
// a hand-typed `kind:` stops parsing, which § 2.2 accepts because once a project
// is migrated the render is the sole writer. ANTS-4065 INV-9
// (tests/features/roadmap_import_mapping/) pins the new behaviour directly.
TEST(roadmap_log_evidence, Inv5HandEditedLabelCase) {
    const QString md = QString::fromUtf8(
        "# Hand-edited Roadmap\n"
        "\n"
        "## Backlog\n"
        "\n"
        "- \xF0\x9F\x93\x8B [ANTS-9002] **A hand-typed bullet.**\n"
        "  kind: fix.\n"
        "  lanes: backend, tests.\n"
        "  evidence: photos/IMG_7.jpg, logs/out.txt.\n"
        "\n");
    const auto b = findBullet(md, QStringLiteral("hand-typed bullet"));

    // The still-anchored `Evidence:` tolerates any case (ANTS-3407).
    ASSERT_EQ(b.evidence.size(), 2);
    EXPECT_EQ(b.evidence.at(0), QStringLiteral("photos/IMG_7.jpg"));
    EXPECT_EQ(b.evidence.at(1), QStringLiteral("logs/out.txt"));

    // The two un-anchored labels deliberately stay case-sensitive, so the
    // lowercase forms are left unparsed. `Lanes:` has been here since
    // ANTS-3407; `Kind:` joined it at ANTS-4065 § 2.2 when it lost its anchor.
    EXPECT_TRUE(b.lanes.isEmpty());
    EXPECT_TRUE(b.kind.isEmpty());
}

// ANTS-4527 helper — the `evidence_not_path_shaped` advisory, or a null object.
namespace {
QJsonObject evidenceWarning(const QJsonObject &out) {
    for (const QJsonValue &w : out.value(QStringLiteral("warnings")).toArray()) {
        const QJsonObject o = w.toObject();
        if (o.value(QStringLiteral("code")).toString()
            == QLatin1String("evidence_not_path_shaped"))
            return o;
    }
    return {};
}
}  // namespace

// INV-6 (ANTS-4527) — prose in Evidence: is reported to the caller. It is an
// advisory on a successful write, never a refusal: this verb is called from
// every project on the machine, and refusing input accepted today would break
// them over a defect affecting a handful of items.
TEST(roadmap_log_evidence, Inv6ProseEvidenceIsReported) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeRoadmap(tmp.path(), freshRoadmap()));
    ASSERT_TRUE(writeCounter(tmp.path(), 9001));

    RemoteControl rc(nullptr);
    QJsonObject req = appendReq(tmp.path(), QStringLiteral("Fog is wrong."));
    QJsonArray ev;
    ev.append(QStringLiteral("user screenshot"));
    req["evidence"] = ev;
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();

    EXPECT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << "advisory, not refusal: " << QJsonDocument(out).toJson().toStdString();
    const QJsonObject w = evidenceWarning(out);
    ASSERT_FALSE(w.isEmpty()) << QJsonDocument(out).toJson().toStdString();
    const QJsonArray els = w.value(QStringLiteral("elements")).toArray();
    ASSERT_EQ(els.size(), 1);
    EXPECT_EQ(els.at(0).toString(), QStringLiteral("user screenshot"));
}

// INV-7 (ANTS-4527) — the control, and the one that stops the advisory being
// "always fire". A path with a separator and a path with only an extension
// must BOTH stay silent; ANTS-4502's separator-only predicate would have
// flagged the second, and `shot.png` at the repo root is legitimate evidence.
TEST(roadmap_log_evidence, Inv7RealPathsRaiseNoAdvisory) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeRoadmap(tmp.path(), freshRoadmap()));
    ASSERT_TRUE(writeCounter(tmp.path(), 9001));

    RemoteControl rc(nullptr);
    QJsonObject req = appendReq(tmp.path(), QStringLiteral("Crash on open."));
    QJsonArray ev;
    ev.append(QStringLiteral("photos/IMG_2031.jpg"));
    ev.append(QStringLiteral("shot.png"));
    req["evidence"] = ev;
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();

    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(evidenceWarning(out).isEmpty())
        << "a real path tripped the advisory: "
        << QJsonDocument(out).toJson().toStdString();
}

// INV-8 (ANTS-4527) — the measured corpus shape. One prose sentence with
// commas in it is what produced GHUB-0052's "96" / "000 mutants ..." / "clean"
// in the store. The advisory must fire on it however the field folds commas.
TEST(roadmap_log_evidence, Inv8ProseWithCommasIsReported) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeRoadmap(tmp.path(), freshRoadmap()));
    ASSERT_TRUE(writeCounter(tmp.path(), 9001));

    RemoteControl rc(nullptr);
    QJsonObject req = appendReq(tmp.path(), QStringLiteral("Mutants ran."));
    QJsonArray ev;
    ev.append(QStringLiteral("96,000 mutants across all twelve parsers, clean"));
    req["evidence"] = ev;
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();

    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(evidenceWarning(out).isEmpty())
        << QJsonDocument(out).toJson().toStdString();
}
