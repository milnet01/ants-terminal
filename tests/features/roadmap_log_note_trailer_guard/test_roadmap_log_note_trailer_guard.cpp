// ANTS-4549 — a `note` is opaque prose: a bare trailer keyword in one may
// not be read as metadata. Behavioural, against a seeded temp ROADMAP via
// cmdRoadmapLogFlipForTest / cmdRoadmapLogFlipBatchForTest. See spec.md.

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <string>

namespace {

bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

// ants-v1 seed, padded past the 1 KiB minimum-parseable-size gate.
QByteArray seed() {
    return QByteArray(
        "# Test Roadmap\n\n"
        "Intro paragraph that exists purely to pad the file past the\n"
        "1 KiB minimum-parseable-size gate the write paths enforce\n"
        "before they will trust an ants-v1 walk. Lorem ipsum dolor sit\n"
        "amet, consectetur adipiscing elit, sed do eiusmod tempor\n"
        "incididunt ut labore et dolore magna aliqua. Ut enim ad minim\n"
        "veniam, quis nostrud exercitation ullamco laboris nisi ut\n"
        "aliquip ex ea commodo consequat. Duis aute irure dolor in\n"
        "reprehenderit in voluptate velit esse cillum dolore eu fugiat\n"
        "nulla pariatur. Excepteur sint occaecat cupidatat non\n"
        "proident, sunt in culpa qui officia deserunt mollit anim id\n"
        "est laborum. More padding to be safe and clear the gate with\n"
        "comfortable headroom for the parser and the size check above.\n"
        "Sed ut perspiciatis unde omnis iste natus error sit\n"
        "voluptatem accusantium doloremque laudantium, totam rem\n"
        "aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
        "architecto beatae vitae dicta sunt explicabo.\n"
        "\n"
        "## Work Items\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0042] **First seed bullet.**\n"
        "  Kind: feature.\n"
        "  Source: seed.\n"
        "\n"
        "- \xF0\x9F\x93\x8B [ANTS-0043] **Second seed bullet.**\n"
        "  Kind: feature.\n"
        "  Source: seed.\n"
        "\n");
}

QJsonObject req(const QString &root, const QString &op) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("op")]         = op;
    return o;
}

}  // namespace

// INV-1 / INV-6 — annotate with a note declaring a trailer key refuses
// body_shadowed, names the key, quotes the text, and writes nothing.
// Reproduces the AI_Prompts/AIPR-0033 report.
TEST(roadmap_log_note_trailer_guard, Inv1AnnotateRefusesTrailerInNote) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));
    const QByteArray before = readFile(roadmapPath(tmp.path()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
    r[QStringLiteral("note")] = QStringLiteral(
        "Progress (2026-08-20): the bare Kind: keyword in this sentence is "
        "prose about the trailer, not a declaration of one.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << "a note declaring a trailer key must refuse";
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_shadowed"));
    const std::string err =
        (resp.value(QStringLiteral("error")).toString() +
         resp.value(QStringLiteral("hint")).toString()).toStdString();
    EXPECT_TRUE(has(err, "kind")) << "INV-6: the refusal names the key";
    EXPECT_TRUE(has(err, "Kind:")) << "INV-6: the refusal quotes the text";
    EXPECT_EQ(readFile(roadmapPath(tmp.path())), before)
        << "INV-1: the file must be untouched";
}

// INV-2 — the same note with the key in backticks is accepted, so the
// refusal is self-correctable in one edit.
TEST(roadmap_log_note_trailer_guard, Inv2BacktickedKeyIsAccepted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
    r[QStringLiteral("note")] = QStringLiteral(
        "Progress (2026-08-20): the bare `Kind:` keyword in this sentence is "
        "prose about the trailer, not a declaration of one.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(has(readFile(roadmapPath(tmp.path())).toStdString(),
                    "prose about the trailer"));
}

// INV-3 — a flip carrying such a note refuses too, and does not flip.
TEST(roadmap_log_note_trailer_guard, Inv3FlipWithNoteRefusesAndDoesNotFlip) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("ANTS-0042");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    r[QStringLiteral("note")]      = QStringLiteral(
        "Resolved (2026-08-20): Source: is named here in prose.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_shadowed"));
    EXPECT_TRUE(has(readFile(roadmapPath(tmp.path())).toStdString(),
                    "\xF0\x9F\x93\x8B [ANTS-0042]"))
        << "INV-3: one op, one outcome — the status must not flip";
}

// INV-4 — under flip_batch the offending locator lands in skipped[] while
// the others still apply.
TEST(roadmap_log_note_trailer_guard, Inv4FlipBatchSkipsOnlyTheOffender) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("flip_batch"));
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    QJsonArray locs;
    {
        QJsonObject a;
        a[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
        a[QStringLiteral("note")] = QStringLiteral(
            "Resolved (2026-08-20): Lanes: are discussed here in prose.");
        locs.append(a);
        QJsonObject b;
        b[QStringLiteral("id")]   = QStringLiteral("ANTS-0043");
        b[QStringLiteral("note")] = QStringLiteral(
            "Resolved (2026-08-20): ordinary closing prose.");
        locs.append(b);
    }
    r[QStringLiteral("locators")] = locs;
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const QJsonArray skipped = resp.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 1);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("body_shadowed"));
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 1);
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(has(md, "\xF0\x9F\x93\x8B [ANTS-0042]")) << "offender unflipped";
    EXPECT_TRUE(has(md, "\xE2\x9C\x85 [ANTS-0043]"))     << "sibling flipped";
}

// INV-5 — every key whose pattern CAN match mid-line is guarded there, not
// just `kind` (the only one the store's CHECK constraint happens to catch).
// `Layman:` and `Evidence:` are absent by design: their patterns are anchored
// (`^\\s*`) and cannot match mid-line at all, so there is nothing to guard —
// INV-5b pins that they are still accepted in prose.
TEST(roadmap_log_note_trailer_guard, Inv5EveryMidLineKeyGuarded) {
    for (const char *k : {"Kind:", "Source:", "Lanes:"}) {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));
        RemoteControl rc(nullptr);
        QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
        r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
        r[QStringLiteral("note")] =
            QStringLiteral("Progress: prose naming %1 the trailer key.")
                .arg(QString::fromLatin1(k));
        const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("body_shadowed")) << "unguarded key: " << k;
    }
}

// INV-5b — the guard fires exactly where the PARSER would read a declaration
// out of prose, and nowhere else. `Layman:` / `Evidence:` mid-sentence declare
// nothing, so a note naming one in running prose is accepted: a guard stricter
// than the parser refuses notes that were never at risk.
TEST(roadmap_log_note_trailer_guard, Inv5bAnchoredKeysMidLineAreFine) {
    for (const char *k : {"Layman:", "Evidence:"}) {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));
        RemoteControl rc(nullptr);
        QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
        r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
        r[QStringLiteral("note")] =
            QStringLiteral("Progress: prose naming %1 the trailer key.")
                .arg(QString::fromLatin1(k));
        const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();
        EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << "over-strict on a mid-line anchored key: " << k;
    }
}

// INV-7 — a DELIBERATE declaration still works: the label first on its own
// line, as the render writes it. This is § 2.6's remediation route (a
// `Layman:` note is the only way to fill that column on a migrated item, and
// the render gates on it), so the guard may not close it. Leading whitespace
// is allowed — a caller who indents their declaration is not caught out.
TEST(roadmap_log_note_trailer_guard, Inv7DeliberateDeclarationSurvives) {
    for (const char *n : {"Lanes: vt, core.",
                          "Resolved (2026-08-20): done.\nLanes: vt, core.",
                          "Resolved (2026-08-20): done.\n  Kind: fix.",
                          "Layman: what it means in one line."}) {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));
        RemoteControl rc(nullptr);
        QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
        r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
        r[QStringLiteral("note")] = QString::fromLatin1(n);
        const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();
        EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << "refused a declaration in the render's own shape: " << n;
    }
}

// ANTS-4532 — a note is written verbatim, which is right for prose carrying its
// own line breaks and wrong for prose carrying none: a single unwrapped line
// lands in a corpus where every other body is wrapped. A note with no newline
// has no structure to preserve, so it is wrapped; one with newlines is not.
//
// Behavioural, through the same seam as the guard cases above, because the
// wrapping has to be observed in the FILE — that is where the defect was seen.
namespace {
int longestLine(const QByteArray &bytes) {
    int worst = 0;
    for (const QByteArray &ln : bytes.split('\n'))
        worst = std::max<int>(worst, QString::fromUtf8(ln).size());
    return worst;
}
}  // namespace

TEST(roadmap_log_note_trailer_guard, Ants4532SingleLineNoteIsWrapped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));
    ASSERT_LT(longestLine(readFile(roadmapPath(tmp.path()))), 70)
        << "fixture drift: the seed must already be wrapped, or the assertion "
           "below proves nothing about the note";

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
    r[QStringLiteral("note")] = QStringLiteral(
        "Resolved (2026-08-28): one very long single line of prose with no "
        "newline anywhere in it, of the kind a caller writes when they do not "
        "pre-wrap, which previously landed in the file as a single line far "
        "wider than everything around it and showed up as noise in the diff.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();

    const QByteArray after = readFile(roadmapPath(tmp.path()));
    EXPECT_LE(longestLine(after), 74)
        << "the note must be hard-wrapped, not written as one long line";
    // Wrapping must not lose or reorder the prose.
    const std::string s = QString::fromUtf8(after).toStdString();
    EXPECT_TRUE(has(s, "Resolved (2026-08-28): one very long single line"));
    EXPECT_TRUE(has(s, "noise in the diff."));
}

// The other half, and the reason this is not simply "always wrap": a note that
// brought its own line breaks keeps them.
TEST(roadmap_log_note_trailer_guard, Ants4532MultiLineNoteIsVerbatim) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
    r[QStringLiteral("note")] = QStringLiteral(
        "Resolved: a deliberate break follows this sentence.\n"
        "- a bullet that must stay on its own line\n"
        "- and a second one");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();

    const std::string s = QString::fromUtf8(readFile(roadmapPath(tmp.path()))).toStdString();
    EXPECT_TRUE(has(s, "- a bullet that must stay on its own line"))
        << "an authored line break must survive; re-flowing it would join the "
           "bullets into a paragraph";
    EXPECT_TRUE(has(s, "- and a second one"));
}
