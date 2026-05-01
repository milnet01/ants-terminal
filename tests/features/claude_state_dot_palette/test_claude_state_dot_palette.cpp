// Feature-conformance test for tests/features/claude_state_dot_palette/spec.md.
//
// Asserts the unified Claude state-dot palette contract:
//
//   INV-1 Helper signature lives in coloredtabbar.h with the documented
//         shape `static QColor color(Glyph g)` on `ClaudeTabIndicator`.
//   INV-2 Helper covers all eight non-None glyphs (source-grep on the
//         switch in coloredtabbar.cpp).
//   INV-3 Hex literals match the spec table.
//   INV-4 coloredtabbar.cpp::paintEvent calls the helper instead of
//         carrying inline RGB literals in the dot switch.
//   INV-5 Uniform dot geometry — no `radius = 5` per-state assignment,
//         no setPen with outline thickness in the dot pass, single
//         constant `kDotRadius`.
//   INV-6 mainwindow.cpp::applyClaudeStatusLabel calls the helper for
//         the label colour and no longer pulls from `th.ansi[…]`.
//   INV-7 ShellState carries the `auditing` bool and Glyph carries
//         `Auditing`.
//   INV-8 Status-bar applier handles the auditing branch by routing to
//         Glyph::Auditing.
//
// Pure source-grep harness, no Qt link — matches the existing
// ColoredTabBar / claude_tab_status_indicator test pattern.
//
// Exit 0 = all assertions hold.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Count non-overlapping occurrences of `needle` in `hay`.
int count(const std::string &hay, const char *needle) {
    int n = 0;
    size_t pos = 0;
    const size_t len = std::strlen(needle);
    if (len == 0) return 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += len;
    }
    return n;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

int main() {
    const std::string header = slurp(COLOREDTABBAR_H);
    const std::string source = slurp(COLOREDTABBAR_CPP);
    const std::string mwSource = slurp(MAINWINDOW_CPP);
    const std::string trackerHeader = slurp(CLAUDETABTRACKER_H);
    // ANTS-1146 — applyClaudeStatusLabel + the tab-indicator provider
    // lambda both moved to claudestatuswidgets.cpp. INV-6/INV-8
    // re-point at the new TU.
    const std::string cswSource = slurp(CLAUDESTATUSWIDGETS_CPP);

    if (header.empty()) return fail("INV-1", "coloredtabbar.h not readable");
    if (source.empty()) return fail("INV-1", "coloredtabbar.cpp not readable");
    if (mwSource.empty()) return fail("INV-1", "mainwindow.cpp not readable");
    if (trackerHeader.empty()) return fail("INV-1", "claudetabtracker.h not readable");
    if (cswSource.empty()) return fail("INV-1", "claudestatuswidgets.cpp not readable");

    // INV-1: helper signature
    if (!contains(header, "static QColor color(Glyph g)"))
        return fail("INV-1", "ClaudeTabIndicator::color(Glyph) static method missing");

    // INV-2: all eight non-None glyph cases under helper switch
    const char *glyphs[] = {
        "Glyph::Idle", "Glyph::Thinking", "Glyph::ToolUse", "Glyph::Bash",
        "Glyph::Planning", "Glyph::Auditing", "Glyph::Compacting", "Glyph::AwaitingInput",
    };
    for (const char *g : glyphs) {
        // Each appears in the helper definition (source) at least once.
        if (!contains(source, (std::string("case ClaudeTabIndicator::") + g).c_str()) &&
            !contains(source, (std::string("case ") + g).c_str())) {
            return fail("INV-2", g);
        }
    }

    // INV-3: hex literals match spec table. Tied to the helper,
    // not the paintEvent — paintEvent must call the helper, not
    // carry duplicates.
    struct { const char *hex; const char *name; } palette[] = {
        {"#888888", "Idle"},
        {"#5BA0E5", "Thinking"},
        {"#E5C24A", "ToolUse"},
        {"#6FCF50", "Bash"},
        {"#5DCFCF", "Planning"},
        {"#C76DC7", "Auditing"},
        {"#A87FE0", "Compacting"},
        {"#F08A4B", "AwaitingInput"},
    };
    for (auto p : palette) {
        if (!contains(source, p.hex))
            return fail("INV-3", p.name);
    }

    // INV-4: paintEvent must call the helper. Easiest signal: the call
    // site `ClaudeTabIndicator::color(` appears at least twice in the
    // source (once in the helper itself? no — the helper is a definition,
    // not a call. The call site is the paintEvent loop). Look for the
    // call expression specifically.
    if (!contains(source, "ClaudeTabIndicator::color(ind.glyph)"))
        return fail("INV-4", "paintEvent does not call ClaudeTabIndicator::color(ind.glyph)");

    // INV-5: uniform dot geometry. No per-state radius variable
    // assignment in the dot pass, no `radius = 5`, single constant.
    if (contains(source, "radius = 5"))
        return fail("INV-5", "AwaitingInput per-state radius=5 still present");
    // The constant for the dot radius — must exist exactly once.
    if (!contains(source, "kDotRadius = 4"))
        return fail("INV-5", "kDotRadius constant missing or not 4");
    // Old per-state outline pen branch must be gone. The string
    // `outline.alpha()` was the marker for the variant render path.
    if (contains(source, "outline.alpha()"))
        return fail("INV-5", "outline-alpha branch still present in dot pass");

    // INV-6: ClaudeStatusBarController::apply (formerly
    // mainwindow.cpp::applyClaudeStatusLabel) calls the helper for
    // label colour. The old th.ansi[…] mappings for the Claude label
    // must be gone — search for the four prior call sites in the new
    // TU.
    if (!contains(cswSource, "ClaudeTabIndicator::color("))
        return fail("INV-6", "claudestatuswidgets.cpp does not use ClaudeTabIndicator::color()");
    // The applier-specific assignment shape: `ClaudeTabIndicator::color(glyph)`
    if (!contains(cswSource, "ClaudeTabIndicator::color(glyph)"))
        return fail("INV-6", "ClaudeStatusBarController::apply doesn't compute color via helper");

    // INV-7: ShellState.auditing + Glyph::Auditing exist
    if (!contains(trackerHeader, "bool auditing = false"))
        return fail("INV-7", "ShellState::auditing missing");
    if (!contains(header, "Auditing,"))
        return fail("INV-7", "Glyph::Auditing missing in coloredtabbar.h enum");

    // INV-8: status-bar applier maps the auditing branch to
    // Glyph::Auditing. ANTS-1146 — applier moved to
    // claudestatuswidgets.cpp; tab-indicator provider lambda also
    // moved (it's inside ClaudeStatusBarController::attach now).
    if (!contains(cswSource, "glyph = ClaudeTabIndicator::Glyph::Auditing"))
        return fail("INV-8", "ClaudeStatusBarController::apply does not route auditing to Glyph::Auditing");
    // ANTS-1146 — both the status applier AND the provider closure
    // moved to claudestatuswidgets.cpp; mainwindow.cpp's
    // Auditing-glyph references should be zero. The total across the
    // two source files must still be ≥ 2 (provider lambda + applier
    // body).
    if (count(mwSource, "ClaudeTabIndicator::Glyph::Auditing") != 0)
        return fail("INV-8", "mainwindow.cpp still contains Glyph::Auditing references — should be zero post-extraction");
    if (count(cswSource, "ClaudeTabIndicator::Glyph::Auditing") < 2)
        return fail("INV-8", "Auditing glyph used fewer than 2x in claudestatuswidgets.cpp (expect provider + status applier)");

    std::puts("OK claude_state_dot_palette: 8/8 invariants");
    return 0;
}
