// ANTS-1301 — feature-conformance test for recent_errors. Live
// ScrollbackErrors parser behaviour + source-grep wiring. See spec.md.

#include "../../_support/expect.h"

#include "scrollbackerrors.h"

#include <QString>

#include <cstdio>
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
