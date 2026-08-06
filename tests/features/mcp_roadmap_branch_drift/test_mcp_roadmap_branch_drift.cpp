// ANTS-1583 — feature-conformance test for the roadmap_branch_drift
// MCP verb. Mostly source-scrape (dispatcher + descriptor + contract
// + taxonomy); one runtime block builds a tiny QTemporaryDir git
// fixture to exercise the reachability classifier end-to-end.
//
// Spec: docs/specs/ANTS-1583.md

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include "roadmapdialog.h"   // ANTS-3437 — parseBullets precondition test

#include <string>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef MCP_ERROR_CODES_DOC_PATH
#error "MCP_ERROR_CODES_DOC_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Run `git -C <root> <argv>` synchronously. Returns true on exit 0.
bool runGitOk(const QString &root, const QStringList &argv,
              QString *stdoutCapture = nullptr) {
    QProcess p;
    QStringList full;
    full << QStringLiteral("-C") << root;
    full.append(argv);
    p.start(QStringLiteral("git"), full);
    if (!p.waitForStarted(2000)) return false;
    if (!p.waitForFinished(5000)) { p.kill(); return false; }
    if (stdoutCapture) {
        *stdoutCapture = QString::fromUtf8(
            p.readAllStandardOutput()).trimmed();
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

}  // namespace

// ============================================================
// INV-1 — Required contract + schema declares caller_cwd required
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv1RequiredContract) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Contract row.
    expect(contains(ci,
        "if (toolName == QStringLiteral(\"roadmap_branch_drift\")) "
        "return C::Required;"),
        "INV-1: callerCwdContractFor must classify roadmap_branch_drift "
        "as Required");
    // Descriptor block declares caller_cwd in required[].
    const auto descPos = ci.find("\"roadmap_branch_drift\"");
    ASSERT_NE(descPos, std::string::npos)
        << "INV-1: descriptor name literal missing";
    const std::string region = ci.substr(descPos, 6000);
    expect(contains(region, "QStringLiteral(\"caller_cwd\")"),
        "INV-1: schema's required[] must list caller_cwd");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-2 — Etag allowlisted
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv2EtagAllowlisted) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Anchor on the function definition, not call sites or comments.
    // Brace-balanced extraction (ANTS-1474) so a growing allowlist body
    // can't outgrow a fixed byte-window.
    const std::string region = ants_test::slurpFunctionBody(
        ci, "bool ClaudeIntegration::isEtagSupportedTool");
    ASSERT_FALSE(region.empty())
        << "INV-2: isEtagSupportedTool definition not found";
    expect(contains(region, "\"roadmap_branch_drift\""),
        "INV-2: roadmap_branch_drift must be in isEtagSupportedTool "
        "allowlist");
    // makeEtagMatchProp() should appear in the descriptor — anchor on
    // the descriptor's `t["name"] =` line specifically.
    const auto descPos = ci.find("t[\"name\"] = \"roadmap_branch_drift\"");
    ASSERT_NE(descPos, std::string::npos)
        << "INV-2: roadmap_branch_drift descriptor block not found";
    // 6000-byte window (was 4000): ANTS-2057 grew the description + added
    // the against_refs schema property ahead of the etag_match wiring.
    const std::string desc = ci.substr(descPos, 6000);
    expect(contains(desc, "makeEtagMatchProp()"),
        "INV-2: descriptor must wire makeEtagMatchProp()");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-3 — SHA detector regex carries the ANTS-1583 anchor comment
//         and the alpha-required lookahead
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv3RegexAnchored) {
    expect_reset();
    const std::string rcc = ants_test::slurpRemoteControl();
    expect(contains(rcc, "// ANTS-1583 — anchored SHA detector"),
        "INV-3: regex literal must carry the ANTS-1583 anchor comment");
    expect(contains(rcc, "(?=[0-9a-f]*[a-f])"),
        "INV-3: regex must require ≥1 alpha char via lookahead");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-6 — max_drift clamp + drift_truncated flag
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv6MaxDriftClamp) {
    expect_reset();
    const std::string rcc = ants_test::slurpRemoteControl();
    // Anchor on the function definition; the brace-balanced extractor
    // (ANTS-1474) captures the whole ~160-line body regardless of size,
    // so there's no magic byte-window to outgrow.
    const std::string body = ants_test::slurpFunctionBody(
        rcc, "RemoteControl::cmdRoadmapBranchDrift");
    ASSERT_FALSE(body.empty())
        << "cmdRoadmapBranchDrift body not extractable";
    expect(contains(body, "maxDrift < 1"),
        "INV-6: lower-bound clamp missing");
    expect(contains(body, "maxDrift > 100"),
        "INV-6: upper-bound clamp missing");
    expect(contains(body, "drift_truncated"),
        "INV-6: drift_truncated flag missing from envelope");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-8 — No `git branch --contains` fork-per-SHA
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv8NoBranchContains) {
    expect_reset();
    const std::string rcc = ants_test::slurpRemoteControl();
    // Anchor on the function definition. The "no --contains" guard is
    // about the executable code; comments referencing the anti-pattern
    // are intentional and don't violate the invariant. So we test the
    // positive shape instead: `git log --format=%H` is the one-call
    // reachability approach, plus a `--format=%H` literal that
    // unambiguously names the batched form.
    const std::string body = ants_test::slurpFunctionBody(
        rcc, "RemoteControl::cmdRoadmapBranchDrift");
    ASSERT_FALSE(body.empty())
        << "cmdRoadmapBranchDrift body not extractable";
    expect(contains(body, "QStringLiteral(\"log\")"),
        "INV-8: handler must use `git log --format=%H` for reachability");
    expect(contains(body, "QStringLiteral(\"--format=%H\")"),
        "INV-8: handler must invoke git with --format=%H");
    // The handler also shouldn't invoke `git branch` (executable
    // anti-pattern — the comment can mention --contains for documentation).
    expect(!contains(body, "QStringLiteral(\"branch\")"),
        "INV-8: handler must not invoke `git branch` (anti-pattern; "
        "use `git log` for reachability instead)");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-9 — scanned_bullets / with_sha / drift_count emitted as ints
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv9CountFieldsInteger) {
    expect_reset();
    const std::string rcc = ants_test::slurpRemoteControl();
    const std::string body = ants_test::slurpFunctionBody(
        rcc, "RemoteControl::cmdRoadmapBranchDrift");
    ASSERT_FALSE(body.empty())
        << "cmdRoadmapBranchDrift body not extractable";
    expect(contains(body, "scannedBullets") &&
               contains(body, "env[\"scanned_bullets\"]"),
        "INV-9: scanned_bullets must be assigned from an int variable");
    expect(contains(body, "withSha") &&
               contains(body, "env[\"with_sha\"]"),
        "INV-9: with_sha must be assigned from an int variable");
    expect(contains(body, "drift.size()") &&
               contains(body, "env[\"drift_count\"]"),
        "INV-9: drift_count must be derived from drift.size()");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-10 — Handler reuses collectGitSnapshot
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv10ReusesCollectGitSnapshot) {
    expect_reset();
    const std::string rcc = ants_test::slurpRemoteControl();
    const auto fnPos = rcc.find("cmdRoadmapBranchDrift");
    ASSERT_NE(fnPos, std::string::npos);
    const std::string body = rcc.substr(fnPos, 6000);
    expect(contains(body, "collectGitSnapshot(rootCanonical)"),
        "INV-10: handler must reuse collectGitSnapshot rather than "
        "forking a fresh `git rev-parse HEAD`");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-11 — no_git_state code is listed in the taxonomy
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv11ErrorCodeTaxonomy) {
    expect_reset();
    const std::string doc = ants_test::slurpFile(MCP_ERROR_CODES_DOC_PATH);
    expect(contains(doc, "no_git_state"),
        "INV-11: docs/standards/mcp-error-codes.md must list "
        "the no_git_state refusal code");
    expect(contains(doc, "ANTS-1583"),
        "INV-11: taxonomy entry should reference ANTS-1583");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-12 (ANTS-2057) — against_refs / mis_branched cross-branch surface
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv12MisBranchedSurface) {
    expect_reset();
    const std::string rcc = ants_test::slurpRemoteControl();
    const std::string body = ants_test::slurpFunctionBody(
        rcc, "RemoteControl::cmdRoadmapBranchDrift");
    ASSERT_FALSE(body.empty())
        << "cmdRoadmapBranchDrift body not extractable";
    expect(contains(body, "against_refs"),
        "INV-12: handler reads the against_refs[] arg");
    expect(contains(body, "env[\"mis_branched\"]") &&
               contains(body, "missing_from"),
        "INV-12: handler emits mis_branched[] with missing_from");
    expect(contains(body, "env[\"checked_refs\"]") &&
               contains(body, "unknown_refs"),
        "INV-12: handler reports checked_refs + unknown_refs");
    // A mis-branched candidate must be reachable from HEAD (else it's
    // plain drift) — the guard `if (!isReachable(sha)) continue;` inside
    // the against_refs block enforces this.
    expect(contains(body, "isReachable(sha)"),
        "INV-12: mis_branched candidates are gated on HEAD-reachability");
    // Schema declares the property.
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto descPos = ci.find("t[\"name\"] = \"roadmap_branch_drift\"");
    ASSERT_NE(descPos, std::string::npos);
    const std::string desc = ci.substr(descPos, 6000);
    expect(contains(desc, "props[\"against_refs\"]"),
        "INV-12: schema declares the against_refs property");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-13 (ANTS-2057) — Runtime cross-branch reachability. A commit on
// HEAD but absent from a sibling ref is the mis_branched condition.
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv13MisBranchedRuntime) {
    expect_reset();
    if (qEnvironmentVariableIsSet("CI"))
        GTEST_SKIP() << "real-git runtime probe skipped under CI (ANTS-1609)";

    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    const QString root = td.path();
    if (!runGitOk(root, {QStringLiteral("init"),
                         QStringLiteral("--initial-branch=main"),
                         QStringLiteral("-q")})) {
        GTEST_SKIP() << "git init failed — likely no git in PATH";
    }
    runGitOk(root, {QStringLiteral("config"), QStringLiteral("user.email"),
                    QStringLiteral("test@example.invalid")});
    runGitOk(root, {QStringLiteral("config"), QStringLiteral("user.name"),
                    QStringLiteral("Ants Test")});

    QString shaA, shaB, shaC;
    auto emptyCommit = [&](const QString &msg, QString *out) {
        ASSERT_TRUE(runGitOk(root, {QStringLiteral("commit"),
                                    QStringLiteral("--allow-empty"),
                                    QStringLiteral("-q"),
                                    QStringLiteral("-m"), msg}));
        ASSERT_TRUE(runGitOk(root, {QStringLiteral("rev-parse"),
                                    QStringLiteral("HEAD")}, out));
    };
    emptyCommit("A", &shaA);
    emptyCommit("B", &shaB);
    // Sibling 'release' branch forks at B — so C (added next on main) is
    // reachable from HEAD=main but NOT from release: the mis_branched case.
    ASSERT_TRUE(runGitOk(root, {QStringLiteral("branch"),
                                QStringLiteral("release")}));
    emptyCommit("C", &shaC);
    ASSERT_EQ(40, shaC.size());

    auto reachableFrom = [&](const QString &ref) -> QStringList {
        QString raw;
        runGitOk(root, {QStringLiteral("log"), QStringLiteral("--format=%H"),
                        ref, QStringLiteral("--max-count=200000")}, &raw);
        return raw.split(QChar('\n'));
    };
    const QStringList head    = reachableFrom(QStringLiteral("HEAD"));
    const QStringList release = reachableFrom(QStringLiteral("release"));

    // C is on HEAD but missing from release → mis_branched.
    EXPECT_TRUE(head.contains(shaC))
        << "INV-13: C must be reachable from HEAD";
    EXPECT_FALSE(release.contains(shaC))
        << "INV-13: C must be ABSENT from the release sibling (mis_branched)";
    // B is on both → NOT mis_branched (the contrast that proves the
    // classifier isn't just flagging everything).
    EXPECT_TRUE(head.contains(shaB) && release.contains(shaB))
        << "INV-13: B is shared and must not be flagged";
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-4 / INV-5 — Runtime reachability classifier
// ============================================================
//
// Builds a 3-commit linear chain A → B → C with an orphan commit D,
// HEAD at C. The verb's classifier should resolve A, B, C as
// reachable (including via short-SHA prefix for INV-5) and report
// D as `sha_not_in_HEAD`.
//
// The test calls `git` via the helper above to set up the fixture,
// then invokes the verb's logic indirectly by running the same
// commands (`git log --format=%H HEAD` + `git cat-file -e <sha>`)
// the handler runs.

TEST(mcp_roadmap_branch_drift, Inv4ReachabilityRuntime) {
    expect_reset();

    // This test spawns real `git` (init/config/commit/rev-parse/checkout/
    // log/cat-file), each with a 5 s timeout. Skip under CI to keep the
    // correctness lane fast + hermetic; it still gives full coverage
    // locally where git is present (ANTS-1609).
    if (qEnvironmentVariableIsSet("CI"))
        GTEST_SKIP() << "real-git runtime probe skipped under CI (ANTS-1609)";

    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    const QString root = td.path();

    // Defensive: require git in PATH.
    if (!runGitOk(root, {QStringLiteral("init"),
                         QStringLiteral("--initial-branch=main"),
                         QStringLiteral("-q")})) {
        GTEST_SKIP() << "git init failed — likely no git in PATH";
    }
    // Identity for commits.
    runGitOk(root, {QStringLiteral("config"),
                    QStringLiteral("user.email"),
                    QStringLiteral("test@example.invalid")});
    runGitOk(root, {QStringLiteral("config"),
                    QStringLiteral("user.name"),
                    QStringLiteral("Ants Test")});

    // Three linear commits A → B → C.
    QString shaA, shaB, shaC, shaD;
    auto emptyCommit = [&](const QString &msg, QString *out) {
        ASSERT_TRUE(runGitOk(root, {QStringLiteral("commit"),
                                    QStringLiteral("--allow-empty"),
                                    QStringLiteral("-q"),
                                    QStringLiteral("-m"), msg}));
        ASSERT_TRUE(runGitOk(root, {QStringLiteral("rev-parse"),
                                    QStringLiteral("HEAD")}, out));
    };
    emptyCommit("A", &shaA);
    emptyCommit("B", &shaB);
    emptyCommit("C", &shaC);
    ASSERT_EQ(40, shaA.size());
    ASSERT_EQ(40, shaB.size());
    ASSERT_EQ(40, shaC.size());
    ASSERT_NE(shaA, shaB);
    ASSERT_NE(shaB, shaC);

    // Orphan commit D — checkout an orphan branch, commit, then
    // switch back to main so HEAD is at C.
    ASSERT_TRUE(runGitOk(root, {QStringLiteral("checkout"),
                                QStringLiteral("--orphan"),
                                QStringLiteral("scratch")}));
    runGitOk(root, {QStringLiteral("commit"),
                    QStringLiteral("--allow-empty"),
                    QStringLiteral("-q"),
                    QStringLiteral("-m"), QStringLiteral("D")});
    runGitOk(root, {QStringLiteral("rev-parse"),
                    QStringLiteral("HEAD")}, &shaD);
    ASSERT_EQ(40, shaD.size());
    ASSERT_NE(shaC, shaD);
    // Move HEAD back to main (C).
    ASSERT_TRUE(runGitOk(root, {QStringLiteral("checkout"),
                                QStringLiteral("main")}));

    // Drive the classifier helpers via direct git invocations: this
    // mirrors what the handler does. The fixture asserts the
    // classifier's contract end-to-end without invoking the MCP
    // socket (which would require Qt event-loop setup beyond this
    // bundle's scope).
    QString logRaw;
    ASSERT_TRUE(runGitOk(root, {QStringLiteral("log"),
                                QStringLiteral("--format=%H"),
                                QStringLiteral("HEAD"),
                                QStringLiteral("--max-count=200000")},
                        &logRaw));
    QStringList reachable = logRaw.split(QChar('\n'));
    // A, B, C ⇒ reachable.
    EXPECT_TRUE(reachable.contains(shaA))
        << "INV-4: A must be reachable from HEAD";
    EXPECT_TRUE(reachable.contains(shaB))
        << "INV-4: B must be reachable from HEAD";
    EXPECT_TRUE(reachable.contains(shaC))
        << "INV-4: C must be reachable from HEAD";
    // D ⇒ NOT reachable.
    EXPECT_FALSE(reachable.contains(shaD))
        << "INV-4: orphan D must NOT be reachable from HEAD";
    // D exists in git.
    EXPECT_TRUE(runGitOk(root, {QStringLiteral("cat-file"),
                                QStringLiteral("-e"), shaD}))
        << "INV-4: orphan D's object should still exist in git";

    // INV-5 — short-SHA prefix lookup. The 7-char prefix of C should
    // resolve back to C when we check via startsWith on the reachable
    // set (the production code uses a prefix index; the test asserts
    // the underlying contract).
    const QString prefixC = shaC.left(7);
    bool resolved = false;
    for (const QString &full : reachable) {
        if (full.startsWith(prefixC)) { resolved = true; break; }
    }
    EXPECT_TRUE(resolved)
        << "INV-5: short-SHA prefix " << prefixC.toStdString()
        << " must resolve to a reachable full SHA";

    // Fabricated SHA — non-zero hex that isn't in the repo.
    const QString fabricated =
        QStringLiteral("0000000000000000000000000000000000000000");
    // git cat-file -e on the all-zero SHA returns non-zero (object
    // doesn't exist) — that's our `sha_not_in_git` proxy.
    EXPECT_FALSE(runGitOk(root, {QStringLiteral("cat-file"),
                                 QStringLiteral("-e"), fabricated}))
        << "INV-4: all-zero fabricated SHA must NOT exist in git";

    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-14 (ANTS-3437) — legacy no-id roadmap enumeration. A ants-v1
// roadmap with no [PREFIX-NNNN] ids must NOT be skipped wholesale at the
// id-less guard (which exists only to drop modern narrator bullets); the
// handler detects the no-id case, keeps scanning, identifies each bullet
// by a headline slug, and marks the envelope with roadmap_format.
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv14LegacyNoIdEnumerated) {
    expect_reset();
    const std::string rcc = ants_test::slurpRemoteControl();
    const std::string body = ants_test::slurpFunctionBody(
        rcc, "RemoteControl::cmdRoadmapBranchDrift");
    ASSERT_FALSE(body.empty())
        << "cmdRoadmapBranchDrift body not extractable";
    // Detects the legacy no-id case.
    expect(contains(body, "legacyNoId"),
        "INV-14: handler must detect a legacy no-id roadmap");
    // The id-less skip is conditional on NOT being legacy (so a legacy
    // roadmap keeps scanning). Both the drift loop and the mis_branched
    // loop must carry the guard.
    expect(contains(body, "bul.id.isEmpty() && !legacyNoId"),
        "INV-14: drift loop must skip id-less bullets only in a modern "
        "roadmap");
    expect(contains(body, "bulletIdFor(bul)"),
        "INV-14: bullet_id must come from bulletIdFor (headline slug in "
        "the legacy path)");
    // Envelope marks the legacy enumeration path.
    expect(contains(body, "env[\"roadmap_format\"]"),
        "INV-14: envelope must flag the legacy enumeration via "
        "roadmap_format");
    EXPECT_EQ(0, expect_failures());
}

// ============================================================
// INV-14b (ANTS-3437) — precondition: a legacy ants-v1 ✅ bullet parses
// with an EMPTY id, which is exactly what tripped the id-less skip. This
// documents (and locks) the parse-side fact the fix depends on.
// ============================================================
TEST(mcp_roadmap_branch_drift, Inv14bLegacyBulletParsesWithoutId) {
    expect_reset();
    // A minimal legacy ants-v1 roadmap: emoji-prefixed bold bullet, no
    // [PREFIX-NNNN] token, citing a fix SHA in the body.
    const QString md = QStringLiteral(
        "# Roadmap\n\n"
        "## Fixes\n\n"
        "- ✅ **Wall-clip regression resolved.**\n"
        "  Fixed 1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    bool sawShippedNoId = false;
    for (const auto &b : bullets) {
        if (b.status == QStringLiteral("✅") && b.id.isEmpty())
            sawShippedNoId = true;
    }
    EXPECT_TRUE(sawShippedNoId)
        << "INV-14b: a legacy ✅ bullet must parse with an empty id (the "
           "precondition the ANTS-3437 legacy path handles)";
}

// ============================================================
// Dispatch wiring: the verb name appears in
// `m_claudeIntegration->registerToolProvider("roadmap_branch_drift"`
// in mainwindow.cpp + cmdRoadmapBranchDrift is declared in
// remotecontrol.h.
// ============================================================
TEST(mcp_roadmap_branch_drift, DispatchWiringRegistered) {
    expect_reset();
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(mw,
        "registerToolProvider(\"roadmap_branch_drift\""),
        "dispatch: mainwindow.cpp must registerToolProvider for "
        "roadmap_branch_drift");
    const std::string rh = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    expect(contains(rh, "cmdRoadmapBranchDrift"),
        "dispatch: remotecontrol.h must declare cmdRoadmapBranchDrift");
    EXPECT_EQ(0, expect_failures());
}
