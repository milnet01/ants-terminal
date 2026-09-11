// ANTS-1301 — feature-conformance test for recent_errors. Live
// ScrollbackErrors parser behaviour + source-grep wiring. See spec.md.

#include "../../_support/expect.h"

#include "scrollbackerrors.h"
#include "terminalwidget.h"
#include "vtparser.h"

#include <QString>
#include <QStringList>

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

const ScrollbackErrors::ErrorEntry *firstOf(
        const ScrollbackErrors::Result &r, const char *category) {
    for (const auto &e : r.errors)
        if (e.category == QLatin1String(category)) return &e;
    return nullptr;
}

ScrollbackErrors::Result parseS(const QString &s) {
    return ScrollbackErrors::parse(s, {});
}

// ANTS-5061 — a real TerminalWidget/TerminalGrid pair, fed through the
// same PTY -> VtParser -> TerminalGrid path production output takes
// (see CLAUDE.md "Data flow"), narrowed to 40 columns so a compiler
// line wider than that soft-wraps across physical rows the way an
// 80-120-col split pane does. The widget's own grid (widget.grid()) is
// driven directly rather than via startShell(), so no PTY/child process
// is involved — mirrors tests/features/combining_on_resize's
// TerminalGrid+VtParser harness, pointed at the grid a TerminalWidget
// itself owns instead of a standalone one.
struct WidgetHarness {
    TerminalWidget widget;
    VtParser parser{[this](const VtAction &a) { widget.grid()->processAction(a); }};

    // ANTS-5061 — `rows` is sized to exactly the physical-row count a
    // scenario is about to feed (never left at the widget's 24-row
    // default). recentOutput/recentLogicalOutput count back `lines`
    // GLOBAL rows from the fixed bottom of the screen
    // (scrollbackSize()+rows()); a screen taller than the fed content
    // leaves blank rows below it, and a small `lines` window would land
    // on that blank tail instead of on the content under test. `cols`
    // is the pane width the wrap is being tested against.
    void resize(int rows, int cols = 40) {
        widget.grid()->resize(rows, cols);
    }

    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }

    // Feeds each line separated by CRLF — what a real shell sends. A
    // bare '\n' only moves the cursor down (VT100 LF —
    // TerminalGrid::handleExecute's '\n' case calls newLine() only, no
    // carriage return), so joining with '\n' alone would leave every
    // line but the first starting mid-row. No CRLF trails the LAST
    // line: a trailing "\r\n" would move the cursor one row past the
    // sized screen and scroll a blank row into view, defeating the
    // exact-row sizing above.
    void feedLines(std::initializer_list<std::string> lines) {
        bool first = true;
        for (const std::string &l : lines) {
            if (!first) feed("\r\n");
            first = false;
            feed(l);
        }
    }
};

// Each physical row is padded to the grid's column count (lineText()
// always emits `cols` characters), so split-on-'\n' segments carry
// trailing padding on any segment that ends a logical line normally.
// Trim before comparing.
QStringList logicalLines(const QString &s) {
    QStringList out;
    const QStringList raw = s.split(QLatin1Char('\n'));
    out.reserve(raw.size());
    for (const QString &line : raw) out << line.trimmed();
    return out;
}

// A gcc-style line wide enough to soft-wrap at 40 columns with the
// wrap point landing BEFORE "error:" — row 0 ends right after "12:5: "
// (no "error:" on it), row 1 starts with "error: ..." (no file:line:col
// prefix on it), so under today's row-per-line stub NEITHER physical
// row alone matches the compiler pattern.
const std::string kWrapBeforeErrorLine =
    "/home/user/workspace/src/main.cpp:12:5: error: 'foo' was not declared in this scope";

// Same shape, wrapping AFTER "error:" instead — row 1 alone
// ("file.cpp:12:5: error: 'foo' was not decl") already satisfies the
// compiler pattern under the stub, but with the message cut at the
// pane edge.
const std::string kWrapAfterErrorLine =
    "/very/long/absolute/path/to/some/source/file.cpp:12:5: error: 'foo' was not declared in this scope";

}  // namespace

TEST(McpRecentErrors, LiveParse) {
    expect_reset();

    // INV-1 — compiler (with column).
    {
        auto r = parseS(QStringLiteral(
            "src/foo.cpp:42:17: error: 'x' was not declared in this scope"));
        const auto *e = firstOf(r, "compiler");
        expect(e != nullptr, "INV-1: compiler entry produced");
        if (e) {
            expect(e->file == QStringLiteral("src/foo.cpp"), "INV-1: file", e->file);
            expect(e->line == 42, "INV-1: line 42");
            expect(e->column == 17, "INV-1: column 17");
            expect(e->message.contains(QStringLiteral("not declared")),
                   "INV-1: message captured", e->message);
        }
    }
    // INV-1b — compiler without column → column 0.
    {
        auto r = parseS(QStringLiteral("a.cpp:9: error: bad thing"));
        const auto *e = firstOf(r, "compiler");
        expect(e && e->line == 9 && e->column == 0,
               "INV-1b: no-column compiler line, column defaults 0");
    }

    // INV-2 — lint (ruff/flake8).
    {
        auto r = parseS(QStringLiteral("app.py:88:5: E501 line too long (90 > 79)"));
        const auto *e = firstOf(r, "lint");
        expect(e != nullptr, "INV-2: lint entry produced");
        if (e) {
            expect(e->line == 88 && e->column == 5, "INV-2: line/col");
            expect(e->message.contains(QStringLiteral("E501")),
                   "INV-2: message carries the rule code", e->message);
        }
        // INV-6 — a lint line is NOT also a compiler entry.
        expect(firstOf(r, "compiler") == nullptr,
               "INV-6: lint line not double-classified as compiler");
    }
    // INV-6b — a compiler line is exactly one compiler entry, not lint.
    {
        auto r = parseS(QStringLiteral("x.cpp:1:1: error: boom"));
        expect(r.errors.size() == 1 && r.errors[0].category == QStringLiteral("compiler"),
               "INV-6b: compiler line is one compiler entry");
    }

    // INV-3 — lua.
    {
        auto r = parseS(QStringLiteral("lua: scripts/mod.lua:5: attempt to call a nil value"));
        const auto *e = firstOf(r, "lua");
        expect(e && e->file == QStringLiteral("scripts/mod.lua") && e->line == 5,
               "INV-3: lua file/line");
    }

    // INV-4 — ctest block line + marker line.
    {
        auto r = parseS(QStringLiteral("\t  3 - widget_test (Failed)"));
        const auto *e = firstOf(r, "test");
        expect(e && e->message == QStringLiteral("widget_test"),
               "INV-4: ctest block line → test, message = name",
               e ? e->message : QStringLiteral("(none)"));
    }
    {
        auto r = parseS(QStringLiteral("1/2 Test #1: foo .....   ***Failed    0.1 sec"));
        const auto *e = firstOf(r, "test");
        expect(e != nullptr, "INV-4: ***Failed marker → test entry");
    }

    // INV-5 — python traceback collapses to one entry (deepest frame).
    {
        auto r = parseS(QStringLiteral(
            "Traceback (most recent call last):\n"
            "  File \"app/main.py\", line 10, in <module>\n"
            "    run()\n"
            "  File \"app/run.py\", line 88, in run\n"
            "    do()\n"
            "KeyError: 'name'"));
        expect(r.errors.size() == 1, "INV-5: one python entry from a traceback");
        const auto *e = firstOf(r, "python");
        expect(e != nullptr, "INV-5: category python");
        if (e) {
            expect(e->file == QStringLiteral("app/run.py") && e->line == 88,
                   "INV-5: deepest (last) frame is file/line", e->file);
            expect(e->message == QStringLiteral("KeyError: 'name'"),
                   "INV-5: message = exception line", e->message);
        }
    }
    // INV-5b — chained tracebacks → one entry each.
    {
        auto r = parseS(QStringLiteral(
            "Traceback (most recent call last):\n"
            "  File \"a.py\", line 1, in <module>\n"
            "    x()\n"
            "ValueError: first\n"
            "\n"
            "During handling of the above exception, another exception occurred:\n"
            "\n"
            "Traceback (most recent call last):\n"
            "  File \"b.py\", line 2, in <module>\n"
            "    y()\n"
            "KeyError: second"));
        int py = 0;
        for (const auto &e : r.errors) if (e.category == QStringLiteral("python")) ++py;
        expect(py == 2, "INV-5b: chained tracebacks → 2 python entries");
    }
    // INV-5c — frames-only block at EOF still emits.
    {
        auto r = parseS(QStringLiteral(
            "Traceback (most recent call last):\n"
            "  File \"z.py\", line 3, in <module>"));
        const auto *e = firstOf(r, "python");
        expect(e && e->file == QStringLiteral("z.py") && e->line == 3,
               "INV-5c: frames-only traceback at EOF emits last frame");
    }

    // INV-7 — max_results keeps the LAST N (newest), truncated, pre-cap count.
    {
        const QString many = QStringLiteral(
            "a.cpp:1: error: e1\n"
            "a.cpp:2: error: e2\n"
            "a.cpp:3: error: e3\n"
            "a.cpp:4: error: e4\n"
            "a.cpp:5: error: e5");
        ScrollbackErrors::Options o; o.maxResults = 2;
        auto r = ScrollbackErrors::parse(many, o);
        expect(r.errors.size() == 2, "INV-7: capped to maxResults");
        expect(r.errorsTotal == 5, "INV-7: errorsTotal is pre-cap (5)");
        expect(r.truncated, "INV-7: truncated set");
        expect(r.errors.size() == 2 && r.errors[0].line == 4 && r.errors[1].line == 5,
               "INV-7: the LAST (newest) entries are kept");
    }

    // INV-8 — CRLF handled; empty input is empty.
    {
        auto r = parseS(QStringLiteral("a.cpp:1: error: x\r\nb.cpp:2: error: y\r\n"));
        expect(r.errorsTotal == 2, "INV-8: CRLF lines parsed (trailing \\r stripped)");
        auto empty = parseS(QString());
        expect(empty.errorsTotal == 0 && empty.errors.isEmpty(),
               "INV-8: empty input → empty result");
        auto nomatch = parseS(QStringLiteral("just some normal output\nnothing wrong here"));
        expect(nomatch.errorsTotal == 0, "INV-8: no-match input → no errors");
    }

    EXPECT_EQ(0, expect_failures());
}

TEST(McpRecentErrors, WiringContract) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpRemoteControl();
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // INV-9 — declaration + definition.
    expect(contains(rcHdr, "cmdRecentErrors"), "INV-9: declared in remotecontrol.h");
    expect(contains(rcCpp, "RemoteControl::cmdRecentErrors"),
           "INV-9: defined in remotecontrol.cpp");

    // INV-10 — mainwindow registration.
    expect(contains(mwCpp, "registerToolProvider(\"recent_errors\""),
           "INV-10: recent_errors registered in mainwindow.cpp");

    // INV-11 — claudeintegration descriptor, token-cost, kind, contract.
    expect(contains(ciCpp, "t[\"name\"] = \"recent_errors\""),
           "INV-11: tool descriptor present");
    expect(contains(ciCpp, "\"recent_errors\"),     {800,  4000}"),
           "INV-11: token-cost entry");
    expect(contains(ciCpp, "name == QLatin1String(\"recent_errors\")"),
           "INV-11: kindForName terminal-bucket membership");
    expect(contains(ciCpp,
               "if (toolName == QStringLiteral(\"recent_errors\"))      return C::TabSpecific;"),
           "INV-11: callerCwdContractFor TabSpecific branch");

    EXPECT_EQ(0, expect_failures());
}

// ANTS-5061 — TerminalWidget::recentLogicalOutput joins soft-wrapped
// screen/scrollback rows before ScrollbackErrors::parse sees them, so
// an output line wider than the pane is reported whole instead of
// missed or cut at the wrap.
TEST(McpRecentErrors, SoftWrapJoin) {
    expect_reset();

    // INV-12 — a compiler line whose soft-wrap falls BEFORE "error:"
    // (so neither physical row alone carries the whole
    // "file:line:col: error:" shape) is still reported, with the full
    // file, line, column and message, once the wrapped rows are joined.
    {
        WidgetHarness h;
        h.resize(3);  // kWrapBeforeErrorLine wraps into exactly 3 rows at 40 cols
        h.feedLines({kWrapBeforeErrorLine});
        auto r = ScrollbackErrors::parse(h.widget.recentLogicalOutput(50), {});
        const auto *e = firstOf(r, "compiler");
        expect(e != nullptr,
               "INV-12: line soft-wrapped before \"error:\" is reported");
        if (e) {
            expect(e->file == QStringLiteral("/home/user/workspace/src/main.cpp"),
                   "INV-12: file", e->file);
            expect(e->line == 12, "INV-12: line 12");
            expect(e->column == 5, "INV-12: column 5");
            expect(e->message == QStringLiteral("'foo' was not declared in this scope"),
                   "INV-12: full message, not just the wrapped fragment", e->message);
        }
    }

    // INV-13 — the same shape wrapping AFTER "error:" instead: the
    // reported message is the whole message, not cut at the pane edge.
    {
        WidgetHarness h;
        h.resize(3);  // kWrapAfterErrorLine wraps into exactly 3 rows at 40 cols
        h.feedLines({kWrapAfterErrorLine});
        auto r = ScrollbackErrors::parse(h.widget.recentLogicalOutput(50), {});
        const auto *e = firstOf(r, "compiler");
        expect(e != nullptr,
               "INV-13: line soft-wrapped after \"error:\" is reported");
        if (e) {
            expect(e->message == QStringLiteral("'foo' was not declared in this scope"),
                   "INV-13: message not cut at the pane edge", e->message);
        }
    }

    // INV-14 — recentLogicalOutput folds the soft-wrapped run into one
    // line with no embedded '\n'; recentOutput is unchanged (still one
    // row per line), so get-text / the AI dialog see no shape change.
    {
        WidgetHarness h;
        h.resize(3);
        h.feedLines({kWrapAfterErrorLine});
        const QString full = h.widget.recentLogicalOutput(50);
        const QString flat = h.widget.recentOutput(50);
        const QString whole = QString::fromStdString(kWrapAfterErrorLine);
        expect(full.contains(whole),
               "INV-14: recentLogicalOutput carries the whole line as one, unbroken");
        expect(!flat.contains(whole),
               "INV-14: recentOutput is unchanged — still split across rows");
    }

    // INV-15 — two ordinary lines, neither soft-wrapped, stay two
    // lines: no false join.
    {
        WidgetHarness h;
        h.resize(2);
        h.feedLines({"alpha", "beta"});
        const QStringList out = logicalLines(h.widget.recentLogicalOutput(50));
        const int alphaIdx = out.indexOf(QStringLiteral("alpha"));
        const int betaIdx  = out.indexOf(QStringLiteral("beta"));
        expect(alphaIdx >= 0 && betaIdx == alphaIdx + 1,
               "INV-15: two unwrapped lines stay separate and in order");
    }

    // INV-16 — a window whose first row is the continuation of a
    // wrapped line has its start extended backward so the whole
    // logical line is intact, without pulling in the untouched line
    // before it.
    {
        WidgetHarness h;
        // 1 (before) + 3 (the wrapped line) + 1 (after) = 5 physical rows —
        // sized exactly so the window below counts back from the true
        // bottom of the screen (see WidgetHarness::resize).
        h.resize(5);
        h.feedLines({"before", kWrapBeforeErrorLine, "after"});
        const QString windowed = h.widget.recentLogicalOutput(2);
        expect(windowed.contains(QString::fromStdString(kWrapBeforeErrorLine)),
               "INV-16: a mid-wrap window start is extended backward to the whole line");
        expect(!windowed.contains(QStringLiteral("before")),
               "INV-16: the extension stops at the line boundary "
               "— the untouched line before it is excluded");
    }

    EXPECT_EQ(0, expect_failures());
}
