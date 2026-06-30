<!-- ants-mcp-feedback-format-spec: 1 -->
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
file every week (the largest, 3D_Engine, is already ~3,460 lines and
growing) — the
`feedback_query` MCP verb, ANTS-1961.

## Two roles, one file

| Role | Who | Writes |
|------|-----|--------|
| **Contributor** | Any non-Ants CC session that uses the Ants MCP server | Appends dated finding blocks at the end of the file. NEVER edits a maintainer tracking block; NEVER assigns an `ANTS-NNNN` ID. |
| **Maintainer** | The Ants Terminal CC session that owns the MCP server | Appends a `roadmap tracking update` block that maps contributor findings to roadmap IDs. Owns every tracking block; leaves contributor blocks untouched. |

The split is what makes the delta well-defined: a maintainer tracking
block is the watermark. Anything after the **last** tracking block is
un-triaged contributor input.

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
the block headings below are structural.

```markdown
<!-- ants-mcp-feedback: 1 -->
# Ants MCP Feedback — <Project> <short context>

<one-paragraph intro: who this project is, how it uses the MCP server>

> Format: docs/standards/mcp-feedback-files.md in the Ants Terminal repo.
> **Contributors (ANTS-2226):** read new items with the `feedback_query`
> Ants-MCP verb (the un-triaged tail) and append findings with `feedback_log
> op:append_finding` — don't hand-edit, that keeps the read-the-tail
> watermark intact. The maintainer stamps roadmap IDs via `feedback_log
> op:append_tracking`.
> Contributors append below the last maintainer block; never edit a
> maintainer table; never assign ANTS-NNNN IDs.

## YYYY-MM-DD — <session label>     ← contributor block (see below)
...

## 📋 Ants Terminal roadmap tracking update (YYYY-MM-DD, maintainer)   ← maintainer block
...
```

The first-line HTML comment `<!-- ants-mcp-feedback: 1 -->` marks a
conforming file. It is **forward-looking** — the legacy corpus predates
it, so a parser MUST NOT require it: identify feedback files by the
filename glob `*_Ants_MCP_Feedback.md` and treat the marker as an
optional confirmation (same posture as roadmap-format.md's file marker).
New files SHOULD carry it. The blockquote header pointer is the
contributor's one-screen reminder of the rules — including the
`feedback_query` / `feedback_log` verb names (ANTS-2226), so a contributor
session discovers the read/write tools from the file itself rather than
hand-editing. `FeedbackFile::skeleton()` emits this banner for every
new file; most existing corpus files were back-filled with it under
ANTS-2226 (the `DOOM` legacy file is a standing exception — it carries
neither the banner nor the marker). The banner is a blockquote, inert to
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
[Contributor don'ts](#contributor-donts)). Note the maintainer adds a
*new* tracking block below your input each review cycle (3D_Engine has
several, oldest-to-newest down the file), so "after the last maintainer
block" means after whatever the current bottom-most one is — never insert
above it.

Inside, each distinct finding is its own sub-block. Field labels are a
**recommended template, not mandatory** — the corpus is largely
free-form prose and that is fine; the labels just make a finding
self-contained so the maintainer can act without re-running anything:

```markdown
### <short imperative title>

- **What:** one-line summary of the gap / bug / idea.
- **Repro:** the exact tool call + args, or the steps. Paste the
  refusal envelope / wrong output verbatim if there is one.
- **Impact:** who it hurts and how much (blocker / friction / polish).
- **Suggested fix:** optional — what you think would resolve it.
- **Proposed ID:** leave blank. The maintainer assigns it.
```

To report that a previously-filed item is now confirmed fixed / still
broken, add a small status table (`ID | Status | Notes`) using the
[status emojis](#status-emojis) below. The corpus heading for this varies
(`Status of prior items`, `Resolved items from prior feedback`,
`Prior-feedback fulfillment`, …) — any clear label works; it is not
parsed.

## Maintainer tracking block

The maintainer's watermark. A parser keys on this heading. Three forms
exist in the corpus and **all are recognised watermarks**:

```markdown
## 📋 Ants Terminal roadmap tracking (added YYYY-MM-DD)        ← earliest form
## 📋 Ants Terminal roadmap tracking update (YYYY-MM-DD)       ← most common
## 📋 Ants Terminal roadmap tracking update (YYYY-MM-DD, maintainer)   ← recommended
```

- **Anchor regex:** `^## 📋 Ants Terminal roadmap tracking( update)? \(`.
  The `📋` and the literal `Ants Terminal roadmap tracking` are
  **mandatory** — they are what the parser keys on; a heading that drops
  the emoji or reworks that phrase silently won't be recognised as a
  watermark, collapsing every prior triaged block into the delta. Only
  the date and the optional `, maintainer` are freeform inside the parens
  — the parser MUST NOT require the `, maintainer` suffix (most legacy
  blocks omit it). New blocks SHOULD use the third, fully-qualified form.
- Don't rewrite existing headings to the new form; the older two stay
  valid so historical watermarks keep their position.

Body: a prose line or two, then a mapping table. The dominant corpus form
is three columns; `Notes` is an optional fourth. The ID column is plural
(`ID(s)`) because one finding can map to several IDs:

```markdown
| Item | ID(s) | Status |
|------|-------|--------|
| <finding recap> | ANTS-NNNN[, ANTS-MMMM] | 📋 |
```

Column *headers* vary across the corpus (`Item` / `Observation` /
`Prior item` for the finding; `ID` / `ID(s)` / `New ID` for the ID), and
that is fine — **the parser keys on `ANTS-[0-9]+` appearing anywhere in a
cell, never on header text.** A row maps a finding to one or more
`ANTS-NNNN` IDs; rows for not-tracked items legitimately carry a
parenthetical or `n/a` instead (`(self-resolved)`, `(schema fix)`,
`(no roadmap item)`, `n/a` — these forms recur in the corpus's maintainer
tables), so a validator must not reject an ID-less row.

Optionally end the block with a sentinel line as a human breadcrumb for
where the triaged region stops (advisory only — the delta is computed
from heading position, not this line, so a missing sentinel is harmless):

```markdown
End of YYYY-MM-DD maintainer roadmap-tracking update.
```

## The un-triaged delta (parser contract)

A consumer (the `feedback_query` verb, ANTS-1961) computes the work-list
as: **everything after the last maintainer tracking block**, plus the set
of `ANTS-NNNN` IDs already mentioned anywhere in any maintainer block (so
the caller can tell new findings from re-checks of mapped items).

**Skip fenced code regions first.** Contributors paste roadmap snippets
and refusal envelopes that contain real `## `/`# ` lines (e.g.
`RetroDB…:440` is `## Active` inside a ``` fence). The parser MUST ignore
any line inside a fenced block when scanning for boundaries, or a pasted
heading below the last maintainer block would split the delta. Match the
fence opener as `^\s{0,3}(```|~~~)` (CommonMark allows up to 3 spaces of
indent — the corpus has list-nested fences at `RetroArch…:265`) and close
it at the same fence character.

**Only `^# ` and `^## ` headings (exactly one or two hashes), outside
fences, are block boundaries.** `###` and deeper are inert body lines
wherever they sit — the corpus embeds `### ` sub-headings *inside*
maintainer blocks (e.g. `Music_Production…:340`) and contributor sessions
use `### <title>` finding sub-blocks — so a parser that split on any
heading would truncate a block. The parser classifies each boundary
heading as exactly one of two kinds:
**maintainer** (matches the anchor regex above) or **contributor**
(every other `#`/`##` heading). It needs no finer reading of contributor
headings — their level (H1 or H2) and wording are irrelevant. Algorithm:

1. Let `L_m` = the greatest line position of a maintainer heading
   (across all three recognised forms). The maintainer block runs from
   `L_m` to the next contributor heading after it, or EOF.
2. The **delta** = the text from that next contributor heading to EOF.
   The maintainer heading and its own body/table are thereby excluded.
3. The mapped-ID set = `ANTS-[0-9]+` matches **within maintainer-block
   bodies only**, NOT a whole-file scan. Contributor prose freely cites
   IDs (e.g. `RetroArch…:839` "…has landed for `roadmap_query` and
   ANTS-1493…"); counting those would wrongly mark un-triaged findings as
   mapped. A finding with no ID in any maintainer block is unmapped. The
   last maintainer block's body runs from its heading to EOF (no
   contributor heading follows it, by the append-at-end rule).

Edge cases the parser MUST handle:

- **Headings inside code fences** (`RetroDB…:440`): skipped — see the
  fence rule above; never a boundary.
- **Zero maintainer blocks** (every file's earliest state): the whole
  file below the H1 title is the delta.
- **No contributor heading after `L_m`** (fully triaged): empty delta.
  The maintainer block's own heading/body never count as un-triaged.
- **Multiple maintainer blocks / mixed heading forms** (3D_Engine has all
  three): step 1 already picks the max-position one regardless of form.

This is why contributors must append at the end and must not edit
maintainer blocks: an edit inside a triaged block would either resurrect
old findings into the delta or hide new ones.

## Status emojis

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
emoji-bearing values (📋 🚧 ✅ 💭 🔄 ❓) are the machine-readable set.

## Contributor don'ts

- Don't edit or reflow a maintainer tracking block.
- Don't assign yourself an `ANTS-NNNN` ID — propose, leave the slot blank.
- Don't insert a new finding *above* an existing maintainer block; always
  append at the end of the file.
- Don't delete prior findings — a confirmed-fixed item is reported via a
  status table, not by removing the original.
