# ANTS-1878 — `roadmap_log op:create_section` verb

**Status:** spec draft, cold-eyes loops 1 + 2 + 3 + 4 folded (2026-05-26).
**Kind:** enhancement.
**Source:** ROADMAP.md ANTS-1878 (vestige-feedback-2026-05-26 Obs #21).
**Composes with:** ANTS-1879 (`op:append_batch` keys on the slug
returned here).

**Layman:** Today the roadmap_log tool can append a bullet to an
**existing** section, but it can't create a brand-new section header
(like adding "Phase 10.6: Multi-Threading & Concurrency" mid-roadmap).
A research session that needed one had to hand-edit ROADMAP.md, which
defeats the point of having a single typed write path. This change
adds a small verb that inserts a heading + optional intro paragraph in
the right spot and returns the new slug, so a follow-up batch of
`op:append` (or `op:append_batch`) can key on it without a round-trip
through Edit.

## 1. Problem

Vestige's CC session needed a new `## Phase 10.6: Multi-Threading &
Concurrency Architecture` heading. `cmdRoadmapLogAppend`
(`src/remotecontrol_roadmap_log.cpp`) requires an **existing** section slug —
`RoadmapIndex::findBySlug` returns null otherwise, and the verb refuses
with `bad_section`. The session fell back to
Edit, which:

1. Re-implements heading insertion outside the typed write path (no
   slug computation, no format-detection gate, no atomic commit).
2. Risks corrupting the file's section structure if the splice misses
   the right insertion point (intro_body bleeds into the prior
   section).
3. Breaks the contract that `roadmap_log` is the single write surface —
   the same justification that motivated ANTS-1424.

## 2. Surface

### 2.1 Verb shape

`op:"create_section"` accepted by `RemoteControl::cmdRoadmapLog`
(`src/remotecontrol_roadmap_query_verb.cpp`) alongside the existing `append`, `flip`,
`flip_batch`, and `annotate` ops:

```jsonc
{
  "op": "create_section",
  "caller_cwd": "/path/to/project",
  "after_section": "performance",   // slug of existing section
  "level": 2,                       // 2 (##) or 3 (###)
  "title": "Phase 10.6: Multi-Threading & Concurrency",
  "intro_body": "Optional one-paragraph intro prose."  // optional
}
```

The handler `cmdRoadmapLogCreateSection` lives in
`src/remotecontrol_roadmap_log_batch.cpp` (declared in `src/remotecontrol.h`)
next to `cmdRoadmapLogFlipBatch`. It is
m_main-independent (caller_cwd + filesystem only), so the test seam
mirrors `cmdRoadmapLogFlipBatchForTest`:
`cmdRoadmapLogCreateSectionForTest(req)`.

### 2.2 Slug computation

Slug derives from the title via the same transform RoadmapIndex
already applies (`RoadmapIndex::slugifyHeading`,
`src/roadmapindex.cpp:38-54`):

1. Walk each `QChar`; apply `toLower()`.
2. Emit `QChar::isLetterOrNumber()`-true characters verbatim (Unicode
   letters and numbers, not ASCII-only).
3. Collapse any run of non-letter-or-number characters to a single
   `-`. A leading `-` is suppressed at emission via the `prevDash =
   true` seed (`roadmapindex.cpp:41`) — there is no leading-trim
   step because no leading `-` is ever emitted.
4. Trim trailing `-` (`while (out.endsWith('-')) out.chop(1)`,
   `roadmapindex.cpp:52`).

Examples:

| Title                                                  | Slug                                       |
|--------------------------------------------------------|--------------------------------------------|
| `"Phase 10.6: Multi-Threading & Concurrency"`          | `"phase-10-6-multi-threading-concurrency"` |
| `"Café Phase"` (Unicode letter)                        | `"café-phase"` (non-ASCII letter retained) |

If the computed slug already exists in the parsed index, refuse with
`slug_collision` — the caller picks a different title rather than the
verb silently appending `-2`. The verb does **not** auto-deduplicate
slugs; collision is a contract violation the caller resolves.

**Asymmetry with `RoadmapIndex::uniqueSlug`.**
`uniqueSlug` (`roadmapindex.cpp:72-86`) DOES auto-append `-2`, `-3`
on duplicate when `buildIndex` walks the file. The new verb's
collision check intentionally diverges: a refusal is louder than a
silent rename. If a caller refuses, then hand-edits the same
duplicate title into ROADMAP.md anyway, `buildIndex` will assign
`-2` on its next read. That is acceptable — the verb's contract is
"refuse on collision," not "prevent collision globally."

### 2.3 Splice position

`after_section` resolves via `RoadmapIndex::findBySlug` exactly like
the `section` field in `op:append`. The new heading inserts at the
prior section's `lineEnd` (0-indexed, exclusive — same semantics used
by `cmdRoadmapLogAppend`, `src/remotecontrol_roadmap_log.cpp`).

Block written at `lineEnd` (the heading is always the first emitted
line, so the returned `line` points unambiguously at it):

```
<level-hashes> <title>
<blank line>
<intro_body — hard-wrap at 80 cols on word boundaries, one blank
              between paragraphs>
<blank line>          # only if intro_body present
```

The 80-column wrap is **deliberately wider than**
`roadmap-format.md` § 3.5's "roughly 70 columns" bullet-body rule:
section intros are read in-place rather than inside a narrow bullet
list, so the extra horizontal room aids prose flow. The divergence
is intentional; an implementer should not "fix" the wrap to 70.
Trailing `\n\n` per inserted block mirrors the bullet-insertion's
whitespace discipline in `cmdRoadmapLogAppend`.

**`intro_body` reformat policy.** Single newlines in the caller's
`intro_body` are treated as paragraph breaks (caller is responsible
for paragraph segmentation); double newlines collapse to single. The
handler then hard-wraps each paragraph at 80 cols on word
boundaries; no other reformat (no Markdown normalisation, no smart
quotes, no list-item magic). Leading/trailing whitespace per
paragraph is trimmed.

**Stray-heading guard.** The handler refuses (`bad_intro`) when ANY
line of the wrapped `intro_body` matches the regex
`^#{1,6}\s` (1–6 hash chars then whitespace — the exact Markdown
heading shape) — a stray heading marker would silently add a section
to the index. A line that starts with `#` followed immediately by a
non-whitespace character (e.g. `#1234 was tracked`) is NOT a heading
and is allowed.

### 2.3a Derived placement for a release (ANTS-5315)

A roadmap grouped by release names each release in a level-2 section
title: `0.10.0 — Theme`, or a patch stream `0.4.x — Patches`
(`RoadmapIndex::versionOfTitle`). When `after_section` is absent or empty, `level`
is 2 and `title` begins with a version, the verb derives `after_section`
instead of refusing:

1. Among the level-2 sections whose titles carry a version, take the one
   with the greatest version **lower** than the new one. Versions compare
   major, then minor, then patch, each as a number, so `0.10.0` follows
   `0.9.0`. A patch stream `x` sorts after every numbered patch of its
   minor.
2. If none is lower, take the level-2 section immediately before the one
   with the **lowest higher** version, in document order.
3. If that lowest higher version is the first level-2 section, place the
   new section immediately before it (ANTS-5563).
4. If no level-2 title carries a version, refuse `missing_field` as § 2.5
   does: the verb cannot name a place, and the caller must.

The derivation reads the same heading index § 2.3 resolves a supplied
slug against, on the markdown route and the store route alike. The derived
slug is then used exactly as a supplied one (§ 2.3). The success envelope
carries it as `after_section`, or as `before_section` under step 3, with
`placed_by_version: true`; all three keys are absent when the caller
supplied `after_section`.

**Every title that begins with a version is checked, placed or supplied, at
either level.** These checks run first, before the derivation above and
before § 2.2's collision check, so `bad_title` is the answer wherever
`missing_field` or `slug_collision` would also apply:

- A version written with a leading `v`, or followed directly by `-` (a
  pre-release suffix such as `-rc1`), refuses `bad_title`. The release
  ruling writes `0.10.0` and keeps `-rcN` to the git tag.
- A version an existing `##` or `###` title already carries refuses
  `bad_title`. `roadmap_query version:` returns the first such section
  (`RoadmapIndex::findByVersion`), so a second one would never be returned.

### 2.4 Format detection

Same `unrecognised_format` gate as `cmdRoadmapLogAppend`
(`src/remotecontrol_roadmap_log.cpp`): the refusal fires when
`RoadmapDialog::parseBullets(markdown)` is empty **AND**
`markdownBytes > kRoadmapMinParseableSize` — both conditions must
hold. Tiny non-parseable files (≤ the threshold) are allowed through.
A foreign-dialect roadmap above the threshold does not get a new
heading spliced into it.

### 2.5 Refusal codes

- `missing_field` — `caller_cwd`, `level`, or `title` missing/empty,
  or `after_section` missing/empty where § 2.3a derives none.
- `bad_level` — `level` is not 2 or 3.
- `bad_intro` — any wrapped line of `intro_body` matches the
  Markdown-heading regex `^#{1,6}\s` (1–6 hash characters then
  whitespace). A line that starts `#` followed immediately by a
  non-whitespace character (e.g. `#1234`) is allowed.
- `no_roadmap` — `caller_cwd` doesn't canonicalise, or no ROADMAP.md
  under the resolved root.
- `bad_section` — `after_section` slug not found.
- `bad_case` — case-only mismatch on `after_section`; envelope
  carries `canonical_slug` (parity with `cmdRoadmapLogAppend`,
  `src/remotecontrol_roadmap_log.cpp`). `bad_case` is in active use across
  `roadmap_query` and `roadmap_log op:append` but is not yet listed
  in `docs/standards/mcp-error-codes.md`; § 7 adds it.
- `bad_title` — the title yields an empty slug, or (§ 2.3a) its version is
  written with a `v` or a pre-release suffix, or an existing `##` or `###`
  title already carries it. On the store route that includes a section in
  an archive file (ANTS-5777).
- `slug_collision` — computed slug already in the index. Envelope
  carries `computed_slug` so the caller sees the clash.
- `unrecognised_format` — gate above.
- `roadmap_read_failed` / `roadmap_write_failed` — IO faults
  (atomic write via `QSaveFile`).

### 2.6 Success envelope

```jsonc
{
  "ok": true,
  "op": "create_section",
  "slug": "phase-10-6-multi-threading-concurrency",
  "file": "ROADMAP.md",
  "line": 4321,           // 1-based line number of the heading
  "bytes_written": 184    // UTF-8 size of the inserted block
}
```

`line` points at the new heading line (§ 2.3 puts the heading as the
first emitted line — no leading blank). Caller can pipe the
returned `slug` straight into a follow-up `op:append` or
`op:append_batch` (ANTS-1879) call without re-reading ROADMAP.md:
each verb re-parses the file from disk on entry, so the new section
is visible on the next call.

### 2.7 Atomicity

Single read of ROADMAP.md + single `QSaveFile::commit()`. No counter
is touched (a section is not an allocated id — counter stays with
bullet allocation).

## 3. Invariants

- **INV-1** — `op:"create_section"` is dispatched from
  `cmdRoadmapLog` next to `flip` / `flip_batch` / `annotate` /
  `append`. *Test:* source-grep
  `tests/features/mcp_roadmap_log_create_section/`.
- **INV-2** — `caller_cwd`, `level` and `title` are required, and
  `after_section` unless § 2.3a derives it; missing any →
  `missing_field`. *Test:* same.
- **INV-3** — `level` accepts only `2` or `3`; anything else →
  `bad_level`. *Test:* same.
- **INV-4** — Unknown `after_section` slug → `bad_section`; a
  case-only mismatch → `bad_case` with `canonical_slug`. *Test:*
  same.
- **INV-5** — Slug computation calls
  `RoadmapIndex::slugifyHeading` (no separate implementation in
  `remotecontrol.cpp`). *Test:* source-grep the handler for
  `RoadmapIndex::slugifyHeading(`; behavioural tests on the
  transform live with the helper, not duplicated here.
- **INV-6** — A computed slug that already exists in the index →
  `slug_collision` (no silent `-2` suffix), except where § 2.3a's version
  checks refuse `bad_title` first. *Test:* same.
- **INV-7** — Insertion at `after_section`'s `lineEnd` (0-indexed,
  exclusive). The new heading is the FIRST emitted line (§ 2.3) and
  the returned `line` is the 1-based equivalent (`lineEnd + 1`).
  *Test:* assert `line == lineEnd + 1` and the heading text appears
  verbatim at that 1-based line in the on-disk file.
- **INV-8** — The handler's ONLY write path is
  `QSaveFile::commit()`. *Test:* source-grep — exactly one
  `QSaveFile … .commit()` pair appears inside the handler body; no
  `QFile::write`, no `std::ofstream`, no other write mechanism.
  (`QSaveFile`'s rollback guarantee on commit failure is a Qt
  library invariant — see § 5 Out of scope for why we don't own a
  feature-test seam for it here.)
- **INV-9** — `unrecognised_format` gate fires when
  `parseBullets(markdown).isEmpty() && markdownBytes >
  kRoadmapMinParseableSize`; both conjuncts required. *Test:* feed
  a non-parseable ROADMAP fixture larger than
  `kRoadmapMinParseableSize`; envelope returns `unrecognised_format`
  and the file is untouched. (A tiny non-parseable file under the
  threshold is allowed through — this matches the single-bullet
  path's behaviour and isn't a separate test case.)
- **INV-10** — Any wrapped line of `intro_body` matching the regex
  `^#{1,6}\s` → `bad_intro`. A line starting `#` immediately
  followed by a non-whitespace character (e.g. `#1234`) is NOT a
  heading and is allowed. *Test:* feature test feeds a positive
  case (`## stray heading`) and a negative case (`#1234 ticket
  ref`); positive refuses, negative accepts.
- **INV-11 (ANTS-5315)** — With no `after_section`, a level-2 version
  title is placed by § 2.3a. Against `## 0.9.0` alone, `0.10.0` lands
  after it; against `## 0.9.0` and `## 0.11.0`, `0.10.0` lands between
  them; against `## Unscheduled` then `## 0.11.0`, `0.10.0` lands after
  `Unscheduled`. Without `after_section`, a non-version title, and a
  version title whose lowest higher neighbour is the first section, still
  refuse `missing_field`. With or without `after_section`, `v0.10.0 — X`,
  `0.10.0-rc1 — X`, and a version an existing `##` or `###` title carries
  refuse `bad_title`. The envelope names the derived `after_section` with
  `placed_by_version:true`, and carries neither when the caller supplied
  `after_section`. *Test:*
  `tests/features/mcp_roadmap_log_create_section/`.

## 4. RAM / build cost

No new state. The handler reads ROADMAP.md, splices in-memory,
writes back atomically — same memory profile as `cmdRoadmapLogAppend`
(O(file size)). No new external libs. Adds one source-grep test
bundle (`tests/features/mcp_roadmap_log_create_section/`) + ~120
lines of C++ to `src/remotecontrol_roadmap_log_batch.cpp`.

## 5. Out of scope

- **Slug auto-deduplication** (`-2`, `-3` suffixes). The caller picks
  a different title; surface the collision rather than silently
  renaming.
- **`before_section` locator.** `after_section` covers the
  documented use-case (Vestige Obs #21 placed the new heading after
  the prior phase block). Add `before_section` later if a real
  request needs it.
- **Section deletion or renaming.** This verb only creates; deletes
  remain an Edit-only operation today.
- **`setForceRoadmapCommitFailForTest` seam.** A feature-test seam
  forcing `QSaveFile::commit()` to fail (so the rollback contract is
  asserted, not just inherited from Qt) is not built here. The
  single-bullet append path (ANTS-1424) has the same gap on the
  ROADMAP.md leg; symmetry is intentional. Not tracked as a separate
  ROADMAP item — the gap is documented but not actionable today.
- **Stable-prefix project support.** Section creation does not
  allocate an id (no `.roadmap-counter` touch — § 2.7), so unlike
  `op:append` the stable-prefix gate does not apply; this verb works
  on stable-prefix roadmaps. No refusal is needed.

## 6. Tests

Feature test: `tests/features/mcp_roadmap_log_create_section/`.
Covers INV-1..INV-11. Label `features;fast`. Per
`docs/standards/testing.md`, each test must be verified to fail
against pre-fix source before the fix is restored. The test bundle:

1. Builds a synthetic ROADMAP.md in a `QTemporaryDir`, calls
   `cmdRoadmapLogCreateSectionForTest(req)`, asserts both the
   envelope and the resulting file content.
2. Drives the refusal-code matrix (missing fields, bad_level,
   bad_section, bad_case, slug_collision, unrecognised_format).
3. Source-greps the RemoteControl sources (`ANTS_RC_SOURCES`) for the `op == "create_section"`
   dispatch branch in `cmdRoadmapLog`.

Each test asserts against a freshly-built temporary roadmap; no
hand-rolled before/after fixtures.

## 7. Cross-doc impact

- `docs/standards/mcp-error-codes.md` — append `bad_level`,
  `bad_intro`, `slug_collision`, and (catch-up from pre-existing
  usage by `cmdRoadmapLogAppend`) `bad_case` and `no_roadmap` to the
  canonical taxonomy.
- `src/claudeintegration.cpp` — extend the `roadmap_log` schema
  description (`claudeintegration.cpp:5669+`): widen the `op` enum
  in the description string and the `op` property's `enum` array to
  include `"create_section"`; add the four new optional params
  (`after_section`, `level`, `title`, `intro_body`) to the schema's
  `properties` block. No new top-level tool.
- `src/remotecontrol_roadmap_query_verb.cpp` — two edits to `cmdRoadmapLog`:
  1. Insert `if (op == "create_section") return cmdRoadmapLogCreateSection(req);`
     ahead of the `if (op != "append")` refusal,
     otherwise `op:"create_section"` falls through to
     `bad_op_combo`.
  2. Update the `bad_op_combo` error string to
     include `create_section` in the expected-op list.
- ANTS-5315's § 2.3a, when built: `mcp-error-codes.md`'s `bad_title` row
  gains the version cases, and the `after_section` schema description says
  it may be omitted for a level-2 version title.
- ROADMAP.md ANTS-1878 — flip to ✅ once shipped, with a
  resolution note pointing at the spec.
- CHANGELOG — one line under the next release's "Added".

## 8. Cold-eyes loop log

Rows live in [`../reviews/ANTS-1878-roadmap-log-op-create-section-loop-log.md`](../reviews/ANTS-1878-roadmap-log-op-create-section-loop-log.md).
