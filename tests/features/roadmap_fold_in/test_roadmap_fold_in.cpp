// Feature-conformance test for ANTS-1111 RoadmapFoldIn helpers +
// AuditEngine::templateRoadmapFoldInBlock. Extended in ANTS-1372 with
// counter-symlink-escape tests (ANTS-1342 sibling) and caller-cwd
// gate tests (R/G/S series, see docs/specs/ANTS-1372.md § 5).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "auditengine.h"
#include "remotecontrolgate.h"
#include "roadmapfoldin.h"

namespace {

// Write `body` to `<dir>/<name>` and return absolute path.
QString writeFile(const QString &dir, const QString &name,
                  const QByteArray &body) {
    const QString p = QDir(dir).filePath(name);
    QFile f(p);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
    return p;
}

QByteArray slurp(const QString &p) {
    QFile f(p);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

}  // namespace

// ---- INV-5: template emits the documented shape -----------------

TEST(RoadmapFoldIn, Inv5TemplateShape) {
    Finding f;
    f.checkId = "clazy-no-trivial-copyable-in-list";
    f.file = "src/foo.cpp";
    f.line = 42;
    f.message = "Some bug message that should become the theme.";
    QList<Finding> findings = {f};
    QList<int> ids = {1500};
    const QString out = AuditEngine::templateRoadmapFoldInBlock(
        findings, ids, "2026-05-13");
    EXPECT_TRUE(out.startsWith("### 🔍 Audit fold-in (2026-05-13)"));
    EXPECT_TRUE(out.contains("- 📋 [ANTS-1500]"));
    EXPECT_TRUE(out.contains("Kind: audit-fix."));
    EXPECT_TRUE(out.contains("Source: audit-2026-05-13."));
    EXPECT_TRUE(out.contains("Lanes: foo."));
    EXPECT_TRUE(out.contains("`src/foo.cpp:42`"));
}

TEST(RoadmapFoldIn, TemplateEmptyOnSizeMismatch) {
    Finding f;
    QList<Finding> findings = {f, f};
    QList<int> ids = {1};  // wrong size
    EXPECT_TRUE(AuditEngine::templateRoadmapFoldInBlock(
        findings, ids, "2026-05-13").isEmpty());
    EXPECT_TRUE(AuditEngine::templateRoadmapFoldInBlock(
        {}, {}, "2026-05-13").isEmpty());
}

// ---- INV-6: allocateIds returns N consecutive ints --------------

TEST(RoadmapFoldIn, Inv6AllocateConsecutive) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".roadmap-counter", "100\n");

    auto a = RoadmapFoldIn::allocateIds(tmp.path(), 3);
    ASSERT_EQ(a.size(), 3);
    EXPECT_EQ(a[0], 101);
    EXPECT_EQ(a[1], 102);
    EXPECT_EQ(a[2], 103);

    // Counter file post-write contains the new value.
    EXPECT_EQ(slurp(QDir(tmp.path()).filePath(".roadmap-counter")).trimmed(),
              QByteArray("103"));

    // Second call continues from new value.
    auto b = RoadmapFoldIn::allocateIds(tmp.path(), 2);
    ASSERT_EQ(b.size(), 2);
    EXPECT_EQ(b[0], 104);
    EXPECT_EQ(b[1], 105);
}

TEST(RoadmapFoldIn, AllocateZeroOrNegativeReturnsEmpty) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".roadmap-counter", "1\n");
    EXPECT_TRUE(RoadmapFoldIn::allocateIds(tmp.path(), 0).isEmpty());
    EXPECT_TRUE(RoadmapFoldIn::allocateIds(tmp.path(), -1).isEmpty());
}

TEST(RoadmapFoldIn, AllocateMissingCounterFails) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // No .roadmap-counter present — open() in lockExclusive creates one
    // empty, then read returns "" → toInt fails → empty list.
    EXPECT_TRUE(RoadmapFoldIn::allocateIds(tmp.path(), 1).isEmpty());
}

// ---- INV-8: insertBlock places block after named heading --------

TEST(RoadmapFoldIn, Inv8InsertAfterHeading) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray rm =
        "# Test ROADMAP\n"
        "\n"
        "## 0.7.88 — audit fold (target: 2026-05)\n"
        "\n"
        "### Existing subsection\n"
        "- existing bullet\n"
        "\n"
        "## 0.7.87 — shipped\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    const QString block =
        "### 🔍 Audit fold-in (2026-05-13)\n"
        "\n"
        "- 📋 [ANTS-9999] **Test bullet.**\n";
    EXPECT_TRUE(RoadmapFoldIn::insertBlock(
        tmp.path(),
        QStringLiteral("## 0.7.88 — audit fold (target: 2026-05)"),
        block));

    const QByteArray after = slurp(QDir(tmp.path()).filePath("ROADMAP.md"));
    // Block must appear AFTER the target heading and BEFORE the next
    // section.
    const int hPos = after.indexOf("## 0.7.88 — audit fold");
    const int bPos = after.indexOf("### 🔍 Audit fold-in (2026-05-13)");
    const int nextPos = after.indexOf("## 0.7.87 — shipped");
    ASSERT_GT(hPos, -1);
    ASSERT_GT(bPos, hPos);
    ASSERT_GT(nextPos, bPos);
}

TEST(RoadmapFoldIn, Inv8InsertHeadingNotFoundReturnsFalse) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray rm = "# Test\n## existing\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    const QByteArray before = slurp(QDir(tmp.path()).filePath("ROADMAP.md"));
    EXPECT_FALSE(RoadmapFoldIn::insertBlock(
        tmp.path(),
        QStringLiteral("## non-existent heading"),
        QStringLiteral("### foo\n")));
    const QByteArray after = slurp(QDir(tmp.path()).filePath("ROADMAP.md"));
    EXPECT_EQ(before, after);  // unchanged
}

TEST(RoadmapFoldIn, InsertBlockTrimsTrailingNewlineFromHeading) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QByteArray rm = "## 0.7.88 — t (target: 2026-05)\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    EXPECT_TRUE(RoadmapFoldIn::insertBlock(
        tmp.path(),
        QStringLiteral("## 0.7.88 — t (target: 2026-05)\n"),  // trailing \n
        QStringLiteral("### foo\n")));
}

// ---- INV-8b: findActiveReleaseHeading prefers in-flight ---------

TEST(RoadmapFoldIn, Inv8bFindPrefersInflight) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray rm =
        "# Roadmap\n"
        "\n"
        "## Distribution-adoption overview\n"
        "\n"
        "## 0.7.88 — audit fold (target: 2026-05)\n"
        "\n"
        "## 0.7.87 — MCP pack — shipped 2026-05-13\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    const QString h = RoadmapFoldIn::findActiveReleaseHeading(tmp.path());
    EXPECT_EQ(h, QStringLiteral("## 0.7.88 — audit fold (target: 2026-05)"));
}

TEST(RoadmapFoldIn, Inv8bFindFallsBackToShipped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray rm =
        "# Roadmap\n"
        "\n"
        "## Some prose section\n"
        "\n"
        "## 0.7.87 — MCP pack — shipped 2026-05-13\n"
        "\n"
        "## 0.7.86 — older — shipped 2026-05-10\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    const QString h = RoadmapFoldIn::findActiveReleaseHeading(tmp.path());
    EXPECT_EQ(h, QStringLiteral("## 0.7.87 — MCP pack — shipped 2026-05-13"));
}

TEST(RoadmapFoldIn, Inv8bFindEmptyForNoMatches) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray rm =
        "# Roadmap\n"
        "\n"
        "## Distribution-adoption overview\n"
        "\n"
        "## Per-store publication playbook\n";
    writeFile(tmp.path(), "ROADMAP.md", rm);

    EXPECT_TRUE(RoadmapFoldIn::findActiveReleaseHeading(tmp.path()).isEmpty());
}

TEST(RoadmapFoldIn, FindActiveReleaseHeadingMissingFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    EXPECT_TRUE(RoadmapFoldIn::findActiveReleaseHeading(tmp.path()).isEmpty());
}

// ===========================================================================
// ANTS-1342 / ANTS-1372 § 5.1 — R1-R3 counter-symlink-escape regression
// ===========================================================================

// R1: a .roadmap-counter symlink that resolves outside the project root
// must refuse allocation. Prevents two projects sharing a symlinked
// counter from racing on the same ID space.
TEST(RoadmapFoldIn, R1CounterSymlinkEscapeRefused) {
    QTemporaryDir a, b;
    ASSERT_TRUE(a.isValid());
    ASSERT_TRUE(b.isValid());
    writeFile(a.path(), ".roadmap-counter", "0\n");
    writeFile(b.path(), ".roadmap-counter", "0\n");

    // Replace A/.roadmap-counter with a symlink to B/.roadmap-counter.
    const QString aCounter = QDir(a.path()).filePath(".roadmap-counter");
    ASSERT_TRUE(QFile::remove(aCounter));
    const QString bCounter = QDir(b.path()).filePath(".roadmap-counter");
    ASSERT_TRUE(QFile::link(bCounter, aCounter));

    // Allocation against A MUST refuse — the canonical counter is in B.
    auto ids = RoadmapFoldIn::allocateIds(a.path(), 3);
    EXPECT_TRUE(ids.isEmpty())
        << "expected refusal when counter symlinks outside project root";

    // B's counter must be untouched.
    EXPECT_EQ(slurp(bCounter).trimmed(), QByteArray("0"));
}

// R2: well-formed call against a normal project still allocates.
// Regression for "the new guard didn't break the happy path."
TEST(RoadmapFoldIn, R2HappyPathStillAllocates) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".roadmap-counter", "0\n");

    auto ids = RoadmapFoldIn::allocateIds(tmp.path(), 2);
    ASSERT_EQ(ids.size(), 2);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 2);
}

// R3: legitimate first-allocation path (counter doesn't exist yet) —
// guard does not falsely refuse. The existing behaviour was
// "lockExclusive creates an empty file; read returns ""; toInt fails;
// return {}". The new guard should leave this path intact.
TEST(RoadmapFoldIn, R3FirstAllocationStillFalsThroughExistingPath) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // No .roadmap-counter, no symlinks. Guard MUST allow (parent dir
    // canonicalises to the project root), then the legacy
    // "open-with-O_CREAT-and-toInt-fails" path returns {}.
    auto ids = RoadmapFoldIn::allocateIds(tmp.path(), 1);
    EXPECT_TRUE(ids.isEmpty());
}

// Same guard applied to insertBlock — refuse if ROADMAP.md is a symlink
// outside the project root.
TEST(RoadmapFoldIn, RoadmapSymlinkEscapeRefused) {
    QTemporaryDir a, b;
    ASSERT_TRUE(a.isValid());
    ASSERT_TRUE(b.isValid());
    const QByteArray rm = "## 0.7.99 — t (target: 2026-05)\n";
    writeFile(b.path(), "ROADMAP.md", rm);
    const QString aRm = QDir(a.path()).filePath("ROADMAP.md");
    const QString bRm = QDir(b.path()).filePath("ROADMAP.md");
    ASSERT_TRUE(QFile::link(bRm, aRm));

    EXPECT_FALSE(RoadmapFoldIn::insertBlock(
        a.path(),
        QStringLiteral("## 0.7.99 — t (target: 2026-05)"),
        QStringLiteral("### foo\n")));
    // B's ROADMAP.md must be untouched.
    EXPECT_EQ(slurp(bRm), rm);
}

// ===========================================================================
// ANTS-1372 § 5.2 — G1-G5 caller-cwd gate verdict tests
// ===========================================================================

// G1: missing caller_cwd → cwd_missing.
TEST(CallerCwdGate, G1MissingArgRefused) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString focused = QFileInfo(tmp.path()).canonicalFilePath();
    QJsonObject req;  // no caller_cwd
    const auto g = RcGate::checkCallerCwd(focused, req, "test_verb");
    EXPECT_FALSE(g.ok);
    EXPECT_EQ(g.errorCode, "cwd_missing");
    EXPECT_TRUE(g.error.contains("caller_cwd"));
}

// G2: mismatch → cwd_mismatch.
TEST(CallerCwdGate, G2MismatchRefused) {
    QTemporaryDir a, b;
    ASSERT_TRUE(a.isValid());
    ASSERT_TRUE(b.isValid());
    QJsonObject req;
    req["caller_cwd"] = QFileInfo(b.path()).canonicalFilePath();
    const auto g = RcGate::checkCallerCwd(
        QFileInfo(a.path()).canonicalFilePath(), req, "test_verb");
    EXPECT_FALSE(g.ok);
    EXPECT_EQ(g.errorCode, "cwd_mismatch");
}

// G3: canonical match → ok=true.
TEST(CallerCwdGate, G3CanonicalMatchOk) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString focused = QFileInfo(tmp.path()).canonicalFilePath();
    QJsonObject req;
    req["caller_cwd"] = focused;
    const auto g = RcGate::checkCallerCwd(focused, req, "test_verb");
    EXPECT_TRUE(g.ok);
    EXPECT_EQ(g.caller, focused);
    EXPECT_EQ(g.focused, focused);
    EXPECT_TRUE(g.errorCode.isEmpty());
}

// G4: non-existent caller_cwd → cwd_bad.
TEST(CallerCwdGate, G4NonexistentCallerRefused) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QJsonObject req;
    req["caller_cwd"] = QStringLiteral("/tmp/ants-1372-doesnotexist-xyzzy");
    const auto g = RcGate::checkCallerCwd(
        QFileInfo(tmp.path()).canonicalFilePath(), req, "test_verb");
    EXPECT_FALSE(g.ok);
    EXPECT_EQ(g.errorCode, "cwd_bad");
}

// G5: empty focused tab → no_project.
TEST(CallerCwdGate, G5EmptyFocusedRefused) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QJsonObject req;
    req["caller_cwd"] = QFileInfo(tmp.path()).canonicalFilePath();
    const auto g = RcGate::checkCallerCwd("", req, "test_verb");
    EXPECT_FALSE(g.ok);
    EXPECT_EQ(g.errorCode, "no_project");
}

// Envelope shape check — INV-3 + ANTS-1295 key order.
TEST(CallerCwdGate, EnvelopeKeyOrderMatchesAnts1295) {
    RcGate::CallerCwdGate g;
    g.ok = false;
    g.errorCode = "cwd_mismatch";
    g.error = "test message";
    const QJsonObject env = RcGate::gateErrorEnvelope(g);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("error").toString(), "test message");
    EXPECT_EQ(env.value("code").toString(), "cwd_mismatch");
}

// ===========================================================================
// ANTS-1372 § 5.3 — S1-S5 source-grep tests
// ===========================================================================

namespace {

QByteArray slurpAbsolute(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

int countOccurrences(const QByteArray &hay, const QByteArray &needle) {
    int n = 0;
    int pos = 0;
    while ((pos = hay.indexOf(needle, pos)) != -1) { ++n; pos += needle.size(); }
    return n;
}

} // namespace

// S1: remotecontrol.cpp must call RcGate::checkCallerCwd at ≥ 7 sites
// (one per gated verb).
TEST(Ants1372SourceGrep, S1CheckCallerCwdCallSitesPresent) {
    const QByteArray src = slurpAbsolute(
        QStringLiteral(SRC_REMOTECONTROL_CPP_PATH));
    ASSERT_FALSE(src.isEmpty()) << "could not read remotecontrol.cpp";
    const int n = countOccurrences(src, "RcGate::checkCallerCwd");
    EXPECT_GE(n, 7) << "expected ≥7 gate call sites (one per mutating "
                       "verb); got " << n;
}

// S2: the helper module emits the cwd_mismatch literal.
TEST(Ants1372SourceGrep, S2CwdMismatchCodeEmitted) {
    const QString rc = QStringLiteral(SRC_REMOTECONTROL_CPP_PATH);
    const QString gate = QFileInfo(rc).absolutePath()
        + QStringLiteral("/remotecontrolgate.cpp");
    const QByteArray src = slurpAbsolute(gate);
    ASSERT_FALSE(src.isEmpty()) << "could not read " << gate.toStdString();
    EXPECT_TRUE(src.contains("cwd_mismatch"))
        << "gate cpp must emit \"cwd_mismatch\" string literal";
}

// S3: counterStaysInProject is referenced ≥ 2 times in roadmapfoldin.cpp
// (definition + use).
TEST(Ants1372SourceGrep, S3CounterStaysInProject) {
    const QByteArray src = slurpAbsolute(
        QStringLiteral(SRC_ROADMAPFOLDIN_CPP_PATH));
    ASSERT_FALSE(src.isEmpty()) << "could not read roadmapfoldin.cpp";
    const int n = countOccurrences(src, "counterStaysInProject");
    EXPECT_GE(n, 2)
        << "expected ≥2 references (definition + call); got " << n;
}

// S4: roadmapStaysInProject is referenced ≥ 2 times in roadmapfoldin.cpp.
TEST(Ants1372SourceGrep, S4RoadmapStaysInProject) {
    const QByteArray src = slurpAbsolute(
        QStringLiteral(SRC_ROADMAPFOLDIN_CPP_PATH));
    ASSERT_FALSE(src.isEmpty());
    const int n = countOccurrences(src, "roadmapStaysInProject");
    EXPECT_GE(n, 2)
        << "expected ≥2 references (definition + call); got " << n;
}

// S5: the audit-log line for cwd_mismatch refusals is wired. Look in
// remotecontrolgate.cpp via the sibling-file convention (same dir as
// remotecontrol.cpp).
TEST(Ants1372SourceGrep, S5AuditLogLineWired) {
    const QString rc = QStringLiteral(SRC_REMOTECONTROL_CPP_PATH);
    // Compute the gate cpp path from remotecontrol.cpp's dirname.
    const QString gate = QFileInfo(rc).absolutePath()
        + QStringLiteral("/remotecontrolgate.cpp");
    const QByteArray src = slurpAbsolute(gate);
    ASSERT_FALSE(src.isEmpty()) << "could not read " << gate.toStdString();
    EXPECT_TRUE(src.contains("[ANTS-1372]"))
        << "expected audit-log marker '[ANTS-1372]' in gate cpp";
    EXPECT_TRUE(src.contains("qWarning"))
        << "expected qWarning() call in gate cpp";
}
