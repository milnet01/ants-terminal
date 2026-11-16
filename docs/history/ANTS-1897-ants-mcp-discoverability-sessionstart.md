# ANTS-1897 spec — history

Passages moved verbatim out of
[`docs/specs/ANTS-1897-ants-mcp-discoverability-sessionstart.md`](../specs/ANTS-1897-ants-mcp-discoverability-sessionstart.md)
on 2026-11-16 (ANTS-6786). Each names where it stood.

## The **Status:** line, after "**implemented**"

— accepted (2026-05-27), cold-eyes loops 1–5
folded (user signed off at the `--max-loops` cap: 0 CRITICAL, 3 HIGH all
mechanical, all folded); shipped, and the status corrected 2026-08-15
(ANTS-4135) from `accepted`, which it had kept while every invariant here was
live and locked. That was not cosmetic: `spec_lint`'s test-surface check keys
on status, so this spec's two wrong directory names came back as
`test_surface_unresolved` *candidates* instead of the findings they were.

## The preamble, before **Layman:**

**Amended 2026-06-05 (ANTS-2038):** the case-2 rewrite gate is now
**content-parity**, not version-only. A marker-present (Ants-managed)
script is rewritten whenever its on-disk body differs byte-for-byte
from the rendered template — so an in-version template edit (a prelude
reword shipped without a version bump, e.g. ANTS-1985's catalog
repoint) reaches installed sessions on the next launch instead of going
stale until the next bump. This **supersedes** the loop-5 MEDIUM
trade-off below ("user edit survives until next version bump"): a user
hand-edit to the managed body is now overwritten on the next launch;
ownership is taken by deleting the marker line (case 3, INV-2 —
unchanged). Touches § 2.1.1 case 2, INV-1, INV-12, and the script
header comment. Cold-eyes loop log for this amendment at the end of
this file.

## § 2.1.2, after the script-path sentence

Until ANTS-5730 this was `AppConfigLocation`; see the
  amendment at the end.

## § 2.2, after "asserts coverage." (§ 6 names the tests)

**Amended 2026-08-15 (ANTS-4135):
  it shipped as `McpOrientation_Inv6.SelectionHintCoverage` in
  `tests/features/mcp_orientation_install/`, with a second angle in
  `tests/features/mcp_selection_hint/`** — never as
  `mcp_selection_hint_coverage/`, which does not exist.

## INV-13, its last sentence (the test names stay in INV-13)

*(Amended 2026-09-13,
  ANTS-5104, recording what was built; tests
  `WrongTypedHooksNoClobber`, `NoOpInstallLeavesSettingsUntouched`.)*

## INV-14, after its *Test:*

**Amended 2026-08-15
  (ANTS-4135):** the separate `mcp_pty_env_carries_socket/` dir this
  clause named was planned for a `/dev/ptmx` harness and never
  created; the invariant is locked in the orientation bundle
  instead, against the exported socket path rather than through a
  live PTY.

## INV-3, the reason for quoting the path (rewritten: the old path is gone)

the script path so spaces in `QStandardPaths::
  AppConfigLocation` (Qt builds `<home>/.config/Ants Terminal/...`
  from `applicationName`) do not tokenise at hook-run time.

## The "Amendment — ANTS-5730" section; its rules and tests moved to § 5

## Amendment — ANTS-5730 (2026-11-14)

No reviewer was dispatched for this amendment.

`AppConfigLocation` is built from the application name, "Ants Terminal",
so the script sat in `<config>/Ants Terminal/hooks/`, a second folder
beside `<config>/ants-terminal/`. The passages above that name
`AppConfigLocation` record how it was. Now:
