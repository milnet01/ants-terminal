# ANTS-2049 — End-to-end harness to drive Ants Terminal as a user and observe the result — review loop log

Record for [`../specs/ANTS-2049-end-end-harness-drive-ants.md`](../specs/ANTS-2049-end-end-harness-drive-ants.md) § Cold-eyes loop log.

## Cold-eyes loop log

Two independent cold reviewers per loop, briefed against the spec + cited code,
no prior-loop findings carried forward. Every finding verified against live
source before fixing; unverified claims dismissed with the contradicting grep.

- **Loop 1** — 2 CRITICAL/HIGH + MED/LOW. Fixed: the RC `start()` gate is at the
  `mainwindow.cpp:1100-1102` call-site (default-false `remoteControlEnabled()`),
  not in `start()` — pinned the `--e2e` → ctor-param → `setE2eMode` wiring;
  inject targeting must avoid offscreen-unreliable `activeWindow()`; corrected
  the `runClient` exit-code contract; honest framing of the `--remote` surface;
  defined the `ANTS_E2E_ARTIFACT_DIR` validation root; flagged the two new error
  codes for taxonomy registration; line-number + `$TMPDIR` drift.
- **Loop 2** — 2 CRITICAL + HIGH/MED. Fixed: readiness poll + observers used
  MCP-only `current_state`/`last_selection` (no socket branch) → switched to
  socket-dispatchable `get-text`/`tab-list`/`ls` + verb-reply envelopes;
  underscore vs hyphenated verb keys → all on-wire keys + new verbs hyphenated;
  INV-2 grep made provenance-based (not a vacuous literal match); artifact-dir
  guard precedes `validatePath`; `resize-window` returns its post-clamp size;
  second-instance isolation + `launch_e2e` re-entrancy; crash-mid-run handling;
  case-3 dialog open + visibility wait.
- **Loop 3** — 1 CRITICAL + HIGH/MED. Fixed: the `validatePath` empty-root
  rationale was **inverted** — code fails closed (`pathvalidation.cpp:84-89`,
  ANTS-1805) — so the dir guard is for message clarity, not safety; missing/empty
  `path` arg needs its own `bad_args` guard; `--remote-json` must extract `cmd`
  (runClient overwrites it); ctor/`:1082` same-body clarification + New-Window
  note; `.h` decl line refs; INV-4 scope; taxonomy scoping note.
- **Loop 4** — 0 CRITICAL/HIGH; MED/LOW. Fixed: `:918-921` is a dialog detector
  not a target resolver (`e2eResolveTarget` is new code adding the return step);
  `inject-key` post target specified; `runClient` line `:8142`; ctor
  mid-signature insertion safety; case-3 dialog opener concretized.
- **Loop 5** — 0 CRITICAL/HIGH; MED/LOW. Fixed: dropped a socket-lock
  non-sequitur; reconciled "`setE2eMode` called every ctor, true only via
  `--e2e`"; INV-9 now asserts socket-targeting distinctly from the echo;
  **case 2 redesigned** to prove `inject-key` independently (sentinel char into
  the grid → `get-text`), not via an `inject-click` fallback; cross-ref labels;
  exact case count; case-3 offscreen skip-not-fail.
- **Loop 6 (confirming)** — 0 CRITICAL/HIGH. One verified MED: `dispatch()` is an
  `if`-chain, not a `switch` (fixed in §1.1/§2.3); range anchor tidied;
  focus-widget hop clarified. Two LOW line-claims (`defaultSocketPath:1244`,
  `showRoadmapDialog:6263`) dismissed — greps confirm `:1204` / `:6261`. Both
  reviewers: "a mature, well-cited spec; no logic or contract defects." Clean.
- **Implementation phase (Task 1, 2026-06-10)** — runtime observation caught
  what static review could not: a test instance with only `XDG_CONFIG_HOME`
  isolated **restored the user's saved tabs** from `XDG_DATA_HOME`
  (`sessionmanager.cpp:22`, `GenericDataLocation`). INV-3 + §2.2 + §2.5 updated
  to isolate all three XDG dirs (config + data + cache). An early proof of the
  harness's value — the gap was invisible to six doc-review loops.
