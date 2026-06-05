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

// INV-1 — append refuses with format_mismatch and does not mutate.
TEST(McpRoadmapLogPassFormatMismatch, Inv1AppendRefuses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPassRoadmap));
    const QByteArray before = readFile(roadmapPath(tmp.path()));

    RemoteControl rc(nullptr);
    QJsonObject req = baseReq(tmp.path());
    req[QStringLiteral("section")]  = QStringLiteral("active");
    req[QStringLiteral("status")]   = QStringLiteral("planned");
    req[QStringLiteral("headline")] = QStringLiteral("New item.");
    req[QStringLiteral("kind")]     = QStringLiteral("test");
    req[QStringLiteral("source")]   = QStringLiteral("ants-2031");
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(req).object();

    EXPECT_FALSE(out.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(code(out), QStringLiteral("format_mismatch"));
    EXPECT_EQ(out.value(QStringLiteral("format")).toString(),
              QStringLiteral("pass-headings"));
    EXPECT_FALSE(out.value(QStringLiteral("hint")).toString().isEmpty())
        << "INV-1: refusal carries an Edit-fallback hint";

    EXPECT_EQ(readFile(roadmapPath(tmp.path())), before)
        << "INV-1: file left unmutated";
}

// INV-2 — append_batch + create_section refuse identically.
TEST(McpRoadmapLogPassFormatMismatch, Inv2BatchAndCreateSectionRefuse) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPassRoadmap));
    RemoteControl rc(nullptr);

    QJsonObject batch = baseReq(tmp.path());
    batch[QStringLiteral("section")] = QStringLiteral("active");
    QJsonArray bullets;
    QJsonObject b;
    b[QStringLiteral("status")]   = QStringLiteral("planned");
    b[QStringLiteral("headline")] = QStringLiteral("Batch item.");
    b[QStringLiteral("kind")]     = QStringLiteral("test");
    b[QStringLiteral("source")]   = QStringLiteral("ants-2031");
    bullets.append(b);
    batch[QStringLiteral("bullets")] = bullets;
    EXPECT_EQ(code(rc.cmdRoadmapLogAppendBatchForTest(batch).object()),
              QStringLiteral("format_mismatch"));

    QJsonObject cs = baseReq(tmp.path());
    cs[QStringLiteral("after_section")] = QStringLiteral("active");
    cs[QStringLiteral("level")]         = 3;
    cs[QStringLiteral("title")]         = QStringLiteral("New Section");
    EXPECT_EQ(code(rc.cmdRoadmapLogCreateSectionForTest(cs).object()),
              QStringLiteral("format_mismatch"));
}

// INV-3 — flip + annotate refuse with format_mismatch.
TEST(McpRoadmapLogPassFormatMismatch, Inv3FlipAndAnnotateRefuse) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPassRoadmap));
    RemoteControl rc(nullptr);

    QJsonObject flip = baseReq(tmp.path());
    flip[QStringLiteral("op")]        = QStringLiteral("flip");
    flip[QStringLiteral("id")]        = QStringLiteral("PASS-1-1");
    flip[QStringLiteral("to_status")] = QStringLiteral("shipped");
    EXPECT_EQ(code(rc.cmdRoadmapLogFlipForTest(flip).object()),
              QStringLiteral("format_mismatch"));

    QJsonObject ann = baseReq(tmp.path());
    ann[QStringLiteral("op")]   = QStringLiteral("annotate");
    ann[QStringLiteral("id")]   = QStringLiteral("PASS-1-1");
    ann[QStringLiteral("note")] = QStringLiteral("a note.");
    EXPECT_EQ(code(rc.cmdRoadmapLogFlipForTest(ann).object()),
              QStringLiteral("format_mismatch"));
}

// INV-4 — flip_batch refuses with format_mismatch.
TEST(McpRoadmapLogPassFormatMismatch, Inv4FlipBatchRefuses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seed(tmp.path(), kPassRoadmap));
    RemoteControl rc(nullptr);

    QJsonObject fb = baseReq(tmp.path());
    fb[QStringLiteral("to_status")] = QStringLiteral("shipped");
    QJsonArray locators;
    QJsonObject loc;
    loc[QStringLiteral("id")] = QStringLiteral("PASS-1-1");
    locators.append(loc);
    fb[QStringLiteral("locators")] = locators;
    EXPECT_EQ(code(rc.cmdRoadmapLogFlipBatchForTest(fb).object()),
              QStringLiteral("format_mismatch"));
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
