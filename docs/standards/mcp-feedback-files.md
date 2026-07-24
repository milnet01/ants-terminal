<!-- ants-mcp-feedback-format-spec: 2 -->
# Ants MCP cross-session feedback-file format

Format spec for the `*_Ants_MCP_Feedback.md` files that other Claude
Code sessions use to report problems and ideas about the Ants MCP server
back to the Ants Terminal maintainer session. The corpus is whatever
matches the `*_Ants_MCP_Feedback.md` glob at the shared root — one file per
contributing project; the glob, not any list here, is authoritative. Each
filename is the project's **directory leaf** plus the suffix (the
canonical-basename rule below); the human brand is informational only and
never forms the filename. A snapshot (correct at time of writing):

| Project dir (`…/<leaf>/`) | Feedback filename | Brand (informational) |
|---|---|---|
| `3D_Engine` | `3D_Engine_Ants_MCP_Feedback.md` | Vestige |
| `MAME_Curator` | `MAME_Curator_Ants_MCP_Feedback.md` | MAME Curator |
| `Music_Production` | `Music_Production_Ants_MCP_Feedback.md` | Album Builder |
| `RetroArch` | `RetroArch_Ants_MCP_Feedback.md` | RetroArch |
| `RetroDB` | `RetroDB_Ants_MCP_Feedback.md` | RetroDB |
| `Ants_Projects_Hub_Website` | `Ants_Projects_Hub_Website_Ants_MCP_Feedback.md` | Ants Projects Hub |
| `Ants_Terminal` | `Ants_Terminal_Ants_MCP_Feedback.md` | Ants Terminal |
| `Contact_List` | `Contact_List_Ants_MCP_Feedback.md` | Contact List |
| `finbreak` | `finbreak_Ants_MCP_Feedback.md` | Fin Break |
| `DOOM_Ants` | `DOOM_Ants_MCP_Feedback.md` ⚠ leaf-mismatch | DOOM |

`DOOM_Ants/` is the sole exception: its file uses the brand token `DOOM`,
not the dir leaf `DOOM_Ants`, so a session there must pass an explicit
`path` (see the canonical-basename rule below).

This is a *data-file format* spec (what a conforming feedback file must
look like so tooling can parse it), like
[roadmap-format.md](roadmap-format.md) — not a practitioner authoring
guideline. It carries a `-spec` version marker (`ants-mcp-feedback-format-spec`,
line 1) for that reason — distinct from the `ants-mcp-feedback` marker the
conforming *data files* carry (§ "File skeleton").

The load-bearing reason it exists: the maintainer session reviews these
files by reading only the **un-triaged tail** (everything a contributor
appended since the last maintainer review). A regular format lets a tool
return just that delta instead of the maintainer re-reading the whole
file every week (the largest, 3D_Engine, runs to several thousand lines) — the
`feedback_query` MCP verb, ANTS-1961.

## Format version 2 (2026-07-04): status lives in the ROADMAP, not here

**v2 principle — the feedback file stores only what it uniquely owns: the
contributor's write-up and the roadmap ID it became. It never persists a
finding's *status*; status is resolved live from `ROADMAP.md` on read.**

> **Implementation status (2026-07-10): v2 is live and the corpus is
> migrated.** The v2 verb chain — `compact_resolved` (ANTS-3443), `migrate_v2`
> (ANTS-3446), `assign_id` (ANTS-3447), and the marker-aware v2 delta reader
> (ANTS-3448) — is built, shipped, and running in the live MCP server
> (`FeedbackFile::compactResolved` / `migrateV2` / `assignId` + the fence-aware
> `### `-block enumerator `enumerateFindingBlocks` + the marker-aware
> `FeedbackFile::parse()`). All ten corpus files were migrated to `: 2` on
> 2026-07-10 with `migrate_v2 backfill_from_tracking` (ANTS-3474, which carried
> each finding's id in from its own v1 tracking table) and then compacted
> (`compact_resolved` collapsed 60 shipped findings, ~69.5 KB reclaimed). So
> **every corpus file now reads under the v2 rule** — the delta is the findings
> whose `**Proposed ID:**` is still unfilled. `FeedbackFile::skeleton()` now
> births a *new* file as `: 2` with the v2 banner (**ANTS-3476**), and
> `op:append_tracking` is refused (`not_v1`) on a `: 2` file, pointing at
> `assign_id` (**ANTS-3477**). Remaining follow-up: the retained v1 tracking
> tables are pending a strip/declutter pass now that the marker-aware reader no
> longer gates a `: 2` file's delta on them.

v1 (the legacy corpus) recorded triage as an appended maintainer *tracking
table* (`finding → ID → status`) every review cycle. That duplicated the
roadmap's status into the file, where it (a) went **stale** the moment the
roadmap moved on — real corpus tables froze their status at triage time
(`3D_Engine…` rows read `✅ + 📋` as of 2026-05-19 for ids long since shipped)
— and (b) **proliferated**: the same id accrued a fresh row in table after
table, so the largest file reached 267 KB. `compact_shipped` (ANTS-3421) and
`prune_tracking` (ANTS-3442) existed only to fight that self-inflicted bloat.

v2 stops **writing** tracking tables; status is derived from `ROADMAP.md`
instead (§"Tooling"). Migration (`migrate_v2`, ANTS-3446) leaves any existing v1
tables **in place** — it converts a file to `: 2` without moving or collapsing
them (§"Migration from v1"); the byte shrink comes later from the now-shipped
`compact_resolved`:

- **Triage is inline.** The maintainer records `finding → id` by filling the
  finding's own `**Proposed ID:**` slot (already part of the finding template,
  §"Contributor block") with the assigned `ANTS-NNNN`, or a closure marker
  (`n/a — <reason>`). No separate table to write, migrate, or dedup.
- **Status is derived, never stored.** A reader resolves each assigned id's
  current status from `ROADMAP.md`; a reader's status view is always current,
  never persisted. (`feedback_query` renders this at-a-glance as
  `mapped_id_status` — [{id, status}] resolved live from `ROADMAP.md`, ANTS-3478,
  plus an optional `shipped_date` on ✅ ids per ANTS-3504; see §"Tooling" and
  §"Stale-binary self-check".) The
  resolve reuses the cached `roadmap_query` path (100 ms-TTL parsed-bullet
  cache, ANTS-1117), so a render costs one roadmap parse, not one per id, and
  adds no new persistent state.
- **The un-triaged tail is "findings with no id yet."** A finding whose
  `**Proposed ID:**` line is still the blank placeholder is un-triaged; that
  set is the maintainer's work-list (§"The un-triaged delta").
- **Compaction is roadmap-driven.** Once an assigned id is ✅ in the roadmap,
  the finding's write-up collapses to a `→ shipped ✅ <date> (write-up compacted,
  ANTS-3443)` stub that keeps the `**Proposed ID:**` line above it (the id lives
  in that retained line, not the breadcrumb — see §"Maintainer compaction" for
  the canonical form; `compact_resolved`). The `<date>` is the fix's ship-date
  (ANTS-3504), lifted from the id's `Resolved` roadmap line (parens optional), so
  a contributor on an old binary can compare it against their `session_orient`
  `server_build.build_date` and relaunch rather than re-report an already-shipped
  fix (§"Stale-binary self-check"). Nothing goes stale because nothing is stored
  to go stale.

**Marker + back-compat.** A v2 file carries `<!-- ants-mcp-feedback: 2 -->`.
Tooling MUST still read v1 files: the delta parser (§"The un-triaged delta")
recognises **both** the v1 "after the last maintainer tracking table" rule and
the v2 "unfilled `Proposed ID`" rule, and a v1 file is migrated to v2 **lazily**,
never in a flag-day rewrite (§"Migration from v1").

## Two roles, one file

| Role | Who | Writes |
|------|-----|--------|
| **Contributor** | Any non-Ants CC session that uses the Ants MCP server | Appends dated finding blocks at the end of the file, each with a blank `**Proposed ID:**` line. NEVER assigns an `ANTS-NNNN` id; never fills its own Proposed-ID slot. |
| **Maintainer** | The Ants Terminal CC session that owns the MCP server | Triages each un-triaged finding by filling its `**Proposed ID:**` slot in place with the roadmap id(s) it became (or a `n/a — <reason>` closure), and later collapses a shipped finding's write-up to a stub. Never persists status. |

The split is what makes the delta well-defined (v2): a finding whose
`**Proposed ID:**` line is still the blank placeholder is un-triaged
contributor input; a finding with an id (or closure) filled in has been
triaged. (v1: the watermark was the *last maintainer tracking table*, and
anything after it was un-triaged — still honoured for legacy files.)

Note the v2 asymmetry: the maintainer now makes **one** edit *inside* a
contributor finding — filling the `**Proposed ID:**` slot the template
reserves for exactly that — plus the later compaction stub. This is the
sanctioned exception to "leave contributor prose untouched"; the maintainer
still never rewrites a contributor's *description*.

## File location & name

- One file per contributing project, at the shared root
  `/mnt/Games/Scripts/Linux/`.
- Named `<Project>_Ants_MCP_Feedback.md` (e.g.
  `RetroArch_Ants_MCP_Feedback.md`). Project token in `CamelCase` or
  `Snake_Case`, no spaces.
- **Canonical basename = the project directory leaf (ANTS-3384).** A
  contributing project lives in its own directory directly under the
  shared root (`/mnt/Games/Scripts/Linux/<leaf>/`); its feedback file is
  `<leaf>_Ants_MCP_Feedback.md` in that shared root — i.e. the project's
  directory name, verbatim, plus the suffix. This is the rule a brand-new
  project follows so two sessions can't create two differently-named files
  for the same project, and it is what makes the omit-`path` derivation
  (next bullet) land on the right file by construction. Most current files
  already obey it (e.g. `RetroDB/`, `MAME_Curator/`, `Music_Production/`
  each match their dir leaf). The one legacy exception is **DOOM**, whose
  directory is `DOOM_Ants/` but whose file uses the brand token —
  `DOOM_Ants_MCP_Feedback.md` (leaf `DOOM`, not the dir leaf `DOOM_Ants`):
  such a project keeps its historical name and **must pass an explicit
  `path`** (its derived default would be the doubled
  `DOOM_Ants_Ants_MCP_Feedback.md`). Note the not_found candidate ranking
  below does NOT rescue this case: because the file's leaf (`DOOM`) differs
  from the dir leaf (`DOOM_Ants`), the omit-`path` miss matches no sibling
  and reports `all_other_projects:true` even though `DOOM_Ants_MCP_Feedback.md`
  is in fact DOOM's file — which is exactly why the explicit `path` is
  mandatory, not merely advised, for a leaf-mismatch project. New files use
  the dir-leaf basename and avoid the whole problem.
- **Default-path derivation (ANTS-3376).** `feedback_query` and
  `feedback_log` accept `path` being **omitted**: they derive
  `<caller_cwd-leaf>_Ants_MCP_Feedback.md` at the shared root (which, for a
  top-level project, is the parent of `caller_cwd`) and echo
  `path_derived:true` in the reply, so a first-time log needs no filesystem
  hunt. Derivation assumes `caller_cwd` is the project root itself; a
  nested working directory would derive the wrong leaf, so pass an explicit
  `path` then. A legacy project whose file uses a brand alias (DOOM) should
  likewise pass its explicit `path` — the derived default won't match. On a
  `not_found`, the candidate list floats the caller's own file first, or
  sets `all_other_projects:true` when every sibling belongs to a different
  project (so the divergence is visible, not silently fragmented into a new
  file).
- **Suffix guard (canonical home).** Tooling that reads/writes these files
  (`feedback_query` ANTS-1961, `feedback_log` ANTS-1962) guards on the
  exact, case-sensitive basename suffix `_Ants_MCP_Feedback.md`: a `path`
  arg whose resolved basename does not end in that literal is refused with
  `code:"not_feedback_file"` (the suffix is checked on the *resolved /
  canonical* basename, so a symlink or `..` landing on a non-feedback file
  is rejected). The verbs cite this line as the single source for the
  guard literal — do not restate it divergently.

## File skeleton

The H1 title is **free-form** (the legacy corpus varies it freely, e.g.
`# Ants MCP — Feedback from /test-audit …`); only the marker comment and
the block headings below are structural. (`FeedbackFile::skeleton()` emits
exactly this `: 2` marker + v2 banner for a brand-new file as of ANTS-3476, so
a fresh file is born v2 and never needs `migrate_v2`.)

```markdown
<!-- ants-mcp-feedback: 2 -->
# Ants MCP Feedback — <Project> <short context>

<one-paragraph intro: who this project is, how it uses the MCP server>

> Format: docs/standards/mcp-feedback-files.md in the Ants Terminal repo.
> **Contributors (ANTS-2226):** read new items with the `feedback_query`
> Ants-MCP verb (the un-triaged tail) and append findings with `feedback_log
> op:append_finding` — don't hand-edit. Each finding carries a blank
> `**Proposed ID:**` line; leave it blank — the maintainer fills it. Never
> assign an ANTS-NNNN id yourself.

## YYYY-MM-DD — <session label>     ← contributor block (see below)

### <finding title>

- **What:** …
- **Proposed ID:** _(maintainer to assign)_     ← blank ⟹ un-triaged (the tail)
```

A v2 file writes **no new maintainer tracking table**; a **migrated** file
retains its v1 tables in place (§"Migration from v1" — the v2 delta keys on
`**Proposed ID:**`, not the watermark, so the retained tables don't perturb it).
Triage happens in place: the maintainer replaces the `_(maintainer to assign)_`
placeholder with `ANTS-NNNN` (the finding is now triaged), and once that id ships
the whole finding body collapses to a `→ shipped ✅ <date> (write-up compacted,
ANTS-3443)` stub that retains the `**Proposed ID:**` line (the `<date>` is the
ship-date, ANTS-3504; canonical form in §"Maintainer compaction"). Status is
never written here — a reader resolves it live from `ROADMAP.md`.

The first-line HTML comment marks the format version: `<!-- ants-mcp-feedback:
2 -->` is a v2 (inline-id) file; `<!-- ants-mcp-feedback: 1 -->` is v1
(tracking-table) and is accepted-legacy. It is **forward-looking** — the
legacy corpus predates it, so a parser MUST NOT *require* a marker: identify
feedback files by the filename glob `*_Ants_MCP_Feedback.md`, and when the
marker is absent fall back to content (a v1 tracking table ⟹ v1) per
§"The un-triaged delta". A new file SHOULD carry `: 2`; `op:migrate_v2` bumps a
`: 1` file to `: 2`. The blockquote header pointer is the
contributor's one-screen reminder of the rules — including the
`feedback_query` / `feedback_log` verb names (ANTS-2226), so a contributor
session discovers the read/write tools from the file itself rather than
hand-editing (the `DOOM` legacy file carries this banner too — a v1-flavoured one
that still names `op:append_tracking` — plus the `: 2` marker post-migration; its
only real exception is the leaf-mismatch filename noted above). The banner is a blockquote, inert to
the boundary-heading delta parser, so it never perturbs the
un-triaged-tail computation.

## Contributor block

One per session that has something to report.

**Contributor headings are non-structural.** The delta parser does NOT
read them (it keys only on the maintainer-block anchor — see
[the parser contract](#the-un-triaged-delta-parser-contract)), so use
whatever heading the existing file already uses. The corpus is
H1-dominant — most sessions append a fresh top-level
`# <Project> … <date>` block mid-file (MAME and RetroArch do this
repeatedly); some use a dated `## YYYY-MM-DD`. Either is fine. The only
hard rule is **append at the very end of the file** (see
[Contributor don'ts](#contributor-donts)). In v2 the maintainer does not
append a block below your input — it fills the `**Proposed ID:**` slot on
each of your findings in place. Your un-triaged findings are simply the ones
whose slot is still blank, wherever they sit; you never need to track a
watermark position. (v1 legacy: the maintainer appended a tracking table and
"after the last table" was the delta — still honoured for un-migrated files.)

Inside, each distinct finding is its own `### ` sub-block. Most field labels
are a **recommended template, not mandatory** — the corpus is largely
free-form prose and that is fine — but under v2 the `**Proposed ID:**` line is
**structural, not optional**: it is the triage marker (blank ⟹ un-triaged) and
the slot the maintainer fills, so `feedback_log op:append_finding` always emits
it and a hand-written v2 finding MUST include it. A `### ` sub-block with no
`**Proposed ID:**` line is treated as non-finding prose (a note / positive
remark), not a triageable finding.

```markdown
### <short imperative title>

- **What:** one-line summary of the gap / bug / idea.
- **Repro:** the exact tool call + args, or the steps. Paste the
  refusal envelope / wrong output verbatim if there is one.
- **Impact:** who it hurts and how much (blocker / friction / polish).
- **Suggested fix:** optional — what you think would resolve it.
- **Proposed ID:** _(maintainer to assign)_   ← required; leave blank, maintainer fills it
```

To report that a previously-filed item is still broken (or newly fixed),
append a **new finding** that names the prior one in prose — e.g.
`### Issue #8 STILL PRESENT — workspace_search budget` with a
`**Proposed ID:** _(maintainer to assign)_` line. Don't hand-maintain a
status column: the maintainer resolves current status from the roadmap and, on
a "still broken" recheck, reopens the roadmap item. (v1 legacy: a small
`ID | Status | Notes` recheck table — no longer needed, since status isn't
stored in the file.)

## Maintainer triage (v2 — inline id assignment)

The maintainer triages by editing each un-triaged finding in place, not by
appending a block. For each finding in the un-triaged tail:

1. Assign the roadmap id(s) the finding became (allocate via
   `roadmap_log op:append` first), and write them via `feedback_log
   op:assign_id` into the finding's `**Proposed ID:**` line, replacing the
   `_(maintainer to assign)_` placeholder:

   ```markdown
   - **Proposed ID:** ANTS-1525, ANTS-1579
   ```

   Multiple ids are comma-separated (one finding can map to several). A finding
   the maintainer decides not to track carries a **closure marker** instead —
   `n/a — <reason>` (`n/a — out of scope`, `n/a — self-resolved`, `n/a — schema
   fix already shipped`). Both an id and a closure marker count as *triaged*
   (the finding leaves the tail); only the bare `_(maintainer to assign)_`
   placeholder or an **empty value on an existing line** is un-triaged. (An
   *absent* `**Proposed ID:**` line is different: it means the `### ` block is
   not a finding at all — non-finding prose per the delta parser — not that it
   is un-triaged.)

2. **Do not write a status.** A finding's current status (📋/🚧/✅) is resolved
   live from `ROADMAP.md` by whoever reads the file (`feedback_query` renders
   each id's live status as `mapped_id_status` as of ANTS-3478 — see §"Tooling").
   The file records *which id*, never *what state*. If an assigned id
   is **absent** from `ROADMAP.md` (e.g. archive-rotated per roadmap-format.md),
   the reader renders it `archived/unknown` and surfaces it — it is never
   silently treated as shipped.

3. Once an assigned id is ✅ in the roadmap, collapse the finding's write-up to
   a stub with `feedback_log op:compact_resolved` (§"Maintainer compaction").

**Parser contract for the id line.** The id line is matched (case-insensitive)
by `^ *(?:[-*] +)?\*{0,2} *proposed id[ *:]+(.*)$`, and the **value** is capture
group 1 trimmed of surrounding whitespace and stray `*`. The `[ *:]+` run after
`proposed id` deliberately swallows the colon and the closing bold in **either**
order — the canonical template writes `**Proposed ID:**` (colon *inside* the
bold), so the separator is `:**`; a hand-written `**Proposed ID**:` (colon
outside) is `**:` — both are consumed, leaving a clean value. A `### ` finding
uses its **first** matching line; any later one is ignored for **both** the
triaged decision and the assigned-id union (one id line per finding is
canonical). The finding is *triaged* iff that value either contains an
`ANTS-[0-9]+` id **or** begins the literal `n/a` (case-insensitive) followed by
whitespace, a dash, or end-of-value (`n/a\b` — so `n/architecture` is NOT a
false closure) — a **closure**. Any other free-text value (`won't fix`, a bare `(self-resolved)`)
reads as **un-triaged**, so a closure SHOULD be written `n/a — <reason>` (the
reason is advisory). A maintainer writes a v2 closure inline via `assign_id`
when they triage the finding; mechanical `migrate_v2` does **not** normalise the
v1 `(self-resolved)` idiom (that closure history stays in the in-place v1 table).
The placeholder `_(maintainer to assign)_` and any empty value are un-triaged.

A value beginning `n/a` is a **closure regardless of any `ANTS-NNNN` it also
names** — excluded from the assigned-id union and never compacted, so
`n/a — folded into ANTS-1525` closes the finding without relabelling it as
shipped. The assigned-id set is therefore the union of `ANTS-[0-9]+` taken from
each finding's **first** id line **only when that line is not an `n/a` closure**
— a finding whose first id line IS a closure contributes zero ids (the parser
never scans past it to a later line) — including a compacted finding's retained
id line (§"Maintainer compaction"). It is NOT a whole-body scan (a finding may
cite other ids in its prose).

### v1 legacy tracking table (still parsed, never newly written)

Pre-v2 files recorded triage as an appended maintainer *tracking table*; the
parser still recognises it so legacy files read correctly until migrated
(§"Migration from v1"). A v1 watermark heading matches
`^## 📋 Ants Terminal roadmap tracking( update)? \(` (the `📋` + the literal
`Ants Terminal roadmap tracking` are mandatory; the date and optional
`, maintainer` are freeform). Its body is a mapping table whose parser keys on
`ANTS-[0-9]+` in any cell (headers vary: `Item`/`Observation`; `ID`/`ID(s)`).
Note the shipped parser (`feedbackfile.cpp` `parse()`) flags **only an exact
`n/a` id cell** as id-less; any other non-`ANTS` cell (`(self-resolved)`,
`(schema fix)`) is carried as a **literal `ids` token**, not an empty list. This
only matters to the v1 row consumers (`prune_tracking`); the **default** mechanical
`migrate_v2` reads none of a table's id/closure content — it calls `parse()` solely
for the watermark line (§"Migration from v1"); the ANTS-3474 `backfill_from_tracking`
opt-in is the one exception that does read the rows. **Do not author new tracking
tables** — they are read-only history under v2.

## The un-triaged delta (parser contract)

A consumer (the `feedback_query` verb, ANTS-1961) computes the maintainer's
work-list — the **un-triaged findings** — plus the set of already-assigned
ids (so the caller can tell new findings from rechecks of mapped ones).

**Skip fenced code regions first (both format versions).** Contributors paste
roadmap snippets and refusal envelopes that contain real `## `/`# `/`### ` and
`**Proposed ID:**`-looking lines (the `RetroDB…` file has a `## Active` heading
inside a ``` fence). The parser MUST ignore any line inside a fenced block when
scanning for finding boundaries or id lines. Match the fence opener as
`^ {0,3}(```|~~~)` (CommonMark allows up to 3 *spaces* of indent — the corpus has
list-nested fences) and close it at the same fence character.

**Version selection.** A file uses the **v2 rule iff its marker version is
`>= 2`** — `<!-- ants-mcp-feedback: 2 -->` or higher, so a future `: 3` file
also reads under v2 (the reader *degrades forward*, never silently back to v1;
this matches `migrate_v2`'s `>= 2` idempotency short-circuit, ANTS-3446/3448).
Every file with a `: 1`, malformed, or absent marker uses the **v1 rule** until
`op:migrate_v2` converts it (stamping **blank** `**Proposed ID:**` placeholders
on the un-triaged findings and bumping the marker — no id is filled,
§"Migration from v1"). (The *compactor* `op:compact_resolved` deliberately keeps
a stricter exact-`== 2` gate — a compactor must refuse to mutate an unrecognised
future format, whereas a *reader* degrades forward. Reader `>= 2`, compactor
`== 2` is intentional, not drift.) Gating on
the marker — **not** on watermark-absence — is deliberate: a legacy file whose
findings predate the structural `**Proposed ID:**` line (most of the corpus)
must NOT be read under v2, or those line-less findings would be misclassified as
prose (v2 rule step 1) and dropped from the delta. The v2 rule is safe only
because migration and `op:append_finding` guarantee every real v2 finding
carries the `**Proposed ID:**` line.

**v2 rule — un-triaged = findings with an unfilled id.** (Safe only on a
`: 2`-marked file, where every real finding is guaranteed to carry a
`**Proposed ID:**` line — see Version selection.)

1. Enumerate every `### ` finding sub-block (outside fences). A `### ` block runs
   from its heading to the next `#`/`## `/`### ` boundary (or EOF), fences
   skipped — the same extent the compaction ops use. **Note this needs a
   `###`-aware scan**: the shipped v1 `scanBoundaries` (`feedbackfile.cpp`)
   treats only `#`/`## ` as boundaries and `###` as inert, so the v2 path adds
   `### ` as a boundary/extent terminator rather than reusing the v1 scanner
   verbatim. A finding is a `### ` block that carries a `**Proposed ID:**` line;
   a `### ` block without one is non-finding prose (a note) and is never part of
   the delta. (This "no line ⇒ prose" reclassification is why a v1 file, whose
   findings may lack the line, must use the v1 rule until migrated.)
2. A finding is **triaged** iff its `**Proposed ID:**` value holds an
   `ANTS-[0-9]+` id **or** begins `n/a` (a closure); it is **un-triaged**
   otherwise — the empty value, the `_(maintainer to assign)_` placeholder, **and
   any other non-id, non-`n/a` free text** (the authoritative value rule from
   §"Maintainer triage"; the empty/placeholder pair is just its two common
   cases). The **delta** is the ordered list of un-triaged findings (heading +
   body), wherever they sit — position is irrelevant, so no watermark is moved
   and a contributor can append anywhere at EOF without disturbing it.
3. The **assigned-id set** = the union of `ANTS-[0-9]+` across all findings'
   first non-`n/a` `**Proposed ID:**` lines only — NOT a whole-body scan (a
   finding may cite other ids in its prose, as the `RetroArch…` corpus does).
4. **Guard against silent loss.** A `### ` block with **finding-shaped bullets**
   (`- **What:**` / `- **Repro:**` / `- **Impact:**`) but **no** `**Proposed
   ID:**` line is a real finding a hand-editor forgot to tag (the `DOOM` legacy
   file, or any contributor not using `op:append_finding`). It is not in the
   delta (no id line), so `feedback_query` MUST surface it in a
   `suspected_untagged[]` warning rather than let it vanish as prose.

**v1 rule (legacy) — un-triaged = after the last tracking table.** Unchanged
from v1: `L_m` = greatest-position maintainer watermark heading; the delta is
everything from the next `#`/`## ` boundary after `L_m` to EOF; the mapped-id
set is `ANTS-[0-9]+` within maintainer-block bodies only. Retained verbatim so
un-migrated files read identically.

Edge cases: headings inside code fences are skipped (both rules). The earliest
state (no watermark, nothing triaged) yields, **under v1**, the whole post-title
body; **under v2**, every `### ` finding (all un-triaged). Those two sets
coincide only when the file's post-title body is exactly its findings — so the
sets differ, and the version marker decides which applies. A fully-triaged file
yields an empty delta under either rule.

Under v2, filling a `**Proposed ID:**` in place is the *sanctioned* maintainer
edit (§"Two roles") — it removes exactly that finding from the delta and touches
nothing else.

## Status emojis

**v2 scope:** status is no longer *stored* in a feedback file (it's derived live
from the roadmap), so this set is now **advisory prose only** — the vocabulary a
contributor may use when *describing* a recheck ("still broken 🔄"), and the set
`feedback_query` maps a resolved roadmap status onto via its `mapped_id_status`
render (ANTS-3478, §"Tooling"). It is **not** a machine-readable field of the file under v2. (v1
tracking tables used the first four as the parsed `Status` column — legacy.)

Extends the [roadmap-format.md](roadmap-format.md) set (📋 🚧 ✅ 💭) with
three feedback-specific states (🔄 ❓ —) for the contributor-side report:

| Emoji | Meaning |
|-------|---------|
| 📋 | Planned — ID assigned, not started |
| 🚧 | In progress |
| ✅ | Shipped / verified-resolved |
| 💭 | Considered — logged, not committed to |
| 🔄 | Open, no ID yet (contributor-side "still broken") |
| ❓ | Out of scope / not-our-bug / not tracked |
| — | No new evidence / not applicable this session |

`—` is an advisory prose value only — em-dashes saturate normal prose, so
a tool MUST NOT treat a bare `—` as a parseable status token. The
emoji-bearing values (📋 🚧 ✅ 💭 🔄 ❓) are the machine-readable set **in
`feedback_query`'s roadmap-derived render and in v1 tracking tables** — under v2
they are never parsed *from the feedback file itself* (per the scope note above).

## Contributor don'ts

- Don't assign yourself an `ANTS-NNNN` id or fill your own `**Proposed ID:**`
  slot — leave it the `_(maintainer to assign)_` placeholder.
- Don't edit the maintainer's inline id assignments or `→ shipped` compaction
  stubs (the v2 equivalent of "don't edit a maintainer tracking block").
- Every finding must carry a `**Proposed ID:**` line (v2) — it's how the
  maintainer finds your un-triaged input. `feedback_log op:append_finding`
  emits it for you.
- Always append at the very end of the file; never delete a prior finding — a
  "still broken" recheck is a *new* finding that names the old one.

## Maintainer compaction (v2 — `compact_resolved`) — ANTS-3443

Even with inline triage, a *shipped* finding's full write-up stays at full
verbosity forever — yet its detail now lives in the git-tracked ROADMAP bullet +
CHANGELOG (and any spec). `feedback_log op:"compact_resolved"` collapses it.
**Note — these feedback files are not git-tracked** (they live at the shared
root, outside any repo), so "collapse with provenance" means the finding's
substance survives under its `ANTS-NNNN` in those git-tracked artifacts — *not*
that the feedback file's verbatim write-up is recoverable; a collapsed or
stripped write-up is gone from the file. The op keeps a one-line stub.

(The near-identical name to the v1 `compact_shipped` is deliberate — same
"collapse a shipped write-up" job, different gate: `compact_shipped` reads a v1
tracking row, `compact_resolved` reads the finding's inline id + live roadmap.)

It is **auto-discovery** (no target list) and **roadmap-driven**. The
`cmdFeedbackLog` wrapper resolves each finding's assigned id(s) against the live
`ROADMAP.md`; the pure helper replaces the finding's body — every line after the
`### ` heading up to the next `#`/`## `/`### ` boundary **except the
`**Proposed ID:**` line** — with a one-line breadcrumb, keeping the heading AND
the id line **verbatim**. The canonical stub order is fixed: **heading → blank →
the retained `**Proposed ID:**` line → the breadcrumb → trailing blank** (the id
line is lifted to just under the heading regardless of where it sat in the
original body):

```markdown
### Issue #1 — verify_changes timed out

- **Proposed ID:** ANTS-1525, ANTS-1579
→ shipped ✅ 2026-07-12 (write-up compacted, ANTS-3443)
```

The `2026-07-12` is the fix's **ship-date** (ANTS-3504) — each id's
positionally-last `Resolved` date (parentheses optional), then the **max** of
those across the finding's ✅ ids. When no id has a parseable `Resolved` date the
breadcrumb keeps the dateless form (`→ shipped ✅ (write-up compacted,
ANTS-3443)`). See §"Stale-binary self-check" for the exact extraction rule, the
date-not-version/commit rationale, and the contributor self-check convention.

Retaining the `**Proposed ID:**` line is load-bearing: it keeps the block a
*finding* (not reclassified as prose), keeps its ids in the assigned-id set
(§"The un-triaged delta" step 3), and so keeps a later "Issue #1 STILL PRESENT"
recheck correlatable to `ANTS-1525`. (The `ANTS-3443` in the breadcrumb is the
op's provenance tag, **not** the finding's shipped id — that lives in the
retained `**Proposed ID:**` line above. Unlike the v1 stub, the breadcrumb
carries no `confirmed <session> <date>` — the v2 gate is roadmap-✅, not
contributor-confirmation, so there is no confirming session to name.)

Only a `### ` block that carries a `**Proposed ID:**` line is a *finding* and a
candidate; an id-less `### ` block is non-finding prose (a note) — never
enumerated, never in `skipped[]`. A candidate is collapsed only when **all**
hold, evaluated first-failure-wins in this order (else it is left untouched and
reported in `skipped[]` with a `code`):

1. it has a **shippable id** — its first `**Proposed ID:**` value holds ≥ 1
   `ANTS-[0-9]+` id **and is not an `n/a` closure** (`no_shippable_id`;
   closure-wins-over-incidental-id per §"Maintainer triage").
2. its body does not already carry a `→ shipped` breadcrumb line
   (`already_compacted`, idempotent) — checked **before** the roadmap gates, so
   an already-collapsed finding stays collapsed regardless of later roadmap churn
   (idempotency under a reopened id). NB the v2 sentinel differs from v1's "first
   non-blank body line begins `→ shipped ANTS-`": the v2 stub keeps the
   `**Proposed ID:**` line *above* the breadcrumb, so the probe is "**any** body
   line (outside fences) begins `→ shipped`", not the first line.
3. **every** id is ✅ in the live roadmap, unresolved checked before open: an id
   absent from the roadmap (e.g. archive-rotated) fails `roadmap_unresolved_ids`
   (unresolved ids surfaced), and otherwise an id present-but-open fails
   `has_open_id` (open ids surfaced).

A `drop_prose` option to also collapse id-less procedural notes ("Positive
note", "What worked well") is **deferred to the pending spec**: an id-less note
has no `ANTS-NNNN` recovery anchor, so collapsing it is a bare deletion of
contributor prose (against the "never rewrite a contributor's description" rule,
§"Two roles") — the spec must first define a provenance-preserving stub for it.
It is NOT part of this standard's normative contract.

Always preview with `dry_run:true` — it returns every `collapsed[]` /
`skipped[]` entry plus a signed `bytes_saved` and writes nothing. Full contract:
`docs/specs/ANTS-3443.md` **(implemented — `FeedbackFile::compactResolved` +
`cmdFeedbackLog op:compact_resolved`, `tests/features/feedback_log_compact_resolved/`;
the gate list + `skipped[]` codes above are the normative contract. ANTS-3443
scopes `compact_resolved` only — `migrate_v2` is **ANTS-3446** and `assign_id`
gets its own spec id.)**

## Stale-binary self-check (ANTS-3504)

> **Status: shipped (ANTS-3504, cold-eyes clean — `docs/specs/ANTS-3504.md`).**
> `compact_resolved` stamps the ship-date into the stub and `feedback_query`
> `mapped_id_status` carries `shipped_date` on ✅ ids. It goes live in a given
> session once that session's MCP server is relaunched onto the shipping build.

The dominant failure mode across the corpus is the **stale-binary re-report**: a
contributor session running an old MCP-server binary re-files a bug that already
shipped, because it cannot tell "not fixed yet" from "fixed — relaunch to get
it" (the 2026-07-10 batch had 8/14 findings be such re-reports; see ROADMAP
ANTS-3499). ANTS-3504 joins the two facts needed to self-check, both already
available:

- **When the running binary was built** — `session_orient`'s
  `server_build.build_date` (and `build_commit`).
- **When a fix shipped** — the `Resolved <date>` line in its `ROADMAP.md`
  bullet, surfaced as the finding's **ship-date**.

The ship-date is the ISO date on the **last** `Resolved` line in the bullet body
(a bullet may carry `Progress (date)` lines and earlier updates before its final
`Resolved` line). The `Resolved` marker's date appears in the corpus **with or
without** parentheses — `Resolved (2026-07-09):` and `Resolved 2026-05-29:` are
both valid — so the extractor matches `^\s*Resolved\s+\(?(\d{4}-\d{2}-\d{2})`
(parens optional; the line anchor keeps prose like "Resolved the deadlock" from
matching). The narrow anchor also skips the less-common `… . Resolved 2026-… by …`
(mid-line) and `Resolved as of 2026-…` forms — those yield no ship-date and fall
back to the dateless stub (an accepted graceful miss; see spec §2.1). It is
surfaced in two places:

1. **In the file** — `compact_resolved`'s stub carries it:
   `→ shipped ✅ <date> (write-up compacted, ANTS-3443)` (§"Maintainer
   compaction").
2. **In the query** — `feedback_query`'s `mapped_id_status` entries gain
   `shipped_date` for ✅ ids: `{id, status: "✅", shipped_date: "2026-07-09"}`.

**Convention (advisory, never enforced):** before re-reporting a finding whose
`mapped_id_status` is ✅, a contributor compares its `shipped_date` against their
own `session_orient` `server_build.build_date`. If `build_date < shipped_date`,
the running binary predates the fix — relaunch before assuming it did not land.

The anchor is the **date**, not `ANTS_VERSION` (which spans many commits and
cannot distinguish a pre- from a post-fix build) and not a short commit (mapping
an id → its shipping commit is not reliably mechanical; the `Resolved` date is a
single, mechanically-parseable field). When a ✅ bullet carries no `Resolved`
date, no ship-date is emitted — the stub keeps its pre-3504 dateless form and
`shipped_date` is absent; the date is never fabricated. Full contract:
`docs/specs/ANTS-3504.md`.

## v1 legacy compaction ops (un-migrated files only)

The two ops below act on the **v1 tracking table**, which v2 files don't newly
write (a migrated file retains its v1 tables in place). They remain only to clean
up legacy files; do not reach for them on a freshly-authored v2 file.

### `compact_shipped` — ANTS-3421

These files grow without bound: every contributor finding stays at full
verbosity forever, even after its `ANTS-NNNN` ships and the originating
session confirms the fix. `feedback_log op:"compact_shipped"` is the first of
the **two v1 delete-prohibition exceptions** (the second is `prune_tracking`,
below) — it *collapses with provenance* (the same "substance survives under
`ANTS-NNNN`" contract as `compact_resolved`, §"Maintainer compaction"), it never
deletes.

For each maintainer-named target block it replaces the body (every line
after the boundary heading up to the next `#`/`## ` boundary) with a single
breadcrumb, leaving the heading **verbatim** so the parser's
maintainer/contributor classification and the un-triaged delta are
unchanged:

```markdown
## 2. roadmap_log op:flip can't parse the ants-v1 emoji roadmap

→ shipped ANTS-3351, confirmed MAME 2026-07-01 (write-up compacted, ANTS-3421)
```

It is maintainer-only and deterministically gated — a target is collapsed
only when **all** hold (else it lands in `skipped[]` with a `code`, bytes
untouched):

1. the `heading` resolves to exactly one boundary (`heading_line`
   disambiguates a repeated heading);
2. it is not the H1 title / contributor banner (`title_block`);
3. it is not a maintainer tracking block (`maintainer_block`);
4. it sits **above** the watermark — never the un-triaged tail (`in_delta` /
   `not_triaged`);
5. its `id` is ✅ in an effective (last-wins) tracking row (`not_shipped`);
6. the block holds at most one `###` finding (`multi_finding` — never erase
   an un-shipped sibling);
7. it is not already a stub (`already_compacted`, idempotent).

Always preview with `dry_run:true` first — it returns every `outcomes[]` /
`skipped[]` entry plus a signed `bytes_saved` and writes nothing. v1
compacts only **ID-tracked, single-finding** blocks; id-less closures
(`(self-resolved)` …) and multi-finding sessions are out of scope
(docs/specs/ANTS-3421.md § 5).

### `prune_tracking` — ANTS-3442

As an id progresses 📋→🚧→✅ across
sessions it accrues a fresh **tracking-table row in table after table**;
`compact_shipped` shortens contributor *write-ups* but never these repeated
rows. `feedback_log op:"prune_tracking"` removes the **superseded duplicate
rows**, keeping each id's authoritative *last-per-id* row (ANTS-3371's
"later supersedes earlier" rule) plus **every heading, header row, and
`|---|` separator** — so the watermark ledger (the maintainer headings + each
id's final status) and the un-triaged delta are intact. It narrows the ledger
to headings + last-per-id status; the intermediate 📋/🚧 rows are intentionally
dropped (only the current status is authoritative — the ROADMAP bullet's; the superseded per-session snapshots are not preserved elsewhere). It never touches
contributor content — the contributor "don'ts" above are unchanged;
`prune_tracking` is maintainer-only.

Two-stage, per `docs/specs/ANTS-3442.md` § 2.3: Stage 1 marks a row whose
**every ID-column id** has a later duplicate (optionally restricted by
`scope_ids`); Stage 2 removes a marked row only when **every `ANTS-NNNN` token
anywhere in its line** (ID column *or* notes/prose) still appears in a
surviving line, so `mappedIds` is preserved (a notes-cell id with no other
occurrence *pins* its row). Idempotent, atomic, `dry_run` previews
`rows_removed` / `bytes_saved`. An empty `scope_ids:[]` is `bad_args`; an
absent file is `not_found`. Multi-finding-block collapse is a deferred
sibling (§5).

## Migration from v1

A v1 file becomes v2 **lazily**, never in a flag-day rewrite. Migration is
**mechanical by default** (ANTS-3446): it does the unambiguous, lossless work
and defers the fuzzy finding→id triage to
`assign_id` — except under the `backfill_from_tracking:true` opt-in (ANTS-3474),
which carries a confident id in from the tables (detailed below). Crucially, most legacy `### ` findings **carry no `**Proposed ID:**`
line at all** (the corpus predates the structural line — *pre-migration
snapshot*, 2026-07-05: `3D_Engine…` had ~167 `### ` blocks and ~37 id lines), so
migration stamps a **blank** placeholder
on the un-triaged ones. `feedback_log op:"migrate_v2"` does it in **two
mechanical passes**:

1. **Marker bump.** Replace the first `<!-- ants-mcp-feedback: N -->` line (the
   version digits are optional, so a malformed marker is *repaired*) with the
   canonical `<!-- ants-mcp-feedback: 2 -->`, or insert one as line 1 when
   absent. A marker already ≥ 2 short-circuits to a byte-identical no-op
   (`already_v2:true`).
2. **Stamp un-triaged findings.** For each `### ` block **below the v1 watermark**
   (`parse().lastMaintainerLine` — the last canonical `## 📋 …` tracking heading;
   -1 ⟹ every block is below) that is **finding-shaped** and has **no**
   `**Proposed ID:**` line, insert `- **Proposed ID:** _(maintainer to assign)_`
   as the finding's first body bullet. *Finding-shaped* = its heading names
   `issue`/`observation`, **or** its body carries a `- **What/Repro/Impact:**`
   bullet — never a bare prose note (`### Positive note`), or migration would
   flood the delta. A block that already has the line, a prose block, or a
   finding-shaped block *above* the watermark is left byte-identical.

The classification reports two channels so **nothing below the watermark is
silently dropped**: `orphans[]` (a finding-shaped block *above* the watermark —
under v1 it read as already-reviewed, but the inference can be wrong) and
`unclassified[]` (a below-watermark `### ` block left line-less because it is not
finding-shaped — the mechanical heuristic can't tell a freeform finding from a
prose note, so the maintainer eyeballs it). Above-watermark prose is neither
stamped nor reported.

Migration **by default** reads none of a table's *id* content and does no
finding→id auto-stamp or closure normalisation. **Opt-in exception —
`backfill_from_tracking:true` (ANTS-3474):** the migrate then *does* read the v1
tracking rows and stamps a confident row id inline instead of the blank
placeholder — the finding heading is token-matched against each row's item
(overlap-coefficient, grouped per id; blank on any ambiguity, **never a wrong
id**), and `backfilled[]` reports each `{heading, line, id, confidence}`. This is
how the 2026-07-10 corpus migration carried its ids inline. Either way migration
**never moves, collapses, or deletes the v1 tables** — they stay in place, so the
v1 watermark (and thus the un-triaged delta) is unperturbed and there was **no
ordering dependency** on the (now-shipped) v2 reader. The remaining finding→id
triage is `assign_id`'s job; the byte *shrink* comes from `compact_resolved`.
(The ANTS-3446 design spec's own reconciliation for the backfill mode is tracked
by **ANTS-3475**.)

`dry_run:true` previews `stamped[]` / `orphans[]` / `unclassified[]` +
`bytes_delta` and writes nothing. After migration the file is `: 2`; `assign_id` then fills the
placeholders and, once an id ships, `compact_resolved` collapses that finding's
write-up. Now that
ANTS-3448 has shipped and a
`: 2` file's delta no longer depends on the tables, **removing the retained
tables from a migrated file is unblocked** and is the planned final declutter
step: the *post-strip* canonical v2 file will carry no tracking table — the
finding→id mapping lives inline (the backfilled `**Proposed ID:**` lines) and
status comes from the
ROADMAP. Because these files are **not git-tracked**, a strip must snapshot them
first (the tables' notes are otherwise unrecoverable). Full contract:
`docs/specs/ANTS-3446.md` (implemented — `FeedbackFile::migrateV2` +
`cmdFeedbackLog op:migrate_v2`, `tests/features/feedback_log_migrate_v2/`).

## Tooling (verbs) under v2

Every verb that reads or writes these files is affected. Summary (each keeps a
v1 code path until a file is migrated):

| Verb | v2 change |
|---|---|
| `feedback_query` (ANTS-1961) **(marker-aware, ANTS-3448)** | On a `: 2`+ file the delta = un-triaged findings (unfilled `**Proposed ID:**`), not "after the last table"; emits `format_version` + `suspected_untagged[]` (§"The un-triaged delta" step 4) and `mapped_ids` = the inline assigned ids. **Built** — `FeedbackFile::parse()` is marker-aware; the v1 "after last watermark" path is retained for un-migrated files. Rendering each id's **live roadmap status** is **shipped (ANTS-3478)**: `mapped_id_status` = [{id, status}] resolved from the caller project's `ROADMAP.md` (present only when `mapped_ids` is non-empty; an absent id → `"unknown"`, never silently ✅). **ANTS-3504** adds `shipped_date` to each ✅ entry (the id's last roadmap `Resolved` date, parens optional; absent for non-✅ ids and ✅ ids with no `Resolved` line) so a contributor can compare it against `session_orient` `server_build.build_date` before re-reporting (§"Stale-binary self-check"). |
| `session_orient` `feedback_pending` (ANTS-1964) **(ANTS-3448, no code change)** | The per-file un-triaged **count** shares `FeedbackFile::parse`'s delta path, so it now follows the v2 unfilled-`Proposed ID` rule on a `: 2` file **for free** (a v2 file tracks triage inline, so the v1 "after last table" count would miscount — including on a **migrated** file, which retains its v1 tables in place yet triages via `**Proposed ID:**`). No code change on this path — the marker-aware `parse()` supplies the version-correct `deltaPresent`/`deltaLineCount`. |
| `feedback_log op:append_finding` (ANTS-1962) | Already emits the `**Proposed ID:**` placeholder — now **structural**; no behavioural change beyond guaranteeing the line. |
| `feedback_log op:assign_id` **(shipped, ANTS-3447)** | The v2 triage write: fill one finding's `**Proposed ID:**` slot in place with the id(s) or a `n/a — <reason>` closure. Locates the finding by heading over the `### ` enumerator, **inheriting `compact_shipped`'s heading-resolution gate _shape_** (ANTS-3421: `heading_line` disambiguation + `target_ambiguous`+`candidates[]` when a "still broken" recheck reuses a `### ` title). Single-target (batch deferred), so no cross-target `duplicate_target`. Replaces `op:append_tracking` for v2 files. |
| `feedback_log op:compact_resolved` **(shipped, ANTS-3443)** | Auto-collapse shipped findings' write-ups; gates on live roadmap ✅. Refuses `not_v2` on a v1 file. (A `drop_prose` option is **deferred** — see §"Maintainer compaction"; NOT in ANTS-3443 scope.) **ANTS-3504** stamps the fix's ship-date into the stub (`→ shipped ✅ <date> (write-up compacted, ANTS-3443)`; latest `Resolved` date among the finding's ✅ ids, dateless fallback when none) — §"Stale-binary self-check". |
| `feedback_log op:migrate_v2` **(shipped, ANTS-3446)** | One-shot **mechanical** v1→v2 migration (§"Migration from v1"): bumps the marker + stamps blank `**Proposed ID:**` placeholders on un-triaged findings; reports `orphans[]`/`unclassified[]`. Leaves the v1 tables **in place** (no move/collapse); the default path reads no table id content (the `backfill_from_tracking:true` opt-in reads the rows to carry ids inline — ANTS-3474). |
| `feedback_log op:append_tracking` | **Superseded by `assign_id`** (shipped, ANTS-3447) for v2 files — it remains the v1 triage-write op (writes a v1 table) for un-migrated files. On a `: 2` file it now **refuses `not_v1`** (shipped, ANTS-3477) pointing at `assign_id`; on a v1 / un-migrated file it stays valid. |
| `feedback_log op:compact_shipped` (ANTS-3421) / `op:prune_tracking` (ANTS-3442) | **Legacy** — operate on v1 tables; used only to clean up / migrate un-migrated files. |

Each new/changed verb ships spec-first with its own `docs/specs/ANTS-NNNN.md`
and a `tests/features/` conformance test, per the project standards. Spec ids:
`compact_resolved` = **ANTS-3443**, `migrate_v2` = **ANTS-3446**,
`assign_id` = **ANTS-3447**, the marker-aware v2 delta reader = **ANTS-3448**
(all shipped). `migrate_v2`'s ANTS-3474 `backfill_from_tracking` mode has no
separate spec — its design-doc reconciliation is **ANTS-3475**.
