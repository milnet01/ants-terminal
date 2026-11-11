# ANTS-1634 — cold_eyes_partition UX polish (sparse_partition_hint + audit-infra lane + prior_loop_fixes)

**Status:** shipped (parts a + b + c all complete)
**Kind:** enhancement
**Source:** cross-session-reports-2026-05-18 (RetroDB Obs B + Obs G, Vestige
3D_Engine #15, RetroArch /cold-eyes audit-infra)
**Pull:** 22 (parts a + c, MCP cold-eyes partition fold-in) + 29 (part b,
prior_loop_fixes)
**Composes with:** [[ANTS-1619]] (contracts visibility — same engine, same
pull), [[ANTS-1631]] (probe-set widening — same engine, same pull),
[[ANTS-1412]] (`.cold-eyes/partition.json` override that part (a) points to),
[[ANTS-1508]] (lane-agnostic `cold_eyes_brief` that part (a) points to).

## 1. Problem

Three small ergonomic asks observed across four cross-session reports:

1. **(a) `sparse_partition_hint` doesn't point at escape hatches.** When
   the partition comes back sparse (≤ 1 lane in default scope), the
   existing hint suggests "check that the project root carries CLAUDE.md /
   README.md / ROADMAP.md / CHANGELOG.md" but doesn't mention the two
   working workarounds: ANTS-1508 (`cold_eyes_brief` with explicit
   `doc_paths[]`) and ANTS-1412 (`.cold-eyes/partition.json` override).
   The `next_step_hint` field has the ANTS-1508 pointer but isn't always
   read, and ANTS-1412 has no pointer anywhere in the response.

2. **(b) `cold_eyes_brief` lacks a `prior_loop_fixes` parameter.** For
   loop-2 dispatches the orchestrator hand-rolls "already-fixed in loop 1"
   blocks into each subagent prompt. *SHIPPED in pull 29 — see § 2.3.*

3. **(c) No `audit-infra` lane class.** RetroArch /cold-eyes flagged a
   `docs/private/audit/` directory carrying audit suppressions docs,
   partition overrides, and allowlist standards. Those files are
   review-worthy contracts but don't fit any existing lane —
   `cold_eyes_partition` drops them entirely.

## 2. Fix

### 2.1 Part (a): extend `sparse_partition_hint`

`remotecontrol.cpp::cmdColdEyesPartition` extends the hint string from

> "Default scope returned N lane(s). Check that the project root carries
> CLAUDE.md / README.md / ROADMAP.md / CHANGELOG.md (case-insensitive
> match) and that docs/standards/ + docs/decisions/ exist. Pass
> scope=\"contracts_only\" to confirm the contract-doc shape."

to

> "...Pass `doc_paths[]` to `cold_eyes_brief` for one-shot ad-hoc lanes
> (ANTS-1508), or commit `<projectPath>/.cold-eyes/partition.json` per
> ANTS-1412 to persist an override."

The trailing pointer is informative (no `\"`-escaped JSON literal — the
hint is a single human-readable string). `next_step_hint` stays as-is
(redundant but harmless backward-compat surface).

### 2.2 Part (c): `audit-infra` lane detection

A new private helper `scanAuditInfra(projectPath)` in
`coldeyesengine.cpp` returns `{dir, files}` — the resolved relative
audit-infra directory and the list of `*.md` files inside it. Detection
order:

```c++
static const QStringList kAuditDirs = {
    QStringLiteral("docs/audit"),
    QStringLiteral("docs/private/audit"),
    QStringLiteral("docs/internal/audit"),
};
```

Each candidate is resolved case-insensitively. First hit with **≥ 2 `*.md`
files** wins; sub-threshold directories are skipped (a stub
suppressions placeholder doesn't get promoted to lane status).

`derivePartition` calls the helper after the decisions lane and before
the spec-lane walker, so the `audit-infra` lane slots in at a stable
position in the response array. Lane shape:

- `name`: `"audit-infra"`
- `summary`: `"<resolved-dir> (audit suppressions, partition overrides,
  allowlist standards)"`
- `docPaths`: the sorted list of `*.md` files inside the resolved dir
  (one level deep — no recursive descent).

### 2.3 Part (b): `prior_loop_fixes` parameter (shipped pull 29)

`cold_eyes_brief` accepts an optional `prior_loop_fixes` array. Each
entry is a `{title, summary}` record describing one item already
closed by loop-1 of a multi-loop sweep.

**Schema** (`src/claudeintegration.cpp`, inside the
`cold_eyes_brief` registration block):

```jsonc
"prior_loop_fixes": {
  "type": "array",
  "items": {
    "type": "object",
    "properties": {
      "title":   { "type": "string" },
      "summary": { "type": "string" }
    },
    "additionalProperties": false
  }
}
```

**Handler** (`RemoteControl::cmdColdEyesBrief`) parses the array
permissively — non-object items are skipped; trim is applied to
both fields; entries with both fields empty are silently dropped.
The handler builds a `QList<ColdEyesEngine::PriorLoopFix>` and
forwards it to `assembleBriefManifest` as a 3rd arg.

**Engine** (`ColdEyesEngine::assembleBriefManifest`) renders the
section in the slot between the ANTS-1457 false-positive ledger
and the `## Instructions` block. Section header literal:

```
## Previously fixed in loop 1 (do not re-raise)
```

Bullet render shape:

| Fields present  | Rendered bullet                       |
|-----------------|---------------------------------------|
| title + summary | `- **<title>** — <summary>`           |
| title only      | `- **<title>**`                       |
| summary only    | `- <summary>`                         |
| both empty      | (silently dropped — never reaches engine) |

Empty input list omits the section entirely — strict
backward-compat with every existing caller.

**Tests added in pull 29:**

- `tests/features/cold_eyes_engine/test_cold_eyes_engine.cpp`:
  - `Ants1634PriorLoopFixesEmptyOmitsSection` (INV-7)
  - `Ants1634PriorLoopFixesRendered` (INV-8)
  - `Ants1634PriorLoopFixesBeforeInstructions` (INV-9)
  - `Ants1634PriorLoopFixesPartialFields` (INV-10)
- `tests/features/mcp_cold_eyes/test_mcp_cold_eyes.cpp`:
  - `Ants1634PriorLoopFixesSchemaDeclared` (INV-11)
  - `Ants1634PriorLoopFixesExtractedInHandler` (INV-12)

## 3. Invariants

- **INV-1** (hint pointers — ANTS-1508). When `cmdColdEyesPartition`
  emits `sparse_partition_hint`, the string contains the literal
  substring `"cold_eyes_brief"` AND `"ANTS-1508"`.
- **INV-2** (hint pointers — ANTS-1412). When `cmdColdEyesPartition`
  emits `sparse_partition_hint`, the string contains the literal
  substring `".cold-eyes/partition.json"` AND `"ANTS-1412"`.
- **INV-3** (audit-infra lane fires). A workspace with
  `docs/audit/suppressions.md` + `docs/audit/allowlist.md` (≥ 2 .md
  files) produces an `audit-infra` lane carrying both files in
  `docPaths`.
- **INV-4** (audit-infra threshold). A workspace with one
  `docs/audit/only-one.md` (< 2 .md files) does NOT produce an
  `audit-infra` lane.
- **INV-5** (audit-infra private path). A workspace with two
  `*.md` files under `docs/private/audit/` produces the lane even
  though `docs/audit/` doesn't exist.
- **INV-6** (audit-infra one-level cap). A workspace with two `*.md`
  files under `docs/audit/sub/` does NOT produce the lane (subdir
  files don't count toward the threshold or the doc list).
- **INV-7** (prior_loop_fixes — empty omits). An empty
  `priorLoopFixes` list (or the default-arg `{}`) omits the
  "Previously fixed in loop 1" section entirely. Strict
  back-compat with the two-arg call site that pre-dates pull 29.
- **INV-8** (prior_loop_fixes — render shape). A non-empty list
  emits the literal section header `"## Previously fixed in loop
  1 (do not re-raise)"` followed by one `- **<title>** —
  <summary>` bullet per entry.
- **INV-9** (prior_loop_fixes — position). The "Previously
  fixed" section appears before the `## Instructions` block so
  the reviewer reads it before they begin.
- **INV-10** (prior_loop_fixes — partial fields). Title-only
  entries render as `- **<title>**` (no trailing em-dash);
  summary-only entries render as `- <summary>`. Both-empty
  entries are dropped at the MCP layer before reaching the
  engine.
- **INV-11** (prior_loop_fixes — schema). `claudeintegration.cpp`'s
  `cold_eyes_brief` block declares `props["prior_loop_fixes"]`
  as an array of `{title, summary}` objects with
  `additionalProperties: false`.
- **INV-12** (prior_loop_fixes — handler wiring).
  `RemoteControl::cmdColdEyesBrief` reads the
  `prior_loop_fixes` request field, builds
  `QList<ColdEyesEngine::PriorLoopFix>`, and forwards it to
  `assembleBriefManifest`.

## 4. Test

Extended `tests/features/cold_eyes_engine/test_cold_eyes_engine.cpp`:

- `Ants1634AuditInfraLaneFires` (INV-3) — workspace with two `.md`
  files under `docs/audit/`. Asserts an `audit-infra` lane exists and
  carries both files.
- `Ants1634AuditInfraLaneRespectsThreshold` (INV-4) — workspace with
  one `.md` file under `docs/audit/`. Asserts no `audit-infra` lane.
- `Ants1634AuditInfraPrivatePath` (INV-5) — workspace with two `.md`
  files under `docs/private/audit/` (no `docs/audit/`). Asserts the
  lane fires from the fallback path.
- `Ants1634AuditInfraOneLevelDeep` (INV-6) — workspace with two `.md`
  files under `docs/audit/sub/`. Asserts no lane fires (one-level walk).

Extended `tests/features/mcp_cold_eyes/test_mcp_cold_eyes.cpp`:

- `Ants1634SparsePartitionHintMentionsBriefAndOverride` (INV-1, INV-2)
  — source-grep on `remotecontrol.cpp`'s sparse-partition hint block
  for the literal substrings `"cold_eyes_brief"`, `"ANTS-1508"`,
  `".cold-eyes/partition.json"`, `"ANTS-1412"`.

## 5. Out of scope

- **Sibling fold-in tools' narrative shape.** `test_audit_fold_in` /
  `cold_eyes_fold_in` / `indie_review_fold_in` have a related "structured
  vs prose" mismatch tracked at [[ANTS-1635]]. Not blocked by this pull —
  different verb, different engine module.
- **Lane-class detection beyond `audit`/`private/audit`/`internal/audit`.**
  Fork-side audit infra (`docs/fork/audit/`) is unusual; can be added
  on demand without re-spec.
- **Promoting `.audit_suppress` or `.ants_review_falsepos.jsonl` into
  the audit-infra lane.** Those are JSONL/sidecar artefacts, not
  human-review documents — different class. The new lane targets
  prose docs.

## 6. RAM / build budget

- Engine: one new helper (`scanAuditInfra` ~25 LOC) + one call site
  in `derivePartition`. Cost: 1-3 `QDir::entryList` calls per
  `derivePartition` run (cached via the engine's existing 5 s mtime
  cache at `remotecontrol.cpp:m_coldEyesCacheStampMs`).
- MCP envelope: hint string grows by ~140 bytes when sparse path
  fires (rare — by construction, only when ≤ 1 lane in default scope).
- No new state; lane plugs into the existing `result.lanes` vector.

## 7. Sources

- RetroDB cross-session report 2026-05-18 — Observation B (hint),
  Observation G (prior_loop_fixes — deferred).
- Vestige 3D_Engine cross-session report 2026-05-18 — Observation #15.
- RetroArch /cold-eyes cross-session report 2026-05-18 — audit-infra
  ask.
- [[ANTS-1508]] — lane-agnostic `cold_eyes_brief` (hint points at this).
- [[ANTS-1412]] — `.cold-eyes/partition.json` override (hint points at
  this).
