#!/usr/bin/env python3
"""ANTS-1382 — convert legacy `g_failures` + `expect()` test helpers
to the shared tests/_support/expect.h API.

Migration steps applied per file:
  1. Insert `#include "../../_support/expect.h"` after the first
     project-local include.
  2. Insert `ANTS_TEST_SCOPE();` at file scope (after the includes,
     before the first `namespace {`).
  3. Delete the local `int g_failures = 0;` line.
  4. Delete the local `void expect(bool ...) { ... }` function body.
  5. In `runMain()`/`runMain(int, char**)`: replace the trailing
     `if (g_failures) { fprintf...; return 1; } ... return 0;`
     diagnostic block with `return expect_finish();`.
  6. Insert `expect_reset();` at the top of runMain's body.
  7. Replace `if (runMain(...)... != 0) FAIL();` with
     `ASSERT_EQ(0, runMain(...));`.
  8. Drop now-orphaned `++g_failures;` lines (each becomes an
     `expect(false, ...)` if attached to a setup error path —
     surfaced as "MANUAL" in the report so I review them by hand).
  9. Drop `<cstdio>` include if no fprintf/printf remain.

Run with: python3 tools/migrate_expect_helper.py [--apply] [path...]
Default: dry-run, prints proposed diffs."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


EXPECT_FN_RE = re.compile(
    r"^void expect\(bool [^)]*\)\s*\{[^}]*\}\s*\n",
    re.MULTILINE | re.DOTALL,
)

# Some files use multi-line bodies with nested braces; tolerate a wider
# pattern by walking braces.
def strip_local_expect(src: str) -> tuple[str, bool]:
    """Remove the local `void expect(bool ...) { ... }` definition."""
    sig_re = re.compile(r"^(?:static\s+)?void expect\(bool[^)]*\)\s*\{",
                         re.MULTILINE)
    m = sig_re.search(src)
    if not m:
        return src, False
    # brace-walk forward from the opening `{` at m.end() - 1
    i = m.end() - 1
    depth = 0
    while i < len(src):
        c = src[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                # consume trailing \n
                while end < len(src) and src[end] in (" ", "\t"):
                    end += 1
                if end < len(src) and src[end] == "\n":
                    end += 1
                return src[: m.start()] + src[end:], True
        i += 1
    return src, False


def strip_g_failures_decl(src: str) -> tuple[str, bool]:
    """Remove `int g_failures = 0;` (and any tab/space prefix)."""
    new = re.sub(
        r"^[ \t]*int g_failures\s*=\s*0;[ \t]*\n",
        "",
        src,
        flags=re.MULTILINE,
    )
    return new, new != src


def replace_runmain_diagnostic(src: str) -> tuple[str, bool]:
    """Replace the trailing `if (g_failures) { ...; return 1; } ...;
    return 0;` block in runMain with `return expect_finish();`."""

    # Anchor on `if (g_failures)` and brace-walk to the runMain
    # function's closing `}`. This is the safest mechanical replacement
    # since file styles vary widely.
    pattern = re.compile(
        r"""
        ^(?P<indent>[ \t]*)if\s*\(\s*g_failures\s*\)\s*\{    # if (g_failures) {
        .*?                                                    # body (any return / fprintf)
        \n(?P=indent)\}\s*\n                                   # closing brace
        (?P<tail>.*?)                                          # any "all good" line(s)
        ^(?P=indent)return\s+0;\s*\n                           # return 0;
        """,
        re.DOTALL | re.MULTILINE | re.VERBOSE,
    )
    m = pattern.search(src)
    if not m:
        return src, False
    indent = m.group("indent")
    replacement = f"{indent}return expect_finish();\n"
    return src[: m.start()] + replacement + src[m.end() :], True


def insert_runmain_reset(src: str) -> tuple[str, bool]:
    """Insert `expect_reset();` as the first statement of runMain."""
    pattern = re.compile(
        r"^(?P<sig>(?:static\s+)?int\s+runMain\([^)]*\)\s*\{[ \t]*\n)",
        re.MULTILINE,
    )
    m = pattern.search(src)
    if not m:
        return src, False
    # Detect indent of next line (function body)
    after = m.end()
    indent_m = re.match(r"([ \t]*)", src[after:after + 32])
    indent = indent_m.group(1) if indent_m else "    "
    # Don't double-insert
    body_head = src[after : after + 200]
    if "expect_reset()" in body_head:
        return src, False
    inject = f"{indent}expect_reset();\n"
    return src[:after] + inject + src[after:], True


def replace_fail_shim(src: str) -> tuple[str, bool]:
    """Replace `if (runMain(...) != 0) FAIL();` with
    `ASSERT_EQ(0, runMain(...));`."""

    pattern = re.compile(
        r"""
        if\s*\(\s*runMain\((?P<args>[^)]*)\)\s*!=\s*0\s*\)\s*  # if (runMain(...) != 0)
        FAIL\(\)\s*;                                            # FAIL();
        """,
        re.VERBOSE,
    )
    n = 0

    def sub(m: re.Match[str]) -> str:
        nonlocal n
        n += 1
        return f"ASSERT_EQ(0, runMain({m.group('args')}));"

    new = pattern.sub(sub, src)
    return new, n > 0


def insert_support_include(src: str) -> tuple[str, bool]:
    """Insert `#include "../../_support/expect.h"` after the first
    project-local include (`#include "..."`). If only system includes
    exist, place it before them."""
    if '"../../_support/expect.h"' in src:
        return src, False
    # Find first `#include "..."` (project-local).
    local_re = re.compile(r'^#include\s+"[^"]+"\s*\n', re.MULTILINE)
    m = local_re.search(src)
    needle = '#include "../../_support/expect.h"\n'
    if m:
        # Insert as the first project-local include — keep the existing
        # block tidy by placing before the matched line plus a blank
        # separator to preserve clang-format-friendly grouping if
        # already present.
        return src[: m.start()] + needle + src[m.start() :], True
    # Fallback: place before first #include of any kind.
    sys_re = re.compile(r"^#include\s*<", re.MULTILINE)
    m2 = sys_re.search(src)
    if m2:
        return src[: m2.start()] + needle + "\n" + src[m2.start() :], True
    # No includes at all? Place at the top.
    return needle + src, True


def insert_test_scope(src: str) -> tuple[str, bool]:
    """Insert `ANTS_TEST_SCOPE();` after the include block, before the
    first `namespace {`."""
    if "ANTS_TEST_SCOPE()" in src:
        return src, False
    # Find first `namespace {` at file scope.
    ns_re = re.compile(r"^namespace\s*\{\s*\n", re.MULTILINE)
    m = ns_re.search(src)
    if m:
        inject = "ANTS_TEST_SCOPE();\n\n"
        return src[: m.start()] + inject + src[m.start() :], True
    # No anonymous namespace — place after last include.
    last_include = None
    for m2 in re.finditer(r"^#include[^\n]*\n", src, re.MULTILINE):
        last_include = m2
    if last_include:
        pos = last_include.end()
        return src[:pos] + "\nANTS_TEST_SCOPE();\n" + src[pos:], True
    return src, False


def drop_unused_cstdio(src: str) -> tuple[str, bool]:
    """Drop `#include <cstdio>` if no fprintf/printf/FILE remains."""
    has_uses = bool(
        re.search(r"\b(?:f?printf|FILE|stderr|stdout|stdin|f?puts)\b",
                  src)
    )
    if has_uses:
        return src, False
    new = re.sub(r"^#include\s*<cstdio>\s*\n", "", src, flags=re.MULTILINE)
    return new, new != src


def drop_orphan_g_failures_increments(src: str) -> tuple[str, bool]:
    """Drop `++g_failures;` lines that were paired with raw fprintf
    setup-error paths (those should be flagged for manual review —
    they were `expect(false, ...)` in spirit, not generic counter
    bumps). For now we leave them as MANUAL findings instead of
    silently dropping."""
    # We DON'T auto-modify these — flag instead.
    if re.search(r"\+\+g_failures\b", src):
        return src, False  # no change; manual
    return src, False


def migrate_full(path: Path, apply: bool) -> dict:
    """Full conversion for canonical pattern: single runMain wrapped
    by single TEST(...) { if (runMain() != 0) FAIL(); }. All-or-
    nothing — if any required step doesn't fire, leave the file
    untouched and report so we can hand-migrate it."""
    src = path.read_text()
    original = src

    # Skip files that don't have the legacy pattern.
    if "g_failures" not in src and "void expect(bool" not in src:
        return {"path": str(path), "status": "skip-no-legacy"}

    # Required: must have a strippable local expect(), a g_failures
    # decl, AND the FAIL shim. If any is missing, hand-migrate.
    has_local_expect = bool(re.search(
        r"^(?:static\s+)?void expect\(bool[^)]*\)\s*\{",
        src, re.MULTILINE,
    ))
    has_g_failures_decl = bool(re.search(
        r"^[ \t]*int g_failures\s*=\s*0;", src, re.MULTILINE,
    ))
    has_fail_shim = bool(re.search(
        r"if\s*\(\s*runMain\([^)]*\)\s*!=\s*0\s*\)\s*FAIL\(\)\s*;",
        src,
    ))
    has_runmain = bool(re.search(
        r"^(?:static\s+)?int\s+runMain\(", src, re.MULTILINE,
    ))
    # Files that access g_failures in test bodies (e.g.
    # `int before = g_failures;` pattern) need a different
    # migration — defer.
    accesses_g_failures_directly = bool(re.search(
        r"\bg_failures\b(?!\s*=\s*0)", src,
    ))
    g_failures_uses_count = len(re.findall(
        r"\bg_failures\b", src,
    ))
    # The decl itself counts; the runMain check counts; anything
    # beyond ~3 uses suggests delta-check pattern.
    if g_failures_uses_count > 4:
        return {"path": str(path), "status": "skip-complex-pattern",
                "notes": [f"g_failures used {g_failures_uses_count}× "
                         "(delta-check pattern; needs manual migration)"]}

    if not (has_local_expect and has_g_failures_decl and has_fail_shim
            and has_runmain):
        return {
            "path": str(path),
            "status": "skip-needs-manual",
            "notes": [
                f"localExpect={has_local_expect}",
                f"g_failuresDecl={has_g_failures_decl}",
                f"failShim={has_fail_shim}",
                f"runMain={has_runmain}",
            ],
        }
    # Setup-error paths bump g_failures directly. Acceptable IF the
    # only remaining `++g_failures` is in setup helpers (writeTemp,
    # etc.) — the user can rewrite as expect(false, ...) later.
    if re.search(r"\+\+g_failures\b", src):
        # Allow if just one or two: convert to expect(false,...)?
        # For safety, defer.
        return {"path": str(path), "status": "skip-needs-manual",
                "notes": ["++g_failures setup-error path remains"]}

    # All-or-nothing migration.
    src, ok1 = insert_support_include(src)
    src, ok2 = insert_test_scope(src)
    src, ok3 = strip_local_expect(src)
    src, ok4 = strip_g_failures_decl(src)
    src, ok5 = insert_runmain_reset(src)
    src, ok6 = replace_runmain_diagnostic(src)
    src, ok7 = replace_fail_shim(src)
    src, _   = drop_unused_cstdio(src)

    # All required steps must have fired.
    required = [ok1, ok2, ok3, ok4, ok5, ok7]
    # ok6 (runMainTail) is optional — some files just have
    # `return g_failures ? 1 : 0;` instead of the if-block.
    if not all(required):
        return {
            "path": str(path),
            "status": "skip-partial",
            "notes": [f"steps={required}"],
        }

    if src == original:
        return {"path": str(path), "status": "no-change"}
    if apply:
        path.write_text(src)
        return {"path": str(path), "status": "written-full"}
    return {"path": str(path), "status": "would-write-full"}


def migrate_fail_shim_only(path: Path, apply: bool) -> dict:
    """For files that don't have g_failures/expect but DO have the
    `if (runMain() != 0) FAIL();` shim — replace just that with
    ASSERT_EQ. These may use other helper conventions but the FAIL
    shim is universally inferior to ASSERT_EQ."""
    src = path.read_text()
    if not re.search(
        r"if\s*\(\s*runMain\([^)]*\)\s*!=\s*0\s*\)\s*FAIL\(\)\s*;",
        src,
    ):
        return {"path": str(path), "status": "skip-no-shim"}
    src2, ok = replace_fail_shim(src)
    if not ok or src2 == src:
        return {"path": str(path), "status": "no-change"}
    if apply:
        path.write_text(src2)
        return {"path": str(path), "status": "written-shim"}
    return {"path": str(path), "status": "would-write-shim"}


def migrate_one(path: Path, apply: bool) -> dict:
    """Try full migration first; if it doesn't apply, fall back to
    FAIL-shim-only conversion."""
    full = migrate_full(path, apply)
    if full["status"] in ("written-full", "would-write-full"):
        return full
    # Either skip or partial — try the shim-only pass for the
    # benefit of files that don't have the g_failures pattern but
    # still have the FAIL shim.
    if "skip-no-legacy" in full["status"]:
        return migrate_fail_shim_only(path, apply)
    return full


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("paths", nargs="*", default=[])
    p.add_argument("--apply", action="store_true",
                   help="Write changes (default: dry-run).")
    args = p.parse_args()

    if args.paths:
        files = [Path(x) for x in args.paths]
    else:
        files = sorted(
            Path("tests/features").glob("*/test_*.cpp")
        )

    results = [migrate_one(f, args.apply) for f in files]

    # Print compact report.
    by_status: dict[str, list[dict]] = {}
    for r in results:
        by_status.setdefault(r["status"], []).append(r)

    for status, items in sorted(by_status.items()):
        print(f"\n=== {status} ({len(items)}) ===")
        for r in items:
            notes = ", ".join(r.get("notes", []))
            print(f"  {r['path']}  {notes}")

    manual = [r for r in results
              if any("MANUAL" in n for n in r.get("notes", []))]
    if manual:
        print(f"\nMANUAL REVIEW: {len(manual)} file(s) need hand "
              f"editing for ++g_failures setup-error paths.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
