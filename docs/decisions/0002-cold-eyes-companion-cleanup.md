# ADR-0002: Cold-eyes review of the Claude-Code companion bundle

- **Status:** Proposed
- **Date:** 2026-04-30
- **Deciders:** Project lead, Claude
- **Related:** ROADMAP.md ANTS-1110, ANTS-1111, ANTS-1112, ANTS-1113,
  ANTS-1114, ANTS-1116, ANTS-1117, ANTS-1056

## Context

A second-pass cold-eyes read of the Claude-Code companion bullets
surfaced six concerns:

1. The "save tokens" framing across the bundle is **unsupported by
   measurement** — no real session has been instrumented; the 10×
   composite figure in ANTS-1116 is a guess.
2. ANTS-1116 (stateless CLI) and ANTS-1117 (running-GUI IPC) have
   the **wrong ship order** — the highest-leverage verbs (audit-run,
   roadmap-query) all run through the GUI; the standalone CLI
   duplicates parser/audit code that already lives in the GUI binary.
3. ANTS-1110 is a **catalogue, not a spec** — the 13-row table
   mixes mechanical-shape and judgment-shape skills under one bullet
   with no acceptance criteria.
4. ANTS-1114 is a **wish list** — twelve bullets, no leverage
   ranking, items like "Conversation export + search" are UX
   features mis-classified as token savers.
5. ANTS-1116's `id-allocate` subcommand is the **weakest example**
   — Claude already does the work in one Bash call (`echo
   $(($(cat .roadmap-counter)+1)) > .roadmap-counter`). Token
   savings are negligible.
6. ANTS-1056 is **superseded by ANTS-1100** for two of its six
   sub-bullets (search box, status counts) and unspecific for the
   rest. Should be retired or trimmed.

The bullets exist in ROADMAP.md; the implementation work would
collectively span months. Without scope discipline, we'll burn
weeks on bullets that don't actually shrink Claude token bills,
and ship features the user didn't ask for.

## Decision

1. **Promote ANTS-1117 ahead of ANTS-1116** as the v1 of the
   "Claude integrations" theme. Ship the GUI-IPC verbs first
   (smaller diff, leverages an existing transport, exercises the
   real token-saving paths via the audit/roadmap dialogs).
2. **Re-scope ANTS-1116 v1** to **`drift-check` only**. The
   `audit-run` subcommand is deferred to v2 because it would
   pull `auditdialog.cpp` (and its full `Qt6::Widgets` dependency
   tree) into a binary positioned as a "stateless CLI" — that's
   wrong architecturally. v2 follows once the audit logic has
   been extracted into a GUI-free module (see ANTS-1119, below).
   `id-allocate` and `test-runner` stay deferred per the original
   cold-eyes argument (low / medium leverage; defer until
   measured demand).
3. **Retire ANTS-1110 catalogue**. Replace with one-skill-at-a-
   time bullets when an individual skill reaches priority. Start
   with `using-git-worktrees` because that one is unambiguously
   mechanical.
4. **Add ANTS-1119: extract audit logic into a GUI-free module.**
   Pre-requisite for `audit-run` shipping in any non-GUI surface
   (`ants-helper`, MCP server, future CI runners). Move the
   detector orchestration, finding parsing, dedup/baseline-diff
   pipeline out of `auditdialog.cpp` into a new GUI-free
   `auditengine.{h,cpp}` module. The dialog becomes a thin
   presentation layer over the engine.
5. **Add ANTS-1120: companion-instrumentation gate.** Before any
   further companion bullet ships beyond ANTS-1117 v1 + ANTS-1116
   v1, run the side-by-side measurement to ground the
   token-saving claims. Threshold for keep/iterate/drop is
   **deferred to user input at the time the measurement lands** —
   any number set in this ADR would be ungrounded.
6. **Mark ANTS-1056 obsolete** in the roadmap (superseded by
   ANTS-1100). The two remaining additive items
   (theme-emoji filter, export-as-Markdown) only land if the
   user explicitly asks for them; they are not auto-promoted.
7. **Defer the ANTS-1114 trim to user input.** The bullet is
   acknowledged as a wishlist; this ADR does not unilaterally
   pick the surviving sub-bullets — the user does, after seeing
   ANTS-1120 measurement results.
8. **Keep ANTS-1111, ANTS-1112, ANTS-1113** as-is for now, but
   gate their implementation on ANTS-1120 measurement.
9. **Drop the "memory drift" category from ANTS-1113.** Reason:
   the category requires distinguishing "memory entry that
   *looks* stale because the file moved" from "memory entry
   that's *actually* stale because the underlying behaviour
   changed." That distinction is judgment, not mechanical
   detection. The other four ANTS-1113 categories (code drift,
   test coverage gaps, doc drift, packaging drift) all have
   crisp mechanical signatures; memory drift doesn't. Memory
   freshness is better tracked by the existing
   "before recommending from memory" rule (see CLAUDE.md global
   memory section) — verifying at use-time, not via a periodic
   sweep that would itself produce noise.

## Consequences

**Positive:**

- The first ship lands on **read-only IPC verbs** (ANTS-1117
  v1) — smallest surface, smallest blast radius if anything
  breaks.
- Token-saving claims become falsifiable via ANTS-1120 before
  more bullets ship; under-investment and over-investment both
  guarded against.
- The audit-extraction prep (ANTS-1119) unlocks future non-GUI
  audit consumers (CI runners, MCP server) — the architectural
  payoff outlives the immediate `audit-run` use case.
- ROADMAP.md shrinks by retiring ANTS-1110 catalogue and
  ANTS-1056; signal-to-noise ratio goes up.

**Negative:**

- ANTS-1116 v1 ships with **only one** subcommand
  (`drift-check`). Users expecting `audit-run` from this
  bullet will need to wait until ANTS-1119 lands first.
- The audit-extraction (ANTS-1119) is non-trivial — auditdialog.cpp
  is ~5000 lines with logic and presentation interleaved; the
  refactor will need its own audit pass.
- ROADMAP body changes for ANTS-1056/1110/1113/1114 invalidate
  any deep-anchor links readers had to those bullet bodies.
  The stable IDs themselves are preserved per
  roadmap-format.md § 3.5.1 (stable-ID immutability).

**Neutral:**

- The 9-step App-Build loop stays the gating mechanism — every
  new bullet arrives with `docs/specs/<ID>.md`, tests before
  code, audit + indie-review before close.
- `docs/specs/ANTS-1100.md` does not need to be retroactively
  written — that bullet shipped under the older convention,
  with `tests/features/roadmap_viewer_tabs/spec.md` already
  serving as the per-feature contract. The older convention is
  documented at `docs/standards/testing.md § feature-conformance`
  (per-feature `spec.md` lives under `tests/features/<name>/`,
  not `docs/specs/`); the App-Build skill formalised the
  `docs/specs/<ID>.md` location for projects scaffolded by
  `/start-app`. Ants Terminal predates that skill — both
  locations remain valid contracts; new bullets under the App-Build
  loop go to `docs/specs/`, in-tree feature tests keep their
  inline `spec.md`.

## Cold-eyes review pass 1 (2026-04-30)

This ADR was reviewed against itself before the user signed it.
Findings folded back in:

- D-1 Resolved: ANTS-1119 / ANTS-1120 IDs no longer presumed
  pre-allocated; allocation happens only after user sign-off.
- D-2 Resolved: ANTS-1114 trim explicitly deferred to user
  decision (was previously presented as if the ADR had picked).
- D-3 Resolved: memory-drift drop now has a one-paragraph
  reasoning citing the alternative coverage path.
- D-4 Resolved: "links invalidated" language clarified — IDs
  are stable; only bullet bodies change.
- D-5 Acknowledged: some Decision/Consequences overlap remains
  by design (decisions specify the *what*, consequences the
  *implications*). Not duplicated content; matched perspectives.
