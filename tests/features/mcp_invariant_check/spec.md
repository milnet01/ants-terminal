# mcp_invariant_check — ANTS-1308

Locks the wiring contract for the `invariant_check` MCP tool — the
"which specs reference this file, and what invariants do they
declare" pre-edit surface that scans `docs/specs/*.md` for
substring mentions of the input `files[]`.

## Invariants

| # | Statement |
|---|-----------|
| 1 | `cmdInvariantCheck(const QJsonObject &req)` declared public on `RemoteControl` in `src/remotecontrol.h`. |
| 2 | `cmdInvariantCheck` defined in the remotecontrol TUs and carries an `ANTS-1308` anchor comment in or above the function body. |
| 3 | The body refuses with `code:"bad_files"` when the `files` argument is missing, empty, or normalises to an empty list. |
| 4 | The body iterates the specs dir via `QDir` (the directory listing surface, not a hardcoded file list), unfiltered by any project-specific id prefix — ANTS-4376 retired the `ANTS-*.md` glob that made this verb blind on every non-ANTS project. |
| 5 | The body delegates parsing to the shared `parseSpecBody` helper (same helper that backs `cmdSpecQuery`, so the parser is single-sourced). |
| 6 | `MainWindow::setupClaudeMcpProviders` (`src/mainwindow.cpp`) registers `"invariant_check"` via `registerToolProvider` and delegates to `m_remoteControl->cmdInvariantCheck`. Falls back to `kRcUnavailable` when `m_remoteControl` is null. |
| 7 | The `tools/list` block in `src/claudeintegration.cpp` registers an `"invariant_check"` entry. Schema declares `properties.files` (required, array of string, minItems 1) and `properties.caller_cwd` (required). |
| 8 | `callerCwdContractFor` in `src/claudeintegration.cpp` classifies `"invariant_check"` as `Required` (explicit branch). |
| 9 | (ANTS-3699) With no `mode` argument the response is the SUMMARY shape: `matched_specs[]` entries keep `id`/`path`/`title`/`matched_terms`/`invariants_count` but carry no `invariants` key at all; the envelope reports `mode:"summary"`, `invariants_included:false`, and — when there is at least one match — a `hint` naming `spec_query` and `mode:"full"`. Summary is the default, not an opt-in. |
| 10 | (ANTS-3699) `mode:"full"` restores `invariants:[{id, body}]` verbatim, with `mode:"full"`, `invariants_included:true` and no `hint`. `invariants_count` is the true count in both modes. |
| 11 | (ANTS-3699) Any other `mode` value refuses with `code:"bad_mode"` — a typo must not silently resolve to a shape the caller did not ask for. |
| 12 | (ANTS-4644) When the paths as given match NOTHING and specs were scanned, the scan is retried with each path's suffixes, longest first: `src/finbreak/services/auth.py` against a spec citing `services/auth.py` returns that spec, with `fallback_kind:"path_suffix"`. |
| 13 | (ANTS-4644) The bare basename is a SEPARATE, later tier reported as `fallback_kind:"basename"` — it is the tier that can collide, and it is the one this repo needs (`docs/specs/ANTS-2161.md` cites `projectsettings.cpp`). |
| 14 | (ANTS-4644) The basename tier runs only when every fuller form failed: with one spec citing `services/auth.py` and another citing an unrelated `auth.py`, a query for `src/finbreak/services/auth.py` returns the first alone. |
| 15 | (ANTS-4644) `fallback_match` is on EVERY reply (an absent flag is indistinguishable from a build with no fallback); on a direct hit it is `false` and neither `matched_as` nor `fallback_kind` is emitted. `matched_as` maps each path passed in to the form that actually matched, most specific first. |
| 16 | (ANTS-4645) Every reply carries `roadmap_scanned:false` and a `scope_note` naming `roadmap_query` — the harm case is a CONFIDENT NON-ZERO answer that reads as "is this under contract?" fully answered while the ROADMAP was never in scope. |
| 17 | (ANTS-4566) A NEAR MISS is reported beside a non-empty result. INV-12's rescue fires only on a zero, so a call matching one spec on an incidental mention never learned that the spec GOVERNING the file cites it by basename. With one spec citing `src/ui/card.cpp` and another citing `card.cpp`, a query for the full path returns the first in `matched_specs` with `fallback_match:false`, AND the second in `basename_matches[]`, whose `matched_terms` names the shorter form that hit. Reported, never merged: a shorter form can collide, so promoting it would trade a silent miss for a silent wrong answer. |
| 18 | (ANTS-4566) `basename_matches` (with `basename_matches_count` and `basename_matches_hint`) is emitted ONLY when non-empty — unlike INV-15's `fallback_match`, because here absence and an empty array carry the same meaning, and a constant empty array on every reply is cost with no signal. |

INV-7's schema scrape reads the descriptor block via
`ants_test::mcpToolDescriptor` (ANTS-3720), not a fixed-byte window:
INV-9's `mode` property pushed `req.append("files")` past the old
3000-byte bound, which would have reddened the test for a reason that
had nothing to do with the wiring it locks.

## Acceptance

Exit 0 = all 18 invariants hold.

Wired as a source file in `ants_add_gui_bundle(test_claude …)` in
top-level `CMakeLists.txt`. Uses the existing
`SRC_CLAUDE_INTEGRATION_CPP_PATH`, `SRC_RC_HEADER`,
`ANTS_RC_SOURCES`, `SRC_MAINWINDOW_CPP_PATH` compile
defs already declared on `test_claude`.

## Out of scope

- Runtime accuracy of the substring match (no symbol resolution by
  design; v1 is path-substring only).
- Cross-spec dedup of invariants when two specs share an INV-N
  identifier — each spec's invariants are returned independently.
