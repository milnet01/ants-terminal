<!-- ants-mcp-feedback: 2 -->
# Ants MCP Feedback — Fixture Mixed

> Format: docs/standards/mcp-feedback-files.md in the Ants Terminal repo.
> Trimmed from Ants_Projects_Hub_Website and Groundwork, 2026-11-04.

## 2026-08-05 — P01 dashboard-readability bundle (APHW-0001..0006)

Prose before the first finding, which no renderer of fields would keep.

### apply_edits uses {old,new} while the native Edit tool uses {old_string,new_string}

- **Proposed ID:** ANTS-4089
→ shipped ✅ 2026-08-11 (write-up compacted, ANTS-3443)

### file_outline (javascript) omits top-level non-function const bindings

- **Proposed ID:** ANTS-4090, ANTS-4091
→ shipped ✅ 2026-08-12 (write-up compacted, ANTS-3443)

## 2026-09-25 — groundwork 2026-09-25

### Confirmed: ANTS-5371 and the ANTS-5353 prefix pin work on Groundwork

- **What:** A dry-run append_batch returned ok:true with would_be_ids [GRND-0001, GRND-0002].
- **Impact:** None. This confirms the fixes.
- **Proposed ID:** n/a — reporter confirmation of ANTS-5371 / ANTS-5353; no action

### changelog_log has no op to amend or remove an existing entry's text

- **What:** changelog_log offers add, add_from_roadmap and add_batch, but nothing that changes a sentence.
- **Proposed ID:** _(maintainer to assign)_
- **Note:** Same gap as ANTS-5246 (filed 2026-09-19).

### A question waits on the reporter

- **What:** Which layout did you mean?
- **Proposed ID:** _(awaiting reporter — the flat one or the nested one?)_

### A finding someone hand-edited without its id line

- **What:** The slot was deleted by hand.
- **Repro:** Open the file and remove the line.

### Positive note — apply_edits is fast

It felt instant on a 2 k-line file. Nice.

```
### a heading inside a fence is not a finding
- **Proposed ID:** ANTS-1
```

## 2026-10-29

### The skeleton's ROADMAP.md is not in canonical render form

- **What:** The skeleton lacks the generated-file header.
- **Proposed ID:** ANTS-4740
- **Note:** Ants side: roadmap_migrate op:init (ANTS-4740, shipped) is the registration call.
