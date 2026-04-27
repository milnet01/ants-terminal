// Claude Code background-tasks button invariants. Source-grep harness.
// See spec.md for the contract (INV-1 through INV-10).

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_BGTASKS_CPP_PATH
#error "SRC_BGTASKS_CPP_PATH compile definition required"
#endif
#ifndef SRC_BGTASKS_H_PATH
#error "SRC_BGTASKS_H_PATH compile definition required"
#endif
#ifndef SRC_BGDIALOG_CPP_PATH
#error "SRC_BGDIALOG_CPP_PATH compile definition required"
#endif
#ifndef SRC_BGDIALOG_H_PATH
#error "SRC_BGDIALOG_H_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_CMAKELISTS_PATH
#error "SRC_CMAKELISTS_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_H_PATH
#error "SRC_CLAUDE_INTEGRATION_H_PATH compile definition required"
#endif

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Extract a function body from a translation unit by signature prefix.
// Returns substring from the first matching signature to the next
// `\nvoid ` (heuristic — good enough for our top-level free / member
// functions in the project; the body never contains a top-level void).
static std::string functionBody(const std::string &src, const std::string &sig) {
    const auto start = src.find(sig);
    if (start == std::string::npos) return {};
    const auto next = src.find("\nvoid ", start + sig.size());
    return src.substr(start, next == std::string::npos ? std::string::npos
                                                       : next - start);
}

int main() {
    const std::string bgcpp = slurp(SRC_BGTASKS_CPP_PATH);
    const std::string bgh   = slurp(SRC_BGTASKS_H_PATH);
    const std::string dlgcpp = slurp(SRC_BGDIALOG_CPP_PATH);
    const std::string dlgh   = slurp(SRC_BGDIALOG_H_PATH);
    const std::string mw     = slurp(SRC_MAINWINDOW_CPP_PATH);
    const std::string cml    = slurp(SRC_CMAKELISTS_PATH);
    const std::string cicpp  = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string cih    = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: parser references run_in_background.
    if (bgcpp.find("run_in_background") == std::string::npos) {
        fail("INV-1: claudebgtasks.cpp must reference the "
             "`run_in_background` JSON key — that's the signal that "
             "marks a tool_use as a background task. Without it the "
             "parser never sees any starts and the tracker is empty");
    }

    // INV-2: parser references backgroundTaskId.
    if (bgcpp.find("backgroundTaskId") == std::string::npos) {
        fail("INV-2: claudebgtasks.cpp must reference the "
             "`backgroundTaskId` field — without it the parser cannot "
             "map a launch to its output path or completion event");
    }

    // INV-3: tracker declares + emits tasksChanged.
    if (bgh.find("void tasksChanged()") == std::string::npos) {
        fail("INV-3: ClaudeBgTaskTracker must declare a "
             "`void tasksChanged()` signal — the button and dialog "
             "depend on it to live-update");
    }
    if (bgcpp.find("emit tasksChanged()") == std::string::npos) {
        fail("INV-3: ClaudeBgTaskTracker::rescan must "
             "`emit tasksChanged()` when the task-list shape changes");
    }

    // INV-4: tracker uses QFileSystemWatcher.
    if (bgh.find("QFileSystemWatcher") == std::string::npos) {
        fail("INV-4: ClaudeBgTaskTracker header must include a "
             "QFileSystemWatcher member — pure polling drops live tail "
             "events for in-flight transcript writes");
    }
    if (bgcpp.find("addPath") == std::string::npos) {
        fail("INV-4: ClaudeBgTaskTracker must call `addPath` on its "
             "watcher when the transcript path is set");
    }

    // INV-5: status-bar button hidden when no tasks running.
    const std::string body = functionBody(mw,
        "void MainWindow::refreshBgTasksButton()");
    if (body.empty()) {
        fail("INV-5: cannot locate MainWindow::refreshBgTasksButton");
    } else {
        std::regex hideGuard(
            R"(if\s*\(\s*running\s*<=\s*0\s*\)[^}]*?->hide\s*\(\s*\))");
        if (!std::regex_search(body, hideGuard)) {
            fail("INV-5: refreshBgTasksButton must hide the button "
                 "when runningCount() <= 0 — otherwise the button stays "
                 "visible after every task finishes");
        }
    }

    // INV-6: button connected to showBgTasksDialog, which shows the
    // ClaudeBgTasksDialog.
    if (mw.find("showBgTasksDialog") == std::string::npos) {
        fail("INV-6: mainwindow.cpp must define a `showBgTasksDialog` "
             "slot wired to the bg-tasks button click");
    }
    const std::string showBody = functionBody(mw,
        "void MainWindow::showBgTasksDialog()");
    if (showBody.empty()) {
        fail("INV-6: cannot locate MainWindow::showBgTasksDialog");
    } else {
        if (showBody.find("new ClaudeBgTasksDialog") == std::string::npos) {
            fail("INV-6: showBgTasksDialog must construct a "
                 "`new ClaudeBgTasksDialog(...)` — without this the "
                 "button click is a no-op");
        }
        if (showBody.find("->show()") == std::string::npos) {
            fail("INV-6: showBgTasksDialog must call `dlg->show()`");
        }
    }
    // The signal-connect path:
    //   connect(m_claudeBgTasksBtn, &QPushButton::clicked,
    //           this, &MainWindow::showBgTasksDialog);
    std::regex btnConnect(
        R"(connect\s*\(\s*m_claudeBgTasksBtn[^;]*showBgTasksDialog\s*\))");
    if (!std::regex_search(mw, btnConnect)) {
        fail("INV-6: m_claudeBgTasksBtn must be `connect`-ed to "
             "MainWindow::showBgTasksDialog");
    }

    // INV-7: dialog rebuild reuses scroll-preservation pattern.
    const std::string rebuildBody = functionBody(dlgcpp,
        "void ClaudeBgTasksDialog::rebuild()");
    if (rebuildBody.empty()) {
        fail("INV-7: cannot locate ClaudeBgTasksDialog::rebuild");
    } else {
        std::regex skipGuard(
            R"(if\s*\(\s*\*m_lastHtml\s*==\s*html\s*\))");
        if (!std::regex_search(rebuildBody, skipGuard)) {
            fail("INV-7: rebuild must guard with "
                 "`if (*m_lastHtml == html)` early-return — without it "
                 "every refresh re-runs setHtml and the scroll bar "
                 "snaps back to the top");
        }
        // Capture before setHtml.
        const auto capPos = rebuildBody.find("vbar->value()");
        const auto setPos = rebuildBody.find("m_viewer->setHtml(");
        if (capPos == std::string::npos) {
            fail("INV-7: rebuild must capture vertical scroll-bar "
                 "value via `vbar->value()` BEFORE setHtml");
        } else if (setPos == std::string::npos) {
            fail("INV-7: cannot locate `m_viewer->setHtml(`");
        } else if (capPos >= setPos) {
            fail("INV-7: scroll-bar `value()` capture must come "
                 "BEFORE `m_viewer->setHtml(` — capturing after reads "
                 "the post-reset value (always 0)");
        }
        // Restore with qMin clamp.
        std::regex restoreClamp(
            R"(vbar->setValue\s*\(\s*qMin\s*\(\s*vPos\s*,\s*vbar->maximum\s*\(\s*\)\s*\)\s*\))");
        if (!std::regex_search(rebuildBody, restoreClamp)) {
            fail("INV-7: rebuild must restore vbar via "
                 "`vbar->setValue(qMin(vPos, vbar->maximum()))` — "
                 "without the clamp a longer-then-shorter content "
                 "sequence over-scrolls past the new document end");
        }
    }

    // INV-8: dialog watches output files.
    const std::string rewatchBody = functionBody(dlgcpp,
        "void ClaudeBgTasksDialog::rewatch()");
    if (rewatchBody.empty()) {
        fail("INV-8: cannot locate ClaudeBgTasksDialog::rewatch");
    } else {
        if (rewatchBody.find("outputPath") == std::string::npos) {
            fail("INV-8: rewatch must enumerate `outputPath` of each "
                 "tracked task — without this the live-tail pane shows "
                 "a stale snapshot");
        }
        if (rewatchBody.find("addPaths") == std::string::npos
                && rewatchBody.find("addPath") == std::string::npos) {
            fail("INV-8: rewatch must add task output paths to the "
                 "dialog's QFileSystemWatcher");
        }
    }

    // INV-9: dialog debounces (interval ≤ 500 ms, single-shot).
    if (dlgh.find("QTimer m_debounce") == std::string::npos
            && dlgcpp.find("m_debounce") == std::string::npos) {
        fail("INV-9: dialog must hold a QTimer named `m_debounce`");
    }
    std::regex debounceInterval(
        R"(m_debounce\.setInterval\s*\(\s*([0-9]+)\s*\))");
    std::smatch m;
    if (!std::regex_search(dlgcpp, m, debounceInterval)) {
        fail("INV-9: dialog must call `m_debounce.setInterval(...)`");
    } else {
        const int ms = std::stoi(m[1].str());
        if (ms <= 0 || ms > 500) {
            fail("INV-9: m_debounce interval must be in (0, 500] ms — "
                 "longer makes live-tail feel laggy, smaller defeats "
                 "the coalescing purpose");
        }
    }
    if (dlgcpp.find("m_debounce.setSingleShot(true)") == std::string::npos) {
        fail("INV-9: m_debounce must be single-shot — repeating fires "
             "every interval whether or not new events arrived");
    }

    // INV-10: CMakeLists adds both source files.
    if (cml.find("src/claudebgtasks.cpp") == std::string::npos) {
        fail("INV-10: CMakeLists.txt must include "
             "src/claudebgtasks.cpp in the ants-terminal target");
    }
    if (cml.find("src/claudebgtasksdialog.cpp") == std::string::npos) {
        fail("INV-10: CMakeLists.txt must include "
             "src/claudebgtasksdialog.cpp in the ants-terminal target");
    }

    // INV-11 (added 0.7.44): bg-tasks lookup is scoped to the active
    // tab's project tree, not system-wide newest. The fix changed
    // `activeSessionPath()` to accept an optional `projectCwd`
    // argument and walk up the cwd, encoding each ancestor and
    // probing `~/.claude/projects/<encoded>/` for the deepest match.
    // Without this, sessions from another open project leak into the
    // bg-tasks button — the user-reported 2026-04-27 bug.
    //
    // Source-grep three pieces:
    //   (a) header signature accepts a projectCwd argument
    //   (b) implementation references `encodeProjectPath` AND `cdUp`
    //       — the walk-up logic, no shortcut to a single string match
    //   (c) refreshBgTasksButton in MainWindow reads cwd from
    //       `focusedTerminal()->shellCwd()` and passes it to
    //       `activeSessionPath(...)`.
    {
        std::regex projectScopedSig(
            R"(activeSessionPath\s*\(\s*const\s+QString\s*&\s*projectCwd)");
        if (!std::regex_search(cih, projectScopedSig)) {
            fail("INV-11(a): ClaudeIntegration::activeSessionPath must "
                 "accept `const QString &projectCwd` — without that "
                 "scope parameter the bg-tasks surface returns the "
                 "newest .jsonl across every project Claude has ever "
                 "touched");
        }
        if (cicpp.find("encodeProjectPath") == std::string::npos) {
            fail("INV-11(b): activeSessionPath must reference "
                 "`encodeProjectPath` to translate the active tab's "
                 "cwd into Claude Code's `<dashed-cwd>` directory name");
        }
        if (cicpp.find("cdUp") == std::string::npos) {
            fail("INV-11(b): activeSessionPath must call `cdUp` to "
                 "walk up the project tree — Claude may have been "
                 "launched from a parent of the shell's current cwd");
        }
        // (c) MainWindow::refreshBgTasksButton must source the cwd
        // from focusedTerminal()->shellCwd() and pass it through.
        std::regex refreshScoped(
            R"(focusedTerminal[^;]*shellCwd[\s\S]*?activeSessionPath\s*\([^)]*cwd[^)]*\))");
        const std::string refreshBody = functionBody(mw,
            "void MainWindow::refreshBgTasksButton()");
        if (refreshBody.empty() ||
                !std::regex_search(refreshBody, refreshScoped)) {
            fail("INV-11(c): refreshBgTasksButton must read the "
                 "active tab's cwd via `focusedTerminal()->shellCwd()` "
                 "AND pass it to `activeSessionPath(cwd)` — without "
                 "this wiring the global-newest fallback fires and "
                 "the dialog leaks other projects' bg-tasks");
        }
    }

    // INV-12 (added 0.7.49): liveness sweep on parseTranscript.
    // The transcript-only completion detection misses background tasks
    // that were spawned and never polled — those entries linger as
    // `finished == false` forever, producing a phantom running-count on
    // the status-bar chip (12-task report from a session with zero
    // genuinely-running tasks). The sweep cross-checks each unfinished
    // task against its on-disk output file: if the file is gone OR its
    // mtime is older than a 60 s staleness window, mark finished.
    //
    // Three pieces:
    //   (a) parseTranscript references `lastModified` (the mtime check)
    //       AND `QFileInfo::exists` / `fi.exists()` (the file-gone path)
    //   (b) ClaudeBgTaskTracker::rescan is exposed in `public slots:`
    //       so the status-tick path can drive periodic rescans (the
    //       liveness sweep needs to fire even when the transcript has
    //       gone silent — otherwise the chip stays stale forever).
    //   (c) MainWindow::refreshBgTasksButton calls `m_claudeBgTasks->
    //       rescan()` when the path is unchanged, AND the 2 s status
    //       timer drives refreshBgTasksButton.
    if (bgcpp.find("lastModified") == std::string::npos) {
        fail("INV-12(a): parseTranscript must call `lastModified()` on "
             "each task's outputPath — without the mtime check, "
             "tasks whose completion was never polled stay 'running' "
             "forever (2026-04-27 phantom-12-tasks bug)");
    }
    if (bgcpp.find("fi.exists()") == std::string::npos
            && bgcpp.find("QFileInfo::exists") == std::string::npos) {
        fail("INV-12(a): parseTranscript must check whether each "
             "outputPath still exists — /tmp/claude-$UID purges across "
             "reboots, and a missing file is the strongest "
             "task-finished signal");
    }
    {
        std::regex publicRescan(
            R"(public\s+slots\s*:\s*[\s\S]*?void\s+rescan\s*\(\s*\))");
        if (!std::regex_search(bgh, publicRescan)) {
            fail("INV-12(b): ClaudeBgTaskTracker::rescan must be in "
                 "`public slots:` — the periodic liveness sweep "
                 "(refreshBgTasksButton on the 2 s status tick) needs "
                 "to call rescan() directly when the transcript path "
                 "is unchanged. setTranscriptPath short-circuits on "
                 "same-path so it can't be the entry point");
        }
    }
    {
        const std::string body = functionBody(mw,
            "void MainWindow::refreshBgTasksButton()");
        if (body.empty()) {
            fail("INV-12(c): refreshBgTasksButton body not found");
        } else if (body.find("m_claudeBgTasks->rescan()") == std::string::npos) {
            fail("INV-12(c): refreshBgTasksButton must call "
                 "`m_claudeBgTasks->rescan()` directly when the "
                 "transcript path is unchanged. setTranscriptPath "
                 "early-returns on same-path so it cannot drive the "
                 "periodic liveness sweep on its own");
        }
        if (mw.find("&MainWindow::refreshBgTasksButton") == std::string::npos) {
            fail("INV-12(c): the 2 s status timer must connect to "
                 "MainWindow::refreshBgTasksButton — without periodic "
                 "rescans the liveness sweep never re-runs while the "
                 "transcript is silent and the chip stays stale");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: claude background-tasks button invariants present "
                "(12/12)\n");
    return 0;
}
