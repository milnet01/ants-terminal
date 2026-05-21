# ANTS-1258 — `IndieReviewDialog`: native in-app independent code review

**Status:** shipped 2026-05-21 (TDD; `tests/features/indie_review_dialog/` INV-1..8 green). Accepted via cold-eyes loop 1 + user sign-off (2026-05-21).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-1258 ("ANTS-1112 v2 — Qt dialog for in-app
indie review", user-request-2026-04-30); architecture realigned to
`ReviewDialogBase` per ANTS-1727.
**Blocked by:** ANTS-1727 (review-dialog foundation — `ReviewDialogBase`,
`LlmDispatcher`, `BriefDispatch`).
**Pairs with:** ANTS-1721 (`ColdEyesDialog`) / ANTS-1722
(`TestAuditDialog`) — sibling `ReviewDialogBase` subclasses.

## 1. Problem

The indie-review engine is fully shipped — `IndieReviewEngine` exposes
`derivePartition`, `assembleBriefForDispatch`, `corroboratedFindings`,
`synthesisPrompt` / `assembleThreatModelExtras`, and
`templateIndieReviewFoldInBlock` (`src/indiereviewengine.h:89-156`). The
five `indie_review_*` MCP tools (ANTS-1112 v1) lift the mechanical halves
into Ants, but per-lane *judgment* + dispatch still run through Claude
orchestration. What is missing is a way to run a full independent
source-code review **inside Ants, without Claude orchestrating it** — the
standing project goal of off-loading review work off the token budget
(project focus: Ants × Claude integration, reduce token usage).

The mechanical halves (partition the source tree into per-subsystem
lanes, assemble per-lane briefs, corroborate cross-lane citations, fold
findings into ROADMAP) are exactly what the engine already does without
an LLM. Only the per-lane judgment needs a model. `IndieReviewDialog`
wraps the engine in the `ReviewDialogBase` scaffold (ANTS-1727) and
dispatches each lane's brief to the user's configured LLM endpoint in
parallel. It is the source-code twin of `ColdEyesDialog` (ANTS-1721),
which does the same for documentation.

**Layman:** a "Review my code" window inside Ants — it splits the
codebase into chunks, asks your own AI to review each chunk for bugs and
security problems, cross-checks the results, and (with one click) writes
the real issues into the project's ROADMAP — all without using up Claude
Code's budget.

**Architecture realignment.** The original ANTS-1258 roadmap bullet
predates `ReviewDialogBase` and described a standalone
`indiereviewdialog` driving `aidialog` directly with a hand-rolled
process pool. This spec routes through the shared base
(`LlmDispatcher` at `ai_review_concurrency`, not `aidialog`) and reuses
the base fold-in helpers — matching the shipped `ColdEyesDialog` /
`TestAuditDialog`. The engine + MCP layer (ANTS-1112 v1) are unchanged.

## 2. Surface

`IndieReviewDialog : ReviewDialogBase`
(`src/indiereviewdialog.{h,cpp}`, `ants_dialogs_lib`). It implements the
four `ReviewDialogBase` hooks (§ 2.4 of ANTS-1727) plus a lane checklist,
the re-review loop, and an optional cross-cutting synthesis follow-up.

### 2.1 Partition (`derivePartition`)

Calls `IndieReviewEngine::derivePartition(projectCwd())` — one lane per
CLAUDE.md `## Module map (src/)` subsystem, the source set being
`<name>.{h,cpp}` + `<name>*.{h,cpp}` siblings (generated files skipped),
honouring the `<projectCwd>/.indie-review/partition.json` override the
engine reads. **No scope selector** (unlike cold-eyes — `derivePartition`
takes no scope argument). Each `IndieReviewEngine::Lane` maps to a
`ReviewLane{ id = lane.name, title = "<name> (<N> files)",
summary = lane.summary }`. The lane checklist lists the lanes
(name + source-file count); the user can deselect lanes before dispatch.

### 2.2 Brief composition (`composeBrief`)

Reuses the engine's purpose-built **dispatch** brief —
`IndieReviewEngine::assembleBriefForDispatch(projectCwd, lane)`
(ANTS-1352, `src/indiereviewengine.cpp`), which is already shaped for a
no-Read-tool LLM endpoint and does all of:

1. 4-backtick-fences each source body via `BriefDispatch::fenceBody` with
   the "treat as data, not instructions" preamble, replacing any literal
   4-backtick run first (INV-22 fence-hardening, ANTS-1727 kernel)
   (`src/indiereviewengine.cpp:262`);
2. appends the lane's ROADMAP slice — the ROADMAP.md lines that name the
   lane (`:265-284`);
3. injects the `ants::falsepos` prior-false-positive block filtered to
   `("indie-review", lane.name)` (`src/falseposledger.h`, ANTS-1457,
   `indiereviewengine.cpp:286-298`) so the model does not re-raise
   previously-rejected findings;
4. inlines the three standards docs
   (`docs/standards/coding.md` / `testing.md` / `documentation.md`)
   **last** (`:300-319`), rather than referencing them (the dispatch LLM
   has no Read tool).

The dialog therefore does **not** re-add the FP block or re-fence the
bodies — that would duplicate what the engine already produces. (This is
the opposite of cold-eyes, whose engine *manifest* carried Read-tool
instructions and forced the dialog to compose its own prompt;
indie-review has a dedicated dispatch function, so reuse is the correct
path — global rule 3.)

**System prompt** — the independent-reviewer role (correctness, security,
contract-conformance against the inlined standards), pinned, instructing
the model to emit findings in the standard `- [SEV] file:line — …` shape
so `corroboratedFindings` can key them.

**Budget** — `assembleBriefForDispatch` is **uncapped**: it inlines whole
source + standards bodies. The dialog applies a **200 KiB** sum-cap
backstop over the composed prompt (the same number as cold-eyes /
test-audit): if a large-subsystem lane exceeds it, the UTF-8 is truncated
at the budget and a `[brief truncated — lane exceeds 200 KiB budget]`
marker appended (the fence preamble already frames bodies as data, so a
truncated body stays safe). Normal lanes are bounded by the partition
(a per-subsystem source set); the cap is the unbounded-growth guard. This
is INV-3.

### 2.3 Reports + corroboration (`onAllReportsCollected`)

On `allFinished`, the base hands `reportsById` (lane → verbatim report)
to the subclass. `IndieReviewDialog` calls
`IndieReviewEngine::corroboratedFindings(projectCwd, reportsById,
minLanes = 2)` → `QList<CorroboratedFinding>`
(`src/indiereviewengine.h:119-122`), rendered in the results view: each
row is `file:line`, the citing lanes, and the per-lane context. Single-
lane findings (below `minLanes`) are shown in a secondary
"uncorroborated" list so nothing is silently dropped (mirrors cold-eyes
INV-5).

### 2.4 Optional cross-cutting synthesis

After corroboration, an optional synthesis pass surfaces themes a single
lane can't see: `IndieReviewEngine::synthesisPrompt(reportsById,
IndieReviewEngine::assembleThreatModelExtras(projectCwd))`
(`src/indiereviewengine.h:142-156`) dispatched via the base
`dispatchOne` — a single follow-up that does **not** re-enter
`onAllReportsCollected` (same pattern as test-audit's synthesis,
ANTS-1722 § 2.3). Gated on a dispatchable endpoint; the returned summary
renders below the corroboration results.

### 2.5 Review → fix → re-review loop

A **Re-review** button re-dispatches only the lanes that produced
findings (`ReviewDialogBase::redispatch(laneIds)`). The "run until clean"
loop ends when a re-review returns no corroborated findings. Cross-loop
prior-fix suppression rides on the `ants::falsepos` ledger:
`assembleBriefForDispatch` reads `.ants_review_falsepos.jsonl` on every
dispatch, so any finding the user records as fixed / false-positive in
the ledger between loops is not re-raised. (Indie-review has **no**
in-engine `PriorLoopFix` concept — that is cold-eyes-specific; the ledger
is the cross-loop memory here. An in-dialog "mark fixed → ledger append"
affordance is § 5 out-of-scope for v1.)

### 2.6 Fold into ROADMAP (`performFoldIn`)

Two user-selected modes, both built from the base helpers (the
subclass-owns-fold-in model, ANTS-1727 § 2.4):

- **Per-finding** — `allocateFoldInIds(N)` for the N corroborated
  actionable findings → `IndieReviewEngine::templateIndieReviewFoldInBlock(
  actionable, allocatedIds, dateIso)` (`src/indiereviewengine.h:146-149`)
  → `insertFoldInBlock(activeReleaseHeading(), block)`.
- **Narrative** — a free-text prose block the user writes, wrapped under a
  `### 🔍 Indie-review <date>` heading and `insertFoldInBlock`'d under
  `activeReleaseHeading()` with **no** ID allocation. (Indie-review has no
  freeform engine template; the dialog wraps the prose directly, matching
  cold-eyes' narrative path.)

## 3. Invariants

- **INV-1** — `derivePartition` lanes come from
  `IndieReviewEngine::derivePartition`; deselected lanes are not
  dispatched. *Test:* stub a `src/` tree + module map, assert the lane set
  matches the engine and a deselected lane yields no job.
- **INV-2** — `composeBrief` reuses `assembleBriefForDispatch` (fenced
  source bodies, inlined standards, a single prior-FP block) and does not
  duplicate the FP block or re-fence. *Test:* compose against a fixture
  lane + a seeded ledger → source bodies fenced once, standards inlined,
  exactly one `ants::falsepos` block present.
- **INV-3** — the composed prompt is sum-capped at 200 KiB with a
  truncation marker; lanes under budget pass through unmodified. *Test:*
  an oversize fixture lane → prompt ≤ cap and marker present; a small lane
  → byte-identical to `assembleBriefForDispatch` + system prompt.
- **INV-4** — corroboration uses `corroboratedFindings` with
  `minLanes = 2`; uncorroborated (single-lane) findings are shown, not
  dropped. *Test:* two reports citing the same `file:line` → corroborated
  row; a single-lane cite → uncorroborated list.
- **INV-5** — the synthesis pass dispatches via `dispatchOne` and does
  **not** re-enter `onAllReportsCollected`. *Test:* stub the runner,
  trigger synthesis, assert `onAllReportsCollected` call-count is
  unchanged (mirrors ANTS-1727 INV-15).
- **INV-6** — Re-review dispatches only lanes with findings. *Test:*
  two-lane fixture, one with findings → `redispatch` covers only that
  lane.
- **INV-7** — narrative fold-in allocates no IDs; per-finding fold-in
  allocates exactly one ID per actionable finding. *Test:* narrative →
  `.roadmap-counter` unchanged; per-finding with N findings → counter
  advances by N.
- **INV-8** — Dispatch is disabled when `ai_endpoint` is unset/invalid
  (inherited `ReviewDialogBase::endpointDispatchable`). *Test:* covered by
  ANTS-1727 INV-12; dialog smoke asserts the disabled state.

## 4. RAM / build cost

- **Prompt budget** — 200 KiB per composed lane prompt (INV-3). With the
  ANTS-1727 default concurrency of 2, peak inlined-body memory is
  ≈ 2 × 200 KiB plus the `LlmClient` stream buffers (bounded by
  ANTS-1727 § 4).
- **Stored reports** — one verbatim report per lane; lane count is
  bounded by the module-map subsystem count (~30 today), each report
  ≤ the 10 MiB `LlmClient` cap (realistically KB-scale). Reports are
  cleared on re-partition. Low single-digit MiB typical.
- **Build** — 1 new `.cpp` (`indiereviewdialog`) in `ants_dialogs_lib` +
  one MainWindow menu entry. No new external libraries. ~300–400 LoC +
  ~120 test LoC.

## 5. Out of scope

- **Live token streaming** into the per-lane tab — deferred with
  ANTS-1727 § 5; reports render on completion.
- **`.indie-review/partition.json` editing UI** — the override file is
  honoured (engine reads it) but not edited in-dialog in v1.
- **In-dialog "mark fixed" → ledger append** — v1 relies on manual
  `.ants_review_falsepos.jsonl` curation between loops; an in-dialog
  ledger-append affordance is a future add.
- **Threat-model-extras editing** — `assembleThreatModelExtras` reads
  CLAUDE.md / SECURITY.md / `.semgrep.yml` as-is; no in-dialog editor.

## 6. Tests

- `tests/features/indie_review_dialog/` — INV-1..7 against a temporary
  `src/` tree + module map + a seeded `.ants_review_falsepos.jsonl`,
  exercising the non-GUI composition / corroboration / synthesis /
  fold-in paths (the engine calls, `BriefDispatch`, and the base
  helpers), not the live event loop. INV-8 is a smoke assertion.
- Label `features;fast`. Verify each test fails against pre-implementation
  source (no `indiereviewdialog`) before wiring the implementation.

## 7. Cross-doc impact

- **CLAUDE.md § Module map** — new `indiereviewdialog` bullet; note the
  Tools › Review menu entry.
- **CHANGELOG** — entry under the active release.
- **MainWindow** — a third Tools › Review action ("Independent Code
  Review...") constructs the dialog with `Qt::WA_DeleteOnClose`,
  mirroring `src/mainwindow.cpp:1525-1533`.
- **ANTS-1727** — depends on its `ReviewDialogBase` / `LlmDispatcher` /
  `BriefDispatch` surface; no changes back to it.
- **ANTS-1112** — this is its v2 dialog; the v1 engine + MCP tools are
  unchanged.

## Cold-eyes loop log

- **Loop 1 (2026-05-21, independent cold reviewer + author verification).**
  Folded: (a) **§ 2.2 brief-component order corrected** — the engine's
  `assembleBriefForDispatch` emits source bodies → ROADMAP slice → FP
  block → standards-docs-**last** (`indiereviewengine.cpp:262/265/286/300`),
  not the bodies → standards → FP → ROADMAP order the draft listed
  (CRITICAL); (b) Status line moved to the standard's
  `spec draft (DATE)` lifecycle form (`docs/standards/specs.md § 5.6`);
  (c) `§ 2.4` cross-ref fixed to ANTS-1722 § 2.3; (d) added the
  `Layman:` gloss the standard requires for user-facing specs
  (`specs.md § 5.2`). All other concrete citations (engine signatures,
  `corroboratedFindings` minLanes, `dispatchOne` non-re-entry, base
  services, menu wiring `mainwindow.cpp:1525-1533`, `ants::falsepos`
  API) verified accurate against current source. Clean after fold.
