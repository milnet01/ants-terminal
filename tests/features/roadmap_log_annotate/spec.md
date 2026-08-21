# ANTS-1717 / ANTS-1793 — roadmap_log note append (annotate + flip-note)

## Background

`roadmap_log op:"flip"` only changes a bullet's status emoji (and
injects a caret anchor on first touch). Closing a deferred item almost
always also wants a one-line resolution note appended to the bullet
body ("Resolved (date): …"), and recording partial progress on a
*still-open* item wants the same note with NO status change. Before
this change both fell back to a hand `Edit` of ROADMAP.md, defeating
the MCP write path (ANTS-1793 hit during indie-review #5 remediation;
ANTS-1717 hit during the RetroArch Bundle 76 cross-session report).

Two ops, one shared primitive:

- **ANTS-1793** — `op:"flip"` gains an optional `note`: flip the status
  AND append the resolution line in one call.
- **ANTS-1717** — new `op:"annotate"`: append `note` to a located
  bullet, status untouched.

Both reuse the flip handler's locator + atomic-write machinery and a
shared `appendBodyNote` helper.

## Invariants

### INV-1 — shared body-note helper

`appendBodyNote(lines, headlineLine, note)` inserts `note` as indented
continuation line(s) at the END of the bullet's body — after the run of
indented continuation lines that follows the headline, before the
terminating blank line / column-0 line / EOF. It inherits the bullet's
existing continuation indent when present, else a 2-space hang
(matching op:"append"'s body). Format-agnostic (ants-v1 + GFM).

### INV-2 — op:"annotate" appends without flipping

`op:"annotate"` with a locator + `note` appends the note and leaves the
status emoji unchanged. The success envelope carries `op:"annotate"`,
`from_status == to_status`, `note_appended:true`, and `note_line`.

### INV-3 — op:"annotate" requires a non-empty note

`op:"annotate"` without `note` (or a note that scrubs to empty) refuses
with `code:"missing_field"`.

### INV-4 — op:"annotate" rejects to_status

Passing `to_status` under `op:"annotate"` refuses with
`code:"bad_op_combo"` — annotate never changes status.

### INV-5 — op:"flip" + note flips AND appends

`op:"flip"` with `to_status` + `note` changes the status emoji AND
appends the note in one atomic write. Envelope carries `op:"flip"`,
the new `to_status`, and `note_appended:true`.

### INV-6 — op:"flip" without note unchanged (back-compat)

`op:"flip"` with no `note` behaves exactly as before — no
`note_appended` field, status flipped, byte-for-byte the prior shape.

### INV-7 — note lands at body end, not after the headline

For a bullet that already has body lines, the note is inserted after
the last body line (so metadata lines like `Source:` keep their order
relative to the headline and the note reads as the latest addition).

The body is the bullet's WHOLE indented continuation run, blank lines
included — it ends at the next column-0 line or EOF, not at the first
blank line (ANTS-3696). A multi-paragraph bullet is exactly the shape
this matters on: stopping at the first blank line put the note after
paragraph one, stranding the rest of the body and the `Kind:`/`Source:`
trailer below the resolution, so the item read as resolved and then
went on arguing for itself. Same span `amendBodyExact` walks.

### INV-8 — leaked-tool-XML scrub + fenced refusal

`note` is scrubbed of leaked `<parameter …>` / `</invoke>` wrappers via
the shared `rcScrubLeakedToolXml` helper (same as op:"append"'s body); a
bullet inside a fenced code block refuses with
`code:"anchor_unsafe_context"`.

A bare XML-ish tag at the very START or END of the note is stripped too
(ANTS-3703) — a truncated wrapper is the commonest shape a leak takes,
and a stray `</note>` reached ROADMAP.md verbatim before this. Edges
only: markup mid-sentence is prose and is left alone.

Peeling that closing tag EXPOSES the opening half of the same pair, alone
on its line with its scalar value, and the ANTS-3703 loop stopped there —
the trailing token was then `true`, not a tag, so `<compact>true` reached
ROADMAP.md under an `ok:true` envelope. ANTS-4609 strips an opening tag as
the same token class. What bounds it is the shape of the rest of the line:
only a **lone scalar** after the tag marks a leaked parameter, so
`<div> wrapper is the culprit` is prose and survives.

### INV-11 — `bytes_written` is the delta, `file_bytes` the whole file

Every roadmap_log op reports `bytes_written` as the bytes it ADDED
(ANTS-3702). The whole-file-rewrite ops — flip, flip_batch, amend_body
and the pass-heading variants — used to report the size of the rewritten
file, so six short notes came back as a 459 KB write. The full size is
still available as `file_bytes`.

### INV-9 — MCP descriptor + schema advertise the surface

The `roadmap_log` descriptor names `op:"annotate"` and the `note`
param; the schema `op` enum includes `"annotate"` and the properties
include `note`.

### INV-10 — retried note append is idempotent (ANTS-3440)

A flip/annotate can commit its ROADMAP.md write but still surface to the
caller as a *client-side* transport timeout (mcp-bridge read-timeout);
the caller then retries. The status flip is naturally idempotent
(emoji→same emoji), but a second `appendBodyNote` would duplicate the
resolution note. `appendBodyNote` therefore dedups: when the exact
rendered note already occupies the trailing body lines immediately
before the insertion point, the append is skipped. The compare is
byte-exact and bounded to the located bullet's body, so a legitimately
different note (new date, reworded) never false-dedups. The retry's
success envelope carries `note_already_present:true` and
`note_appended:false`; the note appears in the file exactly once. This
holds for every note path — flip (GFM + ants-v1), annotate, and
flip_batch — because they share the one `appendBodyNote` primitive.

## Test plan

INV-1, INV-2, INV-5, INV-6, INV-7 are verified behaviourally by driving
`cmdRoadmapLogFlipForTest` against a seeded temp ROADMAP and asserting
the resulting file content + envelope. INV-3, INV-4 are negative-path
envelope assertions. INV-8 (scrub) is asserted behaviourally; the
fenced refusal + INV-9 descriptor/schema are source-grep assertions.
INV-10 is behavioural: two identical flips against a seeded ROADMAP,
asserting the note appears once and the retry envelope flags
`note_already_present`.
