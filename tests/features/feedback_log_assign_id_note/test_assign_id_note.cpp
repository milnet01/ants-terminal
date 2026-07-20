// ANTS-3571 — feature-conformance test for feedback_log op:"assign_id"
// honouring its documented `note` param. Pure FeedbackFile::assignId over
// synthetic v2 fixtures + a live RemoteControl::cmdFeedbackLog drive (the
// exact repro: assign_id{ids, note} used to drop the note silently).

#include "feedbackfile.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <string>

namespace {

// A minimal v2 fixture: heading → Proposed-ID placeholder → What bullet, so a
// note inserted under the id line lands BETWEEN the id and the What bullet.
const char *kFixture =
    "<!-- ants-mcp-feedback: 2 -->\n"                       // 1
    "# Feedback TEST\n"                                     // 2
    "\n"                                                    // 3
    "### roadmap_query eats prose roadmaps\n"              // 4
    "- **Proposed ID:** _(maintainer to assign)_\n"        // 5
    "- **What:** a gap.\n";                                // 6

QString fixture() { return QString::fromUtf8(kFixture); }
const QString kHeading =
    QStringLiteral("### roadmap_query eats prose roadmaps");
const QString kNote =
    QStringLiteral("fixed \xE2\x80\x94 please relaunch Ants");

FeedbackFile::AssignTarget target(const QString &value, const QString &note) {
    FeedbackFile::AssignTarget t;
    t.heading = kHeading;
    t.value   = value;
    t.note    = note;
    return t;
}

bool writeStr(const QString &path, const QString &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray u = body.toUtf8();
    const bool ok = (f.write(u) == u.size());
    f.close();
    return ok;
}
QString readStr(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV — the note is written as a `- **Note:**` bullet directly under the
// Proposed ID line (between it and the following body bullet).
TEST(FeedbackAssignIdNote, NoteWrittenUnderIdLine) {
    const FeedbackFile::AssignResult r =
        FeedbackFile::assignId(fixture(), target(QStringLiteral("ANTS-3571"),
                                                 kNote));
    ASSERT_TRUE(r.code.isEmpty()) << r.code.toStdString();
    EXPECT_TRUE(r.changed);
    EXPECT_TRUE(r.noteWritten);

    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    const int h = out.indexOf(kHeading);
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 1), QStringLiteral("- **Proposed ID:** ANTS-3571"));
    EXPECT_EQ(out.at(h + 2), QStringLiteral("- **Note:** ") + kNote);
    EXPECT_EQ(out.at(h + 3), QStringLiteral("- **What:** a gap."));
}

// INV-8 — re-assign with the SAME id + note is a byte-identical no-op.
TEST(FeedbackAssignIdNote, ReassignSameIsNoOp) {
    const FeedbackFile::AssignResult r1 =
        FeedbackFile::assignId(fixture(), target(QStringLiteral("ANTS-3571"),
                                                 kNote));
    const FeedbackFile::AssignResult r2 =
        FeedbackFile::assignId(r1.newContent,
                               target(QStringLiteral("ANTS-3571"), kNote));
    EXPECT_FALSE(r2.changed);
    EXPECT_EQ(r2.bytesDelta, 0);
    EXPECT_EQ(r2.newContent, r1.newContent);   // byte-identical
}

// A changed note REPLACES the existing note bullet in place — never a second
// one (idempotency of the note line across re-assigns).
TEST(FeedbackAssignIdNote, NoteReplacedNotDuplicated) {
    const FeedbackFile::AssignResult r1 =
        FeedbackFile::assignId(fixture(), target(QStringLiteral("ANTS-3571"),
                                                 kNote));
    const FeedbackFile::AssignResult r2 =
        FeedbackFile::assignId(
            r1.newContent,
            target(QStringLiteral("ANTS-3571"),
                   QStringLiteral("shipped in 0.7.101")));
    EXPECT_TRUE(r2.changed);
    EXPECT_EQ(r2.newContent.count(QStringLiteral("- **Note:**")), 1);
    EXPECT_TRUE(r2.newContent.contains(
        QStringLiteral("- **Note:** shipped in 0.7.101")));
    EXPECT_FALSE(r2.newContent.contains(kNote));
}

// No note supplied → no Note bullet touched, note_written stays false.
TEST(FeedbackAssignIdNote, NoNoteLeavesBlockNoteFree) {
    const FeedbackFile::AssignResult r =
        FeedbackFile::assignId(fixture(),
                               target(QStringLiteral("ANTS-3571"), QString()));
    EXPECT_TRUE(r.changed);           // the id line still changed
    EXPECT_FALSE(r.noteWritten);
    EXPECT_FALSE(r.newContent.contains(QStringLiteral("- **Note:**")));
}

// ---- live wrapper: the exact finding repro ----

// assign_id{ids:[…], note:"…"} used to write ONLY the Proposed-ID line and drop
// the note. Now the note lands on disk and the envelope reports note_written.
TEST(FeedbackAssignIdNote, LiveIdsPlusNoteWritesNote) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeStr(p, fixture()));

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"]         = "assign_id";
    req["path"]       = p;
    req["caller_cwd"] = dir.path();
    req["heading"]    = kHeading;
    QJsonArray ids; ids.append("ANTS-3571");
    req["ids"]        = ids;
    // Newline in the note must fold to a single space (one bullet per line).
    req["note"]       = "fixed — relaunch Ants\nto pick it up";

    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_TRUE(env.value("note_written").toBool());

    const QString md = readStr(p);
    EXPECT_TRUE(md.contains(QStringLiteral("- **Proposed ID:** ANTS-3571")));
    EXPECT_TRUE(md.contains(
        QStringLiteral("- **Note:** fixed — relaunch Ants to pick it up")));
    // The folded note is a single line (no embedded newline survived).
    EXPECT_FALSE(md.contains(QStringLiteral("relaunch Ants\nto pick it up")));
}

// Schema — the note property description names assign_id (ANTS-3571).
TEST(FeedbackAssignIdNote, SchemaDocumentsAssignIdNote) {
    QFile f(QStringLiteral(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QString ci = QString::fromUtf8(f.readAll());
    EXPECT_TRUE(ci.contains(QStringLiteral("ANTS-3571")));
    EXPECT_TRUE(ci.contains(QStringLiteral("on assign_id it is")));
}
