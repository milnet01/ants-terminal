// Feature-conformance test for spec.md (ANTS-5059) — the Review Changes
// diff has no size cap and is rebuilt/rendered as HTML on the GUI thread
// on every change burst.
//
// Three RED invariants (defect still live as of this test's authoring) and
// one GUARD (must already hold and must keep holding):
//   INV-1 (RED, behavioural)   — a multi-MiB rewrite of a tracked file is
//                                 capped and the render says so.
//   INV-2 (RED, behavioural)   — the probe QProcesses are children of the
//                                 dialog, not of the caller's widget.
//   INV-3 (RED, source-scrape) — a generation/round counter gates a stale
//                                 refresh's render.
//   INV-4 (GUARD, behavioural) — an ordinary small diff still renders in
//                                 full, with no truncation notice.
//
// A fifth, separately-reported RED invariant (ANTS-5128, still live):
//   INV-5 (RED, behavioural)   — closing the dialog must cancel its probe
//                                 QProcesses before they are destroyed, so
//                                 none is destroyed while still running and
//                                 no probe handler runs against a dialog
//                                 mid-teardown.
//
// See spec.md for the full contract, rationale, and why each check is
// shaped the way it is.

#include "diffviewer.h"

#include "../../_support/srcgrep.h"

#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextStream>
#include <QWidget>

#include <cstdio>
#include <regex>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#ifndef SRC_DIFFVIEWER_CPP_PATH
#  error "SRC_DIFFVIEWER_CPP_PATH compile definition required"
#endif

namespace {

bool gitOnPath() {
    return !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
}

// Run a git subcommand in `cwd`; true on exit 0. Mirrors
// tests/features/roadmap_inprogress_age's runGit() helper.
bool runGit(const QString &cwd, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(cwd);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(5000)) return false;
    if (!p.waitForFinished(60000)) {
        p.kill();
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

bool writeTextFile(const QString &path, const QString &content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    QTextStream out(&f);
    out << content;
    f.close();
    return true;
}

// git init + local identity + first commit of `relPath` == `content1`.
// A LOCAL identity (not global) so the fixture never depends on — or
// touches — the host's git config.
bool makeRepoAndCommit(const QString &dir, const QString &relPath,
                       const QString &content1) {
    if (!runGit(dir, {QStringLiteral("init"), QStringLiteral("-q")})) return false;
    if (!runGit(dir, {QStringLiteral("config"), QStringLiteral("user.name"),
                      QStringLiteral("Test")})) return false;
    if (!runGit(dir, {QStringLiteral("config"), QStringLiteral("user.email"),
                      QStringLiteral("test@example.com")})) return false;
    if (!writeTextFile(dir + QLatin1Char('/') + relPath, content1)) return false;
    if (!runGit(dir, {QStringLiteral("add"), relPath})) return false;
    if (!runGit(dir, {QStringLiteral("commit"), QStringLiteral("-q"),
                      QStringLiteral("-m"), QStringLiteral("initial")})) return false;
    return true;
}

// Pump the event loop until `pred()` is true or `budgetMs` elapses. The
// diffviewer's probes are async QProcess callbacks, so this is how the
// test observes finalize() having run — same shape as
// tests/features/tree_watcher's pumpUntil().
template <typename Pred>
bool pumpUntil(Pred pred, int budgetMs) {
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < budgetMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return pred();
}

// The dialog starts with a "Loading git status..." placeholder (see
// diffviewer.cpp) and replaces it wholesale once every probe has
// finished and finalize() has rendered. Its disappearance is the signal
// that the async round is done, regardless of how big the real diff is.
bool waitForRealRender(QTextBrowser *viewer, int budgetMs) {
    return pumpUntil([viewer] {
        return !viewer->toPlainText().contains(QStringLiteral("Loading git status"));
    }, budgetMs);
}

// Extract diffviewer::show's body from the RAW (uncommented) source, so
// the end anchor `}  // namespace diffviewer` — itself a comment — still
// matches. Comments are stripped from the EXTRACTED substring afterward,
// by the one caller that needs a comment-free search space (INV-3).
std::string showBodyRaw(const std::string &src) {
    const std::string sig = "QDialog *show(QWidget *parent,";
    const auto start = src.find(sig);
    if (start == std::string::npos) return {};
    const auto end = src.find("}  // namespace diffviewer", start);
    return src.substr(start, end == std::string::npos ? std::string::npos
                                                       : end - start);
}

// Sink for Qt's own warnings during the close-while-running window.
// qInstallMessageHandler takes a plain function pointer, so the captured
// text has to live at file scope.
QStringList g_qtMessages;
bool g_capturing = false;

void captureQtMessage(QtMsgType type, const QMessageLogContext &ctx,
                      const QString &msg) {
    if (g_capturing) g_qtMessages << msg;
    Q_UNUSED(type);
    Q_UNUSED(ctx);
}

}  // namespace

// ---------------------------------------------------------------------
// INV-1 (RED, behavioural) — the patch has no byte cap.
// ---------------------------------------------------------------------
TEST(ReviewChangesDiffCap, LargeDiffIsCappedAndTruncated) {
    if (!gitOnPath()) GTEST_SKIP() << "git not in PATH";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();

    ASSERT_TRUE(makeRepoAndCommit(dir, QStringLiteral("big.txt"),
        QStringLiteral("line one\nline two\nline three\nline four\nline five\n")))
        << "git fixture setup failed";

    // Rewrite the TRACKED file with ~100,000 distinct lines, uncommitted —
    // the ANTS-5059 scenario (a lockfile / minified bundle / regenerated
    // ROADMAP.md rewrite): `git diff --stat --patch HEAD` renders the
    // whole thing, several MiB, as one HTML string built on the GUI
    // thread.
    QString big;
    constexpr int kLines = 100000;
    big.reserve(kLines * 28);
    for (int i = 0; i < kLines; ++i) {
        big += QStringLiteral("line %1 xxxxxxxxxxxxxxxxxxxxx\n")
                   .arg(i, 6, 10, QLatin1Char('0'));
    }
    ASSERT_TRUE(writeTextFile(dir + QStringLiteral("/big.txt"), big));
    const qint64 rawApproxBytes = big.toUtf8().size();
    ASSERT_GT(rawApproxBytes, 2 * 1024 * 1024)
        << "fixture diff should be several MiB; got only " << rawApproxBytes
        << " bytes — widen the fixture before trusting this test's result";

    // A real parent: before ANTS-5059, show() used it as the context object
    // of every probe callback, and Qt refuses a null context, so with
    // nullptr no probe would ever report back.
    QWidget host;
    QDialog *dialog = diffviewer::show(&host, dir, QStringLiteral("Dark"));
    ASSERT_NE(dialog, nullptr);
    auto *viewer = dialog->findChild<QTextBrowser *>();
    ASSERT_NE(viewer, nullptr)
        << "Review Changes dialog has no QTextBrowser child — widget "
           "topology changed, test harness needs updating";

    // Generous: pre-fix, this is exactly the multi-second-to-tens-of-
    // seconds GUI-thread freeze ANTS-5059 describes; it must still
    // finish, just slowly.
    const bool rendered = waitForRealRender(viewer, 120000);
    ASSERT_TRUE(rendered)
        << "diff viewer never replaced its loading placeholder within "
           "120s of a large-diff refresh";

    const QString text = viewer->toPlainText();
    // Generous bound (2 MiB) against the intended ~1 MiB cap, well below
    // the several-MiB raw diff, and matching wording containing
    // "truncated" the way the existing New-files 200 KB cap already
    // does (see diffviewer.cpp's "(truncated at 200 KB)").
    EXPECT_LT(text.size(), 2 * 1024 * 1024)
        << "viewer text is " << text.size() << " bytes (raw diff fixture "
           "was " << rawApproxBytes << " bytes) — the diff patch has no "
           "byte cap (ANTS-5059)";
    EXPECT_TRUE(text.contains(QStringLiteral("truncated")))
        << "viewer text does not mention truncation for a multi-MiB diff "
           "(ANTS-5059 — the patch is rendered in full, uncapped). Viewer "
           "text size: " << text.size() << " bytes";

    dialog->close();
    QCoreApplication::processEvents();
}

// ---------------------------------------------------------------------
// INV-2 (RED, behavioural) — probe QProcesses are children of the
// caller's widget, not of the dialog, so closing the dialog cannot kill
// them.
// ---------------------------------------------------------------------
TEST(ReviewChangesDiffCap, ProbeProcessesParentedToDialog) {
    if (!gitOnPath()) GTEST_SKIP() << "git not in PATH";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();
    ASSERT_TRUE(makeRepoAndCommit(dir, QStringLiteral("f.txt"),
                                  QStringLiteral("v1\n")))
        << "git fixture setup failed";
    ASSERT_TRUE(writeTextFile(dir + QStringLiteral("/f.txt"),
                              QStringLiteral("v2\n")));

    // Stand-in for MainWindow, the real-world `parent` argument.
    QWidget callerStandIn;

    QDialog *dialog = diffviewer::show(&callerStandIn, dir, QStringLiteral("Dark"));
    ASSERT_NE(dialog, nullptr);

    // Checked IMMEDIATELY after show() returns, with no processEvents()
    // in between: runProbes() constructs and starts all five git
    // QProcess objects synchronously inside show() (QProcess::start() is
    // async — it does not run or finish within the call), so they are
    // still alive as children of whatever they were parented to at this
    // point.
    //
    // This checks the dialog side only (not "callerStandIn has zero
    // QProcess children"): show() also spawns a `rev-parse` / `ls-files`
    // pair via reseed()/enumerate() for the live-refresh watcher, and the
    // fix's own scope note ("Fix: cap the patch bytes ... and cancel
    // superseded probes with a generation counter") only commits to
    // reparenting "the probe QProcesses" — not necessarily every process
    // the dialog ever spawns. Asserting the caller ends up with zero
    // QProcess children would bind this test to an implementation
    // decision the roadmap item doesn't make. Asserting the dialog gets
    // at least one is the part every reading of the fix agrees on, and it
    // is false today.
    const auto dialogProcs = dialog->findChildren<QProcess *>();
    EXPECT_FALSE(dialogProcs.isEmpty())
        << "no QProcess is parented to the Review Changes dialog right "
           "after show() returns — the probe processes are parented to "
           "the caller's widget instead, so closing the dialog cannot "
           "kill them (ANTS-5059)";

    dialog->close();
    QCoreApplication::processEvents();
}

// ---------------------------------------------------------------------
// INV-3 (RED, source-scrape) — a generation/round counter gates a stale
// refresh's render.
// ---------------------------------------------------------------------
TEST(ReviewChangesDiffCap, GenerationCheckGatesStaleRender) {
    const std::string raw = ants_test::slurpFile(SRC_DIFFVIEWER_CPP_PATH);
    ASSERT_FALSE(raw.empty()) << "cannot read " << SRC_DIFFVIEWER_CPP_PATH;

    const std::string bodyRaw = showBodyRaw(raw);
    ASSERT_FALSE(bodyRaw.empty())
        << "cannot locate `QDialog *show(QWidget *parent,` in "
        << SRC_DIFFVIEWER_CPP_PATH << " — signature changed, test needs "
           "updating";
    // Comments stripped so a rationale comment naming "generation" (as
    // this very test's own comments do, describing the fix) can never
    // satisfy the check in place of real code.
    const std::string body = ants_test::stripComments(bodyRaw);

    int failures = 0;
    auto fail = [&](const char *what) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    };

    // Spelling-tolerant needles: an identifier containing "generation",
    // "epoch", "sequence", or "seq" (case-insensitive). "generation" is
    // this codebase's existing convention for exactly this shape —
    // `m_sendGeneration` / `++m_sendGeneration` in src/llmclient.cpp,
    // `autoProfileRulesGeneration()` in src/config.h — so it is the most
    // likely spelling; the others are accepted so a differently-named
    // but equivalent counter still passes.
    static const std::regex reBump(
        R"(\+\+\s*[A-Za-z0-9_]*(generation|epoch|sequence|seq)[A-Za-z0-9_]*\b)"
        R"(|\b[A-Za-z0-9_]*(generation|epoch|sequence|seq)[A-Za-z0-9_]*\s*\+\+)",
        std::regex::icase);
    if (!std::regex_search(body, reBump))
        fail("no `++` bump on a generation/epoch/sequence/seq-named counter "
             "found in diffviewer::show — a refresh round that isn't "
             "tagged as a new generation can't be told apart from a "
             "superseded one (ANTS-5059)");

    // A comparison that gates an early return — e.g.
    // `if (state->generation != *currentGeneration) return;`, mirroring
    // the `if (gen != m_sendGeneration) return;` shape in
    // src/llmclient.cpp's emitDeferredError().
    static const std::regex reGate(
        R"(if\s*\([^)]*\b[A-Za-z0-9_]*(generation|epoch|sequence|seq)[A-Za-z0-9_]*\b[^)]*(!=|==)[^)]*\)\s*\{?\s*return)",
        std::regex::icase);
    std::smatch gateMatch;
    const bool hasGate = std::regex_search(body, gateMatch, reGate);
    if (!hasGate)
        fail("no `if (...generation/epoch/sequence/seq... != ...) return` "
             "(or `==`) guard found — finalize has no way to refuse "
             "rendering a round that is no longer the newest, so an older "
             "slow diff can overwrite a newer view (ANTS-5059)");

    const auto setHtmlPos = body.find("viewerGuard->setHtml(");
    if (setHtmlPos == std::string::npos) {
        fail("cannot locate `viewerGuard->setHtml(` anchor in the "
             "comment-stripped function body");
    } else if (hasGate) {
        const auto gatePos =
            static_cast<std::string::size_type>(gateMatch.position(0));
        if (gatePos >= setHtmlPos)
            fail("the generation/epoch/sequence/seq guard appears AFTER "
                 "`viewerGuard->setHtml(` — it must gate the render, not "
                 "follow it (ANTS-5059)");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see spec.md INV-3\n", failures);
        FAIL();
    }
    std::printf("OK: generation-guard invariant present\n");
}

// ---------------------------------------------------------------------
// INV-4 (GUARD, behavioural) — an ordinary small diff still renders in
// full, with no truncation notice. Must hold before AND after the fix.
// ---------------------------------------------------------------------
TEST(ReviewChangesDiffCap, SmallDiffRendersFullyNoTruncationNotice) {
    if (!gitOnPath()) GTEST_SKIP() << "git not in PATH";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();
    ASSERT_TRUE(makeRepoAndCommit(dir, QStringLiteral("small.txt"),
        QStringLiteral("alpha\nbeta\ngamma\n")))
        << "git fixture setup failed";
    ASSERT_TRUE(writeTextFile(dir + QStringLiteral("/small.txt"),
        QStringLiteral("alpha\nUNIQUE_MARKER_ANTS5059\ngamma\n")));

    // A real parent: before ANTS-5059, show() used it as the context object
    // of every probe callback, and Qt refuses a null context, so with
    // nullptr no probe would ever report back.
    QWidget host;
    QDialog *dialog = diffviewer::show(&host, dir, QStringLiteral("Dark"));
    ASSERT_NE(dialog, nullptr);
    auto *viewer = dialog->findChild<QTextBrowser *>();
    ASSERT_NE(viewer, nullptr);

    const bool rendered = waitForRealRender(viewer, 30000);
    ASSERT_TRUE(rendered)
        << "diff viewer never replaced its loading placeholder within "
           "30s of an ordinary small-diff refresh";

    const QString text = viewer->toPlainText();
    EXPECT_TRUE(text.contains(QStringLiteral("UNIQUE_MARKER_ANTS5059")))
        << "an ordinary small diff did not render its own added line — "
           "viewer text was:\n" << text.left(2000).toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("truncated")))
        << "an ordinary small diff carries a truncation notice it should "
           "not — viewer text was:\n" << text.left(2000).toStdString();

    dialog->close();
    QCoreApplication::processEvents();
}

// ---------------------------------------------------------------------
// INV-5 (RED, behavioural — ANTS-5128) — closing the dialog while its
// probe QProcesses are still running must not block the GUI thread in
// ~QProcess::waitForFinished(), and must not re-enter a probe's
// finished/errorOccurred handler against a dialog mid-teardown.
// ---------------------------------------------------------------------
TEST(ReviewChangesDiffCap, ClosingDoesNotDestroyRunningProbes) {
    if (!gitOnPath()) GTEST_SKIP() << "git not in PATH";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();
    // Built with the REAL git — runGit()/makeRepoAndCommit() resolve "git"
    // via the untouched process PATH, before the slow shim below exists.
    ASSERT_TRUE(makeRepoAndCommit(dir, QStringLiteral("f.txt"),
                                  QStringLiteral("v1\n")))
        << "git fixture setup failed";
    ASSERT_TRUE(writeTextFile(dir + QStringLiteral("/f.txt"),
                              QStringLiteral("v2\n")));

    // A slow `git` shim, first on PATH, so every probe is still running
    // when the dialog closes. A real probe on a one-file repo finishes in
    // milliseconds, which is why this defect reached main: it is only
    // reachable while a probe is in flight, and only a loaded CI runner
    // was slow enough to get there.
    QTemporaryDir shimDir;
    ASSERT_TRUE(shimDir.isValid());
    const QString shimPath = shimDir.path() + QStringLiteral("/git");
    ASSERT_TRUE(writeTextFile(shimPath, QStringLiteral("#!/bin/sh\nsleep 6\n")))
        << "could not write the slow git shim";
    ASSERT_TRUE(QFile::setPermissions(shimPath,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
        QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther))
        << "could not make the slow git shim executable";

    const QByteArray originalPath = qgetenv("PATH");
    qputenv("PATH", shimDir.path().toUtf8() + ":" + originalPath);

    QWidget host;
    QDialog *dialog = diffviewer::show(&host, dir, QStringLiteral("Dark"));

    // Every QProcess show() starts is launched synchronously inside that
    // call, so the real PATH goes back now — before any assertion that
    // could return early and leave a later test in this binary running
    // against the shim.
    qputenv("PATH", originalPath);

    ASSERT_NE(dialog, nullptr);
    QPointer<QDialog> guard(dialog);

    const auto probes = dialog->findChildren<QProcess *>();
    ASSERT_FALSE(probes.isEmpty())
        << "no QProcess is parented to the dialog right after show() — "
           "cannot exercise the close-while-running path (fixture or "
           "parentage regressed)";

    // The probes must be RUNNING, not merely Starting. ~QProcess takes a
    // different path for a process that never reached Running: it emits
    // nothing, so closing during Starting exercises none of this and the
    // test would pass while the defect is live. Measured 2026-09-12.
    ASSERT_TRUE(pumpUntil([&probes] {
        for (QProcess *p : probes)
            if (p->state() != QProcess::Running) return false;
        return true;
    }, 10000))
        << "the probe processes never reached Running — the slow-git shim "
           "did not take effect, so this run cannot discriminate the defect";

    g_qtMessages.clear();
    g_capturing = true;
    QtMessageHandler previous = qInstallMessageHandler(captureQtMessage);
    dialog->close();
    const bool destroyed = pumpUntil([&guard] { return guard.isNull(); }, 20000);
    g_capturing = false;
    qInstallMessageHandler(previous);

    ASSERT_TRUE(destroyed)
        << "the dialog was not destroyed after close() while its git probes "
           "were still running (ANTS-5128)";

    // Qt warns whenever a QProcess is destroyed in a running state — which
    // is the defect itself: the dialog's destructor deletes its running
    // probe children, and ~QProcess then re-emits finished() into a handler
    // that dereferences QPointer<QDialog> on a dialog already degraded to
    // QWidget (UBSan: "downcast of address ... object is of type QWidget").
    // Closing must cancel the probes BEFORE they are destroyed, so no probe
    // is ever destroyed while running.
    QStringList destroyedWhileRunning;
    for (const QString &m : std::as_const(g_qtMessages)) {
        if (m.contains(QStringLiteral("Destroyed while process")))
            destroyedWhileRunning << m;
    }
    EXPECT_TRUE(destroyedWhileRunning.isEmpty())
        << "closing the dialog destroyed " << destroyedWhileRunning.size()
        << " still-running probe process(es) of " << probes.size()
        << "; Qt reported: "
        << destroyedWhileRunning.join(QStringLiteral(" | ")).toStdString()
        << " (ANTS-5128)";
}
