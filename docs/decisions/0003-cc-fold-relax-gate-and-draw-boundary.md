# ADR-0003: Relax the ANTS-1120 gate for the /audit /indie-review /debt-sweep folds, and draw the ANTS-1108 ↔ ANTS-1111 / ANTS-1113 boundary

- **Status:** Accepted
- **Date:** 2026-05-13
- **Deciders:** Project lead, Claude
- **Supersedes (in part):** [ADR-0002](0002-cold-eyes-companion-cleanup.md) decision 8
- **Related:** ROADMAP.md ANTS-1108, ANTS-1111, ANTS-1112, ANTS-1113, ANTS-1120; CHANGELOG.md [0.7.87]

## Context

ADR-0002 decision 8 (2026-04-30) said: *"Keep ANTS-1111, ANTS-1112,
ANTS-1113 as-is for now, but gate their implementation on ANTS-1120
measurement."* ANTS-1120 is the companion-instrumentation gate — a
side-by-side measurement of token spend with vs without the
companion features, so the "save tokens" framing isn't asserted
without evidence.

Two facts have changed since:

1. **0.7.87 shipped on 2026-05-13** with the MCP token-reduction
   pack (ANTS-1248..1252 + 1254 — five new MCP tools, hook pack,
   provider-registry consolidation). Cumulative session saving
   quoted in the release notes: *~50–100 K tokens on a typical
   /indie-review + /audit workflow.* The figure was estimated, not
   measured by ANTS-1120 instrumentation. The release proceeded
   under the user's standing pinned focus on Claude-Code integration
   (memory: `project_focus_claude_integration`). In effect the gate
   is already informally relaxed for tools of this shape.

2. **The user explicitly asked to move on to ANTS-1111 / 1112 /
   1113** on 2026-05-13. That ask is incompatible with the
   strict reading of ADR-0002 dec 8, and the user's pinned focus
   memory carries enough force to amend the gate without further
   prompting.

Separately, a code-side reading surfaced a **boundary ambiguity**:

- **ANTS-1108** (Native App-Build runner) describes a Workflow
  panel with buttons that *invoke* `/audit`, `/debt-sweep`, `/bump`,
  `/release` natively, with fold-into-roadmap done by Ants. The
  bullet's deliverable list overlaps with ANTS-1111 piece (b) and
  ANTS-1113 by name.
- **ANTS-1111** describes folding `/audit` triage into the Project
  Audit dialog itself.
- **ANTS-1113** describes folding `/debt-sweep` into the Project
  Audit dialog as a new tab.

Without a boundary statement, an implementer can't tell which bullet
owns the "framework auto-detect", "fold-into-roadmap", "allowlist
learning loop", or "Triage with AI" surfaces. The risk is shipping
the same logic twice (Workflow panel button + AuditDialog tab) under
two IDs.

## Decision

1. **Relax ADR-0002 decision 8.** ANTS-1111, ANTS-1112, and
   ANTS-1113 may proceed without waiting for ANTS-1120 measurement.
   The 0.7.87 release establishes the precedent: when the user has
   explicitly pinned Claude-Code integration as the project's top
   priority, the burden of proof for shipping a fold flips from
   *"prove it saves tokens before shipping"* to *"ship the fold,
   measure post-hoc, retire if measurement comes back flat."*
   ANTS-1120 stays on the roadmap as `📋 research`, with success
   criteria reframed: *retire any fold whose post-ship measurement
   shows < 5 % token-spend reduction on a typical
   /indie-review + /audit workflow over a 7-day window.*

2. **ANTS-1108 owns the *runner* surface; ANTS-1111 + ANTS-1113
   own the *engine + presentation* surfaces.** Concretely:

   | Surface | Owner | Reason |
   |---------|-------|--------|
   | Workflow panel (`View → Workflow`) buttons that *trigger* an audit / debt sweep / bump / release | ANTS-1108 | Panel-level UX: one-click invocation of an existing pipeline. Doesn't know how triage works; just kicks it off. |
   | The audit triage logic itself (drop catalog, corroboration tier shift, `// audit: drop[=rule]`, since-baseline diff, fold-into-roadmap, allowlist learning) | **ANTS-1111** | Lives inside `AuditDialog` (or its engine) — the dialog already shows N findings; ANTS-1111 reduces N → K mechanically. |
   | The debt-sweep detectors + per-finding affordances (Fix / Defer / Allow) + 4 auto-fix paths | **ANTS-1113** | New "Debt Sweep" tab in `AuditDialog`; reuses the engine + RoadmapFoldIn helper from ANTS-1111. |
   | The independent-review orchestration (partition reader, brief assembler, parallel dispatch, ≥ 2-reviewer corroboration, optional synthesis, fold-into-roadmap) | **ANTS-1112** | New `indiereviewengine` (Qt::Core) + `indiereviewdialog` — no overlap with the other three. |

   ANTS-1108's Workflow-panel buttons call into the public API
   exposed by ANTS-1111 / 1112 / 1113. Neither side reimplements
   the other's work. If a feature shows up in both bullets'
   deliverable lists post-this-ADR, it lives where the engine code
   already lives (auditdialog / featurecoverage / new
   indiereviewengine) and Workflow panel just dispatches.

3. **Phase the work into three releases**, smallest residual first:

   - **0.7.88** — ANTS-1111 (residual triage features + the
     `RoadmapFoldIn` shared helper that 1112 + 1113 will reuse).
   - **0.7.89** — ANTS-1112 (the big new build: indie-review
     engine + dialog + parallel dispatch via aidialog).
   - **0.7.90** — ANTS-1113 (Debt Sweep tab; reuses RoadmapFoldIn
     and the allowlist-learning loop from 0.7.88).

   Releases 0.7.88 and 0.7.89 land independently — neither blocks
   the other once the foundation in 0.7.88 has shipped. 0.7.90
   strictly depends on 0.7.88's RoadmapFoldIn helper.

   **Outcome (2026-05-13):** 0.7.88, 0.7.89, and 0.7.90 all
   shipped on schedule; ANTS-1111, ANTS-1112, and ANTS-1113
   closed in their respective release sections of CHANGELOG.md.

4. **Acknowledge what already exists in code** (so neither spec
   re-invents shipped scaffolding):

   - `auditdialog.h` already defines `m_allowlist`,
     `loadAllowlist()`, `allowlisted(Finding &)` (per-project
     `<project>/.audit_allowlist.json`, grep-rule scope only).
   - `auditdialog.h` already defines `requestAiTriage()`,
     `requestAiTriageBatch()`, `visibleUntriagedKeys()`,
     `m_batchTriageBtn`, `onBatchTriageClicked()` — the "Triage
     visible (N)" affordance is shipped. ANTS-1111 doesn't
     re-build this; it adds the *fold-into-roadmap* button next
     to it.
   - `auditengine.h` already defines `applyFilter`,
     `parseFindings`, `capFindings`, `computeDedup`,
     `isCatastrophicRegex`, `hardenUserRegex`, `summariseSarif`
     (the last one shipped in 0.7.87 as ANTS-1254). The
     `AuditEngine` namespace is the engine seam; ANTS-1111 adds
     two new pure functions to it: `applyCorroborationShift`
     (severity-tier shift on cross-tool corroboration) and
     `templateRoadmapFoldInBlock` (string-only render of a fold-in
     block). The widened allowlist scope is implemented by moving
     the existing `AuditDialog::allowlisted()` call site, not by
     adding a new function.
   - `featurecoverage.h` already defines `extractSpecTokens`,
     `findDriftTokens`, `extractTopVersionBullets`,
     `bulletMatchesAnyTitle`, `runSpecDriftCheck`,
     `runChangelogCoverageCheck`. Two of ANTS-1113's four
     categories — test-coverage gaps and one half of doc drift
     (the `[Unreleased]` vs git diff lane) — are already half
     done here; ANTS-1113 extends rather than rebuilds.
   - `audithygiene.h` already parses `.semgrep.yml` exclude
     blocks and `pyproject.toml` ruff `S<nnn>` codes. Framework
     auto-detect (ANTS-1111 piece 3) lifts the `/audit` skill's
     framework-detection logic into this namespace.
   - The roadmap-format spec already documents `Kind: audit-fix`
     / `Kind: review-fix` / `Kind: chore` and `Source: <slug>` —
     RoadmapFoldIn just templates against them.
   - `.roadmap-counter` exists at the project root (current value
     1256). It is incremented today by hand or by the
     `/start-app` skill via `echo $(($(cat .roadmap-counter)+1))`,
     with no in-process locking. RoadmapFoldIn introduces the
     first programmatic writer; it adopts the
     `::flock(fd, LOCK_EX | LOCK_NB)` pattern already used by
     `ConfigWriteLock` in `configbackup.h:82-120` (advisory lock
     on the counter file itself). This is new code reusing an
     existing pattern, not re-binding to existing infrastructure.

5. **Defer the docs-level allowlist (`docs/audit-allowlist.md`).**
   ANTS-1111 originally referenced `docs/audit-allowlist.md` per
   ANTS-1107 (App-Build doc folder structure). ANTS-1107 has not
   shipped. For 0.7.88 the allowlist learning loop reuses the
   existing per-project `.audit_allowlist.json` file (grep-rule
   scope today; ANTS-1111 widens its scope to cover *every*
   detector's findings, not just custom grep rules). When ANTS-1107
   ships, a separate ROADMAP item will lift this per-project file
   to `docs/audit-allowlist.json` with the human-readable `.md`
   mirror. For now, scope is deliberately bounded.

## Consequences

**Positive:**

- The user's pinned focus is honoured without further negotiation
  every time a CC-fold bullet comes up.
- The 1108 ↔ 1111 / 1113 boundary becomes one paragraph
  implementers can quote when scoping work, rather than reading
  three ROADMAP bullets and inferring intent.
- Three small releases beat one mega-release: each lands on a
  green CI run, each gets its own measurement window for the
  ANTS-1120 success criteria reframe.
- Code-side fact-finding (decision 4) prevents the foundation
  spec from re-specifying scaffolding that already exists in
  `auditdialog.h` / `auditengine.h` / `featurecoverage.h` /
  `audithygiene.h`.

**Negative:**

- ANTS-1120 measurement still hasn't actually happened. If a
  fold ships and turns out to flatline token spend, the
  retire-criterion in decision 1 says we drop it — but cleanup
  cost is real (audit dialog refactor to remove a tab is
  non-trivial). Mitigation: pick the residual-first ordering
  (1111 first) so the smallest spec lands on the smallest blast
  radius.
- ADR-0002 decision 8 is amended, not entirely retracted — the
  *spirit* (don't ship token-savers without evidence) survives
  via decision 1's reframed retire-criterion. Readers
  cross-referencing ADR-0002 should now follow the chain to
  this ADR.

**Neutral:**

- 9-step App-Build loop remains the per-bullet gate (spec → tests
  → audit → indie-review → close). Each of the three CC-fold
  bullets still gets its own `docs/specs/<ID>.md` and its own
  cold-eyes review pass.
- ANTS-1110 retired-catalogue status is unchanged.
- ANTS-1116 / ANTS-1117 ship-order decisions in ADR-0002 (decisions
  1–2) are unchanged — both have shipped under their stated v1
  scope.

## Cold-eyes review pass

This ADR was self-reviewed before sign-off, then folded back into
the cold-eyes loop on `docs/specs/ANTS-1111.md` (2026-05-13).

### Findings raised (self-review)

- D-1: ANTS-1120 was at risk of silent retirement; the original
  draft did not specify what becomes of its measurement mandate.
- D-2: Boundary statement was bullet-bullet ("ANTS-1108 vs
  ANTS-1111"), making implementers diff full ROADMAP bodies to
  find which bullet owns a given feature.
- D-3: The "what already exists" inventory was at risk of
  re-inventing scaffolding (the original draft did not enumerate
  shipped helpers, opening the door to a parallel allowlist
  implementation).
- D-4: The docs-level allowlist (`docs/audit-allowlist.md`)
  reference would have required a non-shipped dependency
  (ANTS-1107) without a named bypass.

### Resolutions

- D-1 Resolved: ANTS-1120 scope reframed in decision 1 (post-hoc
  measurement instead of pre-ship gate; concrete retire criterion
  of < 5 % over 7 days).
- D-2 Resolved: decision 2 replaced the bullet-bullet wording
  with a per-surface table (Workflow panel vs triage logic vs
  debt-sweep detectors vs indie-review orchestration) so future
  implementers have a per-feature lookup.
- D-3 Resolved: decision 4 enumerates pre-existing scaffolding
  before any spec is written. Counter-checked against
  `auditdialog.h`, `auditengine.h`, `featurecoverage.h`,
  `audithygiene.h` on 2026-05-13.
- D-4 Resolved: decision 5 declares the docs-level allowlist
  out-of-scope for this ADR's bullets, with a named follow-up
  trigger (ANTS-1107 ship). No silent dependency on a
  non-shipped item.

### Findings raised (cold-eyes loop on the paired spec, 2026-05-13)

- HIGH: dec 4 listed `severityShiftForCorroboration` and
  `applyAllowlistDrops` as new pure functions; the spec uses
  `applyCorroborationShift` and never defines
  `applyAllowlistDrops` (the actual change is to widen the
  existing call site).
- LOW: dec 4 said "RoadmapFoldIn calls into the existing flock
  mechanism" — there is no existing flock mechanism on
  `.roadmap-counter`; the helper introduces it, adopting the
  `configbackup.h` pattern.

Both resolved in-place by amending decision 4. No further
substantive findings — the ADR's body remains as drafted.
