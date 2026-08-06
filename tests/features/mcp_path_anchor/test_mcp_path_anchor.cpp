// Feature-conformance test for ANTS-1295 — per-tool cwd-anchor
// enforcement on path-accepting MCP tools. Engine cases exercise
// PathValidation::validatePath directly (test_claude links the
// ants_core_lib TU that owns it). Wiring cases slurp remotecontrol.cpp
// + pathvalidation.{h,cpp} + CMakeLists and assert pinned substrings
// and counts.

#include "pathvalidation.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTextStream>

#include <sstream>
#include <string>

#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef CMAKELISTS_PATH
#error "CMAKELISTS_PATH compile definition required"
#endif

namespace {


QString pathvalidationHeaderPath() {
    // ANTS-3833 — these two never read remotecontrol; they were using its path
    // macro as a handle on src/. That is what ANTS_RC_SRC_DIR says directly,
    // and it no longer depends on which TU happens to be listed first.
    return QDir(QStringLiteral(ANTS_RC_SRC_DIR))
        .filePath(QStringLiteral("pathvalidation.h"));
}

QString pathvalidationCppPath() {
    return QDir(QStringLiteral(ANTS_RC_SRC_DIR))
        .filePath(QStringLiteral("pathvalidation.cpp"));
}

}  // namespace

// -------------------- Engine cases --------------------

// PV-1: empty rawPath returns neutral Check.
TEST(McpPathAnchor, EmptyRawPathIsNeutral) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    const auto check = PathValidation::validatePath(
        QString(), root, QStringLiteral("file_outline"));
    EXPECT_FALSE(check.bad);
    EXPECT_TRUE(check.argvForm.isEmpty());
    EXPECT_TRUE(check.resolved.isEmpty());
    EXPECT_TRUE(check.err.isEmpty());
}

// PV-2: a relative path that exists inside root canonicalises +
// accepts; resolved == absolute canonical form.
TEST(McpPathAnchor, RelativeInsideRootAccepts) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QFile f(root + QStringLiteral("/data.txt"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    const auto check = PathValidation::validatePath(
        QStringLiteral("data.txt"), root,
        QStringLiteral("file_outline"));
    EXPECT_FALSE(check.bad);
    EXPECT_EQ(check.argvForm, QStringLiteral("data.txt"));
    EXPECT_EQ(check.resolved, root + QStringLiteral("/data.txt"));
}

// PV-3: leading `-` triggers argvForm prefix.
TEST(McpPathAnchor, DashPrefixGetsDotSlash) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QFile f(root + QStringLiteral("/-evil"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    const auto check = PathValidation::validatePath(
        QStringLiteral("-evil"), root,
        QStringLiteral("git_state"));
    EXPECT_FALSE(check.bad);
    EXPECT_EQ(check.argvForm, QStringLiteral("./-evil"));
}

// PV-4: relative traversal rejects with the right envelope.
TEST(McpPathAnchor, RelativeTraversalRejects) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    const auto check = PathValidation::validatePath(
        QStringLiteral("../../etc/passwd"), root,
        QStringLiteral("file_outline"), QStringLiteral("path"));
    EXPECT_TRUE(check.bad);
    EXPECT_EQ(check.err.value(QStringLiteral("ok")).toBool(), false);
    EXPECT_EQ(check.err.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_path"));
    const QString errText = check.err.value(QStringLiteral("error")).toString();
    EXPECT_TRUE(errText.contains(QStringLiteral("file_outline")));
    EXPECT_TRUE(errText.contains(QStringLiteral("\"path\"")));
    EXPECT_TRUE(errText.contains(QStringLiteral("escapes project root")));
}

// PV-5: absolute path outside root rejects.
TEST(McpPathAnchor, AbsoluteOutsideRootRejects) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    const auto check = PathValidation::validatePath(
        QStringLiteral("/etc/passwd"), root,
        QStringLiteral("debt_sweep_apply_fix"),
        QStringLiteral("file"));
    EXPECT_TRUE(check.bad);
    EXPECT_EQ(check.err.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_path"));
    const QString errText = check.err.value(QStringLiteral("error")).toString();
    EXPECT_TRUE(errText.contains(QStringLiteral("debt_sweep_apply_fix")));
    EXPECT_TRUE(errText.contains(QStringLiteral("\"file\"")));
    // ANTS-3430 — a non-feedback outside-root path is still refused; hints
    // were retired (feedback files are now allowed, not hinted-at).
    EXPECT_FALSE(check.err.contains(QStringLiteral("hint")));
}

// ANTS-3430 — a *_Ants_MCP_Feedback.md path lives one level ABOVE the project
// root by convention (the shared corpus dir), so it always "escapes" the root.
// The general project-scoped verbs are now permitted to reach it directly
// (superseding ANTS-3419, which refused with a redirect hint). Only this exact
// basename suffix may escape; a non-feedback escape (PV-5) still refuses.
TEST(McpPathAnchor, FeedbackFileAllowedOutsideRoot) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Real layout: <shared>/proj is the project root; the feedback file lives
    // at <shared>/Demo_Ants_MCP_Feedback.md — one level above.
    const QString shared = QFileInfo(tmp.path()).canonicalFilePath();
    const QString root = shared + QStringLiteral("/proj");
    ASSERT_TRUE(QDir().mkpath(root));
    const QString fb = shared + QStringLiteral("/Demo_Ants_MCP_Feedback.md");
    QFile f(fb);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("# placeholder\n");
    f.close();

    // Existing feedback file above the root → allowed, canonical resolved set.
    const auto check = PathValidation::validatePath(
        fb, root, QStringLiteral("read_region"), QStringLiteral("path"));
    EXPECT_FALSE(check.bad)
        << "feedback file above root must be allowed (ANTS-3430); err="
        << check.err.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(check.resolved, QFileInfo(fb).canonicalFilePath());
    EXPECT_FALSE(check.err.contains(QStringLiteral("hint")));

    // A not-yet-existing feedback path is likewise allowed (won't canonicalise,
    // so resolved stays empty — the writer reconstructs it). feedback_log
    // creates the file on first append, so this case is load-bearing.
    //
    // ANTS-3616 narrowed WHERE that is true. This assertion used to pass
    // `/nonexistent/DOOM_Ants_MCP_Feedback.md` — an arbitrary absolute path
    // in a directory unrelated to the project — and expected acceptance,
    // which is exactly the unbounded surface ANTS-3616 closes. The
    // requirement it was really guarding is "not-yet-existing", not
    // "anywhere", so it now uses a fresh name in the CORRECT directory:
    // same intent, bounded location. Refusal of the far-away variant is
    // covered by FeedbackSuffixElsewhereRefused.
    const auto check2 = PathValidation::validatePath(
        shared + QStringLiteral("/DOOM_Ants_MCP_Feedback.md"), root,
        QStringLiteral("apply_edits"), QStringLiteral("path"));
    EXPECT_FALSE(check2.bad)
        << "non-existent feedback path in the project root's parent must be "
           "allowed (ANTS-3430) — feedback_log creates it on first append; "
           "err="
        << check2.err.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(check2.resolved.isEmpty());
}

// PV-6: control character in path rejects.
TEST(McpPathAnchor, ControlCharacterRejects) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    QString hostile;
    hostile += QChar('a');
    hostile += QChar(0x01);
    hostile += QChar('b');
    const auto check = PathValidation::validatePath(
        hostile, root,
        QStringLiteral("file_outline"));
    EXPECT_TRUE(check.bad);
    const QString errText = check.err.value(QStringLiteral("error")).toString();
    EXPECT_TRUE(errText.contains(QStringLiteral("control or backslash")));
}

// PV-7: backslash rejects.
TEST(McpPathAnchor, BackslashRejects) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    const auto check = PathValidation::validatePath(
        QStringLiteral("foo\\bar"), root,
        QStringLiteral("file_outline"));
    EXPECT_TRUE(check.bad);
    const QString errText = check.err.value(QStringLiteral("error")).toString();
    EXPECT_TRUE(errText.contains(QStringLiteral("control or backslash")));
}

// PV-8: NFD form of an ASCII-only string canonicalises identically.
// (We can't easily fabricate a non-ASCII filename portably; the spec
//  is that NFC-normalisation happens before the anchor check. We
//  verify that an already-NFC path round-trips.)
TEST(McpPathAnchor, NfcRoundTrips) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    const QString name = QStringLiteral("plain.txt");
    QFile f(root + QStringLiteral("/") + name);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("y");
    f.close();
    const auto check = PathValidation::validatePath(
        name, root, QStringLiteral("file_outline"));
    EXPECT_FALSE(check.bad);
    EXPECT_EQ(check.argvForm, name);
}

// PV-9: a path whose head doesn't exist but whose lexical form
// escapes still rejects (the cleanPath fallback).
TEST(McpPathAnchor, NonexistentEscapeRejects) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    const auto check = PathValidation::validatePath(
        QStringLiteral("nonexistent/../../etc/passwd"), root,
        QStringLiteral("git_state"));
    EXPECT_TRUE(check.bad);
    EXPECT_EQ(check.err.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_path"));
}

// PV-10: a path that doesn't exist but resolves inside root still
// accepts (git pathspec for deleted files).
TEST(McpPathAnchor, NonexistentInsideRootAccepts) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    const auto check = PathValidation::validatePath(
        QStringLiteral("new_dir/new_file"), root,
        QStringLiteral("git_state"));
    EXPECT_FALSE(check.bad);
    // resolved is empty because the path doesn't exist; argvForm
    // carries the relative form for git argv.
    EXPECT_TRUE(check.resolved.isEmpty());
    EXPECT_EQ(check.argvForm, QStringLiteral("new_dir/new_file"));
}

// PV-11: ANTS-1837 — a project root in decomposed (NFD) Unicode form
// must not false-reject a path that genuinely lives inside it. Before
// the fix validatePath NFC-normalised the input but compared it against
// the raw (NFD) root, so a decomposed accented home dir rejected every
// path under it with "escapes project root".
TEST(McpPathAnchor, NfdRootDoesNotFalseReject) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Decomposed "café": c, a, f, e, U+0301 combining acute accent.
    QString nfdName = QStringLiteral("caf");
    nfdName += QChar(0x0065);
    nfdName += QChar(0x0301);
    ASSERT_NE(nfdName, nfdName.normalized(QString::NormalizationForm_C))
        << "test precondition: the name must actually be decomposed";
    const QString subdir = QDir(tmp.path()).filePath(nfdName);
    ASSERT_TRUE(QDir().mkpath(subdir));
    // canonicalFilePath returns the on-disk (NFD) byte form on Linux.
    const QString rootNfd = QFileInfo(subdir).canonicalFilePath();
    ASSERT_FALSE(rootNfd.isEmpty());
    QFile f(rootNfd + QStringLiteral("/data.txt"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("z");
    f.close();

    const auto check = PathValidation::validatePath(
        rootNfd + QStringLiteral("/data.txt"), rootNfd,
        QStringLiteral("file_outline"));
    EXPECT_FALSE(check.bad)
        << "NFD-form root false-rejected an in-root path; err="
        << check.err.value(QStringLiteral("error")).toString().toStdString();
}

// PV-12: ANTS-1832 shared helper — isInsideProject accepts a path inside
// the project and rejects a traversal escape.
TEST(McpPathAnchor, IsInsideProjectAnchors) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QDir().mkpath(root + QStringLiteral("/src"));
    QFile f(root + QStringLiteral("/src/foo.cpp"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    EXPECT_TRUE(PathValidation::isInsideProject(
        root, root + QStringLiteral("/src/foo.cpp")));
    EXPECT_FALSE(PathValidation::isInsideProject(
        root, root + QStringLiteral("/src/../../etc/passwd")));
    // Non-existent candidate → false (canonicalisation miss).
    EXPECT_FALSE(PathValidation::isInsideProject(
        root, root + QStringLiteral("/src/does-not-exist.cpp")));
}

// -------------------- Wiring cases --------------------

// WI-1: header exists and exposes the right names.
TEST(McpPathAnchorWiring, HeaderExposesValidator) {
    const QString hp = pathvalidationHeaderPath();
    const std::string h = ants_test::slurpFile(hp.toUtf8().constData());
    ASSERT_FALSE(h.empty()) << "missing pathvalidation.h at "
                            << hp.toStdString();
    EXPECT_NE(h.find("namespace PathValidation"), std::string::npos);
    EXPECT_NE(h.find("struct Check"), std::string::npos);
    EXPECT_NE(h.find("Check validatePath("), std::string::npos);
    EXPECT_NE(h.find("argvForm"), std::string::npos);
    EXPECT_NE(h.find("resolved"), std::string::npos);
}

// WI-2: the .cpp defines validatePath and holds the anchor logic.
TEST(McpPathAnchorWiring, ImplDefinesValidator) {
    const QString cp = pathvalidationCppPath();
    const std::string c = ants_test::slurpFile(cp.toUtf8().constData());
    ASSERT_FALSE(c.empty()) << "missing pathvalidation.cpp at "
                            << cp.toStdString();
    // Definition lives inside `namespace PathValidation { ... }`, so
    // the qualified name doesn't appear in the source as a literal —
    // the unqualified body signature is the right thing to pin.
    EXPECT_NE(c.find("namespace PathValidation"), std::string::npos);
    EXPECT_NE(c.find("Check validatePath("), std::string::npos);
    // Anchor logic — both halves must be present in the same TU. The
    // comparison was extracted into the NFC-aware anchoredUnder() helper
    // (ANTS-1837); pin that + the NFC normalisation it depends on.
    EXPECT_NE(c.find("canonicalFilePath()"), std::string::npos);
    EXPECT_NE(c.find("anchoredUnder("), std::string::npos);
    EXPECT_NE(c.find("NormalizationForm_C"), std::string::npos);
}

// WI-3: remotecontrol.cpp has no inline anchor pair. We count
// occurrences of `canonicalFilePath()` and `startsWith(rootCanonical`
// and require that no `startsWith(rootCanonical` line is within 5
// lines after a `canonicalFilePath()` assignment.
TEST(McpPathAnchorWiring, RemoteControlNoInlineAnchor) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());

    // Split into lines.
    std::vector<std::string> lines;
    {
        std::stringstream ss(rc);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }

    // Find every line that has `canonicalFilePath()` AND assigns a
    // QString (`= QFileInfo(...).canonicalFilePath()` or similar).
    // For each such line, check the next 5 lines for `startsWith(rootCanonical`.
    int violations = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &l = lines[i];
        if (l.find("canonicalFilePath()") == std::string::npos) continue;
        // Look at lines i+1 through i+5 inclusive.
        for (size_t j = i + 1; j < std::min(lines.size(), i + 6); ++j) {
            if (lines[j].find("startsWith(rootCanonical") != std::string::npos) {
                ++violations;
                ADD_FAILURE() << "inline anchor pair at remotecontrol.cpp:"
                              << (i + 1) << " → " << (j + 1)
                              << "\n  " << l << "\n  " << lines[j];
                break;
            }
        }
    }
    EXPECT_EQ(violations, 0);
}

// WI-4: count of PathValidation::validatePath( calls.
TEST(McpPathAnchorWiring, EightValidatePathCallsites) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    size_t count = 0;
    size_t pos = 0;
    while ((pos = rc.find("PathValidation::validatePath(", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    EXPECT_GE(count, 8u)
        << "expected at least 8 PathValidation::validatePath( calls in "
           "remotecontrol.cpp (workspace_search, file_outline, "
           "runLogOp, runDiffOp, resolveLaneFiles, "
           "indie_review_corroborate, debt_sweep_apply_fix, "
           "cold_eyes_cross_doc_diff); found "
        << count;
}

// WI-5: `bad_lane` no longer appears in cmdWorkspaceSearch's anchor
// block. The string may persist elsewhere in the file for other
// error codes, but the slice between the function header and the
// next function definition must contain zero `bad_lane`s.
TEST(McpPathAnchorWiring, WorkspaceSearchNoBadLane) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());

    auto wsStart = rc.find("QJsonDocument RemoteControl::cmdWorkspaceSearch(");
    ASSERT_NE(wsStart, std::string::npos);
    // Heuristic: next function definition starts with
    // `\nQJsonDocument RemoteControl::cmd` at the start of a line.
    auto wsEnd = rc.find("\nQJsonDocument RemoteControl::cmd", wsStart + 1);
    ASSERT_NE(wsEnd, std::string::npos);

    const std::string body = rc.substr(wsStart, wsEnd - wsStart);
    // Pin the quoted error-code emit, not the bare identifier — a
    // comment referring to `bad_lane` shouldn't trip the test.
    EXPECT_EQ(body.find("\"bad_lane\""), std::string::npos)
        << "`\"bad_lane\"` still emitted from cmdWorkspaceSearch — "
           "ANTS-1295 retired it in favour of `bad_path`";
}

// WI-6: CMakeLists.txt lists pathvalidation.cpp in ants_core_lib.
TEST(McpPathAnchorWiring, CMakeListsWiresPathValidation) {
    const std::string ck = ants_test::slurpFile(CMAKELISTS_PATH);
    ASSERT_FALSE(ck.empty());
    EXPECT_NE(ck.find("src/pathvalidation.cpp"), std::string::npos)
        << "ants_core_lib SOURCES list missing pathvalidation.cpp";
}

// ── PV-FB (ANTS-3430 / ANTS-3616) — the feedback-file carve-out is
// bounded by a DIRECTORY, not just a basename suffix.
//
// ANTS-3430 lets a `*_Ants_MCP_Feedback.md` escape the project root,
// because that shared cross-session file lives outside every project by
// convention. ANTS-3616 anchored it: the suffix used to be the only
// condition, so the exception admitted the name at any depth from any
// directory — and because apply_edits runs through this same chokepoint,
// that made the suffix a filesystem-wide write primitive.
//
// Fixture shape mirrors the live layout: <shared>/<project>/ with the
// feedback file in <shared>.
namespace {
struct FeedbackFixture {
    QTemporaryDir tmp;
    QString shared;    // canonical <shared>
    QString root;      // canonical <shared>/project
    bool build() {
        if (!tmp.isValid()) return false;
        shared = QFileInfo(tmp.path()).canonicalFilePath();
        if (!QDir(shared).mkpath(QStringLiteral("project"))) return false;
        if (!QDir(shared).mkpath(QStringLiteral("elsewhere/deep"))) return false;
        root = QFileInfo(shared + QStringLiteral("/project"))
                   .canonicalFilePath();
        return true;
    }
    bool write(const QString &rel) {
        QFile f(shared + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write("x");
        return true;
    }
};
}  // namespace

TEST(McpPathAnchor, FeedbackFileInParentAccepted) {
    FeedbackFixture fx;
    ASSERT_TRUE(fx.build());
    ASSERT_TRUE(fx.write(QStringLiteral("Proj_Ants_MCP_Feedback.md")));

    // The documented location: one level above the project root.
    const auto check = PathValidation::validatePath(
        QStringLiteral("../Proj_Ants_MCP_Feedback.md"), fx.root,
        QStringLiteral("apply_edits"));
    EXPECT_FALSE(check.bad)
        << "the conventional feedback location must stay reachable: "
        << QString::fromUtf8(
               QJsonDocument(check.err).toJson(QJsonDocument::Compact))
               .toStdString();
}

TEST(McpPathAnchor, FeedbackSuffixElsewhereRefused) {
    FeedbackFixture fx;
    ASSERT_TRUE(fx.build());
    ASSERT_TRUE(fx.write(
        QStringLiteral("elsewhere/deep/Evil_Ants_MCP_Feedback.md")));

    // Same suffix, wrong directory — pre-ANTS-3616 this was accepted, which
    // is what turned the naming convention into a write primitive.
    const auto check = PathValidation::validatePath(
        QStringLiteral("../elsewhere/deep/Evil_Ants_MCP_Feedback.md"),
        fx.root, QStringLiteral("apply_edits"));
    EXPECT_TRUE(check.bad)
        << "the carve-out must be bounded to the project root's parent, "
           "not granted to the suffix anywhere on the filesystem";
}

TEST(McpPathAnchor, FeedbackFileReachableFromProjectSubdirectory) {
    FeedbackFixture fx;
    ASSERT_TRUE(fx.build());
    ASSERT_TRUE(QDir(fx.root).mkpath(QStringLiteral("src/deep")));
    ASSERT_TRUE(fx.write(QStringLiteral("Proj_Ants_MCP_Feedback.md")));

    // resolveRootCanonical returns the caller's cwd verbatim — it does NOT
    // walk up to a git root — so a session launched from a subdirectory has
    // a root BELOW the shared dir. An immediate-parent-only rule would lock
    // exactly those sessions out of their own feedback file, which is why
    // the bound is the ancestor chain.
    const QString subRoot = QFileInfo(fx.root + QStringLiteral("/src/deep"))
                                .canonicalFilePath();
    ASSERT_FALSE(subRoot.isEmpty());

    const auto check = PathValidation::validatePath(
        fx.shared + QStringLiteral("/Proj_Ants_MCP_Feedback.md"), subRoot,
        QStringLiteral("apply_edits"));
    EXPECT_FALSE(check.bad)
        << "a session whose caller_cwd is a project subdirectory must still "
           "reach the shared feedback file; err="
        << check.err.value(QStringLiteral("error")).toString().toStdString();
}

TEST(McpPathAnchor, NonFeedbackFileInParentStillRefused) {
    FeedbackFixture fx;
    ASSERT_TRUE(fx.build());
    ASSERT_TRUE(fx.write(QStringLiteral("secrets.txt")));

    // The directory is right but the name is not — narrowing the carve-out
    // must not have widened it into "anything in the parent dir".
    const auto check = PathValidation::validatePath(
        QStringLiteral("../secrets.txt"), fx.root,
        QStringLiteral("apply_edits"));
    EXPECT_TRUE(check.bad);
}
