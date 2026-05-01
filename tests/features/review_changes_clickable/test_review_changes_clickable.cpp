// Feature-conformance test for spec.md — locks the 0.6.33 tri-state
// Review Changes policy:
//
//   Not a git repo                                → hidden
//   Git repo, clean + in-sync                      → visible, disabled
//   Git repo with dirty worktree or unpushed work  → visible, enabled
//
// And the three load-bearing source-level invariants that make the
// visible-but-disabled state user-friendly:
//
//   1. The global QPushButton:hover rule IS gated with :enabled so the
//      disabled button doesn't light up on hover (which in the 0.6.29
//      era made users think a disabled button was actionable).
//   2. refreshReviewButton's exit-0 branch uses setEnabled(...) + show(),
//      NOT hide() — the whole point of the tri-state policy.
//   3. The git probe is `git status --porcelain=v1 -b`, which reports
//      both dirty worktree AND ahead-of-upstream in one subprocess. The
//      old `git diff --quiet HEAD` probe missed unpushed commits.
//
// Source-grep style (same as test_review_changes_click.cpp and
// test_shift_enter.cpp) — avoids instantiating MainWindow for a
// purely policy-level assertion.

#include <QFile>
#include <QString>
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
        std::fprintf(stderr, "FAIL: cannot open %s\n", qUtf8Printable(path));
        ++failures;
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

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

// Invariant 1 — refreshReviewButton's successful-probe branch uses
// setEnabled + show (tri-state), NOT hide. The forbidden shape is the
// 0.6.29-era "hide on clean" which we've retired.
void testRefreshShowsDisabledOnClean() {
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

    // The tri-state policy hinges on calling setEnabled(...) and then
    // show() in the successful-probe branch. Check both are present.
    CHECK(body.contains(QStringLiteral("setEnabled(")),
          "refreshReviewButton no longer calls setEnabled() — tri-state "
          "policy (hidden / visible-disabled / visible-enabled) requires it");
    CHECK(body.contains(QStringLiteral("btn->show()")),
          "refreshReviewButton no longer calls btn->show() in its "
          "success branch — visible-disabled state cannot be reached");
}

// Invariant 2 — the global QPushButton:hover rule MUST be gated with
// :enabled. Without it, a visible-disabled button still lights up on
// hover, which is the exact misleading-UX bug that the 0.6.29 spec was
// written to prevent. The tri-state policy is only safe because this
// gate exists.
void testHoverStylesheetEnabledGated() {
    // ANTS-1147 — global QSS moved into
    // themedstylesheet::buildAppStylesheet. Read the new TU.
    const QString src = readSource(QStringLiteral(SRC_THEMEDSTYLESHEET_CPP_PATH));
    if (src.isEmpty()) return;

    // Accept either `QPushButton:hover:enabled` or
    // `QPushButton:enabled:hover` (Qt stylesheet selector order is
    // free). Reject a bare `QPushButton:hover {` not followed by
    // `:enabled` within the selector.
    static const QRegularExpression gated(
        QStringLiteral(R"(QPushButton:(?:hover:enabled|enabled:hover))"));
    static const QRegularExpression bareHover(
        QStringLiteral(R"(QPushButton:hover\s*\{)"));

    const bool hasGate   = gated.match(src).hasMatch();
    const bool hasBare   = bareHover.match(src).hasMatch();

    CHECK(hasGate,
          "global QPushButton:hover rule is not gated with :enabled in "
          "themedstylesheet.cpp — the visible-disabled Review Changes "
          "button will light up on hover, advertising itself as "
          "actionable when it isn't. Update the QSS rule in "
          "themedstylesheet::buildAppStylesheet to "
          "`QPushButton:hover:enabled { ... }`");
    CHECK(!hasBare,
          "an un-gated `QPushButton:hover {` rule is present. If it's "
          "intentional for a specific widget, anchor it with an object-"
          "name selector. The un-scoped global form breaks the "
          "visible-disabled contract from review_changes_clickable/spec.md");
}

// Invariant 3 — the probe covers BOTH dirty worktree AND
// ahead-of-upstream. `git status --porcelain=v1 -b` is the canonical
// combo; accept any `git status` invocation with `-b` as a viable
// spelling. Reject a bare `git diff --quiet HEAD` — that was the
// 0.6.29-era probe which missed unpushed commits.
void testProbeCoversAheadOfUpstream() {
    const QString src = readSource(QStringLiteral(SRC_MAINWINDOW_PATH));
    if (src.isEmpty()) return;

    const QString body = extractFunctionBody(
        src, QStringLiteral("void MainWindow::refreshReviewButton()"));
    if (body.isEmpty()) return;

    const bool usesStatusBranch = body.contains(QStringLiteral("\"status\""))
                                 && body.contains(QStringLiteral("\"-b\""));
    CHECK(usesStatusBranch,
          "refreshReviewButton's git probe does not use `git status -b` "
          "— restore the combined dirty-and-ahead probe or document why "
          "a different probe is equivalent. The 0.6.32-era "
          "`git diff --quiet HEAD` missed unpushed commits.");

    // Reject the specific retired probe. If a future refactor legitimately
    // needs two separate probes, it can rename the function or split the
    // logic out; the source-grep here is intentionally strict.
    const bool usesRetiredProbe = body.contains(
        QStringLiteral("\"--quiet\"")) && body.contains(
        QStringLiteral("\"HEAD\""))
        && body.contains(QStringLiteral("\"diff\""));
    CHECK(!usesRetiredProbe,
          "refreshReviewButton still references the retired "
          "`git diff --quiet HEAD` probe — see spec.md regression history.");
}

}  // namespace

int main() {
    testRefreshShowsDisabledOnClean();
    testHoverStylesheetEnabledGated();
    testProbeCoversAheadOfUpstream();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", failures);
        return 1;
    }
    std::fprintf(stderr, "All Review Changes tri-state invariants hold.\n");
    return 0;
}
