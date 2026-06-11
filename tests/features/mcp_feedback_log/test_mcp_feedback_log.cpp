// Feature-conformance test for the feedback_log MCP tool (ANTS-1962).
// Drives FeedbackFile renderers directly + cmdFeedbackLog (absolute
// path, m_main-independent). See spec.md + docs/specs/ANTS-1962.md.

#include "feedbackfile.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

namespace {

const char *kClip = "\xF0\x9F\x93\x8B";  // 📋

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QJsonObject finding(const QString &title, const QString &what = {},
                    const QString &repro = {}) {
    QJsonObject o;
    o["title"] = title;
    if (!what.isEmpty())  o["what"]  = what;
    if (!repro.isEmpty()) o["repro"] = repro;
    return o;
}

}  // namespace

// T1 — render full template; omitting repro drops the Repro line;
// Proposed ID always present + blank.
TEST(McpFeedbackLog, RenderFindingTemplate) {
    QVector<FeedbackFile::Finding> fs;
    FeedbackFile::Finding f;
    f.title = "Title here"; f.what = "summary"; f.impact = "blocker";
    // repro intentionally omitted
    f.suggestedFix = "do x";
    fs.append(f);
    const QString out = FeedbackFile::renderFindingBlock(
        "2026-06-03", "session label", /*h1=*/false, "intro note", fs);
    EXPECT_TRUE(out.startsWith("## 2026-06-03 — session label"));
    EXPECT_TRUE(out.contains("intro note"));
    EXPECT_TRUE(out.contains("### Title here"));
    EXPECT_TRUE(out.contains("- **What:** summary"));
    EXPECT_TRUE(out.contains("- **Impact:** blocker"));
    EXPECT_TRUE(out.contains("- **Suggested fix:** do x"));
    EXPECT_FALSE(out.contains("**Repro:**"));   // omitted
    EXPECT_TRUE(out.contains("- **Proposed ID:**"));  // always present
}

// h1 heading form.
TEST(McpFeedbackLog, RenderFindingH1) {
    QVector<FeedbackFile::Finding> fs;
    FeedbackFile::Finding f; f.title = "T"; fs.append(f);
    const QString out = FeedbackFile::renderFindingBlock(
        "2026-06-03", QString(), /*h1=*/true, QString(), fs);
    EXPECT_TRUE(out.startsWith("# 2026-06-03\n"));  // date-only h1
}

// T2 — tracking watermark matches the ANTS-1961 anchor regex.
TEST(McpFeedbackLog, RenderTrackingWatermark) {
    QVector<FeedbackFile::TrackingRow> rows;
    FeedbackFile::TrackingRow r;
    r.item = "an item"; r.ids = {"ANTS-1961"};
    r.status = QString::fromUtf8(kClip);
    rows.append(r);
    const QString out = FeedbackFile::renderTrackingBlock(
        "2026-06-03", "prose", rows, /*sentinel=*/true);
    static const QRegularExpression anchor(QString::fromUtf8(
        "^## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking( update)? \\("));
    EXPECT_TRUE(anchor.match(out.section('\n', 0, 0)).hasMatch())
        << out.section('\n', 0, 0).toStdString();
    EXPECT_TRUE(out.contains("| an item | ANTS-1961 |"));
    EXPECT_TRUE(out.contains("End of 2026-06-03"));
}

// T4 — empty ids → n/a; multi-id joined; Notes column iff any notes.
TEST(McpFeedbackLog, TrackingTableShapes) {
    QVector<FeedbackFile::TrackingRow> rows;
    FeedbackFile::TrackingRow r1;
    r1.item = "x"; r1.status = QString::fromUtf8(kClip);  // empty ids
    FeedbackFile::TrackingRow r2;
    r2.item = "y"; r2.ids = {"ANTS-1961", "ANTS-1962"};
    r2.status = QString::fromUtf8(kClip);
    rows = {r1, r2};
    const QString noNotes = FeedbackFile::renderTrackingBlock(
        "2026-06-03", QString(), rows, false);
    EXPECT_TRUE(noNotes.contains("| x | n/a |"));
    EXPECT_TRUE(noNotes.contains("| y | ANTS-1961, ANTS-1962 |"));
    EXPECT_FALSE(noNotes.contains("Notes"));

    rows[1].notes = "some note";
    const QString withNotes = FeedbackFile::renderTrackingBlock(
        "2026-06-03", QString(), rows, false);
    EXPECT_TRUE(withNotes.contains("| Item | ID(s) | Status | Notes |"));
    // note-less row still has an empty 4th cell (well-formed GFM).
    EXPECT_TRUE(withNotes.contains("| x | n/a | \xF0\x9F\x93\x8B |"));
    EXPECT_TRUE(withNotes.contains("some note |"));
}

// T6 — absent file + append_finding creates a skeleton; created:true.
TEST(McpFeedbackLog, CreatesSkeleton) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/Proj_Ants_MCP_Feedback.md";
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = p; req["caller_cwd"] = dir.path();
    req["op"] = "append_finding"; req["date"] = "2026-06-03";
    req["session_label"] = "first";
    QJsonArray fs; fs.append(finding("Title", "what"));
    req["findings"] = fs;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool()) << "create should succeed";
    EXPECT_TRUE(env.value("created").toBool());
    const QByteArray on = readAll(p);
    EXPECT_TRUE(on.contains("<!-- ants-mcp-feedback: 1 -->"));
    EXPECT_TRUE(on.contains("# Ants MCP Feedback — Proj"));  // derived H1
    EXPECT_TRUE(on.contains("Format: docs/standards/mcp-feedback-files.md"));
    EXPECT_TRUE(on.contains("### Title"));
}

// T7 — append-at-end below a maintainer block (verified via parse).
TEST(McpFeedbackLog, AppendsBelowMaintainerBlock) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/P_Ants_MCP_Feedback.md";
    QString seed = QStringLiteral(
        "# Title\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11, maintainer)\n\n"
        "| a | ANTS-1 | 📋 |\n").arg(QString::fromUtf8(kClip));
    { QFile f(p); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
      f.write(seed.toUtf8()); f.close(); }

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = p; req["caller_cwd"] = dir.path();
    req["op"] = "append_finding"; req["date"] = "2026-06-03";
    QJsonArray fs; fs.append(finding("Brand new finding"));
    req["findings"] = fs;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_FALSE(env.value("created").toBool());

    const FeedbackFile::ParseResult r =
        FeedbackFile::parse(QString::fromUtf8(readAll(p)));
    EXPECT_TRUE(r.deltaPresent);
    EXPECT_TRUE(r.delta.contains("Brand new finding"))
        << "new finding must land in the delta (below the watermark)";
}

// T3 — round-trip: un-triaged delta → append_tracking → delta empties +
// id appears in mapped_ids.
TEST(McpFeedbackLog, RoundTrip) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/R_Ants_MCP_Feedback.md";
    QString seed = QStringLiteral(
        "# Title\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11, maintainer)\n\n"
        "| a | ANTS-1 | 📋 |\n\n"
        "## 2026-05-20 — session\n\n"
        "### untriaged finding\n\n- **What:** x.\n")
        .arg(QString::fromUtf8(kClip));
    { QFile f(p); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
      f.write(seed.toUtf8()); f.close(); }

    // Before: delta present.
    {
        const FeedbackFile::ParseResult before =
            FeedbackFile::parse(QString::fromUtf8(readAll(p)));
        EXPECT_TRUE(before.deltaPresent);
    }

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = p; req["caller_cwd"] = dir.path();
    req["op"] = "append_tracking"; req["date"] = "2026-06-03";
    QJsonArray rows;
    QJsonObject row; row["item"] = "untriaged finding";
    QJsonArray ids; ids.append("ANTS-9001"); row["ids"] = ids;
    row["status"] = QString::fromUtf8(kClip);
    rows.append(row); req["rows"] = rows;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool()) << "append_tracking should succeed";

    const FeedbackFile::ParseResult after =
        FeedbackFile::parse(QString::fromUtf8(readAll(p)));
    EXPECT_FALSE(after.deltaPresent) << "watermark advanced, delta empty";
    EXPECT_TRUE(after.mappedIds.contains("ANTS-9001"));
}

// T5 — refusals.
TEST(McpFeedbackLog, Refusals) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/F_Ants_MCP_Feedback.md";
    { QFile f(p); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
      f.write(QByteArray("# Title\n")); f.close(); }
    RemoteControl rc(nullptr);
    auto base = [&]() {
        QJsonObject r; r["path"] = p; r["caller_cwd"] = dir.path();
        return r;
    };

    // bad op → bad_mode
    { QJsonObject r = base(); r["op"] = "weird";
      EXPECT_EQ(rc.cmdFeedbackLog(r).object().value("code").toString(),
                "bad_mode"); }
    // no findings → bad_args
    { QJsonObject r = base(); r["op"] = "append_finding";
      r["findings"] = QJsonArray();
      EXPECT_EQ(rc.cmdFeedbackLog(r).object().value("code").toString(),
                "bad_args"); }
    // empty title → bad_args
    { QJsonObject r = base(); r["op"] = "append_finding";
      QJsonArray fs; fs.append(finding(""));  r["findings"] = fs;
      EXPECT_EQ(rc.cmdFeedbackLog(r).object().value("code").toString(),
                "bad_args"); }
    // bad status → bad_status
    { QJsonObject r = base(); r["op"] = "append_tracking";
      QJsonArray rows; QJsonObject row; row["item"] = "x";
      row["status"] = "Z"; rows.append(row); r["rows"] = rows;
      EXPECT_EQ(rc.cmdFeedbackLog(r).object().value("code").toString(),
                "bad_status"); }
    // non-ANTS id → bad_args
    { QJsonObject r = base(); r["op"] = "append_tracking";
      QJsonArray rows; QJsonObject row; row["item"] = "x";
      row["status"] = QString::fromUtf8(kClip);
      QJsonArray ids; ids.append("1234"); row["ids"] = ids;
      rows.append(row); r["rows"] = rows;
      EXPECT_EQ(rc.cmdFeedbackLog(r).object().value("code").toString(),
                "bad_args"); }
    // bad date → bad_args
    { QJsonObject r = base(); r["op"] = "append_finding";
      r["date"] = "06-2026";
      QJsonArray fs; fs.append(finding("T")); r["findings"] = fs;
      EXPECT_EQ(rc.cmdFeedbackLog(r).object().value("code").toString(),
                "bad_args"); }
    // non-feedback basename → not_feedback_file
    { const QString np = dir.path() + "/notes.md";
      QJsonObject r; r["path"] = np; r["caller_cwd"] = dir.path();
      r["op"] = "append_finding";
      QJsonArray fs; fs.append(finding("T")); r["findings"] = fs;
      EXPECT_EQ(rc.cmdFeedbackLog(r).object().value("code").toString(),
                "not_feedback_file"); }
    // append_tracking on absent file → not_found
    { const QString np = dir.path() + "/Gone_Ants_MCP_Feedback.md";
      QJsonObject r; r["path"] = np; r["caller_cwd"] = dir.path();
      r["op"] = "append_tracking";
      QJsonArray rows; QJsonObject row; row["item"] = "x";
      row["status"] = QString::fromUtf8(kClip); rows.append(row);
      r["rows"] = rows;
      EXPECT_EQ(rc.cmdFeedbackLog(r).object().value("code").toString(),
                "not_found"); }
}

// T8 — atomicity: forced write failure leaves the original untouched.
TEST(McpFeedbackLog, AtomicWriteFailure) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/A_Ants_MCP_Feedback.md";
    const QByteArray seed = "# Title\n\nexisting content.\n";
    { QFile f(p); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
      f.write(seed); f.close(); }

    RemoteControl rc(nullptr);
    QJsonObject req; req["path"] = p; req["caller_cwd"] = dir.path();
    req["op"] = "append_finding"; req["date"] = "2026-06-03";
    QJsonArray fs; fs.append(finding("Won't land")); req["findings"] = fs;

    RemoteControl::setForceFeedbackWriteFailForTest(true);
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    RemoteControl::setForceFeedbackWriteFailForTest(false);

    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), "write_failed");
    EXPECT_EQ(readAll(p), seed) << "original must be byte-identical";
}
