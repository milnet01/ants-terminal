// TerminalWidget must be a plain QWidget (not QOpenGLWidget) —
// source-grep regression test. See spec.md for rationale.
//
// This test was originally called "partial_update_mode" because the
// intended fix was setUpdateBehavior(PartialUpdate) on QOpenGLWidget.
// That fix didn't work — Qt6's QOpenGLWidget composes its FBO through
// the parent's backing store regardless of PartialUpdate, forcing
// ~54 Hz full-window repaint cascades that flickered the menubar
// behind open dropdown popups. The real fix is to not be a
// QOpenGLWidget at all.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_TERMINALWIDGET_H_PATH
#error "SRC_TERMINALWIDGET_H_PATH compile definition required"
#endif
#ifndef SRC_TERMINALWIDGET_CPP_PATH
#error "SRC_TERMINALWIDGET_CPP_PATH compile definition required"
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

int main() {
    const std::string hdr = slurp(SRC_TERMINALWIDGET_H_PATH);
    const std::string src = slurp(SRC_TERMINALWIDGET_CPP_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: header inherits QWidget, not QOpenGLWidget.
    std::regex baseQWidget(
        R"(class\s+TerminalWidget\s*:\s*public\s+QWidget)");
    if (!std::regex_search(hdr, baseQWidget)) {
        fail("INV-1: terminalwidget.h must declare "
             "`class TerminalWidget : public QWidget` — reverting to "
             "QOpenGLWidget re-opens the ~54 Hz full-window repaint "
             "cascade and dropdown-flicker regression");
    }

    // INV-2 (neg): QOpenGLWidget include is gone from the header.
    if (hdr.find("#include <QOpenGLWidget>") != std::string::npos) {
        fail("INV-2 (neg): terminalwidget.h still includes QOpenGLWidget");
    }

    // INV-3 (neg): .cpp no longer calls QOpenGLWidget:: base methods.
    if (src.find("QOpenGLWidget::") != std::string::npos &&
        // Allow comment mentions; require code references to be absent.
        // We look for :: followed by identifier-start, which is how
        // the base call appears in code. Comments use " " after ::.
        std::regex_search(src, std::regex(R"(QOpenGLWidget::\w)"))) {
        // The conservative check: any QOpenGLWidget::<identifier>
        // token in the .cpp is suspect. Comments happen to also
        // contain this pattern (e.g. "QOpenGLWidget::resizeEvent
        // recreates..."), so we only fail if we find a call-shape
        // match — "QOpenGLWidget::<identifier>(..." with no leading
        // comment marker on the same line.
        std::smatch m;
        std::string::size_type pos = 0;
        std::regex callShape(R"(QOpenGLWidget::\w+\s*\()");
        auto begin = std::sregex_iterator(src.begin(), src.end(), callShape);
        auto end   = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            size_t mp = it->position();
            // Find the start of the line containing mp.
            size_t lineStart = src.rfind('\n', mp);
            if (lineStart == std::string::npos) lineStart = 0;
            else ++lineStart;
            std::string line = src.substr(lineStart, mp - lineStart);
            // If the line contains a "//" before the match, treat as
            // a comment reference (allowed).
            if (line.find("//") == std::string::npos) {
                fail("INV-3 (neg): terminalwidget.cpp still calls a "
                     "QOpenGLWidget:: base method. Use QWidget:: instead.");
                break;
            }
        }
        pos = 0;  // silence unused-var warning
        (void)pos;
    }

    // INV-4 (neg): makeCurrent / doneCurrent are GL-context-only APIs.
    // After the refactor they must not appear in live code. (Comments
    // are fine.)
    std::regex makeCurrentCall(R"((^|[^/])makeCurrent\s*\()");
    // Simpler form: look for "makeCurrent();" as a statement. The
    // refactor's class-top comment still mentions "makeCurrent" but
    // only inside // comments.
    {
        std::regex callStmt(R"(\bmakeCurrent\s*\(\s*\)\s*;)");
        auto begin = std::sregex_iterator(src.begin(), src.end(), callStmt);
        auto end   = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            size_t mp = it->position();
            size_t lineStart = src.rfind('\n', mp);
            if (lineStart == std::string::npos) lineStart = 0;
            else ++lineStart;
            std::string line = src.substr(lineStart, mp - lineStart);
            if (line.find("//") == std::string::npos) {
                fail("INV-4 (neg): live makeCurrent() call in "
                     "terminalwidget.cpp — TerminalWidget has no GL "
                     "context now; this must be inside a // comment");
                break;
            }
        }
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: TerminalWidget is plain QWidget (0.7.4 refactor)\n");
    return 0;
}
