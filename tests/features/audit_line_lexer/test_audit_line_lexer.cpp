// Feature-conformance test for spec.md (ANTS-2210) — locks the per-language
// comment/string lexer that ANTS-1270 added, now extracted to the pure
// AuditHygiene::lineHasCode surface so it is regression-testable without
// linking the AuditDialog QDialog TU.
//
// Headless: pure (source string, path, line) -> bool, no QApplication.

#include "audithygiene.h"

#include <QString>

#include <gtest/gtest.h>

using AuditHygiene::lineHasCode;

// AL-1 — Python `#` comment is non-code; the secret inside is masked.
TEST(AuditLineLexer, PythonHashComment) {
    const QString py =
        "x = 1\n"                       // 1 — real assignment
        "# password = \"10.0.0.1\"\n";  // 2 — comment (IP/secret masked)
    EXPECT_TRUE(lineHasCode(py, "f.py", 1));
    EXPECT_FALSE(lineHasCode(py, "f.py", 2));
}

// AL-2 — shell `#` comment is non-code.
TEST(AuditLineLexer, ShellHashComment) {
    const QString sh =
        "echo deploying\n"   // 1
        "# secret token\n";  // 2 — comment
    EXPECT_FALSE(lineHasCode(sh, "deploy.sh", 2));
}

// AL-3 — Lua `--` line comment, `--[[ ]]` block, and `[[ ]]` long string.
TEST(AuditLineLexer, LuaCommentsAndLongString) {
    const QString lua =
        "local x = 1\n"        // 1 — code
        "-- note here\n"       // 2 — line comment
        "--[[ block\n"         // 3 — block-comment open
        "still comment ]]\n"   // 4 — inside --[[ ]]
        "y = [[ longstr\n"     // 5 — `y =` code, opens long string
        "inside string ]]\n";  // 6 — inside [[ ]]
    EXPECT_TRUE(lineHasCode(lua, "a.lua", 1));
    EXPECT_FALSE(lineHasCode(lua, "a.lua", 2));
    EXPECT_FALSE(lineHasCode(lua, "a.lua", 4));
    EXPECT_FALSE(lineHasCode(lua, "a.lua", 6));
}

// AL-4 — C-style `//` line comment and `/* */` block interior are non-code.
// ANTS-2230 — the `//` and `/*` introducers are now excluded from
// code-detection (the C-style analogue of ANTS-1270's `#`/`--` exclusion), so
// a finding inside a comment-only C/C++ line is dropped, not kept.
TEST(AuditLineLexer, CStyleComments) {
    const QString cpp =
        "int x;\n"               // 1 — code
        "// a note\n"            // 2 — line comment (ANTS-2230: non-code)
        "/* block\n"             // 3 — block open
        "still in block */\n";   // 4 — inside /* */
    EXPECT_TRUE(lineHasCode(cpp, "a.cpp", 1));
    EXPECT_FALSE(lineHasCode(cpp, "a.cpp", 2));  // ANTS-2230 — comment, non-code
    EXPECT_FALSE(lineHasCode(cpp, "a.cpp", 4));  // block interior: non-code
}

// AL-4b (ANTS-2230) — a bare division `/` still reads as code; only the
// comment-opening `//` and `/*` are excluded.
TEST(AuditLineLexer, CStyleDivisionStillCode) {
    EXPECT_TRUE(lineHasCode("a = b / c;\n", "a.cpp", 1));
}

// AL-5 — a C++ raw string body does not desync the lexer for later lines.
TEST(AuditLineLexer, RawStringNoDesync) {
    const QString raw =
        "const char *p = R\"(a//b\"c)\";\n"  // 1 — code + raw string
        "int y;\n";                          // 2 — still code (not desynced)
    EXPECT_TRUE(lineHasCode(raw, "a.cpp", 1));
    EXPECT_TRUE(lineHasCode(raw, "a.cpp", 2));
}

// AL-6 — lines wholly inside a Python triple-quoted docstring are non-code.
TEST(AuditLineLexer, PythonTripleQuote) {
    const QString doc =
        "x = 1\n"                // 1 — code
        "s = \"\"\"\n"           // 2 — `s =` code, opens triple
        "secret in docstring\n"  // 3 — inside """ """
        "\"\"\"\n";              // 4 — closes triple
    EXPECT_FALSE(lineHasCode(doc, "a.py", 3));
}

// AL-7 — an unknown/empty extension falls back to the C-style lexer, where
// `#` is NOT a comment introducer. Proven by contrast: the same `# note`
// line is non-code under the Hash lexer (.py) but code under the default.
TEST(AuditLineLexer, UnknownExtensionDefaultsCStyle) {
    const QString src =
        "v = 1\n"     // 1 — code
        "# note\n";   // 2 — `#` comment under Hash, bare `#` under C-style
    EXPECT_FALSE(lineHasCode(src, "f.py", 2));    // Hash lexer: comment
    EXPECT_TRUE(lineHasCode(src, "noext", 2));    // default C-style: not a comment
}

// AL-8 — line <= 0 returns true (safe default: treat as code).
TEST(AuditLineLexer, NonPositiveLineIsSafeDefault) {
    EXPECT_TRUE(lineHasCode("whatever\n", "a.py", 0));
    EXPECT_TRUE(lineHasCode("whatever\n", "a.py", -5));
}
