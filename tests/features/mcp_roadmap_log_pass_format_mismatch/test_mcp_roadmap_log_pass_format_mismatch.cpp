// ANTS-2031 — feature-conformance test for roadmap_log's
// format_mismatch refusal on `#### Pass N.M` heading roadmaps.
// Behavioural via the *ForTest seams (mirrors mcp_roadmap_log_atomicity).

#include "remotecontrol.h"

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

// A pass-headings roadmap: ≥2 `#### Pass` headings + ≥2 `- **Status**:`
// markers, no ants-v1 emoji bullets — the sniffer's pass-headings shape.
const char *kPassRoadmap =
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

// ANTS-2048 — a pass-headings roadmap that ALSO contains stray GFM
// checkbox sub-tasks. RetroDB's real file had `- [ ]` sub-tasks under its
// passes, which flipped the format sniffer to github-task-list and made
// flip_batch return bullet_not_found instead of this format_mismatch. The
// strong 2+2 pass-headings signal must win over a lone checkbox.
const char *kPassRoadmapWithCheckbox =
    "# Project Passes\n"
    "\n"
    "## Active\n"
    "\n"
    "#### Pass 41.5 (CRITICAL, S) First pass\n"
    "- **Status**: in-progress\n"
    "- [ ] a stray sub-task checklist item\n"
    "\n"
    "#### Pass 41.12 (LOW, S) Second pass\n"
    "- **Status**: todo\n"
    "- [x] another stray checkbox\n";

// A normal ants-v1 roadmap (control). 📋 is U+1F4CB in UTF-8.
const char *kV1Roadmap =
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

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

bool seed(const QString &root, const char *body) {
    if (!writeFile(roadmapPath(root), QByteArray(body))) return false;
    return writeFile(QDir(root).filePath(QStringLiteral(".roadmap-counter")),
                     QByteArray("100\n"));
}

QJsonObject baseReq(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    return o;
}

QString code(const QJsonObject &o) {
    return o.value(QStringLiteral("code")).toString();
}

}  // namespace

// INV-2 — create_section still refuses format_mismatch on a pass-headings
// roadmap. ANTS-2126 § 6: append / append_batch / flip / flip_batch /
// annotate now WRITE (covered by mcp_roadmap_log_pass_writer); only
// create_section keeps refusing (out of scope), so the old INV-1/3/4 and
// the append_batch half of this INV-2 were removed.
TEST(McpRoadmapLogPassFormatMismatch, Inv2CreateSectionRefuses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPassRoadmap));
    RemoteControl rc(nullptr);

    QJsonObject cs = baseReq(tmp.path());
    cs[QStringLiteral("after_section")] = QStringLiteral("active");
    cs[QStringLiteral("level")]         = 3;
    cs[QStringLiteral("title")]         = QStringLiteral("New Section");
    const QJsonObject out = rc.cmdRoadmapLogCreateSectionForTest(cs).object();
    EXPECT_EQ(code(out), QStringLiteral("format_mismatch"));
    EXPECT_EQ(out.value(QStringLiteral("format")).toString(),
              QStringLiteral("pass-headings"));
}

// INV-6 (ANTS-2048) — a pass-headings roadmap carrying stray `- [ ]`
// checkbox sub-tasks is STILL detected as pass-headings (not gfm). Under
// ANTS-2126 flip_batch now WRITES on pass-headings, so the proof shifts:
// correct detection routes to the pass writer and flips PASS-41-5
// (format:"pass-headings", flipped_count 1). Had the lone checkbox
// flipped the sniffer to github-task-list, the GFM flip_batch would
// instead skip PASS-41-5 as bullet_not_found (format:"gfm",
// flipped_count 0) — the RetroDB hit this guards against.
TEST(McpRoadmapLogPassFormatMismatch, Inv6StrayCheckboxStillPassHeadings) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPassRoadmapWithCheckbox));
    RemoteControl rc(nullptr);

    QJsonObject fb = baseReq(tmp.path());
    fb[QStringLiteral("to_status")] = QStringLiteral("shipped");
    QJsonArray locators;
    QJsonObject loc;
    loc[QStringLiteral("id")] = QStringLiteral("PASS-41-5");
    locators.append(loc);
    fb[QStringLiteral("locators")] = locators;
    const QJsonObject out = rc.cmdRoadmapLogFlipBatchForTest(fb).object();

    EXPECT_EQ(out.value(QStringLiteral("format")).toString(),
              QStringLiteral("pass-headings"))
        << "ANTS-2048: a stray `- [ ]` must not flip the sniffer to gfm";
    EXPECT_EQ(out.value(QStringLiteral("flipped_count")).toInt(), 1)
        << "PASS-41-5 located + flipped via the pass-headings writer";
    EXPECT_TRUE(readFile(roadmapPath(tmp.path()))
                    .contains("- **Status**: done"))
        << "Pass 41.5's Status line rewritten in place";
}

// INV-5 — a normal ants-v1 roadmap still appends (no false refusal).
TEST(McpRoadmapLogPassFormatMismatch, Inv5V1RoadmapStillAppends) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kV1Roadmap));
    RemoteControl rc(nullptr);

    QJsonObject req = baseReq(tmp.path());
    req[QStringLiteral("section")]  = QStringLiteral("work-items");
    req[QStringLiteral("status")]   = QStringLiteral("planned");
    req[QStringLiteral("headline")] = QStringLiteral("Control item.");
    req[QStringLiteral("kind")]     = QStringLiteral("test");
    req[QStringLiteral("source")]   = QStringLiteral("ants-2031");
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();

    EXPECT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << "INV-5: ants-v1 append must not be falsely refused; code="
        << code(out).toStdString();
    EXPECT_NE(code(out), QStringLiteral("format_mismatch"));
}
