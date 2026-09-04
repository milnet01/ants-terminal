# Feature: Per-finding test-audit fold-in has a production parser

**Status:** shipped (ANTS-4445)

`TestAuditDialog`'s fold-in offers two modes. Narrative works. Per-finding
read `m_actionable`, whose only setter's only caller in the tree was a test
— so in production the array was always empty, `rawFindings` was always 0,
and `TestAuditEngine::foldIn` refused every attempt with
"Fold-in failed (…)". A shipped control that cannot succeed.

`TestAuditEngine::parseActionableFindings` supplies the missing half. It
reads the shape the dialog's own system prompt asks the reviewer for —
``- [SEV] file:line — description`` grouped under `## <Dimension>` headers —
and returns the keys `foldIn` consumes.

It is deliberately **additive** rather than an extension of `synthesize`'s
histogram walk. That walk counts three finding shapes and is pinned by
several tests; this needs the field values rather than the counts, and new
code cannot destabilise it.

## Invariants

**INV-1 — a well-formed finding parses into every field `foldIn` reads.**
`dimension`, `severity`, `file`, `line`, `summary`. A finding with no
summary is dropped rather than emitted, because `foldIn` refuses the whole
batch with `bad_actionable` on one missing headline — and refusing the batch
is the failure this feature exists to end.

**INV-2 — the dimension comes from the preceding section header,
canonicalised.** A `## 🧪 Naming (7)` header sets `naming`: the emoji and
the trailing count are not part of the value, and the case is normalised
against the known dimension list so prose styling does not fragment it. An
unrecognised header is kept as its own trimmed text rather than dropped.

**INV-3 — all three dash forms are accepted.** The prompt asks for an em
dash; a model asked for one routinely writes a hyphen or an en dash.
Rejecting a real finding over its punctuation would reproduce the silent
empty-array failure this replaces.

**INV-4 — severity is canonicalised.** `CRITICAL` → `crit`, `MEDIUM` →
`med`, matching what the histogram walk already emits, so the two halves of
the engine describe a finding the same way.

**INV-5 — a path containing a colon survives.** Only the trailing
`:<digits>` is the line number.

**INV-6 — a report with no findings yields an empty array, and the dialog
says so.** The caller must be able to distinguish "nothing to fold in" from
"the fold-in failed", which is exactly what the previous behaviour could
not do.
