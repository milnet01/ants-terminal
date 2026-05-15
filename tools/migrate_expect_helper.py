#!/usr/bin/env python3
"""ANTS-1382 / ANTS-1385 — convert legacy `g_failures` + `expect()` test
helpers to the shared tests/_support/expect.h API.

Patterns handled:
  A — runMain() wrapper + `if (runMain() != 0) FAIL();` shim.
  B — single TEST + `EXPECT_EQ(g_failures, 0);` at end.
  C — multi-TEST delta-check (`int before = g_failures; ...;
      if (g_failures > before) FAIL();`).
  E — single TEST + trailing
      `if (g_failures) { fprintf...; FAIL(); }` block.

Patterns NOT auto-handled (refused, surfaced for hand-migration):
  F — multi-TEST that never checks g_failures (silently broken — fix
      requires per-TEST instrumentation).
  Variants with macros, custom counter names, or stream-form FAIL —
      surfaced as 'OTHER'.

Run with: python3 tools/migrate_expect_helper.py [--apply] [path...]
Default: dry-run, prints classification + planned actions."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


# ---- shared helpers --------------------------------------------------


def insert_support_include(src: str) -> tuple[str, bool]:
    if '"../../_support/expect.h"' in src:
        return src, True  # already present is OK
    local_re = re.compile(r'^#include\s+"[^"]+"\s*\n', re.MULTILINE)
    m = local_re.search(src)
    needle = '#include "../../_support/expect.h"\n'
    if m:
        return src[: m.start()] + needle + src[m.start() :], True
    sys_re = re.compile(r"^#include\s*<", re.MULTILINE)
    m2 = sys_re.search(src)
    if m2:
        return src[: m2.start()] + needle + "\n" + src[m2.start() :], True
    return needle + src, True


def insert_test_scope(src: str) -> tuple[str, bool]:
    if "ANTS_TEST_SCOPE()" in src:
        return src, True
    ns_re = re.compile(r"^namespace\s*\{\s*\n", re.MULTILINE)
    m = ns_re.search(src)
    if m:
        return src[: m.start()] + "ANTS_TEST_SCOPE();\n\n" + src[m.start() :], True
    last_include = None
    for m2 in re.finditer(r"^#include[^\n]*\n", src, re.MULTILINE):
        last_include = m2
    if last_include:
        pos = last_include.end()
        return src[:pos] + "\nANTS_TEST_SCOPE();\n" + src[pos:], True
    return src, False


def strip_local_expect(src: str) -> tuple[str, bool]:
    """Find `void expect(bool ...)` (handles nested parens like
    `= QString()` default args) and strip the entire function body."""
    sig_re = re.compile(
        r"^(?:static\s+)?void expect\(bool",
        re.MULTILINE,
    )
    m = sig_re.search(src)
    if not m:
        return src, False
    # Walk the parameter-list parens balanced from the `(` after `expect`.
    open_paren = src.index("(", m.start())
    i = open_paren
    pdepth = 0
    while i < len(src):
        c = src[i]
        if c == "(":
            pdepth += 1
        elif c == ")":
            pdepth -= 1
            if pdepth == 0:
                break
        i += 1
    if i >= len(src):
        return src, False
    # Skip whitespace to opening brace.
    j = i + 1
    while j < len(src) and src[j] in (" ", "\t", "\n"):
        j += 1
    if j >= len(src) or src[j] != "{":
        return src, False
    # Brace-walk body.
    depth = 0
    k = j
    while k < len(src):
        c = src[k]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = k + 1
                while end < len(src) and src[end] in (" ", "\t"):
                    end += 1
                if end < len(src) and src[end] == "\n":
                    end += 1
                return src[: m.start()] + src[end:], True
        k += 1
    return src, False


def strip_g_failures_decl(src: str) -> tuple[str, bool]:
    new = re.sub(
        r"^[ \t]*int g_failures\s*=\s*0;[ \t]*\n",
        "",
        src,
        flags=re.MULTILINE,
    )
    return new, new != src


def convert_orphan_increments(src: str) -> tuple[str, int]:
    """After local expect() is stripped, any remaining `++g_failures;`
    is a setup-error path. Convert each to `expect(false, "setup", "");`
    so the helper's failure count is bumped. The preceding fprintf is
    left intact; readers see both the diagnostic and the fail tick."""
    new, n = re.subn(
        r"^([ \t]*)\+\+g_failures\s*;[ \t]*\n",
        r'\1expect(false, "setup-error", "");\n',
        src,
        flags=re.MULTILINE,
    )
    # Also handle `if (!ok) ++g_failures;` (one-line form inside helper
    # bodies that aren't the local expect()).
    new, n2 = re.subn(
        r"if\s*\(\s*!\s*ok\s*\)\s*\+\+g_failures\s*;",
        r'expect(ok, "check-result", "");',
        new,
    )
    return new, n + n2


def drop_unused_cstdio(src: str) -> tuple[str, bool]:
    has_uses = bool(
        re.search(r"\b(?:f?printf|FILE|stderr|stdout|stdin|f?puts)\b", src)
    )
    if has_uses:
        return src, False
    new = re.sub(r"^#include\s*<cstdio>\s*\n", "", src, flags=re.MULTILINE)
    return new, new != src


# ---- pattern detection -----------------------------------------------


def classify(src: str) -> str:
    if "ANTS_TEST_SCOPE" in src:
        return "ALREADY"
    if "int g_failures = 0" not in src:
        return "NO_LEGACY"
    has_runmain = bool(re.search(r"^\s*(?:static\s+)?int\s+runMain\(", src, re.M))
    has_fail_shim = bool(
        re.search(r"if\s*\(\s*runMain\([^)]*\)\s*!=\s*0\s*\)\s*FAIL\(\)\s*;", src)
    )
    has_test_fail_block = bool(
        re.search(
            r"if\s*\(\s*g_failures(?:\s*>\s*0)?\s*\)\s*\{[^}]*FAIL\(\)[^}]*\}",
            src,
            re.S,
        )
    )
    has_test_expect_eq = "EXPECT_EQ(g_failures, 0)" in src
    has_delta = "int before = g_failures" in src
    test_count = len(re.findall(r"^\s*TEST\s*\(", src, re.M))

    # Pattern G: multi-TEST with per-TEST `if (g_failures) FAIL();` or
    # the multi-line `if (g_failures) { FAIL() << ...; }` form.
    # Explicit `g_failures = 0;` reset per-TEST is optional — if absent
    # we inject `expect_reset();` at the top of each TEST so state
    # doesn't leak across them (most legacy multi-TEST files miss this
    # reset, leading to silent cascade failures).
    has_per_test_fail = bool(
        re.search(r"if\s*\(\s*g_failures(?:\s*>\s*0)?\s*\)\s*\{?\s*FAIL\s*\(\)", src)
    )

    has_explicit_reset = bool(re.search(r"^\s*g_failures\s*=\s*0\s*;", src, re.M))
    has_stream_fail = bool(
        re.search(r"FAIL\(\)\s*<<\s*g_failures", src)
    )

    if has_runmain and has_fail_shim:
        return "A"
    if has_delta:
        return "C"
    if has_test_expect_eq:
        return "B"
    # G handles: multi-TEST per-test-fail (with or without reset), OR
    # any pattern with explicit `g_failures = 0;` resets, OR any
    # pattern with `FAIL() << g_failures` stream form.
    if has_per_test_fail and (test_count > 1 or has_explicit_reset
                               or has_stream_fail):
        return "G"
    if has_test_fail_block and test_count == 1:
        return "E"
    return "OTHER"


# ---- pattern-specific transforms -------------------------------------


def apply_pattern_a(src: str) -> tuple[str, bool, list[str]]:
    """runMain wrapper — replace runMain trailing g_failures check with
    expect_finish, replace shim with ASSERT_EQ, insert reset at top."""
    notes: list[str] = []
    # Tail variant 1: `if (g_failures[ > 0]) { ...; return 1; } return 0;`
    pat1 = re.compile(
        r"""
        ^(?P<indent>[ \t]*)if\s*\(\s*g_failures(?:\s*>\s*0)?\s*\)\s*\{
        .*?
        \n(?P=indent)\}\s*\n
        (?P<tail>.*?)
        ^(?P=indent)return\s+0;\s*\n
        """,
        re.DOTALL | re.MULTILINE | re.VERBOSE,
    )
    # Tail variant 2: `return g_failures == 0 ? 0 : 1;` (single-line).
    pat2 = re.compile(
        r"^(?P<indent>[ \t]*)return\s+g_failures\s*==\s*0\s*\?\s*0\s*:\s*1\s*;\s*\n",
        re.MULTILINE,
    )
    # Tail variant 3: `return g_failures != 0 ? 1 : 0;`
    pat3 = re.compile(
        r"^(?P<indent>[ \t]*)return\s+g_failures\s*!=\s*0\s*\?\s*1\s*:\s*0\s*;\s*\n",
        re.MULTILINE,
    )
    # Tail variant 4: `return g_failures ? 1 : 0;`
    pat4 = re.compile(
        r"^(?P<indent>[ \t]*)return\s+g_failures\s*\?\s*1\s*:\s*0\s*;\s*\n",
        re.MULTILINE,
    )
    for pat in (pat1, pat2, pat3, pat4):
        m = pat.search(src)
        if m:
            indent = m.group("indent")
            src = src[: m.start()] + f"{indent}return expect_finish();\n" + src[m.end():]
            notes.append("runMain-tail->expect_finish")
            break
    else:
        notes.append("runMain-tail-MISSING")
        return src, False, notes

    # FAIL shim → ASSERT_EQ.
    src, n = re.subn(
        r"if\s*\(\s*runMain\((?P<args>[^)]*)\)\s*!=\s*0\s*\)\s*FAIL\(\)\s*;",
        lambda m: f"ASSERT_EQ(0, runMain({m.group('args')}));",
        src,
    )
    if n == 0:
        notes.append("fail-shim-MISSING")
        return src, False, notes
    notes.append(f"fail-shim->ASSERT_EQ ({n})")

    # Insert expect_reset at top of runMain.
    sig = re.compile(
        r"^(?P<sig>(?:static\s+)?int\s+runMain\([^)]*\)\s*\{[ \t]*\n)",
        re.MULTILINE,
    )
    sm = sig.search(src)
    if sm:
        after = sm.end()
        indent_m = re.match(r"([ \t]*)", src[after:after + 32])
        indent = indent_m.group(1) if indent_m else "    "
        body_head = src[after : after + 200]
        if "expect_reset()" not in body_head:
            src = src[:after] + f"{indent}expect_reset();\n" + src[after:]
            notes.append("runMain-reset-inserted")

    return src, True, notes


def apply_pattern_b(src: str) -> tuple[str, bool, list[str]]:
    """TEST + EXPECT_EQ(g_failures, 0) end."""
    notes: list[str] = []
    src, n = re.subn(
        r"EXPECT_EQ\(g_failures,\s*0\)",
        "EXPECT_EQ(0, expect_failures())",
        src,
    )
    if n == 0:
        return src, False, notes
    notes.append(f"EXPECT_EQ-replaced ({n})")
    src = _insert_reset_at_test_top(src, notes)
    return src, True, notes


def apply_pattern_c(src: str) -> tuple[str, bool, list[str]]:
    """Multi-TEST delta-check: rewrite g_failures references."""
    notes: list[str] = []
    # int before = g_failures;  →  const int before = expect_failures();
    src, n1 = re.subn(
        r"\bint\s+before\s*=\s*g_failures\s*;",
        "const int before = expect_failures();",
        src,
    )
    if n1:
        notes.append(f"before-decl ({n1})")
    # if (g_failures > before)  → if (expect_failures() > before)
    src, n2 = re.subn(
        r"\bg_failures\s*([!><]=?)\s*before\b",
        r"expect_failures() \1 before",
        src,
    )
    if n2:
        notes.append(f"delta-check ({n2})")
    if n1 == 0 or n2 == 0:
        return src, False, notes
    return src, True, notes


def apply_pattern_e(src: str) -> tuple[str, bool, list[str]]:
    """Single TEST + trailing if (g_failures) { fprintf...; FAIL(); }."""
    notes: list[str] = []
    pattern = re.compile(
        r"""
        ^(?P<indent>[ \t]*)if\s*\(\s*g_failures(?:\s*>\s*0)?\s*\)\s*\{
        .*?
        FAIL\(\)\s*;
        .*?
        \n(?P=indent)\}\s*\n
        """,
        re.DOTALL | re.MULTILINE | re.VERBOSE,
    )
    m = pattern.search(src)
    if not m:
        return src, False, notes
    indent = m.group("indent")
    replacement = f"{indent}ASSERT_EQ(0, expect_finish());\n"
    src = src[: m.start()] + replacement + src[m.end() :]
    notes.append("test-fail-block->ASSERT_EQ")
    src = _insert_reset_at_test_top(src, notes)
    return src, True, notes


def apply_pattern_g(src: str) -> tuple[str, bool, list[str]]:
    """Multi-TEST + per-TEST `if (g_failures) FAIL();` (with optional
    explicit `g_failures = 0;` reset and/or stream form). Replace each
    piece with the helper API; inject expect_reset() at the top of
    every TEST that doesn't already have one."""
    notes: list[str] = []
    # Step 1: `g_failures = 0;` (standalone) → `expect_reset();`
    src, n1 = re.subn(
        r"^([ \t]*)g_failures\s*=\s*0\s*;[ \t]*\n",
        r"\1expect_reset();\n",
        src,
        flags=re.MULTILINE,
    )
    if n1:
        notes.append(f"explicit-reset->expect_reset ({n1})")

    # Step 2a: multi-line `if (g_failures) { FAIL() << ...; }` block.
    block_re = re.compile(
        r"""
        ^(?P<indent>[ \t]*)if\s*\(\s*g_failures(?:\s*>\s*0)?\s*\)\s*\{
        \s*FAIL\(\)\s*<<\s*g_failures(?P<rest>[^;]*);\s*\n
        (?P=indent)\}\s*\n
        """,
        re.MULTILINE | re.VERBOSE,
    )

    def _block_sub(m: re.Match[str]) -> str:
        return (f"{m.group('indent')}EXPECT_EQ(0, expect_failures()) "
                f"<< expect_failures(){m.group('rest')};\n")

    src, n_block = block_re.subn(_block_sub, src)
    if n_block:
        notes.append(f"block-fail->EXPECT_EQ ({n_block})")

    # Step 2b: one-liner with stream-form
    # `if (g_failures) FAIL() << g_failures << "...";` →
    #   `EXPECT_EQ(0, expect_failures()) << expect_failures() << "...";`
    src, n2a = re.subn(
        r"if\s*\(\s*g_failures(?:\s*>\s*0)?\s*\)\s*FAIL\(\)\s*<<\s*g_failures",
        r"EXPECT_EQ(0, expect_failures()) << expect_failures()",
        src,
    )
    src, n2b = re.subn(
        r"if\s*\(\s*g_failures(?:\s*>\s*0)?\s*\)\s*FAIL\(\)\s*<<",
        r"EXPECT_EQ(0, expect_failures()) <<",
        src,
    )
    src, n2c = re.subn(
        r"if\s*\(\s*g_failures(?:\s*>\s*0)?\s*\)\s*FAIL\(\)\s*;",
        r"EXPECT_EQ(0, expect_failures());",
        src,
    )
    n2 = n2a + n2b + n2c
    if n2 + n_block:
        notes.append(f"per-test-fail->EXPECT_EQ ({n2 + n_block})")

    # Step 3: inject `expect_reset();` at the top of every TEST that
    # doesn't already have one — protects against state-leak across
    # tests that historically relied on a (silently-broken) global
    # counter.
    src, n_inj = _inject_reset_into_every_test(src)
    if n_inj:
        notes.append(f"reset-injected ({n_inj})")

    if (n2 + n_block) == 0:
        return src, False, notes
    return src, True, notes


def _inject_reset_into_every_test(src: str) -> tuple[str, int]:
    """Walk every `TEST(...) { ... }` block; if it doesn't start with
    `expect_reset()`, insert one. Brace-balance, so nested blocks don't
    confuse us."""
    test_re = re.compile(r"^TEST\s*\([^)]*\)\s*\{[ \t]*\n", re.MULTILINE)
    out_chunks = []
    pos = 0
    n = 0
    for m in test_re.finditer(src):
        out_chunks.append(src[pos:m.end()])
        # Indentation of body.
        body_start = m.end()
        indent_m = re.match(r"([ \t]*)", src[body_start:body_start + 32])
        indent = indent_m.group(1) if indent_m else "    "
        body_head = src[body_start:body_start + 200]
        if "expect_reset()" not in body_head:
            out_chunks.append(f"{indent}expect_reset();\n")
            n += 1
        pos = body_start
    out_chunks.append(src[pos:])
    return "".join(out_chunks), n


def _insert_reset_at_test_top(src: str, notes: list[str]) -> str:
    """Insert `expect_reset();` as the first statement of the (single)
    TEST block. Single-TEST helper — used by Pattern B and E."""
    test_re = re.compile(
        r"^(?P<sig>TEST\s*\([^)]*\)\s*\{[ \t]*\n)",
        re.MULTILINE,
    )
    m = test_re.search(src)
    if not m:
        return src
    after = m.end()
    indent_m = re.match(r"([ \t]*)", src[after:after + 32])
    indent = indent_m.group(1) if indent_m else "    "
    body_head = src[after : after + 200]
    if "expect_reset()" in body_head:
        return src
    notes.append("test-top-reset-inserted")
    return src[:after] + f"{indent}expect_reset();\n" + src[after:]


# ---- driver ----------------------------------------------------------


def migrate_one(path: Path, apply: bool) -> dict:
    src = path.read_text()
    pat = classify(src)

    if pat == "ALREADY":
        return {"path": str(path), "pattern": pat, "status": "skip-already"}
    if pat == "NO_LEGACY":
        # Try legacy FAIL-shim conversion.
        src2, n = re.subn(
            r"if\s*\(\s*runMain\((?P<args>[^)]*)\)\s*!=\s*0\s*\)\s*FAIL\(\)\s*;",
            lambda m: f"ASSERT_EQ(0, runMain({m.group('args')}));",
            src,
        )
        if n > 0:
            if apply:
                path.write_text(src2)
                return {"path": str(path), "pattern": "shim",
                        "status": "written", "notes": [f"shim ({n})"]}
            return {"path": str(path), "pattern": "shim",
                    "status": "would-write", "notes": [f"shim ({n})"]}
        return {"path": str(path), "pattern": pat, "status": "skip-no-legacy"}
    if pat == "OTHER":
        return {"path": str(path), "pattern": pat,
                "status": "skip-needs-manual",
                "notes": ["unrecognized pattern"]}

    # All-or-nothing: apply scope+strips+pattern transform; if any
    # required step fails, leave the file untouched and report.
    original = src
    src, ok_inc = insert_support_include(src)
    src, ok_scope = insert_test_scope(src)
    src, ok_decl = strip_g_failures_decl(src)
    src, ok_expect = strip_local_expect(src)

    transform = {
        "A": apply_pattern_a,
        "B": apply_pattern_b,
        "C": apply_pattern_c,
        "E": apply_pattern_e,
        "G": apply_pattern_g,
    }[pat]
    src, ok_pat, notes = transform(src)

    # After local-expect strip + pattern transform, convert any orphan
    # ++g_failures sites to expect(false, ...).
    src, n_orphan = convert_orphan_increments(src)
    if n_orphan:
        notes.append(f"orphan-incs->expect ({n_orphan})")
    if "++g_failures" in src:
        return {"path": str(path), "pattern": pat,
                "status": "skip-orphan-increment",
                "notes": notes + ["++g_failures remains after strip"]}
    # No bare `g_failures` should remain either (decl + uses gone).
    if re.search(r"\bg_failures\b", src):
        return {"path": str(path), "pattern": pat,
                "status": "skip-residual-references",
                "notes": notes + ["bare g_failures remains"]}

    src, _ = drop_unused_cstdio(src)

    if not (ok_inc and ok_scope and ok_decl and ok_expect and ok_pat):
        return {"path": str(path), "pattern": pat, "status": "skip-partial",
                "notes": notes + [
                    f"steps inc={ok_inc} scope={ok_scope} decl={ok_decl} "
                    f"expect={ok_expect} pat={ok_pat}",
                ]}
    if src == original:
        return {"path": str(path), "pattern": pat, "status": "no-change"}

    if apply:
        path.write_text(src)
        return {"path": str(path), "pattern": pat,
                "status": "written", "notes": notes}
    return {"path": str(path), "pattern": pat,
            "status": "would-write", "notes": notes}


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("paths", nargs="*", default=[])
    p.add_argument("--apply", action="store_true")
    args = p.parse_args()

    files = [Path(x) for x in args.paths] if args.paths else sorted(
        Path("tests/features").glob("*/test_*.cpp")
    )
    results = [migrate_one(f, args.apply) for f in files]

    by_status: dict[str, list[dict]] = {}
    for r in results:
        by_status.setdefault(r["status"], []).append(r)

    for status, items in sorted(by_status.items()):
        print(f"\n=== {status} ({len(items)}) ===")
        for r in items:
            notes = ", ".join(r.get("notes", []))
            print(f"  [{r['pattern']}] {r['path']}  {notes}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
