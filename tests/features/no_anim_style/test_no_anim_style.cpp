// NoAnimStyle — source-grep regression test pinning the 60 Hz
// QPropertyAnimation(geometry) fix. See spec.md.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_MAIN_PATH
#error "SRC_MAIN_PATH compile definition required"
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
    const std::string m = slurp(SRC_MAIN_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: a NoAnimStyle class derived from QProxyStyle exists.
    std::regex classDecl(R"(class\s+NoAnimStyle\s*:\s*public\s+QProxyStyle)");
    if (!std::regex_search(m, classDecl)) {
        fail("INV-1: class NoAnimStyle : public QProxyStyle not declared in main.cpp");
    }

    // INV-2: styleHint override returns 0 for SH_Widget_Animation_Duration.
    // Accept either a case label + return 0, or a ternary/if returning 0.
    std::regex hint1(
        R"(SH_Widget_Animation_Duration[\s\S]{0,200}?return\s*0)");
    if (!std::regex_search(m, hint1)) {
        fail("INV-2: SH_Widget_Animation_Duration -> 0 not found — without it, "
             "Fusion's QWidgetAnimator creates a 60 Hz QPropertyAnimation(geometry) "
             "cycle on the idle window (2026-04-20 dropdown-flicker root cause).");
    }

    // INV-3: SH_Widget_Animate -> 0.
    std::regex hint2(
        R"(SH_Widget_Animate[\s\S]{0,200}?return\s*0)");
    if (!std::regex_search(m, hint2)) {
        fail("INV-3: SH_Widget_Animate -> 0 not found — Fusion consults this "
             "hint alongside SH_Widget_Animation_Duration; missing either "
             "re-enables the animation cycle.");
    }

    // INV-4: QApplication::setStyle wraps Fusion in NoAnimStyle (not plain
    // setStyle("Fusion"), which would be a regression).
    std::regex wrappedSetStyle(
        R"(setStyle\s*\(\s*new\s+NoAnimStyle\s*\(\s*QStyleFactory::create\s*\(\s*"Fusion"\s*\)\s*\)\s*\))");
    if (!std::regex_search(m, wrappedSetStyle)) {
        fail("INV-4: app.setStyle(new NoAnimStyle(QStyleFactory::create(\"Fusion\"))) "
             "not found — a plain setStyle(\"Fusion\") bypasses the proxy and "
             "re-enables the animation cycle.");
    }

    // INV-5 (negative): a plain `app.setStyle("Fusion");` line is a regression
    // if it appears without the NoAnimStyle wrapper.
    std::regex plainSetStyle(
        R"(\bapp\.setStyle\s*\(\s*"Fusion"\s*\)\s*;)");
    if (std::regex_search(m, plainSetStyle)) {
        fail("INV-5 (neg): plain app.setStyle(\"Fusion\") detected — must wrap "
             "in NoAnimStyle to suppress Fusion's style-driven animations.");
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: NoAnimStyle invariants present\n");
    return 0;
}
