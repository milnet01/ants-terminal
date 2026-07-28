// ANTS-1717 / ANTS-1793 — feature-conformance test for roadmap_log
// note append (op:"annotate" + op:"flip" with `note`). Behavioural:
// drives cmdRoadmapLogFlipForTest against a seeded temp ROADMAP and
// asserts file content + envelope. Source-grep for the descriptor /
// schema / fenced-refusal surface.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <string>

ANTS_TEST_SCOPE();

namespace {

// ants-v1 seed, padded past kRoadmapMinParseableSize (1024 B) so the
// flip path's ants-v1 walker fallback engages. 📋 = U+1F4CB.
QByteArray seedV1() {
    QByteArray b =
        "# Test Roadmap\n\n"
        "Intro paragraph that exists purely to pad the file past the\n"
        "1 KiB minimum-parseable-size gate the flip path enforces\n"
        "before it will trust an ants-v1 walk. Lorem ipsum dolor sit\n"
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
        "architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam\n"
        "voluptatem quia voluptas sit aspernatur aut odit aut fugit,\n"
        "sed quia consequuntur magni dolores eos qui ratione.\n"
        "\n"
        "## Work Items\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0042] **Seed bullet headline.**\n"
        "  First body line.\n"
        "  Source: seed.\n"
        "\n";
    return b;
}

// GFM seed — walker runs regardless of size. Plain (non-bold)
// headline so a headline locator matches verbatim.
QByteArray seedGfm() {
    return QByteArray(
        "# Test Roadmap\n\n"
        "## Work Items\n\n"
        "- [ ] Some GFM headline.\n"
        "  First body line.\n"
        "\n");
}

// ANTS-3696 fixture — seedV1()'s bullet has ONE unbroken run of indented
// body lines, which is why a walk that stopped at the first blank line
// looked correct for years. This adds a bullet whose body has paragraphs,
// which is what a well-documented roadmap item actually looks like.
QByteArray seedV1MultiPara() {
    QByteArray b = seedV1();
    b += "- \xF0\x9F\x93\x8B [ANTS-0043] **Multi-paragraph bullet.**\n"
         "  First paragraph of the body.\n"
         "\n"
         "  Second paragraph of the body.\n"
         "  Source: multi.\n"
         "\n";
    return b;
}

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

QJsonObject req(const QString &root, const QString &op) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("op")]         = op;
    return o;
}


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int countOccurrences(const std::string &hay, const std::string &needle) {
    int n = 0;
    for (std::string::size_type p = hay.find(needle);
         p != std::string::npos;
         p = hay.find(needle, p + needle.size())) {
        ++n;
    }
    return n;
}

}  // namespace

// INV-2 + INV-7 — annotate appends a note, leaves status unchanged, and
// lands the note at the END of the body (after the Source: line).
TEST(roadmap_log_annotate, Inv2Inv7AnnotateAppendsAtBodyEnd) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
    r[QStringLiteral("note")] =
        QStringLiteral("Progress 2026-05-25: did the bounded slice.");
    const QJsonObject resp =
        rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "annotate should succeed";
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("annotate"));
    EXPECT_EQ(resp.value(QStringLiteral("from_status")).toString(),
              resp.value(QStringLiteral("to_status")).toString())
        << "annotate must not change status";
    EXPECT_TRUE(resp.value(QStringLiteral("note_appended")).toBool());

    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md,
        "Progress 2026-05-25: did the bounded slice."))
        << "note text must be written into the file";
    // Status emoji line unchanged (still planned).
    EXPECT_TRUE(contains(md,
        "- \xF0\x9F\x93\x8B [ANTS-0042] **Seed bullet headline.**"));
    // INV-7: note sits AFTER the Source: line (body end), not after
    // the headline.
    const auto srcPos  = md.find("Source: seed.");
    const auto notePos = md.find("Progress 2026-05-25:");
    ASSERT_NE(srcPos,  std::string::npos);
    ASSERT_NE(notePos, std::string::npos);
    EXPECT_LT(srcPos, notePos)
        << "note must land at the end of the body, after Source:";
    // Note carries the 2-space body indent.
    EXPECT_TRUE(contains(md,
        "  Progress 2026-05-25: did the bounded slice."));
}

// INV-3 — annotate without a note refuses missing_field.
TEST(roadmap_log_annotate, Inv3AnnotateRequiresNote) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")] = QStringLiteral("ANTS-0042");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_field"));
}

// ANTS-2053 — a roadmap_query synthetic id (10 lowercase base36 chars,
// the content-hash emitted for an ID-less GFM bullet) is not a valid
// roadmap_log locator. flip/annotate must refuse with the targeted
// synthetic_id_not_locatable code naming the headline/anchor fallback,
// not a generic bullet_not_found.
TEST(roadmap_log_annotate, Ants2053SyntheticIdRefusedWithTargetedCode) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("czf2ld2kjz");  // synthetic shape
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("synthetic_id_not_locatable"));
    EXPECT_EQ(resp.value(QStringLiteral("locator")).toString(),
              QStringLiteral("czf2ld2kjz"));
    EXPECT_FALSE(resp.value(QStringLiteral("hint")).toString().isEmpty());
}

// A non-synthetic-shaped missing id still gets the generic
// bullet_not_found — the targeted refusal must not over-fire.
TEST(roadmap_log_annotate, Ants2053NonSyntheticMissStaysBulletNotFound) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("ANTS-9999");  // real shape, absent
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bullet_not_found"));
}

// INV-4 — annotate + to_status refuses bad_op_combo.
TEST(roadmap_log_annotate, Inv4AnnotateRejectsToStatus) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]        = QStringLiteral("ANTS-0042");
    r[QStringLiteral("note")]      = QStringLiteral("x.");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_op_combo"));
}

// INV-5 — flip + note flips the status AND appends the note.
TEST(roadmap_log_annotate, Inv5FlipWithNote) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("ANTS-0042");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    r[QStringLiteral("note")]      =
        QStringLiteral("Resolved 2026-05-25: fixed the thing.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("flip"));
    EXPECT_EQ(resp.value(QStringLiteral("to_status")).toString(),
              QStringLiteral("✅"));
    EXPECT_TRUE(resp.value(QStringLiteral("note_appended")).toBool());

    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md,
        "- \xE2\x9C\x85 [ANTS-0042] **Seed bullet headline.**"))
        << "status must flip to shipped (✅)";
    EXPECT_TRUE(contains(md, "Resolved 2026-05-25: fixed the thing."));
}

// INV-6 — flip without note is unchanged: status flips, no
// note_appended field (back-compat).
TEST(roadmap_log_annotate, Inv6FlipWithoutNoteBackCompat) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("ANTS-0042");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(resp.contains(QStringLiteral("note_appended")))
        << "no note ⇒ no note_appended field (back-compat shape)";
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md,
        "- \xE2\x9C\x85 [ANTS-0042] **Seed bullet headline.**"));
}

// INV-10 (ANTS-3440) — a retried flip with an identical trailing note is
// idempotent: the note is NOT duplicated, and the retry reports
// note_already_present:true / note_appended:false. This is the
// retry-safety half of ANTS-3440 — a committed flip that surfaces to the
// caller as a transport timeout gets retried, and the retry must not
// double-append the resolution note.
TEST(roadmap_log_annotate, Inv10RetryDedupesIdenticalNote) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    auto flip = [&] {
        QJsonObject r = req(tmp.path(), QStringLiteral("flip"));
        r[QStringLiteral("id")]        = QStringLiteral("ANTS-0042");
        r[QStringLiteral("to_status")] = QStringLiteral("shipped");
        r[QStringLiteral("note")]      =
            QStringLiteral("Resolved 2026-07-09: fixed the flaky transport.");
        return rc.cmdRoadmapLogFlipForTest(r).object();
    };

    const QJsonObject first = flip();
    EXPECT_TRUE(first.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(first.value(QStringLiteral("note_appended")).toBool());
    EXPECT_FALSE(first.contains(QStringLiteral("note_already_present")))
        << "first flip genuinely appends — no already-present signal";

    // Retry (caller saw a false transport timeout on the first call).
    const QJsonObject second = flip();
    EXPECT_TRUE(second.value(QStringLiteral("ok")).toBool())
        << "retry must still succeed (idempotent)";
    EXPECT_TRUE(second.value(QStringLiteral("note_already_present")).toBool())
        << "retry must flag the note as already present";
    EXPECT_FALSE(second.value(QStringLiteral("note_appended")).toBool())
        << "retry must not claim a fresh append";

    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_EQ(1, countOccurrences(md,
        "Resolved 2026-07-09: fixed the flaky transport."))
        << "the resolution note must appear exactly once after a retry";
    // Status still shipped (the flip half is naturally idempotent).
    EXPECT_TRUE(contains(md,
        "- \xE2\x9C\x85 [ANTS-0042] **Seed bullet headline.**"));
}

// INV-8 — note is scrubbed of leaked tool-call XML.
TEST(roadmap_log_annotate, Inv8NoteScrubsLeakedXml) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
    r[QStringLiteral("note")] = QStringLiteral(
        "Clean prose."
        "<parameter name=\"lanes\">a,b</parameter>");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "Clean prose."));
    EXPECT_FALSE(contains(md, "<parameter"))
        << "leaked <parameter> wrapper must be scrubbed from the note";
}

// Cross-format — annotate works on a GFM-task-list bullet too (the
// ANTS-1717 source was a GFM/narrator roadmap), located by headline,
// with no anchor injection.
TEST(roadmap_log_annotate, GfmAnnotateByHeadline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedGfm()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("headline")] = QStringLiteral("Some GFM headline.");
    r[QStringLiteral("note")]     =
        QStringLiteral("Progress note on a GFM bullet.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "GFM annotate by headline should succeed";
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("annotate"));
    EXPECT_FALSE(resp.value(QStringLiteral("anchor_injected")).toBool())
        << "annotate must not inject an anchor";
    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "Progress note on a GFM bullet."));
    // Checkbox unchanged.
    EXPECT_TRUE(contains(md, "- [ ] Some GFM headline."));
}

// INV-1 — shared helper anchor + INV-8 fenced refusal + INV-9
// descriptor/schema surface (source-grep).
TEST(roadmap_log_annotate, Inv1Inv8Inv9SourceSurface) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    expect(contains(rc, "int appendBodyNote("),
           "INV-1: shared appendBodyNote helper present");
    expect(contains(rc, "rcScrubLeakedToolXml(note"),
           "INV-8: note routed through the shared leaked-XML scrub");
    expect(contains(rc, "cannot edit it safely"),
           "INV-8: fenced refusal message generalised for note edits");
    expect(contains(rc, "annotateMode"),
           "INV-2: flip handler parameterised for annotate mode");

    expect(contains(ci, "\"annotate\""),
           "INV-9: schema op enum advertises annotate");
    expect(contains(ci, "props[\"note\"]"),
           "INV-9: schema registers the note property");
    expect(contains(ci, "op:\\\"annotate\\\" (ANTS-1717)"),
           "INV-9: descriptor documents op:annotate");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 over a MULTI-PARAGRAPH body (ANTS-3696). The contract already said
// "after the last body line"; the fixture that verified it had a body with no
// blank lines in it, so a walk that terminated at the first blank line
// satisfied the test and violated the invariant. On a real, well-documented
// bullet the note landed after paragraph ONE.
TEST(roadmap_log_annotate, Inv7NoteLandsAfterMultiParagraphBody) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1MultiPara()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("ANTS-0043");
    r[QStringLiteral("note")] =
        QStringLiteral("Resolved 2026-07-28: landed at the end.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "annotate on the multi-paragraph bullet should succeed";

    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    const auto note   = md.find("Resolved 2026-07-28: landed at the end.");
    const auto second = md.find("Second paragraph of the body.");
    const auto source = md.find("Source: multi.");
    ASSERT_NE(note, std::string::npos)   << "note missing from the file";
    ASSERT_NE(second, std::string::npos) << "body paragraph 2 was destroyed";
    ASSERT_NE(source, std::string::npos) << "Source: trailer was destroyed";
    EXPECT_GT(note, second)
        << "INV-7: the note must follow paragraph 2, not split the body";
    EXPECT_GT(note, source)
        << "INV-7: the note must follow the Source: trailer";
}

// INV-8 — a bare closing tag at the END of the note never reaches the file
// (ANTS-3703). The malformed call is the caller's fault; the artefact is a
// permanent line in an append-only file, so the scrub is what has to hold.
TEST(roadmap_log_annotate, Inv8EdgeTagStrippedFromNote) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
    r[QStringLiteral("note")] =
        QStringLiteral("Resolved 2026-07-28: shipped it.</note>");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());

    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "Resolved 2026-07-28: shipped it."))
        << "the prose itself must survive the scrub";
    EXPECT_FALSE(contains(md, "</note>"))
        << "INV-8: a stray closing tag must not reach ROADMAP.md";
}

// INV-8 — markup in the MIDDLE of a note is prose and is left alone. The
// edge-only rule is what keeps the ANTS-3703 strip from eating real text.
TEST(roadmap_log_annotate, Inv8MidSentenceMarkupSurvives) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seedV1()));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
    r[QStringLiteral("note")] =
        QStringLiteral("Progress: the <div> wrapper is the culprit here.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());

    const std::string md = readFile(roadmapPath(tmp.path())).toStdString();
    EXPECT_TRUE(contains(md, "the <div> wrapper is the culprit"))
        << "INV-8: mid-sentence markup must survive the edge strip";
}

// INV-11 — `bytes_written` is the DELTA; `file_bytes` carries the whole file
// (ANTS-3702). Before this, a one-line note against a 452 KB ROADMAP.md
// reported ~459592 bytes written, which reads as a duplicated file.
TEST(roadmap_log_annotate, Inv11BytesWrittenIsDeltaNotFileSize) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray seed = seedV1();
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()), seed));

    RemoteControl rc(nullptr);
    QJsonObject r = req(tmp.path(), QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("ANTS-0042");
    r[QStringLiteral("note")] = QStringLiteral("Progress: one short line.");
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(r).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());

    const qint64 written = resp.value(QStringLiteral("bytes_written")).toInteger();
    const qint64 fileBytes = resp.value(QStringLiteral("file_bytes")).toInteger();
    const qint64 onDisk = readFile(roadmapPath(tmp.path())).size();

    EXPECT_GT(written, 0) << "a note append adds bytes";
    EXPECT_LT(written, 200)
        << "INV-11: one note line must not report the whole file as written";
    EXPECT_EQ(fileBytes, onDisk)
        << "INV-11: file_bytes must be the post-write file size";
    EXPECT_EQ(written, onDisk - seed.size())
        << "INV-11: bytes_written must equal the size delta";
}
