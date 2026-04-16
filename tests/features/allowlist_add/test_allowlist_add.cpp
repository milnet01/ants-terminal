// Feature-conformance test for spec.md — asserts
//   (A/B/C) rule normalisation + generalisation + subsumption
//           (every splitter, safety denylist, segment-aware subsumption).
//   (D)     hook-path button retraction: the production site must
//           connect toolFinished + sessionStopped, not only
//           claudePermissionCleared. Source-grep assertion.
//   (E)     openClaudeAllowlistDialog must call show+raise+activate so
//           the dialog becomes the active window on a frameless parent.
//           API-level + source-grep pair, same pattern as
//           tests/features/review_changes_click.
//
// Links against src/claudeallowlist.cpp for the rule-logic tests. The
// §D/§E assertions grep src/mainwindow.cpp directly (SRC_MAINWINDOW_PATH
// baked in by CMake) — no MainWindow instantiation required.
//
// Runs under QT_QPA_PLATFORM=offscreen; QApplication::activeWindow is
// reliable under offscreen (tracks the most-recently-activated
// top-level QWidget regardless of real WM).
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "claudeallowlist.h"

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QMainWindow>
#include <QString>

#include <cstdio>

namespace {

struct NormCase {
    const char *label;
    const char *input;
    const char *expected;
};

// `normalizeRule` contract. Each raw input → canonical form.
const NormCase kNormalize[] = {
    {"colon-to-space",       "Bash(git:*)",                "Bash(git *)"},
    {"colon-space-to-space", "Bash(git: *)",               "Bash(git *)"},
    {"bare-single-word",     "Bash(git)",                  "Bash(git *)"},
    {"bare-make-kept",       "Bash(make)",                 "Bash(make)"},
    {"bare-env-kept",        "Bash(env)",                  "Bash(env)"},
    {"webfetch-add-domain",  "WebFetch(example.com)",      "WebFetch(domain:example.com)"},
    {"read-add-slashslash",  "Read(/etc/passwd)",          "Read(//etc/passwd)"},
    {"write-add-slashslash", "Write(/tmp/x)",              "Write(//tmp/x)"},
};

struct GenCase {
    const char *label;
    const char *input;      // already normalized
    const char *expected;   // "" = return empty (no generalisation)
};

// `generalizeRule` contract — the splitter coverage is the headline.
const GenCase kGeneralize[] = {
    // Simple single-command cases.
    {"simple-git",           "Bash(git status --short)",           "Bash(git *)"},
    {"simple-gh",            "Bash(gh pr create --title x)",       "Bash(gh *)"},
    {"already-wildcarded",   "Bash(git *)",                        ""},
    {"already-double-wild",  "Bash(find **)",                      ""},

    // Safety denylist.
    {"rm-denylist",          "Bash(rm -rf /tmp/foo)",              ""},
    {"bare-sudo-denylist",   "Bash(sudo apt install foo)",         ""},
    {"SUDO_OTHER-denylist",  "Bash(SUDO_OTHER=x do-thing arg)",    ""},

    // SUDO_ASKPASS — specific form.
    {"sudo-askpass-specific",
     "Bash(SUDO_ASKPASS=/usr/libexec/ssh/ksshaskpass sudo -A zypper install foo)",
     "Bash(SUDO_ASKPASS=/usr/libexec/ssh/ksshaskpass sudo -A zypper *)"},

    // `&&` chains — preserve structure, wildcard each side.
    {"cd-and-cmd",           "Bash(cd /mnt/Storage && cmake --build build)",
                             "Bash(cd * && cmake *)"},
    {"make-and-echo",        "Bash(make && echo done)",
                             "Bash(make && echo *)"},
    {"triple-and",           "Bash(a && b && c x y)",
                             "Bash(a && b && c *)"},

    // `;` chains.
    {"semicolon-pair",       "Bash(git pull; make)",
                             "Bash(git * ; make)"},

    // `|` pipes.
    {"pipe-pair",            "Bash(cat file.txt | grep foo)",
                             "Bash(cat * | grep *)"},

    // `||` chains.
    {"or-pair",              "Bash(make || echo failed)",
                             "Bash(make || echo *)"},

    // rm on either side of a compound → denylist triggers.
    {"compound-with-rm-lhs", "Bash(rm -rf x && make)",             ""},
    {"compound-with-rm-rhs", "Bash(make && rm -rf x)",             ""},
};

// ----------------------------------------------------------------------------

int checkNormalize() {
    int failures = 0;
    for (const auto &c : kNormalize) {
        const QString got = ClaudeAllowlistDialog::normalizeRule(c.input);
        const bool ok = (got == QString::fromUtf8(c.expected));
        std::fprintf(stderr,
                     "[norm/%-24s] input='%s' got='%s' want='%s'  %s\n",
                     c.label, c.input, qUtf8Printable(got), c.expected,
                     ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    // Idempotency: normalize(normalize(x)) == normalize(x) for every case.
    for (const auto &c : kNormalize) {
        const QString once  = ClaudeAllowlistDialog::normalizeRule(c.input);
        const QString twice = ClaudeAllowlistDialog::normalizeRule(once);
        if (once != twice) {
            std::fprintf(stderr,
                "[norm/idempotent:%s] once='%s' twice='%s'  FAIL\n",
                c.label, qUtf8Printable(once), qUtf8Printable(twice));
            ++failures;
        }
    }
    return failures;
}

int checkGeneralize() {
    int failures = 0;
    for (const auto &c : kGeneralize) {
        const QString got = ClaudeAllowlistDialog::generalizeRule(c.input);
        const bool ok = (got == QString::fromUtf8(c.expected));
        std::fprintf(stderr,
                     "[gen/ %-24s] input='%s' got='%s' want='%s'  %s\n",
                     c.label, c.input, qUtf8Printable(got), c.expected,
                     ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }
    return failures;
}

struct SubCase {
    const char *label;
    const char *broad;
    const char *narrow;
    bool expected;
};

// `ruleSubsumes` contract — asserts segment-aware semantics. The key
// regressions this guards against:
//   (a) A simple "Bash(cmd *)" rule must NOT be reported as subsuming a
//       compound narrow rule whose segments don't all start with "cmd " —
//       that was the 0.6.22 bug where "Bash(make *)" wrongly blocked
//       "Bash(make * | tail * && ctest * | tail *)" from being added.
//   (b) Simple-vs-simple behavior unchanged: prefix match still subsumes.
//   (c) Compound narrow where EVERY segment does match the broad prefix
//       is legitimately subsumed (e.g. Bash(git *) covers
//       Bash(git status && git push)).
const SubCase kSubsume[] = {
    // (b) Simple subsumption — preserved from pre-0.6.23 behavior.
    {"simple-prefix",          "Bash(git *)",     "Bash(git status)",           true},
    {"simple-exact-prefix",    "Bash(git *)",     "Bash(git)",                  true},
    {"simple-different-cmd",   "Bash(git *)",     "Bash(make test)",            false},
    {"bare-tool-any",          "Read",            "Read(//etc/passwd)",         true},
    {"bare-tool-diff",         "Read",            "Write(//tmp/x)",             false},
    {"path-glob",              "Read(//etc/**)",  "Read(//etc/passwd)",         true},
    {"path-glob-diff-prefix",  "Read(//etc/**)",  "Read(//tmp/x)",              false},

    // (c) Compound narrow, every segment covered → legitimately subsumed.
    {"compound-all-git-and",   "Bash(git *)",
     "Bash(git status && git push)",                                            true},
    {"compound-all-git-pipe",  "Bash(git *)",
     "Bash(git log | git show)",                                                true},
    {"compound-all-make-semi", "Bash(make *)",
     "Bash(make build ; make test)",                                            true},

    // (a) THE HEADLINE BUG: simple broad does NOT subsume compound narrow
    // when any segment diverges. The exact 0.6.22 reproduction case:
    {"reproduction-case",      "Bash(make *)",
     "Bash(make * | tail * && ctest * | tail *)",                               false},
    {"compound-pipe-diverge",  "Bash(make *)",
     "Bash(make x | tail y)",                                                   false},
    {"compound-and-diverge",   "Bash(make *)",
     "Bash(make x && echo done)",                                               false},
    {"compound-or-diverge",    "Bash(make *)",
     "Bash(make x || echo failed)",                                             false},
    {"compound-semi-diverge",  "Bash(git *)",
     "Bash(git status ; make test)",                                            false},

    // Compound broad: err on the side of false (safe) — matcher semantics
    // for segment-wise compound-vs-compound aren't modeled yet. A false
    // negative means at worst a redundant rule; a false positive blocks
    // the user from adding a rule they need.
    {"compound-broad-skip",    "Bash(make * && echo *)",
     "Bash(make test && echo done)",                                            false},
};

int checkSubsume() {
    int failures = 0;
    for (const auto &c : kSubsume) {
        const bool got = ClaudeAllowlistDialog::ruleSubsumes(
            QString::fromUtf8(c.broad), QString::fromUtf8(c.narrow));
        const bool ok = (got == c.expected);
        std::fprintf(stderr,
                     "[sub/ %-24s] broad='%s' narrow='%s' got=%s want=%s  %s\n",
                     c.label, c.broad, c.narrow,
                     got ? "true" : "false",
                     c.expected ? "true" : "false",
                     ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }
    return failures;
}

// --- §E API-level invariant -------------------------------------------------
//
// A QDialog shown on a frameless QMainWindow parent becomes the active
// window ONLY after show+raise+activateWindow. show() + raise() alone
// is not sufficient — the dialog is raised in the stacking order but
// not marked as the input-focus target, so when a queued refocus (from
// the focusChanged redirect lambda in mainwindow.cpp) fires it can
// re-activate the main window over it. Exactly the same invariant as
// tests/features/review_changes_click's Invariant A; we duplicate it
// here so the allowlist-add test fails independently if the dialog's
// show sequence ever reverts.

int checkDialogActivation() {
    int failures = 0;

    QMainWindow win;
    // Match the project's MainWindow flag at mainwindow.cpp:74. Without
    // FramelessWindowHint, most WMs auto-activate child dialogs via the
    // WM_TRANSIENT_FOR hint on decorated windows — the bug only
    // manifests on frameless parents.
    win.setWindowFlag(Qt::FramelessWindowHint);
    win.resize(800, 600);
    win.show();
    QApplication::processEvents();

    if (QApplication::activeWindow() != &win) {
        std::fprintf(stderr,
            "[E1/harness-precondition] main window not active after show — "
            "QT_QPA_PLATFORM=offscreen not set?  FAIL\n");
        ++failures;
    }

    auto *dialog = new QDialog(&win);
    dialog->setWindowTitle("Claude Code Allowlist");
    dialog->resize(400, 300);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    // The exact sequence openClaudeAllowlistDialog must end with.
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    QApplication::processEvents();

    if (!dialog->isVisible()) {
        std::fprintf(stderr,
            "[E1/visible] dialog not visible after show()  FAIL\n");
        ++failures;
    }
    if (QApplication::activeWindow() != dialog) {
        std::fprintf(stderr,
            "[E1/active] dialog did not become active window after "
            "raise()+activateWindow()  FAIL\n");
        ++failures;
    }

    dialog->close();
    QApplication::processEvents();
    return failures;
}

// --- Helpers for production-binding assertions ------------------------------

// Extract the body of a top-level function by name from a source string.
// Returns empty QString if the function isn't found or braces don't
// balance. Caller is responsible for matching the canonical signature
// passed in.
QString extractFunctionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    int braceStart = src.indexOf(QChar('{'), start);
    if (braceStart < 0) return QString();
    int depth = 1;
    int i = braceStart + 1;
    while (i < src.size() && depth > 0) {
        QChar c = src.at(i);
        if (c == QChar('{')) ++depth;
        else if (c == QChar('}')) --depth;
        ++i;
    }
    if (depth != 0) return QString();
    return src.mid(braceStart, i - braceStart);
}

// Read src/mainwindow.cpp. Path baked in by CMake as SRC_MAINWINDOW_PATH.
QString readMainWindowSrc() {
    const QString path = QStringLiteral(SRC_MAINWINDOW_PATH);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "FAIL: cannot open %s — test harness wiring broken\n",
                     qUtf8Printable(path));
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

// --- §E production binding: openClaudeAllowlistDialog calls show+raise+activate ---

int checkDialogShowSequence() {
    int failures = 0;
    const QString src = readMainWindowSrc();
    if (src.isEmpty()) return 1;

    const QString body = extractFunctionBody(
        src,
        QStringLiteral(
            "void MainWindow::openClaudeAllowlistDialog("
            "const QString &prefillRule)"));
    if (body.isEmpty()) {
        std::fprintf(stderr,
            "[E2/find] openClaudeAllowlistDialog not found in mainwindow.cpp "
            "— function renamed?  FAIL\n");
        return 1;
    }

    // We don't care about the exact receiver expression (m_claudeDialog
    // vs. m_claudeDialog-> vs. some wrapped call) — what matters is that
    // the three calls all appear inside the function body. A simple
    // substring check is sufficient because this function body contains
    // no string literals that mention these method names.
    struct Req {
        const char *needle;
        const char *why;
    };
    const Req kRequired[] = {
        {"->show()",
         "openClaudeAllowlistDialog no longer calls show() on the dialog"},
        {"->raise()",
         "openClaudeAllowlistDialog no longer calls raise() — dialog may open "
         "behind frameless main window (0.6.31 bug regression)"},
        {"->activateWindow()",
         "openClaudeAllowlistDialog no longer calls activateWindow() — user "
         "reports 'Add to allowlist click does nothing' (0.6.31 bug regression)"},
    };
    for (const auto &r : kRequired) {
        if (!body.contains(QString::fromUtf8(r.needle))) {
            std::fprintf(stderr, "[E2/%s] %s  FAIL\n", r.needle, r.why);
            ++failures;
        }
    }
    return failures;
}

// --- §D production binding: hook-path permissionRequested handler --------
//
// The user-reported 0.6.30 bug: the "Add to allowlist" button lingered
// on the status bar long after the underlying prompt was resolved.
// Root cause: the hook-path (ClaudeIntegration::permissionRequested)
// button listened ONLY to TerminalWidget::claudePermissionCleared for
// retraction — a signal gated on the terminal scroll-scanner having
// independently observed the prompt (terminalwidget.cpp:3658). When a
// prompt reaches the hook but not the scanner (unmatched footer
// format, prompt off-screen, headless session), the clear signal
// never fires and the button is orphaned.
//
// The fix: also connect toolFinished and sessionStopped from
// ClaudeIntegration. Proxy signals, but Claude Code has no canonical
// PermissionResolved event.
//
// This assertion reads mainwindow.cpp and checks that the lambda body
// wired to `permissionRequested` contains connect() calls for those
// two signals. If a future refactor drops either connect, the button
// goes back to hanging indefinitely on hook-only prompts — exactly
// the user-reported 0.6.30 symptom.

int checkHookPathRetraction() {
    int failures = 0;
    const QString src = readMainWindowSrc();
    if (src.isEmpty()) return 1;

    // Locate the connect() to permissionRequested — the lambda body
    // following it contains the button creation and the retraction
    // wiring we care about. We bracket the search between the
    // permissionRequested connect and the next top-level connect /
    // function call that follows the hook handler block. The reliable
    // anchor is the "Set up MCP server" comment that immediately
    // follows the permissionRequested handler.
    const QString startAnchor =
        QStringLiteral("ClaudeIntegration::permissionRequested");
    const QString endAnchor =
        QStringLiteral("// Set up MCP server");
    const int startIdx = src.indexOf(startAnchor);
    const int endIdx = src.indexOf(endAnchor, startIdx);
    if (startIdx < 0 || endIdx < 0 || endIdx <= startIdx) {
        std::fprintf(stderr,
            "[D/locate] permissionRequested handler not found between "
            "expected anchors — refactor broke the test harness's scope bracketing.  FAIL\n");
        return 1;
    }
    const QString handler = src.mid(startIdx, endIdx - startIdx);

    struct Req {
        const char *needle;
        const char *why;
    };
    const Req kRequired[] = {
        {"ClaudeIntegration::toolFinished",
         "hook-path permissionRequested handler no longer connects "
         "toolFinished — button orphaned when scroll-scanner never "
         "observed the prompt (0.6.30 bug regression)"},
        {"ClaudeIntegration::sessionStopped",
         "hook-path permissionRequested handler no longer connects "
         "sessionStopped — button orphaned when session ends without "
         "scroll-scan seeing the prompt (0.6.30 bug regression)"},
        // Existing connection must also still be present — this guards
        // against someone \"fixing\" the new wiring by replacing rather
        // than adding. The TerminalWidget path is still the fastest
        // retraction trigger when it does fire.
        {"TerminalWidget::claudePermissionCleared",
         "hook-path permissionRequested handler dropped "
         "claudePermissionCleared — loses the fast retraction path when "
         "scroll-scanner does see the prompt"},
    };
    for (const auto &r : kRequired) {
        if (!handler.contains(QString::fromUtf8(r.needle))) {
            std::fprintf(stderr, "[D/%s] %s  FAIL\n", r.needle, r.why);
            ++failures;
        }
    }
    return failures;
}

}  // namespace

int main(int argc, char **argv) {
    // QApplication (not QCoreApplication) — §E uses QDialog + QMainWindow,
    // which require the GUI app singleton. Rule-logic tests (§A/B/C) are
    // GUI-independent but a QApplication covers them too (it IS-A
    // QCoreApplication).
    QApplication app(argc, argv);

    int failures = 0;
    failures += checkNormalize();
    failures += checkGeneralize();
    failures += checkSubsume();
    failures += checkDialogActivation();
    failures += checkDialogShowSequence();
    failures += checkHookPathRetraction();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d case(s) failed.\n", failures);
        return 1;
    }
    return 0;
}
