// ANTS-3421 — feature-conformance test for feedback_log
// op:"compact_shipped". Drives RemoteControl::cmdFeedbackLog live over a
// seeded in-QTemporaryDir fixture (m_main-independent; explicit path).
// See spec.md + docs/specs/ANTS-3421.md. Pre-fix the op enum lacks
// compact_shipped, so cmdFeedbackLog returns bad_mode and every collapse
// assertion below fails.

#include "feedbackfile.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

// Fixture: 📋=\xF0\x9F\x93\x8B  ✅=\xE2\x9C\x85  →=\xE2\x86\x92
const char *kFixture =
    "<!-- ants-mcp-feedback: 1 -->\n"
    "# Ants MCP Feedback TEST\n"
    "\n"
    "> Contributor banner: read the tail, append below the last maintainer "
    "block.\n"
    "\n"
    "## A. Shipped finding one\n"
    "\n"
    "Multi-paragraph write-up for finding one, verbose enough that collapsing\n"
    "it reclaims bytes. Additional detail on line two.\n"
    "- **What:** something broke.\n"
    "- **Impact:** LOW.\n"
    "\n"
    "## B. Shipped finding two\n"
    "\n"
    "Write-up for finding two, also verbose enough to save bytes on collapse.\n"
    "- **What:** another thing.\n"
    "\n"
    "## C. Not shipped yet\n"
    "\n"
    "This finding maps to a planned id and must never be compacted.\n"
    "\n"
    "## D. Reopened finding\n"
    "\n"
    "This id was shipped then reopened; the last tracking row wins.\n"
    "\n"
    "## E. Multi-finding session (2026-07-01)\n"
    "\n"
    "Intro to a multi-finding session block.\n"
    "\n"
    "### Finding A\n"
    "\n"
    "Body of finding A.\n"
    "\n"
    "### Finding B\n"
    "\n"
    "Body of finding B.\n"
    "\n"
    "## F. Already compacted\n"
    "\n"
    "\xE2\x86\x92 shipped ANTS-5006, confirmed TEST 2026-07-01 (write-up "
    "compacted, ANTS-3421)\n"
    "\n"
    "## G. Terse win\n"
    "\n"
    "ok.\n"
    "\n"
    "## Dup heading\n"
    "\n"
    "Body for the first dup block.\n"
    "\n"
    "## Dup heading\n"
    "\n"
    "Body for the second dup block.\n"
    "\n"
    "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking update "
    "(2026-07-01, maintainer)\n"
    "\n"
    "Triage note.\n"
    "\n"
    "| Item | IDs | Status |\n"
    "|---|---|---|\n"
    "| finding one | ANTS-5001 | \xE2\x9C\x85 |\n"
    "| finding two | ANTS-5002 | \xE2\x9C\x85 |\n"
    "| not shipped | ANTS-5003 | \xF0\x9F\x93\x8B |\n"
    "| reopened | ANTS-5004 | \xE2\x9C\x85 |\n"
    "| reopened again | ANTS-5004 | \xF0\x9F\x93\x8B |\n"
    "| multi | ANTS-5005 | \xE2\x9C\x85 |\n"
    "| already | ANTS-5006 | \xE2\x9C\x85 |\n"
    "| terse | ANTS-5008 | \xE2\x9C\x85 |\n"
    "| dup | ANTS-5007 | \xE2\x9C\x85 |\n"
    "\n"
    "End of 2026-07-01 maintainer roadmap-tracking update.\n"
    "\n"
    "## H. Un-triaged delta finding\n"
    "\n"
    "This is below the watermark; targeting it must refuse in_delta.\n";

QString readStr(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

bool writeStr(const QString &path, const QString &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray u = body.toUtf8();
    const bool ok = (f.write(u) == u.size());
    f.close();
    return ok;
}

// 1-based line number of the `occurrence`-th line trimming-equal to needle.
int lineOf(const QString &content, const QString &needle, int occurrence = 1) {
    const QStringList ls = content.split(QLatin1Char('\n'));
    int seen = 0;
    for (int i = 0; i < ls.size(); ++i)
        if (ls.at(i).trimmed() == needle.trimmed())
            if (++seen == occurrence) return i + 1;
    return -1;
}

QJsonObject tgt(const QString &heading, const QString &id, int headingLine = -1,
                const QString &session = {}, const QString &date = {}) {
    QJsonObject o;
    o["heading"] = heading;
    o["id"]      = id;
    if (headingLine > 0) o["heading_line"] = headingLine;
    if (!session.isEmpty()) o["session"] = session;
    if (!date.isEmpty())    o["date"]    = date;
    return o;
}

// Seed a fresh fixture in a temp dir; returns the file path.
QString seed(QTemporaryDir &dir) {
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    writeStr(p, QString::fromUtf8(kFixture));
    return p;
}

QJsonObject run(const QString &path, const QString &dir,
                const QJsonArray &targets, bool dryRun) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"]         = "compact_shipped";
    req["path"]       = path;
    req["caller_cwd"] = dir;
    req["targets"]    = targets;
    if (dryRun) req["dry_run"] = true;
    return rc.cmdFeedbackLog(req).object();
}

const char *kStub =
    "\xE2\x86\x92 shipped ";  // "→ shipped "

}  // namespace

// INV-1 — applied target's body → exact stub; heading verbatim.
TEST(FeedbackCompactShipped, Inv1CollapseShape) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    QJsonArray t; t.append(tgt("## A. Shipped finding one", "ANTS-5001", -1,
                               "MAME", "2026-07-01"));
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/false);
    ASSERT_TRUE(env.value("ok").toBool()) << env.value("error").toString().toStdString();
    EXPECT_EQ(env.value("applied_count").toInt(), 1);
    const QString md = readStr(p);
    EXPECT_TRUE(md.contains(QString::fromUtf8(
        "## A. Shipped finding one\n\n\xE2\x86\x92 shipped ANTS-5001, confirmed "
        "MAME 2026-07-01 (write-up compacted, ANTS-3421)\n\n## B. Shipped "
        "finding two")))
        << md.toStdString();
    EXPECT_FALSE(md.contains("something broke"));  // old body gone
}

// INV-2 — an id not ✅ in an effective row → not_shipped.
TEST(FeedbackCompactShipped, Inv2NotShipped) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    QJsonArray t; t.append(tgt("## C. Not shipped yet", "ANTS-5003"));
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/true);
    EXPECT_EQ(env.value("applied_count").toInt(), 0);
    const QJsonArray sk = env.value("skipped").toArray();
    ASSERT_EQ(sk.size(), 1);
    EXPECT_EQ(sk.at(0).toObject().value("code").toString(), "not_shipped");
}

// INV-3 — a heading below the watermark → in_delta.
TEST(FeedbackCompactShipped, Inv3InDelta) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    QJsonArray t; t.append(tgt("## H. Un-triaged delta finding", "ANTS-5001"));
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/true);
    EXPECT_EQ(env.value("skipped").toArray().at(0).toObject().value("code")
                  .toString(), "in_delta");
}

// INV-4 — a maintainer tracking block → maintainer_block.
TEST(FeedbackCompactShipped, Inv4MaintainerBlock) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    QJsonArray t; t.append(tgt(QString::fromUtf8(
        "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking update "
        "(2026-07-01, maintainer)"), "ANTS-5001"));
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/true);
    EXPECT_EQ(env.value("skipped").toArray().at(0).toObject().value("code")
                  .toString(), "maintainer_block");
}

// INV-5 — heading resolution: ambiguous / disambiguated / non-boundary line.
TEST(FeedbackCompactShipped, Inv5Resolution) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    const QString content = QString::fromUtf8(kFixture);

    // Duplicated heading, no heading_line → ambiguous (+candidates).
    { QJsonArray t; t.append(tgt("## Dup heading", "ANTS-5007"));
      const QJsonObject env = run(p, dir.path(), t, true);
      const QJsonObject s = env.value("skipped").toArray().at(0).toObject();
      EXPECT_EQ(s.value("code").toString(), "target_ambiguous");
      EXPECT_EQ(s.value("candidates").toArray().size(), 2); }

    // With the 2nd occurrence's heading_line → resolves + applies.
    const int dup2 = lineOf(content, "## Dup heading", 2);
    { QJsonArray t; t.append(tgt("## Dup heading", "ANTS-5007", dup2));
      const QJsonObject env = run(p, dir.path(), t, true);
      EXPECT_EQ(env.value("applied_count").toInt(), 1);
      EXPECT_EQ(env.value("outcomes").toArray().at(0).toObject()
                    .value("start_line").toInt(), dup2); }

    // heading_line pointing at a non-boundary (body) line → target_not_found.
    const int bodyLn = lineOf(content, "Body for the second dup block.");
    { QJsonArray t; t.append(tgt("## Dup heading", "ANTS-5007", bodyLn));
      const QJsonObject env = run(p, dir.path(), t, true);
      EXPECT_EQ(env.value("skipped").toArray().at(0).toObject().value("code")
                    .toString(), "target_not_found"); }

    // No matching heading at all → target_not_found.
    { QJsonArray t; t.append(tgt("## Nonexistent heading", "ANTS-5007"));
      const QJsonObject env = run(p, dir.path(), t, true);
      EXPECT_EQ(env.value("skipped").toArray().at(0).toObject().value("code")
                    .toString(), "target_not_found"); }
}

// INV-6 — parse() mappedIds / delta / trackingRows byte-identical.
TEST(FeedbackCompactShipped, Inv6ParsePreservation) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    const FeedbackFile::ParseResult before =
        FeedbackFile::parse(readStr(p));
    QJsonArray t; t.append(tgt("## A. Shipped finding one", "ANTS-5001"));
    run(p, dir.path(), t, /*dryRun=*/false);
    const FeedbackFile::ParseResult after =
        FeedbackFile::parse(readStr(p));
    EXPECT_EQ(before.mappedIds, after.mappedIds);
    EXPECT_EQ(before.delta, after.delta);
    ASSERT_EQ(before.trackingRows.size(), after.trackingRows.size());
    for (int i = 0; i < before.trackingRows.size(); ++i) {
        EXPECT_EQ(before.trackingRows.at(i).ids, after.trackingRows.at(i).ids);
        EXPECT_EQ(before.trackingRows.at(i).status,
                  after.trackingRows.at(i).status);
    }
}

// INV-7 — idempotent: a stub body → already_compacted; re-run is a no-op.
TEST(FeedbackCompactShipped, Inv7Idempotent) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);

    // The pre-seeded stub block F refuses already_compacted.
    { QJsonArray t; t.append(tgt("## F. Already compacted", "ANTS-5006"));
      const QJsonObject env = run(p, dir.path(), t, true);
      EXPECT_EQ(env.value("skipped").toArray().at(0).toObject().value("code")
                    .toString(), "already_compacted"); }

    // Compact B for real, then re-run — second pass applies 0, bytes stable.
    QJsonArray t; t.append(tgt("## B. Shipped finding two", "ANTS-5002"));
    run(p, dir.path(), t, false);
    const QByteArray afterFirst = readStr(p).toUtf8();
    const QJsonObject env2 = run(p, dir.path(), t, false);
    EXPECT_EQ(env2.value("applied_count").toInt(), 0);
    EXPECT_EQ(env2.value("skipped").toArray().at(0).toObject().value("code")
                  .toString(), "already_compacted");
    EXPECT_EQ(readStr(p).toUtf8(), afterFirst);
}

// INV-8 — atomic + partial + dry_run leaves disk untouched.
TEST(FeedbackCompactShipped, Inv8PartialDryRun) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    const QByteArray original = readStr(p).toUtf8();
    QJsonArray t;
    t.append(tgt("## A. Shipped finding one", "ANTS-5001"));  // valid
    t.append(tgt("## C. Not shipped yet", "ANTS-5003"));       // not_shipped
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/true);
    EXPECT_EQ(env.value("applied_count").toInt(), 1);
    EXPECT_EQ(env.value("outcomes").toArray().size(), 1);
    EXPECT_EQ(env.value("skipped").toArray().size(), 1);
    EXPECT_GT(env.value("bytes_saved").toInt(), 0);
    EXPECT_EQ(readStr(p).toUtf8(), original);  // dry_run wrote nothing
}

// INV-9 — bytes_saved signed sum; a terse block is ≤ 0 yet still applied.
TEST(FeedbackCompactShipped, Inv9BytesSaved) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    { QJsonArray t; t.append(tgt("## A. Shipped finding one", "ANTS-5001"));
      const QJsonObject env = run(p, dir.path(), t, true);
      const QJsonObject o = env.value("outcomes").toArray().at(0).toObject();
      EXPECT_GT(o.value("bytes_before").toInt(), o.value("bytes_after").toInt());
      EXPECT_EQ(o.value("bytes_saved").toInt(),
                o.value("bytes_before").toInt() - o.value("bytes_after").toInt());
      EXPECT_EQ(env.value("bytes_saved").toInt(), o.value("bytes_saved").toInt()); }

    // Terse block G ("ok.") — stub is longer, so saving ≤ 0 but still applied.
    { QJsonArray t; t.append(tgt("## G. Terse win", "ANTS-5008"));
      const QJsonObject env = run(p, dir.path(), t, true);
      EXPECT_EQ(env.value("applied_count").toInt(), 1);
      EXPECT_LE(env.value("bytes_saved").toInt(), 0); }
}

// INV-10 — bottom-up: two collapses don't corrupt the lower target's range.
TEST(FeedbackCompactShipped, Inv10BottomUp) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    QJsonArray t;
    t.append(tgt("## A. Shipped finding one", "ANTS-5001"));
    t.append(tgt("## B. Shipped finding two", "ANTS-5002"));
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/false);
    EXPECT_EQ(env.value("applied_count").toInt(), 2);
    const QString md = readStr(p);
    EXPECT_TRUE(md.contains("## A. Shipped finding one\n\n"
                            + QString::fromUtf8(kStub) + "ANTS-5001"));
    EXPECT_TRUE(md.contains("## B. Shipped finding two\n\n"
                            + QString::fromUtf8(kStub) + "ANTS-5002"));
    EXPECT_FALSE(md.contains("something broke"));
    EXPECT_FALSE(md.contains("another thing"));
}

// INV-11 — the H1 title/banner → title_block.
TEST(FeedbackCompactShipped, Inv11TitleBlock) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    QJsonArray t; t.append(tgt("# Ants MCP Feedback TEST", "ANTS-5001"));
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/true);
    EXPECT_EQ(env.value("skipped").toArray().at(0).toObject().value("code")
                  .toString(), "title_block");
}

// INV-12 — gate order: below-watermark + un-shipped id → in_delta (gate 4).
TEST(FeedbackCompactShipped, Inv12GateOrder) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    QJsonArray t; t.append(tgt("## H. Un-triaged delta finding", "ANTS-5003"));
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/true);
    EXPECT_EQ(env.value("skipped").toArray().at(0).toObject().value("code")
                  .toString(), "in_delta");  // gate 4 precedes not_shipped
}

// INV-13 — two targets on the same block → both duplicate_target.
TEST(FeedbackCompactShipped, Inv13DuplicateTarget) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    const QByteArray original = readStr(p).toUtf8();
    QJsonArray t;
    t.append(tgt("## A. Shipped finding one", "ANTS-5001"));
    t.append(tgt("## A. Shipped finding one", "ANTS-5001"));
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/false);
    EXPECT_EQ(env.value("applied_count").toInt(), 0);
    const QJsonArray sk = env.value("skipped").toArray();
    ASSERT_EQ(sk.size(), 2);
    EXPECT_EQ(sk.at(0).toObject().value("code").toString(), "duplicate_target");
    EXPECT_EQ(sk.at(1).toObject().value("code").toString(), "duplicate_target");
    EXPECT_EQ(readStr(p).toUtf8(), original);  // nothing collapsed
}

// INV-14 — a block with ≥2 ### findings → multi_finding.
TEST(FeedbackCompactShipped, Inv14MultiFinding) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    QJsonArray t; t.append(tgt("## E. Multi-finding session (2026-07-01)",
                               "ANTS-5005"));
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/true);
    EXPECT_EQ(env.value("skipped").toArray().at(0).toObject().value("code")
                  .toString(), "multi_finding");
}

// INV-15 — effective-status supersession: reopened ✅→📋 id → not_shipped.
TEST(FeedbackCompactShipped, Inv15Supersession) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    QJsonArray t; t.append(tgt("## D. Reopened finding", "ANTS-5004"));
    const QJsonObject env = run(p, dir.path(), t, /*dryRun=*/true);
    EXPECT_EQ(env.value("skipped").toArray().at(0).toObject().value("code")
                  .toString(), "not_shipped");
}

// Request-shape: empty targets / missing field / bad id → bad_args.
TEST(FeedbackCompactShipped, RequestShapeBadArgs) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    { const QJsonObject env = run(p, dir.path(), QJsonArray(), true);
      EXPECT_EQ(env.value("code").toString(), "bad_args"); }
    { QJsonArray t; QJsonObject o; o["heading"] = "## A. Shipped finding one";
      t.append(o);  // missing id
      const QJsonObject env = run(p, dir.path(), t, true);
      EXPECT_EQ(env.value("code").toString(), "bad_args"); }
    { QJsonArray t; t.append(tgt("## A. Shipped finding one", "FOO-1"));
      const QJsonObject env = run(p, dir.path(), t, true);
      EXPECT_EQ(env.value("code").toString(), "bad_args"); }
}

// Default breadcrumb author = project token (basename minus suffix); date today.
TEST(FeedbackCompactShipped, DefaultSessionToken) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    QJsonArray t; t.append(tgt("## A. Shipped finding one", "ANTS-5001"));
    run(p, dir.path(), t, /*dryRun=*/false);
    const QString md = readStr(p);
    static const QRegularExpression re(QString::fromUtf8(
        "\xE2\x86\x92 shipped ANTS-5001, confirmed TEST "
        "\\d{4}-\\d{2}-\\d{2} \\(write-up compacted, ANTS-3421\\)"));
    EXPECT_TRUE(re.match(md).hasMatch()) << md.toStdString();
}
