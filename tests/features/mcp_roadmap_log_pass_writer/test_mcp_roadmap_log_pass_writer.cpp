// ANTS-2126 — feature-conformance test for roadmap_log's pass-headings
// (`#### Pass N.M`) WRITER. Behavioural via the *ForTest seams (mirrors
// mcp_roadmap_log_pass_format_mismatch), plus pure-helper unit cases.
// Pre-fix, every "expect ok" assertion here fails (the ANTS-2031 code
// returned format_mismatch for all five ops).

#include "remotecontrol.h"
#include "roadmapdialog.h"
#include "passheadingwrite.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

namespace {

// A pass-headings roadmap (≥2 `#### Pass` + ≥2 `- **Status**:`), section
// slug "active".
const char *kPass =
    "# Project Passes\n"
    "\n"
    "## Active\n"
    "\n"
    "#### Pass 1.1 (CRITICAL, S) First pass\n"
    "- **Status**: in-progress\n"
    "- **Finding**: something.\n"
    "\n"
    "#### Pass 1.2 (LOW, S) Second pass\n"
    "- **Status**: todo\n";

// Emoji-form Status lines (INV-5).
const char *kPassEmoji =
    "# P\n"
    "\n"
    "## Active\n"
    "\n"
    "#### Pass 2.1 EmojiOnly\n"
    "- **Status**: \xF0\x9F\x93\x8B\n"          // 📋
    "\n"
    "#### Pass 2.2 EmojiKw\n"
    "- **Status**: \xE2\x9C\x85 Done\n";        // ✅ Done

// A pass with no Status line in the window (INV-6); two others carry one
// so the doc still sniffs as pass-headings.
const char *kPassNoStatus =
    "# P\n"
    "\n"
    "## Active\n"
    "\n"
    "#### Pass 3.1 HasStatus\n"
    "- **Status**: todo\n"
    "\n"
    "#### Pass 3.2 HasStatus2\n"
    "- **Status**: todo\n"
    "\n"
    "#### Pass 3.3 NoStatus\n"
    "- **Finding**: only a finding.\n";

// Two `- **Status**:` lines under one pass (INV-13).
const char *kPassTwoStatus =
    "# P\n"
    "\n"
    "## Active\n"
    "\n"
    "#### Pass 4.1 Two\n"
    "- **Status**: todo\n"
    "- **Status**: todo\n"
    "\n"
    "#### Pass 4.2 X\n"
    "- **Status**: todo\n";

// Empty section between two populated ones (INV-15).
const char *kPassEmptySection =
    "# P\n"
    "\n"
    "## Active\n"
    "\n"
    "#### Pass 5.1 A\n"
    "- **Status**: todo\n"
    "\n"
    "#### Pass 5.2 B\n"
    "- **Status**: todo\n"
    "\n"
    "## Empty\n"
    "\n"
    "## Other\n"
    "\n"
    "#### Pass 6.1 C\n"
    "- **Status**: todo\n";

// A normal ants-v1 roadmap (INV-16 control). 📋 is U+1F4CB.
const char *kV1 =
    "# Test Roadmap\n"
    "\n"
    "## Work Items\n"
    "\n"
    "- \xF0\x9F\x93\x8B [ANTS-0099] **Seed bullet.** Kind: chore.\n";

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QString rmPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}
QString counterPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral(".roadmap-counter"));
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// Pass roadmaps have no .roadmap-counter by default (INV-10).
bool seed(const QString &root, const char *body) {
    return writeFile(rmPath(root), QByteArray(body));
}

QJsonObject baseReq(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    return o;
}
QString code(const QJsonObject &o) {
    return o.value(QStringLiteral("code")).toString();
}
bool okOf(const QJsonObject &o) {
    return o.value(QStringLiteral("ok")).toBool();
}

// Find the parsed record for `id` in the on-disk roadmap.
RoadmapDialog::BulletRecord recById(const QString &root, const QString &id) {
    const QString md = QString::fromUtf8(readFile(rmPath(root)));
    for (const auto &r : RoadmapDialog::parseBullets(md))
        if (r.id == id) return r;
    return {};
}

const QString kTodo       = QString::fromUtf8("\xF0\x9F\x93\x8B"); // 📋
const QString kInProgress = QString::fromUtf8("\xF0\x9F\x9A\xA7"); // 🚧
const QString kDone       = QString::fromUtf8("\xE2\x9C\x85");     // ✅
const QString kDeferred   = QString::fromUtf8("\xF0\x9F\x92\xAD"); // 💭

}  // namespace

// ---- INV-2 / INV-12 — append writes a pass block; reader round-trips.
TEST(McpRoadmapLogPassWriter, Inv2AppendWritesPassBlock) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPass));

    RemoteControl rc(nullptr);
    QJsonObject req = baseReq(tmp.path());
    req[QStringLiteral("section")]  = QStringLiteral("active");
    req[QStringLiteral("status")]   = QStringLiteral("shipped");
    req[QStringLiteral("headline")] = QStringLiteral("Third pass.");
    req[QStringLiteral("pass")]     = QStringLiteral("1.3");
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();

    ASSERT_TRUE(okOf(out)) << "code=" << code(out).toStdString();
    EXPECT_EQ(out.value(QStringLiteral("id")).toString(),
              QStringLiteral("PASS-1-3"));
    EXPECT_EQ(out.value(QStringLiteral("format")).toString(),
              QStringLiteral("pass-headings"));

    const QByteArray after = readFile(rmPath(tmp.path()));
    EXPECT_TRUE(after.contains("#### Pass 1.3 Third pass."));
    EXPECT_TRUE(after.contains("- **Status**: done"));

    // INV-12 round-trip through the real reader.
    const auto rec = recById(tmp.path(), QStringLiteral("PASS-1-3"));
    EXPECT_EQ(rec.status, kDone);
    EXPECT_EQ(rec.headline, QStringLiteral("Third pass."));
}

// ---- INV-3 — append refuses bad_args (pass absent / malformed / no status).
TEST(McpRoadmapLogPassWriter, Inv3AppendRefusals) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPass));
    const QByteArray before = readFile(rmPath(tmp.path()));
    RemoteControl rc(nullptr);

    auto base = [&]() {
        QJsonObject r = baseReq(tmp.path());
        r[QStringLiteral("section")]  = QStringLiteral("active");
        r[QStringLiteral("status")]   = QStringLiteral("planned");
        r[QStringLiteral("headline")] = QStringLiteral("X.");
        return r;
    };

    QJsonObject noPass = base();  // pass absent
    EXPECT_EQ(code(rc.cmdRoadmapLogAppendForTest(noPass).object()),
              QStringLiteral("bad_args"));

    QJsonObject badPass = base();
    badPass[QStringLiteral("pass")] = QStringLiteral("1");  // malformed
    EXPECT_EQ(code(rc.cmdRoadmapLogAppendForTest(badPass).object()),
              QStringLiteral("bad_args"));

    QJsonObject noStatus = base();
    noStatus.remove(QStringLiteral("status"));
    noStatus[QStringLiteral("pass")] = QStringLiteral("1.9");
    EXPECT_EQ(code(rc.cmdRoadmapLogAppendForTest(noStatus).object()),
              QStringLiteral("bad_args"));

    EXPECT_EQ(readFile(rmPath(tmp.path())), before) << "file untouched";
}

// ---- INV-4 — flip by id rewrites only the Status line (byte-diff).
TEST(McpRoadmapLogPassWriter, Inv4FlipByIdSurgical) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPass));
    const QByteArray before = readFile(rmPath(tmp.path()));
    RemoteControl rc(nullptr);

    QJsonObject flip = baseReq(tmp.path());
    flip[QStringLiteral("op")]        = QStringLiteral("flip");
    flip[QStringLiteral("id")]        = QStringLiteral("PASS-1-2");
    flip[QStringLiteral("to_status")] = QStringLiteral("shipped");
    const QJsonObject out = rc.cmdRoadmapLogFlipForTest(flip).object();
    ASSERT_TRUE(okOf(out)) << "code=" << code(out).toStdString();

    const QByteArray after = readFile(rmPath(tmp.path()));
    EXPECT_EQ(QString::fromUtf8(after),
              QString::fromUtf8(before).replace(
                  QStringLiteral("- **Status**: todo"),
                  QStringLiteral("- **Status**: done")))
        << "only Pass 1.2's keyword-only Status line changes";
    EXPECT_EQ(recById(tmp.path(), QStringLiteral("PASS-1-2")).status, kDone);
}

// ---- INV-5 — flip preserves emoji-only / emoji+keyword style.
TEST(McpRoadmapLogPassWriter, Inv5FlipPreservesStyle) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPassEmoji));
    RemoteControl rc(nullptr);

    QJsonObject f1 = baseReq(tmp.path());
    f1[QStringLiteral("op")]        = QStringLiteral("flip");
    f1[QStringLiteral("id")]        = QStringLiteral("PASS-2-1");
    f1[QStringLiteral("to_status")] = QStringLiteral("in-progress");
    ASSERT_TRUE(okOf(rc.cmdRoadmapLogFlipForTest(f1).object()));

    QJsonObject f2 = baseReq(tmp.path());
    f2[QStringLiteral("op")]        = QStringLiteral("flip");
    f2[QStringLiteral("id")]        = QStringLiteral("PASS-2-2");
    f2[QStringLiteral("to_status")] = QStringLiteral("considered");
    ASSERT_TRUE(okOf(rc.cmdRoadmapLogFlipForTest(f2).object()));

    const QString after = QString::fromUtf8(readFile(rmPath(tmp.path())));
    // emoji-only stays emoji-only
    EXPECT_TRUE(after.contains(QStringLiteral("- **Status**: ") + kInProgress
                               + QStringLiteral("\n")));
    // emoji+keyword keeps both (canonical glyph + capitalised keyword)
    EXPECT_TRUE(after.contains(QStringLiteral("- **Status**: ") + kDeferred
                               + QStringLiteral(" Deferred")));
    EXPECT_EQ(recById(tmp.path(), QStringLiteral("PASS-2-1")).status, kInProgress);
    EXPECT_EQ(recById(tmp.path(), QStringLiteral("PASS-2-2")).status, kDeferred);
}

// ---- INV-6 — flip of a status-less pass inserts a Status line.
TEST(McpRoadmapLogPassWriter, Inv6FlipInsertsMissingStatus) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPassNoStatus));
    RemoteControl rc(nullptr);

    QJsonObject flip = baseReq(tmp.path());
    flip[QStringLiteral("op")]        = QStringLiteral("flip");
    flip[QStringLiteral("id")]        = QStringLiteral("PASS-3-3");
    flip[QStringLiteral("to_status")] = QStringLiteral("shipped");
    ASSERT_TRUE(okOf(rc.cmdRoadmapLogFlipForTest(flip).object()));

    const QString after = QString::fromUtf8(readFile(rmPath(tmp.path())));
    EXPECT_TRUE(after.contains(
        QStringLiteral("#### Pass 3.3 NoStatus\n- **Status**: done")));
    EXPECT_EQ(recById(tmp.path(), QStringLiteral("PASS-3-3")).status, kDone);
}

// ---- INV-7 — flip_batch flips N by id; unresolvable lands in skipped[].
TEST(McpRoadmapLogPassWriter, Inv7FlipBatchPartial) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPass));
    RemoteControl rc(nullptr);

    QJsonObject fb = baseReq(tmp.path());
    fb[QStringLiteral("to_status")] = QStringLiteral("shipped");
    QJsonArray locs;
    QJsonObject a; a[QStringLiteral("id")] = QStringLiteral("PASS-1-1"); locs.append(a);
    QJsonObject b; b[QStringLiteral("id")] = QStringLiteral("PASS-9-9"); locs.append(b);
    fb[QStringLiteral("locators")] = locs;
    const QJsonObject out = rc.cmdRoadmapLogFlipBatchForTest(fb).object();

    ASSERT_TRUE(okOf(out)) << "code=" << code(out).toStdString();
    EXPECT_EQ(out.value(QStringLiteral("flipped_count")).toInt(), 1);
    EXPECT_EQ(out.value(QStringLiteral("skipped_count")).toInt(), 1);
    EXPECT_EQ(recById(tmp.path(), QStringLiteral("PASS-1-1")).status, kDone);
}

// ---- INV-8 — annotate appends a body note; empty note refuses bad_args.
TEST(McpRoadmapLogPassWriter, Inv8Annotate) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPass));
    RemoteControl rc(nullptr);

    QJsonObject ann = baseReq(tmp.path());
    ann[QStringLiteral("op")]   = QStringLiteral("annotate");
    ann[QStringLiteral("id")]   = QStringLiteral("PASS-1-1");
    ann[QStringLiteral("note")] = QStringLiteral("- **Decision**: keep it.");
    ASSERT_TRUE(okOf(rc.cmdRoadmapLogFlipForTest(ann).object()));

    const QString after = QString::fromUtf8(readFile(rmPath(tmp.path())));
    EXPECT_TRUE(after.contains(QStringLiteral("- **Decision**: keep it.")));
    // status unchanged
    EXPECT_EQ(recById(tmp.path(), QStringLiteral("PASS-1-1")).status, kInProgress);

    QJsonObject empty = baseReq(tmp.path());
    empty[QStringLiteral("op")]   = QStringLiteral("annotate");
    empty[QStringLiteral("id")]   = QStringLiteral("PASS-1-1");
    empty[QStringLiteral("note")] = QStringLiteral("");
    EXPECT_EQ(code(rc.cmdRoadmapLogFlipForTest(empty).object()),
              QStringLiteral("bad_args"));
}

// ---- INV-10 — no pass write path mutates .roadmap-counter.
TEST(McpRoadmapLogPassWriter, Inv10NoCounterMutation) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPass));
    ASSERT_TRUE(writeFile(counterPath(tmp.path()), QByteArray("100\n")));

    RemoteControl rc(nullptr);
    QJsonObject req = baseReq(tmp.path());
    req[QStringLiteral("section")]  = QStringLiteral("active");
    req[QStringLiteral("status")]   = QStringLiteral("planned");
    req[QStringLiteral("headline")] = QStringLiteral("Counter-safe.");
    req[QStringLiteral("pass")]     = QStringLiteral("1.7");
    ASSERT_TRUE(okOf(rc.cmdRoadmapLogAppendForTest(req).object()));

    EXPECT_EQ(readFile(counterPath(tmp.path())), QByteArray("100\n"))
        << "INV-10: .roadmap-counter byte-identical";
}

// ---- INV-13 — flip targets the FIRST of two Status lines.
TEST(McpRoadmapLogPassWriter, Inv13FirstStatusOnly) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPassTwoStatus));
    RemoteControl rc(nullptr);

    QJsonObject flip = baseReq(tmp.path());
    flip[QStringLiteral("op")]        = QStringLiteral("flip");
    flip[QStringLiteral("id")]        = QStringLiteral("PASS-4-1");
    flip[QStringLiteral("to_status")] = QStringLiteral("shipped");
    ASSERT_TRUE(okOf(rc.cmdRoadmapLogFlipForTest(flip).object()));

    const QString after = QString::fromUtf8(readFile(rmPath(tmp.path())));
    EXPECT_TRUE(after.contains(QStringLiteral(
        "#### Pass 4.1 Two\n- **Status**: done\n- **Status**: todo")))
        << "first Status rewritten, second left as-is";
}

// ---- INV-14 — append_batch skip-and-continue; all-invalid leaves file.
TEST(McpRoadmapLogPassWriter, Inv14AppendBatchPartial) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPass));
    RemoteControl rc(nullptr);

    auto mk = [](const QString &pass, const QString &headline) {
        QJsonObject b;
        if (!pass.isEmpty()) b[QStringLiteral("pass")] = pass;
        b[QStringLiteral("status")]   = QStringLiteral("planned");
        b[QStringLiteral("headline")] = headline;
        return b;
    };

    QJsonObject batch = baseReq(tmp.path());
    batch[QStringLiteral("section")] = QStringLiteral("active");
    QJsonArray bs;
    bs.append(mk(QStringLiteral("1.5"), QStringLiteral("Valid.")));
    bs.append(mk(QString(),             QStringLiteral("No pass.")));  // invalid
    batch[QStringLiteral("bullets")] = bs;
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(batch).object();

    ASSERT_TRUE(okOf(out)) << "code=" << code(out).toStdString();
    EXPECT_EQ(out.value(QStringLiteral("applied_count")).toInt(), 1);
    EXPECT_EQ(out.value(QStringLiteral("skipped_count")).toInt(), 1);
    EXPECT_TRUE(readFile(rmPath(tmp.path())).contains("#### Pass 1.5 Valid."));

    // all-invalid batch → file untouched.
    const QByteArray before = readFile(rmPath(tmp.path()));
    QJsonObject allBad = baseReq(tmp.path());
    allBad[QStringLiteral("section")] = QStringLiteral("active");
    QJsonArray bad;
    bad.append(mk(QString(), QStringLiteral("Nope.")));
    allBad[QStringLiteral("bullets")] = bad;
    const QJsonObject out2 = rc.cmdRoadmapLogAppendBatchForTest(allBad).object();
    EXPECT_EQ(out2.value(QStringLiteral("applied_count")).toInt(), 0);
    EXPECT_EQ(readFile(rmPath(tmp.path())), before) << "all-invalid: untouched";
}

// ---- INV-15 — append into an empty section lands under its heading.
TEST(McpRoadmapLogPassWriter, Inv15EmptySection) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPassEmptySection));
    RemoteControl rc(nullptr);

    QJsonObject req = baseReq(tmp.path());
    req[QStringLiteral("section")]  = QStringLiteral("empty");
    req[QStringLiteral("status")]   = QStringLiteral("planned");
    req[QStringLiteral("headline")] = QStringLiteral("Into empty.");
    req[QStringLiteral("pass")]     = QStringLiteral("7.1");
    ASSERT_TRUE(okOf(rc.cmdRoadmapLogAppendForTest(req).object()));

    const QString after = QString::fromUtf8(readFile(rmPath(tmp.path())));
    const int emptyPos = after.indexOf(QStringLiteral("## Empty"));
    const int passPos  = after.indexOf(QStringLiteral("#### Pass 7.1 Into empty."));
    const int otherPos = after.indexOf(QStringLiteral("## Other"));
    ASSERT_GE(emptyPos, 0);
    ASSERT_GE(passPos, 0);
    ASSERT_GE(otherPos, 0);
    EXPECT_LT(emptyPos, passPos) << "block under ## Empty";
    EXPECT_LT(passPos, otherPos) << "block before ## Other";
}

// ---- INV-16 — a stray `pass` on a GFM/ants-v1 roadmap is ignored.
TEST(McpRoadmapLogPassWriter, Inv16StrayPassOnV1Ignored) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kV1));
    ASSERT_TRUE(writeFile(counterPath(tmp.path()), QByteArray("100\n")));
    RemoteControl rc(nullptr);

    QJsonObject req = baseReq(tmp.path());
    req[QStringLiteral("section")]  = QStringLiteral("work-items");
    req[QStringLiteral("status")]   = QStringLiteral("planned");
    req[QStringLiteral("headline")] = QStringLiteral("Control item.");
    req[QStringLiteral("kind")]     = QStringLiteral("test");
    req[QStringLiteral("source")]   = QStringLiteral("ants-2126");
    req[QStringLiteral("pass")]     = QStringLiteral("9.9");  // stray
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();

    ASSERT_TRUE(okOf(out)) << "code=" << code(out).toStdString();
    EXPECT_NE(out.value(QStringLiteral("format")).toString(),
              QStringLiteral("pass-headings"));
    EXPECT_FALSE(out.value(QStringLiteral("id")).toString()
                     .startsWith(QStringLiteral("PASS-")))
        << "ants-v1 append allocates an ANTS-NNNN id, not a PASS id";
}

// ---- INV-11 — passStatusKeyword is the total 4→keyword map.
TEST(McpRoadmapLogPassWriter, Inv11StatusKeywordMap) {
    EXPECT_EQ(PassHeadingWrite::passStatusKeyword(QStringLiteral("planned")),
              QStringLiteral("todo"));
    EXPECT_EQ(PassHeadingWrite::passStatusKeyword(QStringLiteral("in-progress")),
              QStringLiteral("in-progress"));
    EXPECT_EQ(PassHeadingWrite::passStatusKeyword(QStringLiteral("shipped")),
              QStringLiteral("done"));
    EXPECT_EQ(PassHeadingWrite::passStatusKeyword(QStringLiteral("considered")),
              QStringLiteral("deferred"));
    EXPECT_TRUE(PassHeadingWrite::passStatusKeyword(QStringLiteral("frob")).isEmpty());
}

// ---- Pure helper — id synthesis strips leading zeros, keeps sub-pass.
TEST(McpRoadmapLogPassWriter, IdFromDesignator) {
    EXPECT_EQ(PassHeadingWrite::passIdFromDesignator(QStringLiteral("43.05")),
              QStringLiteral("PASS-43-5"));
    EXPECT_EQ(PassHeadingWrite::passIdFromDesignator(QStringLiteral("7.1.B")),
              QStringLiteral("PASS-7-1-B"));
    EXPECT_TRUE(PassHeadingWrite::passIdFromDesignator(QStringLiteral("bad")).isEmpty());
}
