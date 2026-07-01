# Feature: read_region `call_sequence` — integration brief (ANTS-2157)

Test contract for the subsystem-integration-brief capability (Vestige
Obs #17): the recurring "where do I hook into this pipeline?" question,
which the reporter reconstructed by grepping a renderer for stage lines
then reading a 140-line region to find the insertion point + the member
accessors a new stage needs. `subsystem` / `file_outline` return
STRUCTURE, not SEQUENCE.

Implemented reuse-first (CLAUDE.md §3) as a `call_sequence` option on
`read_region`'s symbol-body mode — which already isolates one
definition's body — rather than a new verb. When set, the response also
carries:

- `call_sequence` — the ordered call-expressions inside the region (the
  pipeline STAGES), each `{line, callee}`; the line is the insertion
  point. The signature line is skipped (the function itself isn't a
  stage); scanning stops at the symbol's end.
- `accessors` — the distinct `m_` members + `get`/`is`/`has` getters
  referenced (the helpers a new stage usually needs), sorted.

A heuristic line scan (same Karpathy-§2 bet as file_outline), reusing the
region `read_region` already slices.

## Invariants

- **INV-1** — calls are listed in source order, the signature is not a
  stage, scanning stops at the symbol's end, and accessors captures the
  `m_` members + getters referenced.
- **INV-2** — opt-in: absent the flag, no `call_sequence`/`accessors`
  fields (back-compat).
- **INV-3** — wiring: `cmdReadRegion` threads the option into
  `ReadRegion::Options`; the schema advertises it.
- **INV-4 (ANTS-3379)** — comment prose (`//` line and `/* … */` block,
  the latter tracked across region lines) and capitalised type
  constructions / functional casts / macros (`Engine(...)`, `QString(...)`,
  `Q_ASSERT(...)`) are NOT listed as callees; real lowerCamelCase calls
  still are. **Residual:** a lowercase type ctor (`vec3(...)`) is lexically
  indistinguishable from a call and is left in — the comment strip removes
  the comment-sourced variant, and leading-uppercase covers the common
  type case; a general lowercase-type filter would need type knowledge the
  heuristic scan deliberately lacks.

## Out of scope (v1)

Cross-function / multi-method pipeline mapping (a pipeline spread across
several methods) — v1 is scoped to one driver function's body, the
reporter's concrete case. A dedicated standalone verb (vs the
read_region option) is deferred unless the option proves insufficient.

## Pre-fix check

Against pre-fix code `Options::callSequence`, the extract fields, the
handler thread-through, and the schema property are all absent → INV-1/2
behavioural assertions and INV-3 greps fail. Verified before wiring.

Label: `features;fast`.
