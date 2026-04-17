// Feature-conformance test for spec.md — locks the contract that the
// Review Changes status-bar button is never in the visible-but-disabled
// state, because Qt swallows clicks on disabled buttons (no clicked()
// emission, so showDiffViewer's silent-return-with-flash guards never
// fire) AND the global QPushButton:hover rule isn't :enabled-gated
// (advertising the disabled button as actionable on hover).
//
// Regression vector: user report 2026-04-17 — "Review Changes is
// showing and is active as it has an onmouseover event that highlights
// the button. When I click the button though, nothing happens."
// Root cause: refreshReviewButton's exitCode==0 (clean repo) branch
// called setEnabled(false); show() instead of hide().
//
// Source-level inspection rather than instantiating MainWindow —
// MainWindow drags in terminalwidget + claudeintegration + ~half the
// project just to exercise the six-line policy block. The contract IS
// the policy ("clean → hide, not show-disabled"); locking the source
// at the policy site is the sharpest possible regression guard.
//
// Same approach as test_review_changes_click.cpp and
// test_shift_enter.cpp.

#include <QFile>
#include <QString>
#include <QStringList>
#include <QRegularExpression>

#include <cstdio>

namespace {
int failures = 0;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);  \
        ++failures;                                                          \
    }                                                                        \
} while (0)

QString readSource(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "FAIL: cannot open %s — test harness wiring broken\n",
                     qUtf8Printable(path));
        ++failures;
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

// Extract the body of a function (signature → matching closing brace).
// Same balanced-brace scan used by test_review_changes_click.cpp; no
// string-literal escapes complicate the sites we care about.
QString extractFunctionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int braceStart = src.indexOf(QChar('{'), start);
    if (braceStart < 0) return QString();
    int depth = 1;
    int i = braceStart + 1;
    while (i < src.size() && depth > 0) {
        QChar c = src.at(i);
        if (c == QChar('{')) ++depth;
        else if (c == QChar('}')) --depth;
        ++i;
    }
    return src.mid(braceStart, i - braceStart);
}

// Invariant 1 — refreshReviewButton's clean-repo branch hides the
// button. We grep for `btn->hide()` after an `exitCode == 0` token
// in the function body. The regression shape is `btn->setEnabled(false)`
// followed by `btn->show()` in that same branch — also asserted absent.
void testRefreshHidesOnCleanRepo() {
    const QString src = readSource(QStringLiteral(SRC_MAINWINDOW_PATH));
    if (src.isEmpty()) return;

    const QString body = extractFunctionBody(
        src, QStringLiteral("void MainWindow::refreshReviewButton()"));
    if (body.isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: refreshReviewButton not found — function renamed?\n");
        ++failures;
        return;
    }

    // Locate the exitCode == 0 branch and lift out a slice of body
    // around it for shape assertions. The branch is small (≤25 lines)
    // so a trailing slice covers the whole policy block reliably.
    const int cleanIdx = body.indexOf(QStringLiteral("exitCode == 0"));
    CHECK(cleanIdx >= 0,
          "refreshReviewButton has no `exitCode == 0` branch — git-quiet "
          "exit-code policy was restructured; spec needs review");
    if (cleanIdx < 0) return;

    // Slice forward to the next sibling branch (`} else`) or 2000 chars,
    // whichever is shorter. A long explanatory comment can push the
    // actual `btn->hide()` past a fixed-size cap; bound by the next
    // else clause keeps the slice tight to this branch and rejects
    // matches from the unrelated error-fallback branch below.
    const int sliceMax = 2000;
    int sliceEnd = body.indexOf(QStringLiteral("} else"), cleanIdx + 1);
    if (sliceEnd < 0 || sliceEnd > cleanIdx + sliceMax) sliceEnd = cleanIdx + sliceMax;
    const QString slice = body.mid(cleanIdx, sliceEnd - cleanIdx);

    CHECK(slice.contains(QStringLiteral("btn->hide()")),
          "refreshReviewButton's clean-repo branch (exitCode == 0) does NOT "
          "call btn->hide() — regression: visible-but-disabled state silently "
          "swallows clicks (Qt drops mousePressEvent on disabled buttons), "
          "so user sees the button highlight on hover but get NO feedback "
          "on click. See review_changes_clickable/spec.md.");

    // The forbidden shape: setEnabled(false) followed by show() inside
    // the exit-0 slice. Tolerates whitespace + comments between.
    static const QRegularExpression forbidden(
        QStringLiteral(R"(setEnabled\s*\(\s*false\s*\)[^;]*;[^}]*?->show\s*\(\s*\))"));
    CHECK(!forbidden.match(slice).hasMatch(),
          "refreshReviewButton's clean-repo branch contains the forbidden "
          "shape `setEnabled(false); ... show();` on the Review Changes "
          "button — re-introduces the user-reported click-does-nothing bug.");
}

// Invariant 2 — the global QPushButton stylesheet rule remains UN-gated
// by :enabled. If a future refactor adds `:enabled` to the hover rule,
// then the underlying assumption of this spec (visible-disabled = bad
// because hover lies) shifts and the spec itself needs revisiting.
//
// This is a "tripwire" check: failure means the spec needs review,
// not necessarily that there's a bug. The error message says so.
void testHoverStylesheetUngated() {
    const QString src = readSource(QStringLiteral(SRC_MAINWINDOW_PATH));
    if (src.isEmpty()) return;

    // Look for the global QPushButton stylesheet block. The 0.6.x
    // codebase keeps it in setupUi or applyTheme; we search for the
    // canonical token "QPushButton:hover" and check what comes next.
    int pos = 0;
    bool foundAny = false;
    bool foundEnabledGate = false;
    while (true) {
        const int idx = src.indexOf(QStringLiteral("QPushButton:hover"), pos);
        if (idx < 0) break;
        foundAny = true;
        // Look at the next 20 chars for ":enabled" — that's the gate
        // form `QPushButton:hover:enabled` or `QPushButton:enabled:hover`.
        const QString tail = src.mid(idx, 30);
        if (tail.contains(QStringLiteral(":enabled"))) {
            foundEnabledGate = true;
            break;
        }
        pos = idx + 1;
    }
    CHECK(foundAny,
          "no `QPushButton:hover` rule found in src/mainwindow.cpp — global "
          "button stylesheet was restructured; spec assumption needs review");
    CHECK(!foundEnabledGate,
          "`QPushButton:hover:enabled` rule found — the global stylesheet now "
          "gates hover on :enabled, which means a visible-but-disabled button "
          "would no longer mislead the user via hover highlight. The spec for "
          "review_changes_clickable can RELAX (visible-but-disabled becomes "
          "tolerable). Update spec.md and this test, do NOT just delete the "
          "assertion.");
}

}  // namespace

int main() {
    testRefreshHidesOnCleanRepo();
    testHoverStylesheetUngated();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", failures);
        return 1;
    }
    std::fprintf(stderr, "All Review Changes clickable invariants hold.\n");
    return 0;
}
