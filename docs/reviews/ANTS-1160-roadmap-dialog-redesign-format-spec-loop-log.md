# ANTS-1160 — Roadmap dialog redesign + format spec v2 — review loop log

Record for [`../specs/ANTS-1160-roadmap-dialog-redesign-format-spec.md`](../specs/ANTS-1160-roadmap-dialog-redesign-format-spec.md) § 13 — Cold-eyes review loop (this spec) and § 16 — Cold-eyes review fold-in (round 1, 2026-05-07).

## 13 — Cold-eyes review loop (this spec)

Per the project's strict spec/code review loop:

1. ✅ Self-review (placeholder scan, internal consistency, scope, ambiguity) — done inline before user review.
2. **User review** — sign-off before cold-eyes dispatch.
3. **Cold-eyes review (parallel)** — two independent reviewers:
   - General-purpose agent reviewing against the AI-response inputs in `RoadMap_Design/` (does the spec address what the user originally asked for?).
   - Project-specific agent reviewing against the existing format spec, dialog code, and Ants Terminal conventions (does the spec break anything load-bearing?).
4. Fold findings back into the spec.
5. Repeat 3–4 until both reviewers come back clean.
6. Final user sign-off.
7. Then — and only then — `/writing-plans` produces the implementation plan.

Cold-eyes findings are tracked as numbered amendments at the foot of this file with `Cold-eyes-fold-in (date) — <reviewer-tag>:` prefixes, per project convention.

## 16 — Cold-eyes review fold-in (round 1, 2026-05-07)

Two parallel reviewers per §13: one against the original prompt + 6 AI inputs (gap analysis), one against the existing codebase (drift analysis). Findings consolidated below; **must-address** and **medium-or-higher** items folded in this revision; **low / optional** items folded as one-line declines with rationale in §14 or as inline rationale paragraphs.

| Tag | Finding | Disposition |
|---|---|---|
| R1-F1+F2 | Per-kind prefix decision contradicted prompt + 6/6 AI consensus without rationale | §4.2 — added 3-reason rationale paragraph; numbering "theme" satisfied implicitly by `TASK` uniformity |
| R1-F3 | Tables for grouped same-Kind items missing | §14 — deferred with rationale (revisit after P4) |
| R1-F4+F12 | Three named footer buttons + Reset View semantics missing | §5.13 — added Refresh/Reset View/Close with explicit semantics + hotkeys |
| R1-F5 | Search bar grammar undefined | §5.14 — added `id:` / `kind:` / `status:` modifier table + plain-text fallback |
| R1-F6 | "Far Future" vs prompt's "Future" — no rationale | §5.4 — added 1-line note (existing dialog ships this label) |
| R1-F7 | Keyboard shortcuts absent | §14 — deferred to P5/P6 (Qt defaults + 4 named hotkeys ship) |
| R1-F8 | Manifest decline not justified | §6.2 — added rationale paragraph |
| R1-F9 | Inter-item dependency links missing | §14 — deferred; `dependency` Kind covers common case |
| R1-F10 | Audit trail | §14 — deferred (git blame on `ROADMAP.md` already covers it) |
| R1-F11 | Hide-headers / hide-text filter | §14 — deferred (empty-sections rule covers most cases) |
| R1-F13 | Status name vocabulary | OK as-is (pre-existing v1 convention) |
| R1-F14 | Overall progress bar | §14 — partial coverage via per-section `(N of M)` badges |
| R1-F15 | Assignee field | §14 — out of scope (solo+Claude project) |
| R2-F1 | Parser hardcodes `ANTS-` prefix; spec underspecifies surgery | §3 + §5.12 — added explicit parser-surgery enumeration with site list |
| R2-F2 | Format-spec section numbers (§3.5.5/§3.5.6/§1bis) inconsistent | §4.6/§4.7 — repositioned & clarified |
| R2-F3 | Archive regex already enforced — §6.2 reads as new | §6.2 — reworded "preserved from v1 §3.9, restated for completeness" |
| R2-F4 | `flock` could wedge GUI thread | §7.1 — added worker-thread requirement |
| R2-F5 | §9 line numbers + missed `:4004-4005` call site | §9 — tightened ranges, acknowledged existing call site |
| R2-F6 | Persistence migration from legacy single-key state | §5.9 — added migration paragraph (3 keys mapped) |
| R2-F7 | `roadmap-query` archive-only-via-dialog contract | §6.4 — clarified contract preserved; query side stays archive-free |
| R2-F8 | P3→P4 dependency not explicit; §9 vs P2 wording | §9 + §12 — fixed (P2 ships before redesign; P4 hard-depends on P3) |
| R2-F9 | Acceptance criterion 5 not feature-testable | §10 — explicitly marked as manual review gate |
| R2-F10 | Existing tests need rewrite/retire | §10 (criterion 11) + §12 P4 — explicit migration path |

A second cold-eyes round is dispatched after this fold-in to confirm the substantive amendments don't introduce new contradictions; round 2 expected to find ≤ 2 minor items if any.

**Round 2 (verification pass, 2026-05-07) — clean.** Reviewer verified all 9 spot-checks (R1-F1+F2 / R1-F4+F12 / R1-F5 / R2-F1 / R2-F2 / R2-F4 / R2-F6 / R2-F8 / R2-F9) landed at the cited sections; no new contradictions; no broken cross-references; no phantom fold-ins; §16 self-consistent. Two stylistic tightenings, both marked optional:

| Tag | Finding | Disposition |
|---|---|---|
| R2r2-F1 | §4.4 v1 example edit not called out in §12 P1 | §12 P1 — added one clause |
| R2r2-F2 | §5.13 Refresh button rationale doesn't name the inotify-drop-on-atomic-rewrite failure mode | §5.13 — added one sentence with the canonical pattern reference |

**Round 3 (full re-read of spec + documentation tree audit, 2026-05-07).** Two parallel reviewers; user observed (correctly) that round 2 only spot-checked changes and that wording-drift from the round-1 amendments could have leaked across sections. Reviewer A (full fresh re-read of spec): 5 findings, 3 substantive. Reviewer B (audit of README, CLAUDE.md, CHANGELOG, ROADMAP.md, CONTRIBUTING, PLUGINS, EXPERIMENTAL, all 5 standards docs, the 5 superseded specs): 13 findings — including 3 high-severity the prior rounds missed entirely.

| Tag | Finding | Disposition |
|---|---|---|
| R3-A1 | §9 said "Three regressions from §1" but §1 enumerates two | §9 — reworded ("Two regressions from §1, plus the Task List fix already shipped in `2f5d470`") |
| R3-A2 | §5.4 table cell missing "non-revert / non-release-bump" qualifier | §5.4 — extended cell to match v1 §3.6.3 |
| R3-A3 | Hotkey count drift (3 in §5.13 vs 4 in §14/§16) + Esc double-binding | §5.13 — added Ctrl+F row + Esc precedence rule; §5.14 — refers to §5.13 precedence |
| R3-A4 | (informational) §16 R2r2-F1 disposition accurate | no fix |
| R3-A5 | §3 Persistence row terser than §5.9 | §3 — appended "body view state, nav-split" |
| R3-B1 | `ANTS-1100.md` doesn't exist in `docs/specs/`; spec claims to supersede a missing file | §1 supersedes row — clarified "no per-feature spec file; prior art is ROADMAP bullet + 0.7.59 CHANGELOG" |
| R3-B2 | `ANTS-1125.md` doesn't exist; same shape | §1 supersedes row — clarified "no per-feature spec file; prior art is `roadmap-format.md` §3.9" |
| R3-B3 | (HIGH) `ANTS-1117` defines TWO verbs (`roadmap-query` + `tab-list`); spec only carries forward `roadmap-query` | §1 supersedes row — clarified "ANTS-1117 partially (roadmap-query only; tab-list survives unchanged)" |
| R3-B4 | `research` and `ux` Kinds are de-facto in code (ANTS-1106/1150) but absent from the standards doc | §4.5 — blessed both in v2 (15-Kind net inventory), reconciles code/standards drift |
| R3-B5 | (HIGH) `Config::roadmapStatusFilters` missing from §5.9 migration list — Custom-preset users would silently lose their tuned mask | §5.9 — added status-filter row to migration list |
| R3-B6 | `roadmap-format.md` HTML marker drift (`ants-roadmap-format-spec` vs `ants-roadmap-format`) | §12 P1 — added doc-tree edit to clarify the two markers |
| R3-B7 | `documentation.md:113-116` references non-existent phase ID scheme | §12 P1 — added doc-tree edit to fix |
| R3-B8 | `documentation.md:46-58` README section list stale | §12 P1 — optional sweep added |
| R3-B9 | (HIGH) `CLAUDE.md:117` still says "stable `[ANTS-NNNN]` IDs" | §12 P1 — added doc-tree edit to update |
| R3-B10 | `README.md` claims "7 color themes" twice; reality is 11 | §12 P1 — optional sweep added |
| R3-B11 | `README.md:969` mentions `ROADMAP_FORMAT v1` | §12 P1 — added doc-tree edit (bumps to v2 in lockstep) |
| R3-B12 | `ROADMAP.md:1,8` v1 marker | §12 P1 — added doc-tree edit (bumps to v2) |
| R3-B13 | `PLUGINS.md` self-contradicts on `ants._version` | §12 P1 — added doc-tree edit to remove contradicting parenthetical |

After round 3 fold-in, spec is considered **clean for this revision cycle**. The doc-tree alignment items in §12 P1 ship as part of the P1 commit so the broader documentation reflects v2 reality the moment the spec lands. Further amendments (e.g. P5 cross-project rollout feedback) trigger a new cold-eyes loop per §13.
