# ANTS-1458 — Tasks chip + Task List dialog refresh latency — review loop log

Record for [`../specs/ANTS-1458-tasks-chip-task-list-dialog.md`](../specs/ANTS-1458-tasks-chip-task-list-dialog.md) § Cold-eyes loop log.

## Cold-eyes loop log

Phase 1 (instrumentation), 2026-05-21:

- **Loop 1**: Status format updated to specs.md standard;
  section "Fix" renamed to "Surface"; INV bullets reformatted with `—`
  separator and *Test:* clauses added; cold-eyes loop log added.
- **Loop 2**: No findings — clean pass.
- **Loop 3**: Status lifecycle word fixed (`phase 1 shipped`
  → `shipped`); `## Problem` numbered to `## 1. Problem`; `## Test`
  renamed and numbered to `## 4. Tests`; `Kind: investigate` →
  `Kind: implement` (also in ROADMAP); INV-2 guard description expanded
  to include empty-path case; "five columns" phrasing clarified.
- **Loop 4**: §5 RAM: "one instance per tab" →
  "one instance application-wide (re-targets via setTranscriptPath)";
  `poll_dur_us` → `poll-dur-us` in §2 investigation notes;
  §6 out-of-scope parseTranscript description: "from byte 0" qualified
  to account for 16 MiB seek-to-tail for large files; INV-1 *Test:*
  clause updated to cite both assignment sites. *(This entry was
  originally mis-labelled "Loop 6"; loops 4–5 produced no recorded
  findings, so the phase-1 log is renumbered to a continuous 1–4.)*

Phase 2 (the fix), 2026-06-17:

- **Loop 1**: §2.5 added (lost-update-race root cause + the two-part
  fix); INV-4/5/6 added for the pre-read sampling, dual-signal
  short-circuit, and symmetric `claudebgtasks` change; §4 Tests, §5
  RAM, §6 out-of-scope updated for phase 2. Cold reviewer then found:
  (HIGH) ROADMAP entry still 🚧 with the stale instrumentation-first
  body while the spec over-claimed a shipped version → flipped
  ROADMAP ✅ with a resolution note recording the real root cause,
  added a CHANGELOG `[Unreleased]` entry, and corrected the spec
  status to "implemented, queued for the next release"; (HIGH)
  `## 6. Cold-eyes review log` heading
  violated specs.md §5.7's mandated `## Cold-eyes loop log` (and broke
  `spec_query` keying) → renamed + unnumbered, trailing section
  renumbered 7→6; (MEDIUM) phase-1 Loop 4/5 numbering gap → renumbered
  as above; (MEDIUM) test assertion-label prefix noted in §4.
- **Loop 2**: (HIGH) status "shipped 0.7.97" over-claimed an
  unreleased Patron-RC preview → reworded to "implemented, queued for
  the next release"; (HIGH) cold-eyes-log "migrated to CHANGELOG"
  over-stated the action → corrected to "added a CHANGELOG
  `[Unreleased]` entry + ROADMAP resolution note"; (MEDIUM) INV-6 grep
  recipe counted the pre-read *sample* (a local, not the member) as a
  member hit → recipe corrected to four member hits. ROADMAP note
  date de-versioned to match.
- **Loop 3**: clean — only polish (Status lifecycle-token wording,
  INV-4 "stores the pre-read sample" tighten, a `**Layman:**` gloss
  added per specs.md §5.2). One MEDIUM claim (ROADMAP body carries no
  resolution note) was dismissed as a `roadmap_query` body-truncation
  artifact: the note is present at the bullet tail in `ROADMAP.md`.
  Loop stopped here per the "move on once findings are polish-only"
  directive.
