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

Fix (2026-06-06 completion): the bracketed non-dash form (`[Cl9]`) — the
shape Vestige actually authors (`Cl9` / `Cl10` / `CE18`), reconfirmed
across three sessions — **is** now adopted as the id. The earlier
"deliberately not adopted" stance left this real, repeated data invisible
and unflippable, so it was reversed by user decision 2026-06-06. The
adoption is safe because it is a **positional** signal, not a widening of
the shared `idTokenPattern`: a `[...]` is adopted only when it is

- **head-anchored** — the very first token after the status emoji
  (mid-prose `see [ref] here` never qualifies), and
- **ID-shaped** — a single whitespace-free token
  `^[A-Za-z][A-Za-z0-9_.-]*` (so `[see notes]` with a space is rejected),
  and
- **not a markdown link** — the `(?!\()` guard rejects `[label](url)`.

The body-wide `rxId` is untouched (it still rejects dash-less brackets in
bullet prose), so this does not false-positive on arbitrary `[text]`. The
leading-bracket slot is the id slot by roadmap convention (`[ANTS-1234]`);
the only residual false-positive is a deliberate tag like `- 📋 [WIP] …`,
which is vanishingly rare in a roadmap and costs only a spurious id, never
data loss.

## Invariants

### INV-1 — a native bold-ID-first bullet gets its id

`parseBullets` on an ants-v1 doc with `- 📋 **Cl9.** headline` yields a
bullet whose `id == "Cl9"` (it was empty before the fix).

### INV-2 — the standard `[PROJ-NNNN]` form is unchanged

`- ✅ [ANTS-0001] **Normal bullet.**` still yields `id == "ANTS-0001"`
— `extractBoldId` does not fire (the head starts with `[`, not `**`), so
the bracketed-id path is untouched (no regression).

### INV-3 — the bare-bracket id form IS adopted (the actual Vestige shape)

`- 📋 [Cb7] **Bracketed.**` yields `id == "Cb7"`. Reverses the pre-2026-06-06
contract (this used to stay a narrator bullet). The head-anchored,
ID-shaped, link-guarded match is a positional signal that does NOT widen
the body-wide `idTokenPattern`.

### INV-4 — a multi-word bold-prose bullet stays a narrator bullet

`- 🚧 **In-progress thing.** body.` yields an **empty** `id` — a bold
span with internal whitespace is not ID-shaped, so it is not adopted as
an id (the headline stays the headline).

### INV-5 — a leading markdown link is not adopted

`- 📋 [docs](https://x) **H**` yields an **empty** `id` — the `(?!\()`
link-guard keeps the extractor from eating a single-word link label.

### INV-6 — faithful Vestige scenario (GFM-format doc)

In a `github-task-list` doc (checkbox bullets dominate), bare-bracket
emoji bullets `- 📋 [Cl9] **…**` / `[Cl10]` / `[CE18]` are all indexed by
id — the emoji bullets route through the native path even in a GFM doc.

## Test plan

Behavioural against `RoadmapDialog::parseBullets` over synthetic ants-v1
**and** github-task-list fixtures (no real ROADMAP.md). INV-1 + INV-3
FAIL against pre-fix code (the bold-ID-first and bare-bracket bullets read
with an empty id).
