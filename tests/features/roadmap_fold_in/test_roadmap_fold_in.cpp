// Feature-conformance test for ANTS-1111 RoadmapFoldIn helpers +
// AuditEngine::templateRoadmapFoldInBlock. Extended in ANTS-1372 with
// counter-symlink-escape tests (ANTS-1342 sibling) and caller-cwd
// gate tests (R/G/S series, see docs/specs/ANTS-1372.md § 5).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

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

// ---- ANTS-3450: corpusHighWater + allocateIds floor ----------------

// Build a project whose highest allocated id lives in each of the three
// committed corpus files, above a deliberately-stale counter. Proves the
// scan reaches all three and that allocateIds floors past the stale counter
// so the untracked cache can never make the batch path reissue a live id.
TEST(RoadmapFoldIn, CorpusHighWaterAcrossSources) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    ASSERT_TRUE(QDir(root).mkpath(QStringLiteral("docs/roadmap")));

    // Live bullet (also the prefix-sniff source), a migrated CHANGELOG id,
    // and an archived id — the archive holds the true high-water (250).
    writeFile(root, "ROADMAP.md",
              "# Roadmap\n\n## To Do\n\n"
              "- [ANTS-0150] **Live item.**\n");
    writeFile(root, "CHANGELOG.md",
              "## [0.7.0] - 2026-07-01\n### Fixed\n- Shipped (ANTS-0200)\n");
    writeFile(root, "docs/roadmap/0.6.md",
              "## 0.6.0 — 2026-06\n- [ANTS-0250] **Archived item.**\n");
    writeFile(root, ".roadmap-counter", "100\n");  // stale-low

    // Sniffed prefix and explicit prefix agree; the max spans all sources.
    EXPECT_EQ(RoadmapFoldIn::corpusHighWater(root), 250);
    EXPECT_EQ(RoadmapFoldIn::corpusHighWater(root, QStringLiteral("ANTS")),
              250);
    // A prefix with no ids present → 0 (no false positives from other ids).
    EXPECT_EQ(RoadmapFoldIn::corpusHighWater(root, QStringLiteral("ZZZ")), 0);
    // Bad root → 0, no crash.
    EXPECT_EQ(RoadmapFoldIn::corpusHighWater(
                  QStringLiteral("/nonexistent/path/xyz")), 0);

    // allocateIds floors past the stale counter to corpus-max, so it never
    // reissues 101..103 (which collide with nothing here, but WOULD collide
    // with the archived 150/200/250 in a real project).
    auto ids = RoadmapFoldIn::allocateIds(root, 3);
    ASSERT_EQ(ids.size(), 3);
    EXPECT_EQ(ids[0], 251);
    EXPECT_EQ(ids[1], 252);
    EXPECT_EQ(ids[2], 253);
    EXPECT_EQ(slurp(QDir(root).filePath(".roadmap-counter")).trimmed(),
              QByteArray("253")) << "counter self-heals up to corpus+N";
}

// ANTS-4631 — the scan counts an id only where a line DECLARES one: a
// top-level list item outside a fenced block. A roadmap that documents its
// own id format writes id-shaped sample text in prose, in an indented
// example and inside a fence, and none of those is an allocated id.
//
// Measured on this project 2026-08-24: a deliberately-absurd `ANTS-9999` in
// ANTS-4629's body pinned file_ahead_of_store true against a store that was
// exactly in sync, and then the allocator — which floors to this value —
// issued 10000 for the very item filing the defect, burning ~5,370 ids.
//
// The three exclusions are one rule, not three: a declaration is a list
// item, so prose and indented continuation lines are out by position, and
// the fence mask removes the one case a bullet-shaped line is still an
// example. Anything MarkdownScan treats as a fence is excluded here too, so
// the two cannot diverge.
TEST(RoadmapFoldIn, CorpusHighWaterCountsOnlyDeclaringLines) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    writeFile(root, "ROADMAP.md",
              "# Roadmap\n\n## To Do\n\n"
              "- [ANTS-0150] **Live item.**\n"
              "  Prose naming ANTS-9999 as a sample id, exactly as a bullet\n"
              "  describing the id format does.\n"
              "\n"
              "    changelog_log {op:\"add\", id:\"ANTS-9998\",\n"
              "                   summary:\"**Bold** (ANTS-9998)\"}\n"
              "\n"
              "  ```\n"
              "  - [ANTS-9997] **A bullet quoted inside a fence.**\n"
              "  ```\n");
    writeFile(root, ".roadmap-counter", "100\n");

    // The sample ids are text; 150 is the only id this file declares.
    EXPECT_EQ(RoadmapFoldIn::corpusHighWater(root, QStringLiteral("ANTS")),
              150)
        << "a documented sample id must not raise the high water";

    // The allocator floors to the same value, so the next id is 151 — the
    // half that made the defect permanent rather than cosmetic.
    const auto ids = RoadmapFoldIn::allocateIds(root, 1);
    ASSERT_EQ(ids.size(), 1);
    EXPECT_EQ(ids[0], 151);
}

// ANTS-1618 — empty (or freshly-created) `.roadmap-counter` is a valid
// initial state. Pre-1618 the allocate failed with `id_counter_failed`
// and a misleading "stale .lock sibling" hint; post-1618 the engine
// auto-initialises and returns IDs starting at 1.
TEST(RoadmapFoldIn, Ants1618EmptyCounterAutoInitsToZero) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // No .roadmap-counter present — lockExclusive creates an empty
    // file; allocateIds should now treat empty == current=0.
    const auto ids = RoadmapFoldIn::allocateIds(tmp.path(), 3);
    ASSERT_EQ(ids.size(), 3);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 2);
    EXPECT_EQ(ids[2], 3);
    // And the file now holds the new high-water mark.
    EXPECT_EQ(slurp(QDir(tmp.path()).filePath(".roadmap-counter")).trimmed(),
              QByteArray("3"));
}

// ANTS-1618 — 0-byte (vs absent) counter: same auto-init behaviour.
TEST(RoadmapFoldIn, Ants1618ZeroByteCounterAutoInits) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".roadmap-counter", QByteArray());  // 0 bytes
    const auto ids = RoadmapFoldIn::allocateIds(tmp.path(), 2);
    ASSERT_EQ(ids.size(), 2);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 2);
}

// ANTS-1618 — corrupt content (non-integer) still fails, but
// inspectCounter() reports CounterState::Corrupt so the caller can
// emit a state-specific message instead of the legacy "stale .lock
// sibling" hint.
TEST(RoadmapFoldIn, Ants1618InspectReportsCorruptOnNonIntegerCounter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".roadmap-counter", "not-a-number\n");
    EXPECT_TRUE(RoadmapFoldIn::allocateIds(tmp.path(), 1).isEmpty());
    const auto ins = RoadmapFoldIn::inspectCounter(tmp.path());
    EXPECT_EQ(ins.state, RoadmapFoldIn::CounterState::Corrupt);
    EXPECT_TRUE(ins.preview.contains(QStringLiteral("not-a-number")));
}

// ANTS-1618 — inspectCounter reports Ok with parsed value on a healthy
// counter (used by callers as a positive readiness check).
TEST(RoadmapFoldIn, Ants1618InspectReportsOkOnHealthyCounter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".roadmap-counter", "42\n");
    const auto ins = RoadmapFoldIn::inspectCounter(tmp.path());
    EXPECT_EQ(ins.state, RoadmapFoldIn::CounterState::Ok);
    EXPECT_EQ(ins.value, 42);
}

// ANTS-1618 — inspectCounter reports EmptyOrAbsent when the file is
// absent or empty (both are valid initial states for allocateIds).
TEST(RoadmapFoldIn, Ants1618InspectReportsEmptyOnAbsentCounter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto ins = RoadmapFoldIn::inspectCounter(tmp.path());
    EXPECT_EQ(ins.state, RoadmapFoldIn::CounterState::EmptyOrAbsent);
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
// the ANTS-1342 symlink-escape guard must NOT falsely refuse.
// Pre-ANTS-1618 this returned {} because the empty-file read failed
// toInt(); post-ANTS-1618 the engine auto-initialises empty/absent
// to current=0 and returns [1]. The point of the test (guard does
// not block legitimate paths) is unchanged — only the success shape
// changed from "empty list" to "list with one ID".
TEST(RoadmapFoldIn, R3FirstAllocationStillFallsThroughExistingPath) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // No .roadmap-counter, no symlinks. Guard MUST allow (parent dir
    // canonicalises to the project root), and the auto-init path
    // succeeds with [1] (ANTS-1618).
    auto ids = RoadmapFoldIn::allocateIds(tmp.path(), 1);
    ASSERT_EQ(ids.size(), 1);
    EXPECT_EQ(ids[0], 1);
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

// S1: remotecontrol.cpp must call RcGate::checkCallerCwd at ≥ 6 sites
// (one per remaining gated verb). ANTS-1630 migrated cold_eyes_fold_in +
// indie_review_fold_in off the focused-tab gate onto caller-cwd anchoring,
// dropping the count from 8 to 6 (verify_changes, plan_template,
// session_memory, workflow_state, debt_sweep_apply_fix, debt_sweep_defer).
TEST(Ants1372SourceGrep, S1CheckCallerCwdCallSitesPresent) {
    const QByteArray src =
        QByteArray::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(src.isEmpty()) << "could not read the remotecontrol TUs";
    const int n = countOccurrences(src, "RcGate::checkCallerCwd");
    EXPECT_GE(n, 6) << "expected ≥6 gate call sites (one per remaining "
                       "gated verb); got " << n;
}

// S2: the helper module emits the cwd_mismatch literal.
TEST(Ants1372SourceGrep, S2CwdMismatchCodeEmitted) {
    // ANTS-3833 — a sibling-file anchor, not a read of remotecontrol itself.
    const QString gate = QStringLiteral(ANTS_RC_SRC_DIR)
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
    // ANTS-3833 — src/ directly, rather than via remotecontrol.cpp's dirname.
    const QString gate = QStringLiteral(ANTS_RC_SRC_DIR)
        + QStringLiteral("/remotecontrolgate.cpp");
    const QByteArray src = slurpAbsolute(gate);
    ASSERT_FALSE(src.isEmpty()) << "could not read " << gate.toStdString();
    EXPECT_TRUE(src.contains("[ANTS-1372]"))
        << "expected audit-log marker '[ANTS-1372]' in gate cpp";
    EXPECT_TRUE(src.contains("qWarning"))
        << "expected qWarning() call in gate cpp";
}
