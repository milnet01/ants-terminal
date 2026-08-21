// Feature-conformance test for the feedback_log MCP tool (ANTS-1962).
// Drives FeedbackFile renderers directly + cmdFeedbackLog (absolute
// path, m_main-independent). See spec.md + docs/specs/ANTS-1962.md.

#include "feedbackfile.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

// ANTS-3469 — a `|` or newline in item/notes is escaped so the table row
// keeps its column count. A raw pipe splits the cell, mis-aligning
// Status/Notes and breaking compact_shipped's ✅-row detection. Mirrors
// roadmap_log op:bundle_row's cell escaping.
TEST(McpFeedbackLog, TrackingCellEscapesPipeAndNewline) {
    QVector<FeedbackFile::TrackingRow> rows;
    FeedbackFile::TrackingRow r;
    r.item   = "regex a|b|c fails";
    r.ids    = {"ANTS-3466"};
    r.status = QString::fromUtf8(kClip);
    r.notes  = "bearing | / .*\nsecond line";
    rows.append(r);
    const QString out = FeedbackFile::renderTrackingBlock(
        "2026-07-09", QString(), rows, false);

    EXPECT_TRUE(out.contains("regex a\\|b\\|c fails"));
    EXPECT_TRUE(out.contains("bearing \\| / .*"));
    EXPECT_TRUE(out.contains(".*<br>second line"));   // newline folded
    EXPECT_FALSE(out.contains("bearing | /"));         // no bare pipe survives

    // The data row keeps exactly 5 structural pipes (`| item | ids | status
    // | notes |`) — escaped pipes (preceded by `\`) don't count.
    QString rowLine;
    for (const QString &ln : out.split(QChar('\n')))
        if (ln.contains("ANTS-3466")) { rowLine = ln; break; }
    ASSERT_FALSE(rowLine.isEmpty());
    int barePipes = 0;
    for (int i = 0; i < rowLine.size(); ++i)
        if (rowLine.at(i) == QChar('|') &&
            (i == 0 || rowLine.at(i - 1) != QChar('\\')))
            ++barePipes;
    EXPECT_EQ(barePipes, 5) << rowLine.toStdString();
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
    // ANTS-3476 — a brand-new file is born v2 (inline-ID), not v1.
    EXPECT_TRUE(on.contains("<!-- ants-mcp-feedback: 2 -->"));
    EXPECT_FALSE(on.contains("<!-- ants-mcp-feedback: 1 -->"));
    EXPECT_TRUE(on.contains("# Ants MCP Feedback — Proj"));  // derived H1
    EXPECT_TRUE(on.contains("Format: docs/standards/mcp-feedback-files.md"));
    // v2 banner: names op:append_finding + the blank Proposed-ID rule; the v1
    // append_tracking / maintainer-table language is gone.
    EXPECT_TRUE(on.contains("op:append_finding"));
    EXPECT_TRUE(on.contains("**Proposed ID:**"));
    EXPECT_FALSE(on.contains("op:append_tracking"));
    EXPECT_TRUE(on.contains("### Title"));
}

namespace {
// Write a feedback file with a chosen version marker + minimal body.
QString writeMarked(const QString &dir, const QString &basename, int ver) {
    const QString p = dir + QLatin1Char('/') + basename;
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(QStringLiteral("<!-- ants-mcp-feedback: %1 -->\n"
                           "# Ants MCP Feedback — Proj\n\n"
                           "intro.\n").arg(ver).toUtf8());
    f.close();
    return p;
}
QJsonObject trackingRow() {
    QJsonObject r;
    r["item"]   = "an item";
    r["status"] = QString::fromUtf8("\xF0\x9F\x93\x8B");  // 📋
    QJsonArray ids; ids.append("ANTS-1962");
    r["ids"] = ids;
    return r;
}
}  // namespace

// ANTS-3477 — op:append_tracking is the retired v1 tracking-table write. On a
// v2 (inline-ID) file it must refuse with `not_v1` and point at op:assign_id,
// so a v2 file can't re-grow a maintainer table.
TEST(McpFeedbackLog, AppendTrackingRefusedOnV2File) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeMarked(dir.path(), "Proj_Ants_MCP_Feedback.md", 2);
    ASSERT_FALSE(p.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = p; req["caller_cwd"] = dir.path();
    req["op"] = "append_tracking"; req["date"] = "2026-07-10";
    QJsonArray rows; rows.append(trackingRow());
    req["rows"] = rows;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();

    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString().toStdString(), "not_v1");
    // The file is not mutated — no tracking table appended.
    EXPECT_FALSE(readAll(p).contains("roadmap tracking"));
}

// ANTS-3477 regression — a v1 / un-migrated file still accepts append_tracking
// (the guard keys on marker >= 2, so v1 stays the legacy triage-write path).
TEST(McpFeedbackLog, AppendTrackingStillValidOnV1File) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeMarked(dir.path(), "Proj_Ants_MCP_Feedback.md", 1);
    ASSERT_FALSE(p.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = p; req["caller_cwd"] = dir.path();
    req["op"] = "append_tracking"; req["date"] = "2026-07-10";
    QJsonArray rows; rows.append(trackingRow());
    req["rows"] = rows;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();

    EXPECT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_TRUE(readAll(p).contains("| an item | ANTS-1962 |"));
}

// ANTS-3426 — a fresh "_Ants"-suffixed project (fork checkout "DOOM_Ants")
// logging with `path` omitted must CREATE the de-doubled conventional file
// "DOOM_Ants_MCP_Feedback.md", NOT the doubled "DOOM_Ants_Ants_MCP_Feedback.md"
// — otherwise a naive append forks the feedback history into a wrong-named file.
TEST(McpFeedbackLog, CreatesDeDoubledNameForAntsLeaf) {
    QTemporaryDir root; ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(QDir(root.path()).mkdir("DOOM_Ants"));
    const QString caller = root.path() + "/DOOM_Ants";

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = caller;            // no "path" → derive at shared root
    req["op"] = "append_finding"; req["date"] = "2026-07-09";
    QJsonArray fs; fs.append(finding("Title", "what"));
    req["findings"] = fs;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool()) << "create should succeed";
    EXPECT_TRUE(env.value("created").toBool());
    EXPECT_TRUE(env.value("path_derived").toBool());
    // The de-doubled convention is created; the doubled fork must not appear.
    EXPECT_TRUE(QFileInfo::exists(root.path() + "/DOOM_Ants_MCP_Feedback.md"));
    EXPECT_FALSE(QFileInfo::exists(
        root.path() + "/DOOM_Ants_Ants_MCP_Feedback.md"));
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
    // append_tracking on absent file → not_found. ANTS-3366: the dir
    // holds a real sibling (F_Ants_MCP_Feedback.md, written above), so the
    // not_found envelope lists it under `candidates` + carries a `hint`.
    { const QString np = dir.path() + "/Gone_Ants_MCP_Feedback.md";
      QJsonObject r; r["path"] = np; r["caller_cwd"] = dir.path();
      r["op"] = "append_tracking";
      QJsonArray rows; QJsonObject row; row["item"] = "x";
      row["status"] = QString::fromUtf8(kClip); rows.append(row);
      r["rows"] = rows;
      const QJsonObject e = rc.cmdFeedbackLog(r).object();
      EXPECT_EQ(e.value("code").toString(), "not_found");
      ASSERT_TRUE(e.contains("candidates"));
      const QJsonArray cands = e.value("candidates").toArray();
      ASSERT_EQ(cands.size(), 1);
      EXPECT_EQ(cands.at(0).toString(), p);  // the F sibling
      EXPECT_TRUE(e.contains("hint")); }
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

// ANTS-3376 — append_finding with `path` omitted creates the conventional
// <caller_cwd-leaf>_Ants_MCP_Feedback.md at the shared root (parent of
// caller_cwd) and echoes path_derived:true. No filesystem hunt for a
// first-time log.
TEST(McpFeedbackLog, DerivesDefaultPathOnCreate) {
    QTemporaryDir root; ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(QDir(root.path()).mkdir("DOOM"));
    const QString caller = root.path() + "/DOOM";
    const QString expect = root.path() + "/DOOM_Ants_MCP_Feedback.md";

    RemoteControl rc(nullptr);
    QJsonObject req; req["caller_cwd"] = caller;   // no "path"
    req["op"] = "append_finding"; req["date"] = "2026-06-30";
    QJsonArray fs; fs.append(finding("First report")); req["findings"] = fs;

    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.value("created").toBool());
    EXPECT_TRUE(env.value("path_derived").toBool());
    EXPECT_EQ(env.value("path").toString(), expect);
    EXPECT_TRUE(QFile::exists(expect));
    EXPECT_TRUE(QString::fromUtf8(readAll(expect)).contains("First report"));

    // No resolvable caller_cwd + no path → still bad_args.
    QJsonObject bad; bad["op"] = "append_finding";
    QJsonArray fs2; fs2.append(finding("x")); bad["findings"] = fs2;
    EXPECT_EQ(rc.cmdFeedbackLog(bad).object().value("code").toString(),
              "bad_args");
}

// ANTS-4613 — a caller_cwd whose LEAF begins with a dot (`~/.claude`) derived
// `/home/ants/.claude_Ants_MCP_Feedback.md`: a name that looks like a hidden
// file, in the user's home directory, and NOT the file that project actually
// uses. The write returned ok:true / created:true / path_derived:true, so a
// session that omitted `path` silently started a SECOND feedback file nobody
// reads while the canonical one stayed untouched.
//
// ANTS-3714 already settled the spec half — a leaf starting with a dot cannot
// match the authoritative `*_Ants_MCP_Feedback.md` glob — and the verb kept
// deriving one anyway. Refuse and make the caller name the file: there is no
// correct name to guess here, and success plus creation reads as success.
TEST(McpFeedbackLog, RefusesToDeriveFromADotLeaf) {
    QTemporaryDir root; ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(QDir(root.path()).mkdir(".claude"));
    const QString caller = root.path() + "/.claude";

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = caller;            // no "path" → would derive
    req["op"] = "append_finding"; req["date"] = "2026-08-21";
    QJsonArray fs; fs.append(finding("Title", "what"));
    req["findings"] = fs;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();

    EXPECT_FALSE(env.value("ok").toBool())
        << "ANTS-4613: a dot leaf has no derivable feedback name";
    EXPECT_EQ(env.value("code").toString().toStdString(), "bad_args");
    EXPECT_FALSE(QFileInfo::exists(root.path() + "/.claude_Ants_MCP_Feedback.md"))
        << "ANTS-4613: the stray dotfile-adjacent sibling must not be created";
}

// ANTS-4613 — the refusal has to be actionable. A caller told only "no" will
// guess a name; the sibling files in the shared root are the candidates, and
// op:append_tracking already refuses this way (not_found + candidates + hint).
TEST(McpFeedbackLog, DotLeafRefusalOffersTheSiblings) {
    QTemporaryDir root; ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(QDir(root.path()).mkdir(".claude"));
    QFile sib(root.path() + "/claude_config_Ants_MCP_Feedback.md");
    ASSERT_TRUE(sib.open(QIODevice::WriteOnly));
    sib.write("# Ants MCP Feedback\n"); sib.close();

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root.path() + "/.claude";
    req["op"] = "append_finding"; req["date"] = "2026-08-21";
    QJsonArray fs; fs.append(finding("Title", "what"));
    req["findings"] = fs;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();

    ASSERT_FALSE(env.value("ok").toBool());
    const QJsonArray cands = env.value("candidates").toArray();
    ASSERT_EQ(cands.size(), 1) << "ANTS-4613: the shared root's sibling is the retry";
    EXPECT_TRUE(cands.at(0).toString().endsWith(
        "claude_config_Ants_MCP_Feedback.md"));
}

// ANTS-4613 over-reach guard — only the LEAF matters. A perfectly ordinary
// project that merely SITS under a dotted parent still derives, because its own
// name is a fine feedback stem.
TEST(McpFeedbackLog, DottedParentDoesNotBlockDerivation) {
    QTemporaryDir root; ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(QDir(root.path()).mkpath(".hidden/Proj"));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = root.path() + "/.hidden/Proj";
    req["op"] = "append_finding"; req["date"] = "2026-08-21";
    QJsonArray fs; fs.append(finding("Title", "what"));
    req["findings"] = fs;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();

    ASSERT_TRUE(env.value("ok").toBool()) << "only the leaf is undecidable";
    EXPECT_TRUE(QFileInfo::exists(
        root.path() + "/.hidden/Proj_Ants_MCP_Feedback.md"));
}

// ANTS-3376 / ANTS-2227 — the dry_run preview propagates path_derived from
// the derived (omitted-path) resolution and writes nothing to disk.
TEST(McpFeedbackLog, DryRunPreviewCarriesPathDerivedNoWrite) {
    QTemporaryDir root; ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(QDir(root.path()).mkdir("RetroDB"));
    const QString caller = root.path() + "/RetroDB";
    const QString expect = root.path() + "/RetroDB_Ants_MCP_Feedback.md";

    RemoteControl rc(nullptr);
    QJsonObject req; req["caller_cwd"] = caller;   // no "path"
    req["op"] = "append_finding"; req["date"] = "2026-06-30";
    req["dry_run"] = true;
    QJsonArray fs; fs.append(finding("Preview only")); req["findings"] = fs;

    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.value("dry_run").toBool());
    EXPECT_TRUE(env.value("path_derived").toBool());   // flows into preview
    EXPECT_TRUE(env.value("created").toBool());         // would-create
    EXPECT_EQ(env.value("path").toString(), expect);
    EXPECT_GT(env.value("bytes_appended").toInt(), 0);
    EXPECT_FALSE(QFile::exists(expect));                // nothing written
}

// ANTS-3695 INV-1 — a Repro value is a shell transcript by nature, and a
// column-0 `#` in one used to render as an H1, ending the `###` finding
// block and orphaning the `- **Proposed ID:**` line the verb itself wrote.
// Continuation lines are now indented, so the block stays whole.
TEST(McpFeedbackLog, Ants3695ColumnZeroHashKeepsBlockIntact) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/P_Ants_MCP_Feedback.md";

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = p; req["caller_cwd"] = dir.path();
    req["op"] = "append_finding"; req["date"] = "2026-07-28";
    QJsonArray fs;
    fs.append(finding("Dot-leaf glob finding", "the glob misses dot-leaves",
                      "# list the feedback files\nls *_Feedback.md\n"
                      "# note the miss"));
    req["findings"] = fs;
    ASSERT_TRUE(rc.cmdFeedbackLog(req).object().value("ok").toBool());

    const QString body = QString::fromUtf8(readAll(p));
    const QStringList lines = body.split(QLatin1Char('\n'));
    // No pasted comment may sit at column 0 — that is what markdown reads
    // as a heading.
    for (const QString &l : lines)
        EXPECT_FALSE(l.startsWith(QStringLiteral("# list")) ||
                     l.startsWith(QStringLiteral("# note")))
            << "contributor text rendered flush-left: " << l.toStdString();

    const auto blocks = FeedbackFile::enumerateFindingBlocks(lines);
    ASSERT_EQ(blocks.size(), 1);
    EXPECT_NE(blocks.at(0).idLine0, -1)
        << "the Proposed ID line fell outside its own finding block";
}

// ANTS-3695 INV-2 — read-side hardening for files already corrupted before
// the write fix: a stray H1 inside a block is body text, while a real
// `# <date>` session heading still bounds it.
TEST(McpFeedbackLog, Ants3695StrayH1IsBodyRealSessionHeadingBounds) {
    const QStringList corrupt = QString(QStringLiteral(
        "# Ants MCP Feedback — P\n"
        "\n"
        "## 2026-07-28\n"
        "\n"
        "### A finding\n"
        "\n"
        "- **Repro:** run this:\n"
        "# rm -rf build\n"
        "- **Proposed ID:** _(maintainer to assign)_\n"
        "\n"
        "# 2026-07-29 — next session\n"
        "\n"
        "### Another finding\n"
        "\n"
        "- **Proposed ID:** ANTS-9999\n")).split(QLatin1Char('\n'));

    const auto blocks = FeedbackFile::enumerateFindingBlocks(corrupt);
    ASSERT_EQ(blocks.size(), 2);
    EXPECT_NE(blocks.at(0).idLine0, -1)
        << "a shell comment must not end the finding that contains it";
    EXPECT_EQ(blocks.at(0).idValue, QStringLiteral("_(maintainer to assign)_"));
    // The real session heading still bounds block 0 before block 1 starts.
    EXPECT_LT(blocks.at(0).extentEnd0, blocks.at(1).headingLine0 + 1);
    EXPECT_EQ(blocks.at(1).idValue, QStringLiteral("ANTS-9999"));
}
