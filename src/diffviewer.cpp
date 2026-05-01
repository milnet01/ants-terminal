#include "diffviewer.h"

#include "clipboardguard.h"
#include "themes.h"

#include <QDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

namespace diffviewer {

QDialog *show(QWidget *parent,
              const QString &cwd,
              const QString &themeName) {
    // Build the dialog BEFORE running any git command. Plain
    // Qt::Dialog flags — no modality (ApplicationModal was causing
    // flaky first-open on KWin when combined with the frameless
    // parent + focus-redirect lambda; the async git probes were
    // fine, but Qt's modal state transition doesn't compose well
    // with our chrome-auto-refocus path). Modality is enforced at
    // the call site by disabling the "Review Changes" button until
    // the dialog closes — simpler, no Qt modal state machine, and
    // satisfies the user spec "preventing further clicks until
    // the dialog is closed" at the binding site (the button).
    auto *dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("reviewChangesDialog"));
    dialog->setWindowTitle("Review Changes");
    dialog->resize(800, 600);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(dialog);
    auto *viewer = new QTextEdit(dialog);
    viewer->setReadOnly(true);
    viewer->setFont(QFont("Monospace", 10));

    auto *btnBox = new QHBoxLayout;
    auto *liveStatus = new QLabel(dialog);
    liveStatus->setObjectName(QStringLiteral("reviewLiveStatus"));
    liveStatus->setStyleSheet("color: gray; font-size: 11px;");
    auto *refreshBtn = new QPushButton("Refresh", dialog);
    refreshBtn->setObjectName(QStringLiteral("reviewRefreshBtn"));
    auto *closeBtn = new QPushButton("Close", dialog);
    auto *copyBtn = new QPushButton("Copy Diff", dialog);
    btnBox->addWidget(liveStatus);
    btnBox->addStretch();
    btnBox->addWidget(refreshBtn);
    btnBox->addWidget(copyBtn);
    btnBox->addWidget(closeBtn);
    QObject::connect(closeBtn, &QPushButton::clicked, dialog, &QDialog::close);

    layout->addWidget(viewer);
    layout->addLayout(btnBox);

    // Show NOW. No git has run yet; the viewer carries a loading
    // placeholder so the dialog isn't empty on first paint. raise()
    // + activateWindow() bring it above the frameless parent; the
    // WindowStaysOnTopHint flag already set in the flags keeps it
    // there regardless of KWin stacking heuristics.
    const Theme &th = Themes::byName(themeName);
    viewer->setHtml(QStringLiteral(
        "<pre style='color: %1; background: %2;'>"
        "<span style='color: %3;'>Loading git status, diff, and "
        "unpushed commits for:</span>\n  %4\n\n"
        "<span style='color: %3;'>(Running `git status`, `git diff "
        "HEAD`, and `git log @{u}..HEAD` in the background…)</span>"
        "</pre>")
        .arg(th.textPrimary.name(), th.bgPrimary.name(),
             th.textSecondary.name(), cwd.toHtmlEscaped()));
    dialog->show();
    dialog->raise();
    dialog->activateWindow();

    // Shared state for the async probes. When all finish (or error
    // out), we rebuild the viewer HTML with the real data and
    // re-install the copy handler. std::shared_ptr because lambdas
    // outlive any single QProcess callback.
    //
    // 0.7.32 — branches + crossUnpushed added so the dialog surfaces
    // work that lives on branches OTHER than HEAD. Pre-fix, the only
    // commit-level signal was `@{u}..HEAD` which is HEAD-only — a
    // user with five feature branches each holding unpushed commits
    // would see "no unpushed" if they happened to be on a clean
    // branch. User feedback 2026-04-25.
    //
    // 0.7.32 (live updates) — every refresh constructs a fresh
    // ProbeState so an in-flight refresh whose probes outlive the
    // next refresh can't decrement the new pending counter and
    // render half-populated HTML. The current state is held by
    // QPointer-style ownership inside the runProbes lambda.
    struct ProbeState {
        QString cwd;
        QString status;
        QString diff;
        QString unpushed;
        QString branches;        // git for-each-ref refs/heads
        QString crossUnpushed;   // git log --branches --not --remotes
        int pending = 5;
    };

    QPointer<QDialog> dlgGuard(dialog);
    QPointer<QTextEdit> viewerGuard(viewer);
    QPointer<QPushButton> copyGuard(copyBtn);
    QPointer<QLabel> liveStatusGuard(liveStatus);

    // 0.7.37 — last rendered HTML, shared across runProbes invocations.
    // Used by finalize() to (a) skip setHtml when the new render is
    // byte-identical to the last one (the common case during idle
    // live-update tics — preserves selection, cursor, AND scroll
    // byte-perfectly), and (b) detect "first render" on dialog open
    // (empty string sentinel — first render restores no scroll
    // position). User report 2026-04-25: "the constant resetting of
    // the text means that if I scroll, it resets to the beginning
    // every refresh."
    auto lastHtml = std::make_shared<QString>();

    // 0.7.32 (live updates) — runProbes spawns the five async git
    // probes and fires-and-forgets. Called once on dialog open, then
    // again every time the QFileSystemWatcher debounce timer fires
    // OR the user clicks Refresh. Each call constructs a fresh
    // ProbeState so concurrent in-flight probes from a previous
    // refresh can't poison the new render.
    auto runProbes = [parent, cwd, dlgGuard, viewerGuard, copyGuard,
                      liveStatusGuard, themeName, lastHtml]() {
        if (!dlgGuard) return;
        if (liveStatusGuard) {
            liveStatusGuard->setText(QStringLiteral("● refreshing…"));
            liveStatusGuard->setStyleSheet(
                "color: #e0a020; font-size: 11px;");
        }

        auto state = std::make_shared<ProbeState>();
        state->cwd = cwd;

    // Finalizer: called once per probe. When pending hits 0, render
    // the full HTML.
    auto finalize = [state, dlgGuard, viewerGuard, copyGuard,
                     liveStatusGuard, themeName, lastHtml]() {
        if (--state->pending > 0) return;
        if (!dlgGuard || !viewerGuard) return;
        if (liveStatusGuard) {
            liveStatusGuard->setText(QStringLiteral(
                "● live — auto-refresh on git changes"));
            liveStatusGuard->setStyleSheet(
                "color: #4aa84a; font-size: 11px;");
        }

        // Lambda-local alias `lth` (lambda theme) — avoids shadowing the
        // outer `th` at the enclosing function scope.
        const Theme &lth = Themes::byName(themeName);
        QString html = QStringLiteral("<pre style='color: %1; background: %2;'>")
                           .arg(lth.textPrimary.name(), lth.bgPrimary.name());
        auto section = [&html, &lth](const QString &title) {
            html += QStringLiteral("<span style='color: %1; font-weight:600;'>"
                                  "━━ %2 ━━</span>\n")
                        .arg(lth.ansi[6].name(), title.toHtmlEscaped());
        };
        if (!state->status.isEmpty()) {
            section(QStringLiteral("Status"));
            for (const QString &line : state->status.split('\n')) {
                QString esc = line.toHtmlEscaped();
                if (line.startsWith("##"))
                    html += QStringLiteral("<span style='color: %1;'>")
                                .arg(lth.ansi[4].name()) + esc + "</span>\n";
                else
                    html += esc + "\n";
            }
            html += "\n";
        }
        if (!state->unpushed.isEmpty()) {
            section(QStringLiteral("Unpushed commits (current branch)"));
            for (const QString &line : state->unpushed.split('\n'))
                html += QStringLiteral("<span style='color: %1;'>")
                            .arg(lth.ansi[3].name())
                     + line.toHtmlEscaped() + "</span>\n";
            html += "\n";
        }
        if (!state->branches.isEmpty()) {
            section(QStringLiteral("Branches"));
            // Each line: "<branch>\t<upstream>\t<track>\t<subject>".
            // <track> is "[ahead 2, behind 1]" / "[gone]" / empty.
            // Render as a simple monospaced summary so the user can
            // scan ahead/behind across every local branch.
            for (const QString &line : state->branches.split('\n')) {
                if (line.trimmed().isEmpty()) continue;
                const QStringList parts = line.split('\t');
                const QString branch   = parts.value(0);
                const QString upstream = parts.value(1);
                const QString track    = parts.value(2);
                const QString subject  = parts.value(3);
                QString trackHtml;
                if (track.contains("ahead") || track.contains("behind")) {
                    trackHtml = QStringLiteral(" <span style='color: %1;'>%2</span>")
                                    .arg(lth.ansi[3].name(), track.toHtmlEscaped());
                } else if (track.contains("gone")) {
                    trackHtml = QStringLiteral(" <span style='color: %1;'>%2</span>")
                                    .arg(lth.ansi[1].name(), track.toHtmlEscaped());
                } else if (upstream.isEmpty()) {
                    trackHtml = QStringLiteral(" <span style='color: %1;'>"
                                              "[no upstream]</span>")
                                    .arg(lth.ansi[1].name());
                }
                html += QStringLiteral("<span style='color: %1;'>%2</span>%3 "
                                      "<span style='color: %4;'>%5</span>\n")
                            .arg(lth.ansi[4].name(), branch.toHtmlEscaped(),
                                 trackHtml,
                                 lth.textSecondary.name(), subject.toHtmlEscaped());
            }
            html += "\n";
        }
        if (!state->crossUnpushed.isEmpty()) {
            section(QStringLiteral("Unpushed across all branches"));
            for (const QString &line : state->crossUnpushed.split('\n'))
                html += QStringLiteral("<span style='color: %1;'>")
                            .arg(lth.ansi[3].name())
                     + line.toHtmlEscaped() + "</span>\n";
            html += "\n";
        }
        if (!state->diff.isEmpty()) {
            section(QStringLiteral("Diff"));
            for (const QString &line : state->diff.split('\n')) {
                QString esc = line.toHtmlEscaped();
                if (line.startsWith('+') && !line.startsWith("+++"))
                    html += QStringLiteral("<span style='color: %1;'>").arg(lth.ansi[2].name()) + esc + "</span>\n";
                else if (line.startsWith('-') && !line.startsWith("---"))
                    html += QStringLiteral("<span style='color: %1;'>").arg(lth.ansi[1].name()) + esc + "</span>\n";
                else if (line.startsWith("@@"))
                    html += QStringLiteral("<span style='color: %1;'>").arg(lth.ansi[4].name()) + esc + "</span>\n";
                else if (line.startsWith("diff ") || line.startsWith("index "))
                    html += QStringLiteral("<span style='color: %1;'>").arg(lth.ansi[3].name()) + esc + "</span>\n";
                else
                    html += esc + "\n";
            }
        }
        if (state->status.isEmpty() && state->diff.isEmpty() &&
            state->unpushed.isEmpty() && state->branches.isEmpty() &&
            state->crossUnpushed.isEmpty()) {
            html += QStringLiteral(
                "<span style='color: %1;'>No status, diff, or unpushed commits "
                "to report.</span>\n"
                "<span style='color: %2;'>If you expected changes here, check "
                "that `git status` works in this directory:\n  %3</span>\n")
                .arg(lth.ansi[3].name(), lth.textSecondary.name(),
                     state->cwd.toHtmlEscaped());
        }
        html += "</pre>";

        // 0.7.37 — preserve scroll position across live refreshes. The
        // 0.7.32 live-update path called setHtml unconditionally on every
        // probe completion (every git change, every 300ms debounce
        // window), which resets the QTextEdit document and snaps the
        // scroll bar back to the top. On a long diff with active live
        // updates the dialog became unscrollable — every flick of the
        // wheel races with the next refresh. User report 2026-04-25:
        // "the constant resetting of the text means that if I scroll, it
        // resets to the beginning every refresh."
        //
        // Two-layer fix:
        //   (1) Skip setHtml entirely when the rendered HTML is byte-
        //       identical to the last render (the common case — branch
        //       metadata refreshes that don't change anything visible).
        //       Preserves selection, cursor, AND scroll byte-perfectly.
        //   (2) When content does change, capture vertical/horizontal
        //       scroll positions before setHtml and restore them
        //       (clamped to the new max so a shorter render after a
        //       commit doesn't over-scroll). Selection is lost in this
        //       branch — Qt re-parses HTML into a fresh QTextDocument —
        //       but the dialog still feels stable.
        if (*lastHtml == html) {
            return;
        }
        const bool isFirstRender = lastHtml->isEmpty();
        *lastHtml = html;

        QScrollBar *vbar = viewerGuard->verticalScrollBar();
        QScrollBar *hbar = viewerGuard->horizontalScrollBar();
        const int vPos = (vbar && !isFirstRender) ? vbar->value() : 0;
        const int hPos = (hbar && !isFirstRender) ? hbar->value() : 0;

        viewerGuard->setHtml(html);

        if (vbar && !isFirstRender) vbar->setValue(std::min(vPos, vbar->maximum()));
        if (hbar && !isFirstRender) hbar->setValue(std::min(hPos, hbar->maximum()));

        if (copyGuard) {
            // Wire the Copy button now that the data is known. Disconnect
            // first so we don't stack handlers if finalize were ever
            // invoked twice (shouldn't happen, but cheap insurance).
            copyGuard->disconnect();
            QObject::connect(copyGuard, &QPushButton::clicked, copyGuard, [state]() {
                QString combined;
                if (!state->status.isEmpty())
                    combined += "# Status\n" + state->status + "\n\n";
                if (!state->unpushed.isEmpty())
                    combined += "# Unpushed (current branch)\n" + state->unpushed + "\n\n";
                if (!state->branches.isEmpty())
                    combined += "# Branches\n" + state->branches + "\n\n";
                if (!state->crossUnpushed.isEmpty())
                    combined += "# Unpushed across all branches\n"
                              + state->crossUnpushed + "\n\n";
                if (!state->diff.isEmpty())
                    combined += "# Diff\n" + state->diff;
                clipboardguard::writeText(combined,
                    clipboardguard::Source::Trusted);
            });
        }
    };

    // Spawn one async QProcess per probe. Each one writes into its
    // slot on the shared ProbeState when it finishes, then calls
    // finalize(). No blocking on the UI thread.
    auto runAsync = [parent, cwd, finalize](const QStringList &args,
                                             QString ProbeState::*slot,
                                             ProbeState *st) {
        auto *p = new QProcess(parent);
        p->setWorkingDirectory(cwd);
        p->setProgram("git");
        p->setArguments(args);
        QPointer<QProcess> pg = p;
        auto st_ptr = st;  // raw — lifetime is held by the shared_ptr
                           // captured through `finalize`
        QObject::connect(p,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), parent,
            [st_ptr, slot, pg, finalize](int /*code*/, QProcess::ExitStatus /*es*/) {
                if (pg) st_ptr->*slot = QString::fromUtf8(pg->readAllStandardOutput()).trimmed();
                if (pg) pg->deleteLater();
                finalize();
            });
        QObject::connect(p, &QProcess::errorOccurred, parent,
            [pg, finalize](QProcess::ProcessError) {
                if (pg) pg->deleteLater();
                finalize();
            });
        p->start();
    };

    runAsync({"status", "-b", "--short"},                       &ProbeState::status,   state.get());
    runAsync({"diff", "--stat", "--patch", "HEAD"},              &ProbeState::diff,     state.get());
    runAsync({"log", "--oneline", "--decorate", "@{u}..HEAD"},   &ProbeState::unpushed, state.get());

    // 0.7.32 — branch-aware sections. for-each-ref reports every
    // local branch with its upstream + ahead/behind. The
    // --branches --not --remotes log lists every commit reachable
    // from any local branch but NOT reachable from any remote-
    // tracking branch — i.e. unpushed work across every branch,
    // not just HEAD's lineage. Both probes are O(refs) and finish
    // in milliseconds even on large repos.
    runAsync({"for-each-ref", "refs/heads",
              "--format=%(refname:short)\t%(upstream:short)\t"
              "%(upstream:track)\t%(subject)"},
             &ProbeState::branches, state.get());
    runAsync({"log", "--branches", "--not", "--remotes",
              "--oneline", "--decorate"},
             &ProbeState::crossUnpushed, state.get());
    };  // close runProbes lambda

    // Initial probe spawn — populates the dialog right after show().
    runProbes();

    // 0.7.32 (live updates) — QFileSystemWatcher on the relevant
    // .git/* paths plus the working directory. Git operations
    // (commit, checkout, fetch, pull, branch -d, add, etc.) all
    // touch one or more of these paths; a debounce timer collapses
    // the burst into a single re-probe. Refresh button forces an
    // immediate re-probe regardless of the watcher.
    auto *watcher = new QFileSystemWatcher(dialog);
    auto *debounce = new QTimer(dialog);
    debounce->setSingleShot(true);
    debounce->setInterval(300);  // 300 ms — fast enough to feel live,
                                 // slow enough to coalesce a `git pull`
                                 // burst (which fires fileChanged
                                 // O(refs) times in milliseconds).
    QObject::connect(debounce, &QTimer::timeout, dialog, [runProbes]() {
        runProbes();
    });

    auto addPathSafe = [watcher](const QString &path) {
        if (QFileInfo::exists(path)) watcher->addPath(path);
    };
    addPathSafe(cwd);
    const QString gitDir = cwd + QStringLiteral("/.git");
    addPathSafe(gitDir);
    addPathSafe(gitDir + QStringLiteral("/HEAD"));
    addPathSafe(gitDir + QStringLiteral("/index"));
    addPathSafe(gitDir + QStringLiteral("/refs/heads"));
    addPathSafe(gitDir + QStringLiteral("/refs/remotes"));
    addPathSafe(gitDir + QStringLiteral("/logs/HEAD"));

    // QFileSystemWatcher loses its watch on a file when the file is
    // atomically replaced via rename(2) — git uses this pattern for
    // HEAD/index/logs/HEAD updates. Re-add the path on each
    // fileChanged so subsequent updates also fire.
    auto onFsEvent = [watcher, debounce, addPathSafe](const QString &path) {
        if (QFileInfo::exists(path) && !watcher->files().contains(path) &&
            !watcher->directories().contains(path)) {
            addPathSafe(path);
        }
        debounce->start();
    };
    QObject::connect(watcher, &QFileSystemWatcher::fileChanged, dialog, onFsEvent);
    QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, dialog, onFsEvent);

    // Manual Refresh — bypasses debounce so the user gets an
    // immediate response when they click. Useful when an external
    // tool (a build script, a different terminal) has changed state
    // outside the watched paths.
    QObject::connect(refreshBtn, &QPushButton::clicked, dialog, [runProbes]() {
        runProbes();
    });

    return dialog;
}

}  // namespace diffviewer
