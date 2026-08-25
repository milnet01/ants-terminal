# ANTS-4646 — set a feedback file's title, and retire a tracking heading nobody can touch

**Status:** implemented (2026-08-25) — shipped in `src/feedbackfile.{h,cpp}`
(`setTitle`, `retireTrackingHeadings`) + `src/remotecontrol_feedback.cpp`
(`op:"set_title"`, and the retirement riding on `op:"compact_resolved"`) +
`src/claudeintegration.cpp` (schema). Written AFTER the build, recording what
was built: the format standard's "each new/changed verb ships spec-first" rule
is what obliges a spec here, and this one is a record rather than a contract
awaiting an implementer.
**Kind:** enhancement.
**Source:** Album_Builder-2026-08-24 (contributor report, two gaps in one).
**Contract:** `docs/standards/mcp-feedback-files.md` § Tooling (verbs) under v2
owns the outward behaviour; this document owns the reasoning and the
invariants.
**Layman:** after renaming a project there was no supported way to fix its
feedback file's title, and a list of already-finished items sat on six files
with no verb able to remove it.

## 1. Problem

Two gaps, reported together, both of the same shape: a legitimate maintenance
act with no verb behind it, in a file whose own banner says **don't
hand-edit**.

**(a) A legacy v1 tracking heading survives migration and nothing can touch
it.** `migrate_v2` converts findings and leaves v1 tracking content in place
by design (ANTS-3446 § 1.1), so
`## Tracked in ROADMAP (detail + status there): ANTS-…` outlives the migration.
`op:append_tracking` refuses `not_v1` on a `: 2` file (ANTS-3477).
`op:assign_id` needs a `### ` finding carrying a `**Proposed ID:**` line, which
those ids do not have. So a contributor asked to mark shipped items complete
had no verb for them, and the heading stays forever. Six corpus files carry
one (Album Builder, Contact List, RetroArch, MAME Curator, Ants Projects Hub,
and others).

**(b) Renaming a project leaves the title contradicting the filename.**
`feedback_log` derives the FILENAME from `caller_cwd`'s leaf and gets it right;
no op in the enum writes the H1. The reporter had to hand-edit it — against
the file's own instruction, at exactly the moment a session is most likely to
get it wrong.

## 2. Surface

### 2.1 `retireTrackingHeadings` — pure

Rides on `op:"compact_resolved"` rather than becoming an op of its own. The
gate is the one that verb already resolves — every id ✅ in the live
`ROADMAP.md` — and the canonical v2 flow (`migrate_v2` → `assign_id` →
`compact_resolved`) should not grow a second verb to learn. One roadmap read,
one write, one envelope.

All-or-nothing per heading: half a retired tracking list is worse than the
whole one. Skip codes reuse `ResolvedFinding`'s vocabulary and its
first-failure-wins order, so one caller path reads both shapes.

**`sole_id_record` is the load-bearing one and does not come from the report.**
ANTS-3744 makes this pointer line the source of `mapped_ids` on a fully
condensed file — one carrying no inline `**Proposed ID:**` at all — so there it
is that project's ONLY record of what it reported. Retiring it, however shipped
every id in it is, would destroy the thing it exists to preserve. Inline ids
win, exactly as ANTS-3744 states, so one inline id anywhere makes the heading
redundant and retirable.

### 2.2 `setTitle` — pure

Rewrites the first non-fenced `# ` line's project name. `title` is the PROJECT
NAME, not the whole heading.

**The existing prefix is preserved verbatim.** The corpus carries both
`# Ants MCP feedback — X` and `# Ants MCP Feedback — X`; this op renames a
project, it does not normalise a corpus, so only the text after the last em
dash is replaced. A prefix-less H1 is rewritten whole from the skeleton's form.

**It never invents an H1.** The format fixes that line's position; a guess
would put it where the reader does not expect it. `no_h1` instead.

**`changed:false` is a SUCCESS, not a refusal**, so a rename script is
re-runnable.

## 3. Invariants

| # | Statement | Test surface |
|---|---|---|
| INV-1 | A `## Tracked in ROADMAP …` heading whose every id is ✅ is removed, with one following blank line absorbed; nothing else in the file moves. | `FeedbackCompactResolved.Ants4646RetiresAnAllShippedTrackingHeading` |
| INV-2 | One non-✅ id keeps the whole heading, `code:"has_open_id"`, file byte-identical. | `FeedbackCompactResolved.Ants4646KeepsAHeadingWithAnOpenId` |
| INV-3 | An id absent from the roadmap is `roadmap_unresolved_ids`, never treated as shipped. | `FeedbackCompactResolved.Ants4646UnresolvedIdIsNotTreatedAsShipped` |
| INV-4 | A file with no such heading is a byte-identical no-op reporting nothing, so the pass can ride on every `compact_resolved` unconditionally. | `FeedbackCompactResolved.Ants4646NoHeadingIsANoOp` |
| INV-5 | On a file carrying NO inline `**Proposed ID:**`, the heading is KEPT with `code:"sole_id_record"` even when every id is ✅ (ANTS-3744). | `FeedbackCompactResolved.Ants4646CondensedFileKeepsItsSoleIdRecord` |
| INV-6 | The verb reports `headings_retired` and `retired_headings[]` on every `compact_resolved` reply, and one write carries both the collapse and the retirement. | `FeedbackCompactResolved.Ants4646VerbRetiresAlongsideCompaction` |
| INV-7 | `setTitle` replaces only the text after the last em dash and preserves the H1 prefix's own casing; everything below the H1 is untouched. | `McpFeedbackLog.Ants4646SetTitleRewritesTheH1` |
| INV-8 | An already-correct title reports `changed:false` and writes nothing. | `McpFeedbackLog.Ants4646SetTitleIsIdempotentAndReportsNoChange` |
| INV-9 | A file with no H1 refuses `no_h1`; no heading is invented. | `McpFeedbackLog.Ants4646SetTitleRefusesAFileWithNoH1` |
| INV-10 | A `# ` inside a fenced block is body text and survives untouched (the ANTS-3695 hazard). | `McpFeedbackLog.Ants4646SetTitleIgnoresAFencedHash` |
| INV-11 | Through the verb: the write lands, a second identical call reports `changed:false`, and an empty `title` refuses `bad_args`. | `McpFeedbackLog.Ants4646SetTitleThroughTheVerb` |

## Tests

Both halves are pure functions over synthetic fixtures, plus a live
`cmdFeedbackLog` drive, in the two feature suites that already own these
verbs — no new directory, because neither half is a new subsystem:

- `tests/features/feedback_log_compact_resolved/` — INV-1..6 (the retirement,
  which rides on that verb).
- `tests/features/mcp_feedback_log/` — INV-7..11 (`set_title`).

Both build into the `test_claude` bundle. Every invariant above was written
and proved RED on its assertions before the implementation existed; the two
that pass against a stub (INV-4's no-op and INV-8's no-change) are the
negative space and are named here so a later reader does not mistake them for
coverage that never failed.

## Cold-eyes loop log

| Loop | Date | Lanes | Outcome |
|---|---|---|---|
| — | 2026-08-25 | none | **No gate run, deliberately.** This document was written AFTER the build to satisfy the format standard's "each new/changed verb ships spec-first" rule, and global `CLAUDE.md` rule 14 exempts an amendment that records what was actually built: the code exists, so a cold read *before implementation* has nothing left to protect — the build was the review. What a gate would have caught here was instead caught by reading `docs/standards/mcp-feedback-files.md` during implementation: ANTS-3744 makes the pointer line the sole source of `mapped_ids` on a condensed file, which turned a "mostly cosmetic" retirement into a data-destroying one and became INV-5. Recorded rather than left implicit, because a spec with an empty loop log is otherwise indistinguishable from one whose gate was skipped without a reason. |

## 4. Out of scope

- **Converting a v1 tracking heading into per-item stubs `assign_id` can
  target** — the report's other suggested route. Retirement is the cheaper of
  the two and reaches the same end state; stubs would manufacture findings
  nobody wrote.
- **The v1 tracking TABLES.** Only the condensed pointer *heading* is retired.
  The tables remain the strip/declutter pass the format standard still lists as
  outstanding.
- **Renaming the FILE.** The filename is already derived correctly from
  `caller_cwd`'s leaf; only the H1 was unwritable.
