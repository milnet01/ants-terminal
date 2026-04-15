# Feature conformance tests

This directory holds **behavioral contract tests** — one subdirectory per
feature, each pairing a human-readable spec (`spec.md`) with a runnable
executable that exercises the feature through its real API and asserts
the observable output matches the contract.

This complements, rather than replaces, the unit-style shell tests in
`tests/audit_fixtures/`. The difference:

| Style | Tests what | Format | Example |
|---|---|---|---|
| `audit_fixtures/` | one regex matches the right fixture files | shell + fixture files | `audit_rule_fixtures` |
| `features/` | a feature's observable behavior satisfies a written contract | `spec.md` + C++ test exec | `scrollback_redraw` |

## Layout

```
tests/features/
  <feature-name>/
    spec.md                 # human contract — reviewed like code
    test_<feature>.cpp      # exec — links against src/<relevant>.cpp
```

## Why this matters

Unit tests verify code paths. Feature tests verify user-observable
behavior matches the written contract. When a change silently alters
behavior (e.g. a partial fix that only covers one code path), unit
tests keep passing but the feature regresses. A feature test with a
clear invariant catches that gap.

The first feature test in this directory — `scrollback_redraw` —
exists specifically because 0.6.21 shipped an incomplete fix for the
main-screen TUI scrollback-doubling bug. The 0.6.21 fix covered the
"user scrolled up" path but not the "user at the bottom" path, which
is the common case. A feature test asserting the top-level invariant
("scrollback must not double after CSI 2J repaint of the same content")
would have caught the gap at commit time instead of after the bug was
reported against three prior releases.

## Authoring a new feature test

1. `mkdir tests/features/<feature-name>/`.
2. Write `spec.md`: the feature's invariants, in plain English,
   reviewable by humans. Include motivation, scope, and what's *out*
   of scope.
3. Write `test_<feature>.cpp`: exercise the feature through its real
   public API. Assert invariants. Exit 0 on pass, non-zero on fail.
   Print enough context on failure that a reader can diagnose without
   reproducing.
4. Wire into `CMakeLists.txt` as an `add_executable` + `add_test`
   with label `features`.
5. Run `ctest -L features` and confirm it passes.
6. Run it against the pre-fix code (temporarily revert the fix) to
   confirm it *would* have caught the bug. If it doesn't fail
   pre-fix, the invariant is too loose — tighten it.

The "run against pre-fix to confirm the test would have caught it"
step is non-negotiable. A test that passes on broken code has no
value.

## What goes here vs. unit tests

Belongs here:
- VT protocol conformance (escape → grid state pairs)
- Scrollback / reflow invariants
- OSC 8 / OSC 52 / OSC 133 behavioral contracts
- Session save/restore round-trip invariants
- Audit dialog rule execution end-to-end

Belongs in `audit_fixtures/` or inline unit tests:
- Regex matches expected fixture files
- Pure-function algorithmic correctness (UTF-8 decode, glob-to-regex)
- Data-structure invariants

Rule of thumb: if the invariant makes sense to a user and survives
implementation rewrites, it belongs here. If it only makes sense to
someone reading the code, it belongs elsewhere.
