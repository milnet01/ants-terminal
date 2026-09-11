# spec.md — ANTS-1455 glob walk + synth mode/pagination

Feature-conformance test for the three ANTS-1455 sub-fixes:

| Group | Pin |
|-------|-----|
| G-1 / G-15 | INV-1 / INV-3 / INV-3a — partition default walk honours `test_globs`, excludes app source |
| G-3 | INV-5 — `scope="path:"` / `scope="files:"` branches unaffected |
| G-4 / G-5 / G-6 / G-17 | INV-6 / INV-7 / INV-8 / INV-8a — `reports_dir` validation, opt-in flag, empty-dir refusal |
| G-7 / G-8 / G-9 / G-14 | INV-10 / INV-11 / INV-13 / INV-14 — `mode` arg + pagination shape |
| G-13 | INV-15 — `mcp-error-codes.md` carries the three new rows |
| G-18 | ANTS-3627 — a caller-supplied `path:` / `files:` scope is root-anchored |
| G-19 / G-20 / G-21 | ANTS-5065 — an omitted `limit` must give the mode default; explicit `limit:-1` still opts into "all" per INV-13; local INV-1 / INV-2 / INV-3 below |

**G-18 (ANTS-3627).** `partition()` concatenated the scope onto the
project root with no anchor check, so `scope:"path:../.."` walked a tree
outside the project and returned its file paths in the envelope. The
three cases pin: (a) `path:` escapes refuse `bad_path`; (b) a `files:`
list refuses as a whole when *any* entry escapes — it does not silently
drop the bad entry; (c) an in-tree `..` that resolves back under the root
(`path:tests/../tests`) is still accepted, so the check is an anchor and
not a substring ban. The fixture places a real sibling directory outside
the root, so an escape has something to find and a passing assertion
can't be an empty-walk artifact. Anchoring is lexical
(`QDir::cleanPath`); symlinks inside the root are not resolved.

## Invariants

Local to G-19/G-20/G-21 (ANTS-5065). Every other row in the table above
cites an invariant number from the canonical spec; these three are new
and are not yet reflected there.

- **INV-1** — `mode:"full"` with `limit` omitted returns the documented
  default page size (5, per ANTS-1455 INV-13), not every collected
  report. `truncatedByLimit` and `next_offset` still signal that more
  remain. *Test:* `G19` in `test_audit_glob_walk_synth_mode.cpp`.
- **INV-2** — Guard: an explicit positive `limit` still returns exactly
  that many reports; the omitted-limit fix does not touch this path.
  *Test:* `G20` in `test_audit_glob_walk_synth_mode.cpp`.
- **INV-3** — Guard: an explicit `limit:-1` still opts into returning
  every collected report — ANTS-1455 INV-13's documented "all" opt-in —
  unchanged by the INV-1 fix. `truncatedByLimit` false, `next_offset`
  -1. *Test:* `G21` in `test_audit_glob_walk_synth_mode.cpp`.

## Rationale

ANTS-1455 INV-13 gives `mode:"full"` two distinct behaviours: an
omitted `limit` gets the mode default (5); an explicit `limit:-1` is a
deliberate opt-in to "return every chunk" (caller accepts the size
risk). The engine's branch (`limit == 0` → 5, `limit < 0` → all)
already implements that contract correctly. The defect sits one level
up: `SynthRequest::limit`'s struct default (`src/testauditengine.h`) is
`-1` — the same sentinel the engine reads as the explicit "all" opt-in
— and the `test_audit_synthesis_prompt` MCP provider
(`src/mainwindow.cpp`) only assigns `req.limit` when the caller's JSON
carries a `"limit"` key. So an ordinary call that omits `limit`
inherits the struct's `-1` default and lands on the "all" branch
instead of "unset", building every collected report (each up to 64 KiB)
on the GUI thread — well past ANTS-1455 §4's stated budget of about
80 KiB per page at the default limit of 5. Filed as ANTS-5065. The fix
changes the struct default, not the engine's branch logic — G21 pins
that the branch logic, and the opt-in it implements, are unchanged.

## Scope

In scope: the omitted-limit contract (INV-1), that an explicit positive
limit is unaffected (INV-2), and that the existing explicit `limit:-1`
"return all" opt-in from ANTS-1455 INV-13 survives the fix unchanged
(INV-3). Out of scope: any new opt-in mechanism for "return all" —
none is needed, since INV-13's `limit:-1` opt-in is being preserved,
not replaced.

## Regression history

Introduced alongside ANTS-1455's `mode:"full"` pagination:
`SynthRequest::limit`'s struct default was set to the same sentinel
(`-1`) the engine treats as the explicit "return all" opt-in, so an
omitted limit and an explicit opt-in became indistinguishable at the
call site. Fixed under ANTS-5065 by changing the struct default, not
the engine's branch logic.

Runtime fixtures via `QTemporaryDir`; no committed corpora.
See canonical spec at [`docs/specs/ANTS-1455.md`](../../../docs/specs/ANTS-1455.md).
