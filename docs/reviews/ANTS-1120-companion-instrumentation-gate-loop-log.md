# ANTS-1120 — Companion-instrumentation gate — review loop log

Record for [`../specs/ANTS-1120-companion-instrumentation-gate.md`](../specs/ANTS-1120-companion-instrumentation-gate.md) § Cold-eyes loop log.

## Cold-eyes loop log

### Pass 1 (2026-04-30)

Findings folded back in:

- 19-1 Resolved: N≥3 replays per configuration; mean + σ
  reported; conservative ratio (1σ pessimistic on baseline,
  1σ optimistic on instrumented) added to deliverable.
- 19-2 Resolved: stub-recording method specified — captured
  from real production tooling output before measurement
  begins, not invented.
- 19-3 Resolved: 3×/5× thresholds removed from spec; user
  picks thresholds at measurement-review time.
- 19-4 Resolved: INV-6 lays out the four-way disposition
  (keep/iterate/drop/inconclusive) explicitly, with the
  "drop" case using a `**Retired:**` body annotation rather
  than the misleading 💭 status emoji.
- ID renumbering: was ANTS-1119 in the original draft; renamed
  to ANTS-1120 because ANTS-1119 now covers the audit-engine
  extraction prep work (see ADR-0002 decision 4).

### Pass 2 (2026-05-21)

No findings — clean pass.

### Pass 3 (2026-05-21)

Structural fixes applied: header converted from list-item form to
`**Status:** / **Kind:** / **Source:**` bold key-value form (specs.md
§3.2); `## Why this bullet exists` → `## 1. Problem`; `## Scope` →
`## 2. Surface`; `## Invariants` numbered and all INV bullets gained
` — ` separator + `*Test:*` clauses; `## Test plan` → `## 5. Tests`;
`## Deliverables` numbered + self-referential deliverable 1 removed;
cold-eyes log renamed from `## Cold-eyes review pass 1` to
`## Cold-eyes loop log` with sub-heading `### Pass 1`; `**Owner:**`
field removed (not in specs.md schema).

### Pass 4 (2026-05-21)

`ants-helper audit-run` CLI reference replaced with `audit_run` MCP
tool (ANTS-1351) in §2 stub-sourcing. INV-6 Drop case: inaccurate
`roadmap-format.md §3.5.1` citation replaced with inline statement
of the immutability rule (§3.5.1 covers stable IDs, not emoji
immutability). Status line updated to note 4 cold-eyes loops folded.

### Pass 5 (2026-05-21)

INV-6 Drop rationale: "stable IDs and status emojis are immutable"
replaced with "the four-state emoji set has no 'retired' state; body
annotation is the correct mechanism" (the prior wording was subtly
wrong — emojis DO transition through states). §2 Config B: note
added that ANTS-1116 + ANTS-1117 having shipped means live tools
should replace stubs when the measurement runs. Status line updated
to explicit list format. §3 Out of scope moved to §6 (after Tests)
to match the specs.md skeleton's advisory ordering (optional sections have
no mandated order; Out of scope after Tests is reasonable). Section numbers
§4→§3, §5→§4, §6→§5 updated accordingly.
