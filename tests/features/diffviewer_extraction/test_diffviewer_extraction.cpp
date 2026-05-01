// Feature-conformance test for ANTS-1145 (0.7.73) — Review
// Changes dialog extracted from MainWindow::showDiffViewer into
// src/diffviewer.{cpp,h}. Source-grep only.
//
// INVs map 1:1 to docs/specs/ANTS-1145.md §Acceptance:
//   INV-1   exact function signature in diffviewer.h
//   INV-2a  structural markers in diffviewer.cpp
//   INV-2b  user-visible strings preserved byte-identical
//   INV-2c  sub-object names preserved
//   INV-3a  MainWindow::showDiffViewer body ≤ 50 meaningful LoC
//   INV-3b  MainWindow::showDiffViewer body delegates to
//           diffviewer::show(
//   INV-4   migrated markers no longer present in mainwindow.cpp

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_DIFFVIEWER_H_PATH
#error "SRC_DIFFVIEWER_H_PATH compile definition required"
#endif
#ifndef SRC_DIFFVIEWER_CPP_PATH
#error "SRC_DIFFVIEWER_CPP_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

// Extract MainWindow::showDiffViewer body — from its signature to
// the next top-level `void MainWindow::` definition.
std::string showDiffViewerBody(const std::string &mw) {
    const std::string sig = "void MainWindow::showDiffViewer()";
    const auto start = mw.find(sig);
    if (start == std::string::npos) return {};
    const auto next = mw.find("\nvoid MainWindow::", start + sig.size());
    return mw.substr(start, next == std::string::npos
                                ? std::string::npos
                                : next - start);
}

// INV-3a algorithm (per spec): physical lines between the opening
// `{` and closing `}` of the function body, exclusive of (a)
// blank lines and (b) lines whose first non-whitespace character
// is `//`. Approximation of "meaningful LoC". Doesn't strip block
// comments or trailing-`//`-on-code (rare in this codebase).
int meaningfulLoC(const std::string &body) {
    int n = 0;
    std::stringstream ss(body);
    std::string line;
    while (std::getline(ss, line)) {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i == line.size()) continue;                          // blank
        if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
            continue;                                             // // comment
        ++n;
    }
    return n;
}

}  // namespace

int main() {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    if (mw.empty()) return fail("setup", "mainwindow.cpp not readable");

    const std::string dvHeader = slurp(SRC_DIFFVIEWER_H_PATH);
    const std::string dvImpl   = slurp(SRC_DIFFVIEWER_CPP_PATH);

    // INV-1: exact signature in diffviewer.h. Locks parameter
    // order/types so a "minor cleanup" PR can't silently reorder.
    if (dvHeader.empty())
        return fail("INV-1",
            "src/diffviewer.h not present — extraction not done");
    if (!contains(dvHeader, "namespace diffviewer"))
        return fail("INV-1",
            "src/diffviewer.h missing `namespace diffviewer`");
    static const char *kSignature =
        "QDialog *show(QWidget *parent,\n"
        "              const QString &cwd,\n"
        "              const QString &themeName);";
    if (!contains(dvHeader, kSignature))
        return fail("INV-1",
            "src/diffviewer.h does not declare the exact signature "
            "`QDialog *show(QWidget *parent, const QString &cwd, "
            "const QString &themeName);` — refactor must preserve "
            "parameter order and types");

    // INV-2a: structural markers in diffviewer.cpp.
    if (dvImpl.empty())
        return fail("INV-2a",
            "src/diffviewer.cpp not present — extraction not done");
    static const char *kStructMarkers[] = {
        "reviewChangesDialog",
        "struct ProbeState",
        "pending = 5",
        "for-each-ref",
        "QFileSystemWatcher",
        "runProbes",
        "lastHtml",
    };
    for (const char *m : kStructMarkers) {
        if (!contains(dvImpl, m)) {
            std::fprintf(stderr,
                "[INV-2a] FAIL: diffviewer.cpp missing structural "
                "marker `%s`\n", m);
            return 1;
        }
    }

    // INV-2b: user-visible strings preserved byte-identical.
    static const char *kUserStrings[] = {
        "\"Review Changes\"",
        "\"Refresh\"",
        "\"Close\"",
        "\"Copy Diff\"",
        "● refreshing…",
        "● live — auto-refresh on git changes",
        "Status",
        "Unpushed commits (current branch)",
        "Branches",
        "Unpushed across all branches",
        "Diff",
        "No status, diff, or unpushed commits",
    };
    for (const char *s : kUserStrings) {
        if (!contains(dvImpl, s)) {
            std::fprintf(stderr,
                "[INV-2b] FAIL: diffviewer.cpp missing user-visible "
                "string `%s` — drive-by reformat broke a UI label "
                "or section header\n", s);
            return 1;
        }
    }

    // INV-2c: sub-object names preserved.
    static const char *kObjectNames[] = {
        "reviewLiveStatus",
        "reviewRefreshBtn",
    };
    for (const char *n : kObjectNames) {
        if (!contains(dvImpl, n)) {
            std::fprintf(stderr,
                "[INV-2c] FAIL: diffviewer.cpp missing sub-object "
                "name `%s` — feature-coverage QA helpers won't "
                "find this widget\n", n);
            return 1;
        }
    }

    // INV-3a: MainWindow::showDiffViewer body ≤ 50 meaningful LoC.
    const std::string body = showDiffViewerBody(mw);
    if (body.empty())
        return fail("INV-3a",
            "could not locate MainWindow::showDiffViewer body");
    const int loc = meaningfulLoC(body);
    if (loc > 50) {
        std::fprintf(stderr,
            "[INV-3a] FAIL: MainWindow::showDiffViewer body is %d "
            "meaningful LoC; cap is 50 (per spec — guard against "
            "accidental re-inlining of the dialog body)\n", loc);
        return 1;
    }

    // INV-3b: post-extraction body delegates to diffviewer::show.
    if (!contains(body, "diffviewer::show("))
        return fail("INV-3b",
            "MainWindow::showDiffViewer body does not call "
            "diffviewer::show( — delegation missing");

    // INV-4: migrated markers no longer present in mainwindow.cpp.
    static const char *kMigratedMarkers[] = {
        "struct ProbeState",
        "for-each-ref",
        "runProbes",
        "reviewChangesDialog",
    };
    for (const char *m : kMigratedMarkers) {
        if (contains(mw, m)) {
            std::fprintf(stderr,
                "[INV-4] FAIL: mainwindow.cpp still contains "
                "migrated marker `%s` — extraction incomplete\n", m);
            return 1;
        }
    }

    std::fprintf(stderr,
        "OK — DiffViewerDialog extraction INVs hold "
        "(showDiffViewer body shrunk to %d meaningful LoC).\n", loc);
    return 0;
}
