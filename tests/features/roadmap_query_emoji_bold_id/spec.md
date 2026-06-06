# ANTS-1987 — native-path bold-ID extraction for emoji bullets

## Background

Vestige feedback (2026-06-04): in a roadmap doc, emoji-prefixed bullets
whose ID is a project-local token become invisible to `roadmap_query`.
`RoadmapDialog::parseBullets` routes an emoji bullet (not a `[ ]`/`[x]`
checkbox line) to the native (ants-v1) branch, which only called
`stripInlineEmoji` — never `extractBoldId`. So:

1. A **bold-ID-first** bullet `- 📋 **Cl9.**` produced an empty `id`
   (the GFM branch extracts `**Cl9.**` via `extractBoldId`, but the
   native branch did not). Empty id → narrator bullet → dropped from
   `roadmap_query` (id/ids/section/section_index all miss it).
2. A **bracketed non-dash** token `- 📋 [Cl9] **h**` also misses,
   because the bracket matcher `rxId` requires a `-<digits>` tail
   (`[PROJ-NNNN]`).

Fix (this spec): call `extractBoldId` in the native path too, so the
bold-ID-first form is recognised. `extractBoldId` is head-anchored
(`^\*\*…\*\*`), so the standard `[ID] **headline**` form — whose head
starts with `[` — is untouched. Unlike the GFM branch (where the leading
bold span is an ID-label by convention), in ants-v1 the bold span is
normally the **headline**, so a bold token is adopted as the id **only
when it is ID-shaped** — a single whitespace-free token matching
`^[A-Za-z][A-Za-z0-9_.-]*$` (`Cl9` / `Sh4` / `Ts20-DE1`). A bold-prose
bullet such as `- 🚧 **In-progress thing.**` (internal spaces) stays
id-less, so multi-word narrator bullets are not mis-read as IDs.

The bracketed non-dash form (`[Cl9]`) is **deliberately not** adopted as
an ID — widening `rxId` to accept dash-less brackets would false-positive
on arbitrary `[text]` in bullet prose. Such a bullet stays a narrator
bullet, and a section that contains only such bullets is already surfaced
to a `section=` caller by the ANTS-1538 "default ID-filter dropped all N
bullet(s) … narrator-prose line with no [PROJ-NNNN] id" warning
(remotecontrol.cpp ~L2888): the 📋 narrator is kept by the active filter,
counted in `preIdPruneCountSec`, then dropped by `shouldDropUnnumbered`,
which triggers the warning.

## Invariants

### INV-1 — a native bold-ID-first bullet gets its id

`parseBullets` on an ants-v1 doc with `- 📋 **Cl9.** headline` yields a
bullet whose `id == "Cl9"` (it was empty before the fix).

### INV-2 — the standard `[PROJ-NNNN]` form is unchanged

`- ✅ [ANTS-0001] **Normal bullet.**` still yields `id == "ANTS-0001"`
— `extractBoldId` does not fire (the head starts with `[`, not `**`), so
the bracketed-id path is untouched (no regression).

### INV-3 — the bracketed non-dash form stays a narrator bullet

`- 📋 [Cb7] **Bracketed.**` yields an **empty** `id` — the ID token is
not widened to accept dash-less brackets.

### INV-4 — a multi-word bold-prose bullet stays a narrator bullet

`- 🚧 **In-progress thing.** body.` yields an **empty** `id` — a bold
span with internal whitespace is not ID-shaped, so it is not adopted as
an id (the headline stays the headline).

## Test plan

Behavioural against `RoadmapDialog::parseBullets` over synthetic ants-v1
fixtures (no real ROADMAP.md), mirroring the
`roadmap_parser_pass_emoji_status` harness. INV-1 FAILS against pre-fix
code (the bold-ID-first bullet read with an empty id).
