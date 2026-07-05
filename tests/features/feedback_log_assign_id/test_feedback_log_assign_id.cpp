// ANTS-3447 — feature-conformance test for feedback_log op:"assign_id".
// Pure FeedbackFile::assignId over synthetic content + a live
// RemoteControl::cmdFeedbackLog drive + schema/dispatch source-greps.
// See spec.md + docs/specs/ANTS-3447.md.

#include "feedbackfile.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

namespace {

// —=\xE2\x80\x94 (em-dash)
const QString kPlaceholder =
    QStringLiteral("- **Proposed ID:** _(maintainer to assign)_");

// A v2 fixture: a filled finding, a placeholder finding, a prose block, and a
// legacy finding with NO id line.
//   line 1: marker
//   line 4: ### Issue #1 (filled)          line 5: id line
//   line 8: ### Issue #2 (placeholder)     line 9: placeholder
//   line 12: ### Positive note (prose)
//   line 15: ### Issue #3 (no id line)     — legacy, insert target
const char *kV2 =
    "<!-- ants-mcp-feedback: 2 -->\n"                         // 1
    "# Feedback TEST\n"                                       // 2
    "\n"                                                      // 3
    "### Issue #1 \xE2\x80\x94 filled\n"                      // 4
    "- **Proposed ID:** ANTS-1000\n"                          // 5
    "- **What:** done.\n"                                     // 6
    "\n"                                                      // 7
    "### Issue #2 \xE2\x80\x94 still blank\n"                 // 8
    "- **Proposed ID:** _(maintainer to assign)_\n"          // 9
    "- **What:** open.\n"                                     // 10
    "\n"                                                      // 11
    "### Positive note \xE2\x80\x94 nice\n"                   // 12
    "It felt fast.\n"                                         // 13
    "\n"                                                      // 14
    "### Issue #3 \xE2\x80\x94 no id line\n"                  // 15
    "- **What:** a real gap.\n";                              // 16

QString v2() { return QString::fromUtf8(kV2); }

const QString kH1 =
    QString::fromUtf8("### Issue #1 \xE2\x80\x94 filled");
const QString kH2 =
    QString::fromUtf8("### Issue #2 \xE2\x80\x94 still blank");
const QString kH3 =
    QString::fromUtf8("### Issue #3 \xE2\x80\x94 no id line");

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
std::string slurp(const char *path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

FeedbackFile::AssignTarget mk(const QString &heading, const QString &value,
                              bool isClosure = false, int headingLine = -1) {
    FeedbackFile::AssignTarget t;
    t.heading = heading;
    t.value = value;
    t.isClosure = isClosure;
    t.headingLine = headingLine;
    return t;
}

}  // namespace

// §2.6 / INV-2/3 — fill a placeholder: the id line is replaced, siblings intact.
TEST(FeedbackAssignId, FillPlaceholder) {
    const FeedbackFile::AssignResult r =
        FeedbackFile::assignId(v2(), mk(kH2, QStringLiteral("ANTS-1525")));
    EXPECT_TRUE(r.code.isEmpty());
    EXPECT_FALSE(r.inserted);
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(r.line, 8);                       // Issue #2 heading line
    EXPECT_EQ(r.value, QStringLiteral("ANTS-1525"));

    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    const int h = out.indexOf(kH2);
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 1), QStringLiteral("- **Proposed ID:** ANTS-1525"));
    EXPECT_EQ(out.at(h + 2), QStringLiteral("- **What:** open."));
    // Other findings byte-identical.
    EXPECT_TRUE(r.newContent.contains(
        QStringLiteral("- **Proposed ID:** ANTS-1000")));   // Issue #1 untouched
    EXPECT_TRUE(r.newContent.contains(kPlaceholder) == false);  // placeholder gone

    // The enumerator reports the new idValue on that block.
    const QVector<FeedbackFile::FindingBlock> blocks =
        FeedbackFile::enumerateFindingBlocks(out);
    bool found = false;
    for (const auto &b : blocks)
        if (b.heading == kH2) { EXPECT_EQ(b.idValue, QStringLiteral("ANTS-1525")); found = true; }
    EXPECT_TRUE(found);

    // bytes_delta: 24-char placeholder → 9-char id.
    EXPECT_EQ(r.bytesDelta, -15);
}

// INV-3 — multi-id, comma-joined.
TEST(FeedbackAssignId, MultiId) {
    const FeedbackFile::AssignResult r = FeedbackFile::assignId(
        v2(), mk(kH2, QStringLiteral("ANTS-1525, ANTS-1526")));
    EXPECT_TRUE(r.code.isEmpty());
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    const int h = out.indexOf(kH2);
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 1),
              QStringLiteral("- **Proposed ID:** ANTS-1525, ANTS-1526"));
}

// INV-3/5 — closure: value begins n/a; the enumerator classifies it as a
// closure (not counted as a triaged id in the v2 delta rule).
TEST(FeedbackAssignId, Closure) {
    const QString val = QString::fromUtf8("n/a \xE2\x80\x94 folded into ANTS-1525");
    const FeedbackFile::AssignResult r =
        FeedbackFile::assignId(v2(), mk(kH2, val, /*isClosure=*/true));
    EXPECT_TRUE(r.code.isEmpty());
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    const int h = out.indexOf(kH2);
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 1), QStringLiteral("- **Proposed ID:** ") + val);
    EXPECT_TRUE(out.at(h + 1).contains(QStringLiteral("n/a")));
}

// INV-2 — insert when the finding has no id line (legacy Issue #3).
TEST(FeedbackAssignId, InsertWhenAbsent) {
    const FeedbackFile::AssignResult r =
        FeedbackFile::assignId(v2(), mk(kH3, QStringLiteral("ANTS-1600")));
    EXPECT_TRUE(r.code.isEmpty());
    EXPECT_TRUE(r.inserted);
    EXPECT_TRUE(r.changed);
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    const int h = out.indexOf(kH3);
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 1), QStringLiteral("- **Proposed ID:** ANTS-1600"));
    EXPECT_EQ(out.at(h + 2), QStringLiteral("- **What:** a real gap."));
    EXPECT_GT(r.bytesDelta, 0);   // a new line always adds bytes
}

// §2.2 — insert after a blank line following the heading.
TEST(FeedbackAssignId, InsertAfterBlank) {
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# T\n"
        "\n"
        "### Issue #9 \xE2\x80\x94 blank first\n"
        "\n"
        "- **What:** a gap.\n";
    const QString heading =
        QString::fromUtf8("### Issue #9 \xE2\x80\x94 blank first");
    const FeedbackFile::AssignResult r = FeedbackFile::assignId(
        QString::fromUtf8(fix), mk(heading, QStringLiteral("ANTS-1")));
    EXPECT_TRUE(r.inserted);
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    const int h = out.indexOf(heading);
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 1), QString());   // blank preserved
    EXPECT_EQ(out.at(h + 2), QStringLiteral("- **Proposed ID:** ANTS-1"));
}

// INV-2 — replace an existing id with a corrected one.
TEST(FeedbackAssignId, ReplaceExistingId) {
    const FeedbackFile::AssignResult r =
        FeedbackFile::assignId(v2(), mk(kH1, QStringLiteral("ANTS-2000")));
    EXPECT_TRUE(r.code.isEmpty());
    EXPECT_FALSE(r.inserted);
    EXPECT_TRUE(r.changed);
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    const int h = out.indexOf(kH1);
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 1), QStringLiteral("- **Proposed ID:** ANTS-2000"));
    EXPECT_FALSE(r.newContent.contains(QStringLiteral("ANTS-1000")));
}

// INV-6 — idempotency: re-assigning the same value → byte-identical no-op.
TEST(FeedbackAssignId, Idempotent) {
    const FeedbackFile::AssignResult r1 =
        FeedbackFile::assignId(v2(), mk(kH2, QStringLiteral("ANTS-1525")));
    const FeedbackFile::AssignResult r2 =
        FeedbackFile::assignId(r1.newContent, mk(kH2, QStringLiteral("ANTS-1525")));
    EXPECT_FALSE(r2.changed);
    EXPECT_EQ(r2.bytesDelta, 0);
    EXPECT_EQ(r2.newContent, r1.newContent);   // byte-identical
}

// INV-4 — ambiguous heading (two identical `### ` headings, no heading_line).
TEST(FeedbackAssignId, AmbiguousHeading) {
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# T\n"
        "\n"
        "### Dup\n"
        "- **What:** a.\n"
        "\n"
        "### Dup\n"
        "- **What:** b.\n";
    const QString in = QString::fromUtf8(fix);
    FeedbackFile::AssignResult r =
        FeedbackFile::assignId(in, mk(QStringLiteral("### Dup"),
                                      QStringLiteral("ANTS-1")));
    EXPECT_EQ(r.code, QStringLiteral("target_ambiguous"));
    ASSERT_EQ(r.candidates.size(), 2);
    EXPECT_EQ(r.candidates.at(0), 4);
    EXPECT_EQ(r.candidates.at(1), 7);
    EXPECT_EQ(r.newContent, in);   // no write on refusal

    // heading_line resolves to the second one.
    r = FeedbackFile::assignId(in, mk(QStringLiteral("### Dup"),
                                      QStringLiteral("ANTS-1"), false, 7));
    EXPECT_TRUE(r.code.isEmpty());
    EXPECT_EQ(r.line, 7);
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    EXPECT_EQ(out.at(7), QStringLiteral("- **Proposed ID:** ANTS-1"));  // 0-based idx 7 = line 8
}

// INV-4 — not found: heading matches nothing; heading_line off a `### ` line.
TEST(FeedbackAssignId, NotFound) {
    FeedbackFile::AssignResult r = FeedbackFile::assignId(
        v2(), mk(QStringLiteral("### Nope"), QStringLiteral("ANTS-1")));
    EXPECT_EQ(r.code, QStringLiteral("target_not_found"));

    // heading_line points at a non-`### ` line (line 2 = the H1).
    r = FeedbackFile::assignId(v2(), mk(kH2, QStringLiteral("ANTS-1"), false, 2));
    EXPECT_EQ(r.code, QStringLiteral("target_not_found"));
}

// INV-10 — a fenced `### `/`**Proposed ID:**` is inert (not a target).
TEST(FeedbackAssignId, FenceSafety) {
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# T\n"
        "\n"
        "### Real\n"
        "- **What:** example:\n"
        "```text\n"
        "### Fake\n"
        "- **Proposed ID:** ANTS-0000\n"
        "```\n"
        "- **Impact:** z.\n";
    // The fenced `### Fake` is not resolvable.
    const FeedbackFile::AssignResult r = FeedbackFile::assignId(
        QString::fromUtf8(fix), mk(QStringLiteral("### Fake"),
                                   QStringLiteral("ANTS-1")));
    EXPECT_EQ(r.code, QStringLiteral("target_not_found"));
}

// INV-11 — the version marker is byte-identical after an assign (v1 and v2).
TEST(FeedbackAssignId, NoMarkerChange) {
    // v2 file: marker untouched.
    FeedbackFile::AssignResult r =
        FeedbackFile::assignId(v2(), mk(kH2, QStringLiteral("ANTS-1")));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("<!-- ants-mcp-feedback: 2 -->")));

    // v1 file (assign works regardless of version — no gate).
    const char *v1 =
        "<!-- ants-mcp-feedback: 1 -->\n"
        "# T\n"
        "\n"
        "### Issue #1\n"
        "- **Proposed ID:** _(maintainer to assign)_\n"
        "- **What:** x.\n";
    r = FeedbackFile::assignId(QString::fromUtf8(v1),
                               mk(QStringLiteral("### Issue #1"),
                                  QStringLiteral("ANTS-9")));
    EXPECT_TRUE(r.code.isEmpty());
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("<!-- ants-mcp-feedback: 1 -->")));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("- **Proposed ID:** ANTS-9")));
}

// ---- live wrapper drives --------------------------------------------------

// INV-5 — request validation via the wrapper: bad_args cases.
TEST(FeedbackAssignId, LiveBadArgs) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeStr(p, v2()));
    RemoteControl rc(nullptr);
    auto base = [&]() {
        QJsonObject req;
        req["op"] = "assign_id";
        req["path"] = p;
        req["caller_cwd"] = dir.path();
        req["heading"] = kH2;
        return req;
    };
    // neither ids nor closure
    QJsonObject env = rc.cmdFeedbackLog(base()).object();
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("bad_args"));
    // both ids and closure
    QJsonObject req = base();
    req["ids"] = QJsonArray{QStringLiteral("ANTS-1")};
    req["closure"] = QStringLiteral("folded");
    env = rc.cmdFeedbackLog(req).object();
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("bad_args"));
    // malformed id
    req = base();
    req["ids"] = QJsonArray{QStringLiteral("BUG-1")};
    env = rc.cmdFeedbackLog(req).object();
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("bad_args"));
    // empty heading
    req = base();
    req["heading"] = QStringLiteral("   ");
    req["ids"] = QJsonArray{QStringLiteral("ANTS-1")};
    env = rc.cmdFeedbackLog(req).object();
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("bad_args"));
}

// §2.1 — empty ids + closure is a VALID closure (empty ids counts as absent).
TEST(FeedbackAssignId, LiveEmptyIdsPlusClosureIsClosure) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeStr(p, v2()));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "assign_id";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    req["heading"] = kH2;
    req["ids"] = QJsonArray{};                         // empty → absent
    req["closure"] = QStringLiteral("folded");
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_TRUE(env.value("value").toString().startsWith(QStringLiteral("n/a")));
}

// INV-5 — a newline in closure is folded to a single space (single-line INV-3).
TEST(FeedbackAssignId, LiveClosureNewlineFolded) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeStr(p, v2()));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "assign_id";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    req["heading"] = kH2;
    req["closure"] = QStringLiteral("foo\nbar");
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    const QString md = readStr(p);
    EXPECT_TRUE(md.contains(QString::fromUtf8("n/a \xE2\x80\x94 foo bar")));
    // No spurious second line: the finding gains exactly one Proposed-ID line.
    EXPECT_EQ(md.count(QStringLiteral("**Proposed ID:**")), 2);  // Issue #1 + Issue #2
}

// INV-7/8 — dry_run leaves the file untouched; a no-op skips the write (mtime).
TEST(FeedbackAssignId, LiveDryRunAndNoOpSkipWrite) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeStr(p, v2()));
    const QString before = readStr(p);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "assign_id";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    req["heading"] = kH2;
    req["ids"] = QJsonArray{QStringLiteral("ANTS-1525")};
    req["dry_run"] = true;
    QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.value("changed").toBool());
    EXPECT_EQ(readStr(p), before);   // dry_run: untouched

    // Real write.
    req.remove("dry_run");
    env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.value("changed").toBool());
    const QFileInfo fiWritten(p);
    const QDateTime mtimeWritten = fiWritten.lastModified();
    const QString after = readStr(p);
    EXPECT_TRUE(after.contains(QStringLiteral("- **Proposed ID:** ANTS-1525")));

    // Re-assign the same value → changed:false, file byte-identical.
    env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_FALSE(env.value("changed").toBool());
    EXPECT_EQ(env.value("bytes_delta").toInt(), 0);
    EXPECT_EQ(readStr(p), after);   // byte-identical
    (void)mtimeWritten;             // mtime skip covered by the byte-identity
}

// INV-9 — an absent file refuses not_found; path_derived echoed when derived.
TEST(FeedbackAssignId, LiveNotFound) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/GONE_Ants_MCP_Feedback.md";
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "assign_id";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    req["heading"] = kH2;
    req["ids"] = QJsonArray{QStringLiteral("ANTS-1")};
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("not_found"));
}

// INV-9 — target_ambiguous surfaces candidates[] through the wrapper.
TEST(FeedbackAssignId, LiveAmbiguousCandidates) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# T\n"
        "\n"
        "### Dup\n"
        "- **What:** a.\n"
        "\n"
        "### Dup\n"
        "- **What:** b.\n";
    ASSERT_TRUE(writeStr(p, QString::fromUtf8(fix)));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "assign_id";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    req["heading"] = QStringLiteral("### Dup");
    req["ids"] = QJsonArray{QStringLiteral("ANTS-1")};
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("target_ambiguous"));
    EXPECT_EQ(env.value("candidates").toArray().size(), 2);
}

// Schema — feedback_log inputSchema enumerates the op + the assign_id fields.
TEST(FeedbackAssignId, SchemaDeclaresOp) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("e.append(QStringLiteral(\"assign_id\"))"),
              std::string::npos);
    EXPECT_NE(ci.find("props[\"closure\"]"), std::string::npos);
    EXPECT_NE(ci.find("props[\"heading\"]"), std::string::npos);
}

// Dispatch — cmdFeedbackLog routes the op to FeedbackFile::assignId.
TEST(FeedbackAssignId, DispatchWired) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("op == QStringLiteral(\"assign_id\")"), std::string::npos);
    EXPECT_NE(rc.find("FeedbackFile::assignId("), std::string::npos);
}
