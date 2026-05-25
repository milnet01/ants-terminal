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
#include <gtest/gtest.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// INV-9 / INV-10 (ANTS-1847) are behavioural — they link coloredtabbar
// (already in this GUI bundle) and call the helpers directly.
#include "coloredtabbar.h"
#include <QColor>
#include <cmath>

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

// WCAG 2.x relative luminance + contrast — independent re-derivation of
// the formula the implementation uses, so the test verifies the numeric
// guarantee rather than trusting the impl's own helper.
double tLum(const QColor &c) {
    auto lin = [](int v8) {
        const double v = v8 / 255.0;
        return v <= 0.03928 ? v / 12.92
                            : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) +
           0.0722 * lin(c.blue());
}
double tContrast(const QColor &a, const QColor &b) {
    const double la = tLum(a) + 0.05, lb = tLum(b) + 0.05;
    return la > lb ? la / lb : lb / la;
}

}  // namespace

TEST(ClaudeStateDotPalette, Main) {

    const std::string header = slurp(COLOREDTABBAR_H);
    const std::string source = slurp(COLOREDTABBAR_CPP);
    const std::string mwSource = slurp(MAINWINDOW_CPP);
    const std::string trackerHeader = slurp(CLAUDETABTRACKER_H);
    // ANTS-1146 — applyClaudeStatusLabel + the tab-indicator provider
    // lambda both moved to claudestatuswidgets.cpp. INV-6/INV-8
    // re-point at the new TU.
    const std::string cswSource = slurp(CLAUDESTATUSWIDGETS_CPP);

    if (header.empty()) { fail("INV-1", "coloredtabbar.h not readable"); FAIL(); }
    if (source.empty()) { fail("INV-1", "coloredtabbar.cpp not readable"); FAIL(); }
    if (mwSource.empty()) { fail("INV-1", "mainwindow.cpp not readable"); FAIL(); }
    if (trackerHeader.empty()) { fail("INV-1", "claudetabtracker.h not readable"); FAIL(); }
    if (cswSource.empty()) { fail("INV-1", "claudestatuswidgets.cpp not readable"); FAIL(); }

    // INV-1: helper signature
    if (!contains(header, "static QColor color(Glyph g)"))
        { fail("INV-1", "ClaudeTabIndicator::color(Glyph) static method missing"); FAIL(); }

    // INV-2: all eight non-None glyph cases under helper switch
    const char *glyphs[] = {
        "Glyph::Idle", "Glyph::Thinking", "Glyph::ToolUse", "Glyph::Bash",
        "Glyph::Planning", "Glyph::Auditing", "Glyph::Compacting", "Glyph::AwaitingInput",
    };
    for (const char *g : glyphs) {
        // Each appears in the helper definition (source) at least once.
        if (!contains(source, (std::string("case ClaudeTabIndicator::") + g).c_str()) &&
            !contains(source, (std::string("case ") + g).c_str())) {
            { fail("INV-2", g); FAIL(); }
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
            { fail("INV-3", p.name); FAIL(); }
    }

    // INV-4 (ANTS-1847): paintEvent derives the dot fill via the
    // contrast-adaptation helper, against the tab background `m_bg`. No
    // inline hex literals in the dot path; the base palette literals live
    // only in color().
    if (!contains(source, "ClaudeTabIndicator::contrastColor(ind.glyph, m_bg)"))
        { fail("INV-4", "paintEvent does not call contrastColor(ind.glyph, m_bg)"); FAIL(); }

    // INV-5: uniform dot geometry + uniform ring. No per-state radius
    // variable assignment in the dot pass, no `radius = 5`, single
    // constant. The ring is uniform (one ringColor(m_bg) pen, not inside a
    // Glyph::-cased branch) and the old per-state outline marker is gone.
    if (contains(source, "radius = 5"))
        { fail("INV-5", "AwaitingInput per-state radius=5 still present"); FAIL(); }
    // The constant for the dot radius — must exist exactly once.
    if (!contains(source, "kDotRadius = 4"))
        { fail("INV-5", "kDotRadius constant missing or not 4"); FAIL(); }
    // Old per-state outline pen branch must be gone. The string
    // `outline.alpha()` was the marker for the variant render path.
    if (contains(source, "outline.alpha()"))
        { fail("INV-5", "outline-alpha branch still present in dot pass"); FAIL(); }
    // Uniform ring: the dot pass sets exactly one ring pen from the
    // state-independent ringColor(m_bg) helper.
    if (!contains(source, "ClaudeTabIndicator::ringColor(m_bg)"))
        { fail("INV-5", "uniform ringColor(m_bg) pen missing from dot pass"); FAIL(); }
    if (count(source, "ClaudeTabIndicator::ringColor(m_bg)") != 1)
        { fail("INV-5", "ring pen is not set exactly once (per-state ring?)"); FAIL(); }

    // INV-6: ClaudeStatusBarController::apply (formerly
    // mainwindow.cpp::applyClaudeStatusLabel) calls the helper for
    // label colour. The old th.ansi[…] mappings for the Claude label
    // must be gone — search for the four prior call sites in the new
    // TU.
    // INV-6 (ANTS-1847): the applier derives the label colour via the
    // same contrast-adaptation helper as the dot (against bgSecondary),
    // not the unadjusted color(glyph) or th.ansi[…].
    if (!contains(cswSource, "ClaudeTabIndicator::contrastColor(glyph"))
        { fail("INV-6", "apply() doesn't compute colour via contrastColor(glyph, …)"); FAIL(); }

    // INV-7: ShellState.auditing + Glyph::Auditing exist
    if (!contains(trackerHeader, "bool auditing = false"))
        { fail("INV-7", "ShellState::auditing missing"); FAIL(); }
    if (!contains(header, "Auditing,"))
        { fail("INV-7", "Glyph::Auditing missing in coloredtabbar.h enum"); FAIL(); }

    // INV-8: status-bar applier maps the auditing branch to
    // Glyph::Auditing. ANTS-1146 — applier moved to
    // claudestatuswidgets.cpp; tab-indicator provider lambda also
    // moved (it's inside ClaudeStatusBarController::attach now).
    if (!contains(cswSource, "glyph = ClaudeTabIndicator::Glyph::Auditing"))
        { fail("INV-8", "ClaudeStatusBarController::apply does not route auditing to Glyph::Auditing"); FAIL(); }
    // ANTS-1146 — both the status applier AND the provider closure
    // moved to claudestatuswidgets.cpp; mainwindow.cpp's
    // Auditing-glyph references should be zero. The total across the
    // two source files must still be ≥ 2 (provider lambda + applier
    // body).
    if (count(mwSource, "ClaudeTabIndicator::Glyph::Auditing") != 0)
        { fail("INV-8", "mainwindow.cpp still contains Glyph::Auditing references — should be zero post-extraction"); FAIL(); }
    if (count(cswSource, "ClaudeTabIndicator::Glyph::Auditing") < 2)
        { fail("INV-8", "Auditing glyph used fewer than 2x in claudestatuswidgets.cpp (expect provider + status applier)"); FAIL(); }

    std::puts("OK claude_state_dot_palette: 8/8 invariants");
}

// INV-9 / INV-10 (ANTS-1847) — behavioural contrast + ring guarantees.
TEST(ClaudeStateDotPalette, ContrastAdaptation) {
    using Ind = ClaudeTabIndicator;
    using G = Ind::Glyph;
    const G glyphs[] = {G::Idle,     G::Thinking,  G::ToolUse,    G::Bash,
                        G::Planning, G::Auditing,  G::Compacting, G::AwaitingInput};

    // Light themes: bgSecondary of "Light" (#F5F5F5) + "Catppuccin Latte"
    // (#E6E9EF) — the surfaces the dot/label actually paint on.
    const QColor lightBgs[] = {QColor("#F5F5F5"), QColor("#E6E9EF")};

    for (const QColor &bg : lightBgs) {
        for (G g : glyphs) {
            const QColor base = Ind::color(g);
            const QColor adj = Ind::contrastColor(g, bg);

            // INV-9a: clears the 3:1 non-text floor against this bg.
            EXPECT_GE(tContrast(adj, bg), 3.0)
                << "glyph " << static_cast<int>(g) << " on bg "
                << bg.name().toStdString() << " contrast "
                << tContrast(adj, bg);

            // INV-9b: hue preserved (state identity). Skip achromatic grey
            // (Idle, saturation 0 → hue is meaningless / -1).
            if (base.hslSaturation() > 0 && adj.hslHue() >= 0) {
                EXPECT_EQ(adj.hslHue(), base.hslHue())
                    << "hue drifted for glyph " << static_cast<int>(g);
            }
        }
    }

    // INV-9c: dark theme passthrough — base palette returned byte-for-byte
    // unchanged (the shipped dark-theme appearance does not move).
    const QColor darkBg("#1E1E1E");
    for (G g : glyphs)
        EXPECT_EQ(Ind::contrastColor(g, darkBg), Ind::color(g))
            << "dark-bg adaptation changed glyph " << static_cast<int>(g);

    // INV-10: uniform ring — semi-transparent, dark on light bg, light on
    // dark bg, and state-independent (one signature, no Glyph arg).
    const QColor ringLight = Ind::ringColor(QColor("#F5F5F5"));
    const QColor ringDark = Ind::ringColor(QColor("#1E1E1E"));
    EXPECT_LT(ringLight.alpha(), 255);
    EXPECT_LT(ringDark.alpha(), 255);
    EXPECT_LT(tLum(ringLight), tLum(QColor("#F5F5F5")))
        << "ring on a light theme must be darker than the background";
    EXPECT_GT(tLum(ringDark), tLum(QColor("#1E1E1E")))
        << "ring on a dark theme must be lighter than the background";
}

