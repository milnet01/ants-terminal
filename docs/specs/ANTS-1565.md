# ANTS-1565 — `workspace_search` configurable wall-clock budget

**Status:** implementing
**Source:** cross-session-report-2026-05-18 (Vestige Issue #8)
**Pull:** 12 (MCP timeout-ergonomics fold-in, 0.7.92)

## 1. Problem

`workspace_search`'s `kWorkspaceSearchHardKillMs = 2000` is too short for
mid-size projects. Vestige's 2026-05-18 localization design session hit
three back-to-back `rg_failed` envelopes on patterns scoped to a 3700-file
`engine/**/*.{h,cpp}` lane. The fallback `Bash → rg -l --no-messages
PATTERN -g '*.h' -g '*.cpp'` completed each query in 150–400 ms — `rg`
itself is fast; the 2 s budget is consumed by wrapper setup (gitignore
parse + glob expansion + ANTS-1501 dedup grouping) on the first query of
a cold cache. Subsequent queries on the same project hit warmer state but
the 2 s ceiling is still tight against the gitignore + setup overhead on
larger trees.

## 2. Fix

Three changes, all behind the existing `cmdWorkspaceSearch` entry point in
`src/remotecontrol.cpp`:

- **Bump the default budget from 2 s to 5 s.** `kWorkspaceSearchHardKillMs`
  becomes `5000`. The 2 s value was sized for ~1 k-file projects on
  ANTS-1248 (2026-04-21); the median Ants user's target project is larger
  than that today and the 200 ms kill-grace + 100 ms argv-build are a
  fixed overhead that scales worse than the search itself.

- **Expose a per-call `timeout_sec` arg.** Caller passes an integer
  number of seconds. Clamped to `[1, 30]` server-side. If absent or
  out-of-range, the new 5 s default applies. The effective value is the
  same wall-clock budget the rg process is killed against — same plumb
  path, no surprises. Echoed back on the response envelope as
  `timeout_sec` so callers can see what was used.

- **Surface a fallback hint on hard-kill.** When `hardKilled && matches.
  isEmpty()`, the error envelope grows a `"hint"` field:
  > `"try a narrower lane= or glob= filter, raise timeout_sec (max 30),
  > or fall back to `Bash rg` for one-off queries"`.

  Same `code: "rg_failed"` so existing callers that branch on `code`
  continue to work; the hint is purely additive.

## 3. Invariants

- **INV-1:** Default budget is 5 s when `timeout_sec` is absent. (Was 2 s.)
- **INV-2:** `timeout_sec` accepts integer values in `[1, 30]`. Out-of-
  range or non-numeric values are ignored — the default applies, and the
  effective value is reflected on the response.
- **INV-3:** The `hardKilled` error envelope includes a `hint` field on
  empty-match hard-kill. The hint mentions the three viable next steps:
  narrower filter, larger `timeout_sec`, fallback Bash.
- **INV-4:** Response envelope always includes the effective
  `timeout_sec` (the value the kill timer used, in seconds). Both ok:true
  and ok:false (hard-kill) paths.
- **INV-5:** `timeout_sec` upper bound stays under the MCP transport
  envelope timeout (60 s today) by a safety margin. 30 s leaves 30 s for
  envelope encode + transport.

## 4. Test plan

`tests/features/mcp_workspace_search_timeout_sec/`:

- INV-1: default budget integer matches the new constant.
- INV-2: clamp behaviour — `timeout_sec=0` → default; `timeout_sec=99` →
  capped at 30; `timeout_sec=10` → exactly 10.
- INV-3: hard-kill envelope contains `hint` substring.
- INV-4: response carries `timeout_sec` on both ok and rg_failed paths.

Pure-function + source-scrape (no live PTY/rg required) — same shape as
existing `mcp_workspace_search_*` tests.

## 5. Out-of-scope (deferred)

- Profiling the pre-rg wrapper setup overhead (Vestige's "if setup eats
  > 500 ms that's the real bug" follow-up). Sized as a separate research
  pass — would need scoped profiling on a representative 3.7k-file tree
  and a separate spec.
- Adding `timeout_sec` to other MCP tools (verify_changes already has it
  per ANTS-1525; git_state has fixed 5 s budgets). Per-tool sizing.

## 6. Sources

- Vestige cross-session report 2026-05-18 Issue #8.
- ANTS-1525 (verify_changes timeout_sec) — existing precedent for the
  arg shape.
- ANTS-1248 (workspace_search v1) — original 2 s ceiling.
- ANTS-1501 (also_at dedup) — the wrapper's main setup cost beyond
  gitignore + glob.
