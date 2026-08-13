<!-- ants-roadmap-format-spec: 1.1 -->
# ROADMAP.md & CHANGELOG.md format spec (v1.1)

> Detailed format spec for the two files the Ants Terminal Roadmap
> dialog parses deterministically. Extracted from
> [`documentation.md`](documentation.md) so the documentation standard
> can stay short for projects that don't use the Ants viewer.
>
> Read this file when authoring a `ROADMAP.md` bullet, a `CHANGELOG.md`
> entry, or any tooling that consumes either format. Skip otherwise.

## Contents

- [3. ROADMAP.md format spec](#3-roadmapmd-format-spec)
  - [3.1 File header](#31-file-header)
  - [3.2 Heading hierarchy](#32-heading-hierarchy)
  - [3.3 Status emojis](#33-status-emojis)
  - [3.4 Theme emojis](#34-theme-emojis)
  - [3.5 Bullet structure](#35-bullet-structure)
  - [3.6 Current-work signaling](#36-current-work-signaling)
  - [3.7 Release blocks](#37-release-blocks)
  - [3.8 Findings fold-in subsections](#38-findings-fold-in-subsections)
  - [3.9 Archive rotation](#39-archive-rotation)
  - [3.10 Compatibility with GFM task lists](#310-compatibility-with-gfm-task-lists)
  - [3.11 ROADMAP anti-patterns](#311-roadmap-anti-patterns)
- [4. CHANGELOG.md format spec](#4-changelogmd-format-spec)
  - [4.1 Structure](#41-structure)
  - [4.2 Conventions](#42-conventions)
  - [4.3 Release flow with ROADMAP integration](#43-release-flow-with-roadmap-integration)
- [Cold-eyes loop log](#cold-eyes-loop-log)

## 3. ROADMAP.md format spec

A shareable contract for `ROADMAP.md` files. Following this
sub-spec is **required** for any roadmap intended to render
correctly in the Ants Terminal Roadmap dialog or be parsed
deterministically by LLM agents.

The roadmap is the single place to track unshipped work, and a
release writes its own account of what shipped into the CHANGELOG.

**That does not mean a closed bullet leaves the roadmap when it
ships.** Closed items stay in `ROADMAP.md` — carrying the
**headlines** §§ 3.6.2 and 3.6.3 fuzzy-match CHANGELOG entries and
commit subjects against, and the IDs § 4.2's CHANGELOG citations
resolve — until § 3.9 rotates a *closed minor* out into an archive. Rotation is the size-management mechanism, not the
CHANGELOG. On a store-backed project `roadmap-data-model.md` § 7.5
makes this binding on the render: its only membership exclusions are
`internal` and `dropped` items, so a generated `ROADMAP.md` that
omitted shipped work would not conform.

### 3.1 File header

A conforming file declares the format version with an HTML
comment in the **first five lines**:

```markdown
<!-- ants-roadmap-format: 1 -->
# MyProject — Roadmap
```

Parsers look for the marker; if absent, they fall back to
best-effort parsing. Conforming files render with a `(format v1)`
badge in the Roadmap dialog footer.

### 3.2 Heading hierarchy

| Level | Use | Example |
|-------|-----|---------|
| `#` | File title (one per file) | `# MyProject — Roadmap` |
| `##` | Release block (post-1.0), phase block (pre-1.0), or a § 3.8 fold-in hoisted from `###` | `## 0.7.0 — shell integration` / `## P01 — Bootstrap` / `## FP03 — review fold-in` |
| `###` | Theme group within a release/phase | `### 🎨 Features` |
| `####` | Optional subgroup | `#### Tier 1 — ship-this-week` |

The Roadmap dialog treats `##` as a top-level boundary (release
or phase), `###` as the theme filter, `####` as a fold-out.
Pre-1.0 projects use phase blocks (`## P01 — Bootstrap`) since
there's no real version to anchor to yet. The designator is `P`
followed by digits, optionally with a `.<sub>` for a phase inserted
after the sequence was set (`P07.5`, live in one corpus project).
§ 3.9 names archives after it verbatim and § 3.5.4 step 2 selects
the active phase by its number, so the designator is load-bearing,
not decoration.

**At 1.0, whatever phase work is still in `ROADMAP.md` becomes the
body of `## 1.0.0 — initial release`** — the active phase and any
above it. Phases § 3.9 has already rotated stay archived under
their phase names: they are not renamed, and a reader keeps
accepting the phase archive form after 1.0. Do not promote *early*
to obtain rotation — § 3.9's closing note is why that is a
different organising axis rather than a rename.

**A `## FP<NN>` or `## DS<NN>` block is neither a release nor a
phase.** It is a § 3.8 fold-in hoisted from `###`, which one corpus
project does; § 3.5.4 step 2 and § 3.9 both give the shape defined
behaviour, so a parser must accept it, but § 3.8 is still where a
new fold-in goes.

**Headings are addressable.** The viewer auto-generates anchor
names of the form `roadmap-toc-N` from each heading's *position*
in the document (`tocAnchorAt` in `roadmapdialog.cpp`); the TOC
sidebar scrolls to those. The anchor is positional, so it shifts
when a heading is inserted or removed above it — there is no
edit-stable heading anchor today.

Hand-embedded `<a name="…">` anchors are **not** honored: no code
path scans the roadmap body for them or gives them precedence over
the positional ones, so don't rely on them for cross-references.

### 3.3 Status emojis

Every actionable bullet starts with one of four status emojis:

| Emoji | Meaning |
|-------|---------|
| ✅ | Done / shipped |
| 🚧 | In progress (being tackled now) |
| 📋 | Planned (next up) |
| 💭 | Considered (research phase; scope or feasibility uncertain) |

Plain narration bullets without a status emoji are allowed but
won't match any status filter — they render as context-only.

**Status transitions** follow `💭 → 📋 → 🚧 → ✅`. A bullet can
skip 🚧 if the work is small enough to ship in one commit, but
the expectation is "💭 means we don't know yet, 📋 means it's
queued, 🚧 means I'm doing it right now, ✅ means it's shipped."

### 3.4 Theme emojis

Theme emoji prefixes the level-3 (`###`) section heading:

| Emoji | Theme |
|-------|-------|
| 🎨 | Features (user-visible capabilities) |
| ⚡ | Performance |
| 🔌 | Plugins / extensibility |
| 🖥 | Platform (ports, accessibility, OS-specific) |
| 🔒 | Security |
| 🧰 | Dev experience (tooling, tests, build, CI) |
| 📚 | Documentation (user docs, dev docs, READMEs, contracts) |
| 📦 | Packaging & distribution |
| 🐛 | Bug fixes / regressions |
| 🔍 | Audit / review findings fold-in |
| 🧹 | Cleanup / debt — dead code, stale comments, drift, deferred housekeeping |
| 📝 | Cold-eyes documentation review fold-in (spec / ADR / README sweeps) |

Projects MAY introduce additional theme emojis; the viewer's
filter panel surfaces any emoji it sees in any `###` heading.

### 3.5 Bullet structure

```markdown
- 📋 [PROJ-0123] **One-line headline ending with a period.** Body
  spanning as many lines as needed; lines wrapped to roughly 70
  columns. Cite `file:line` in backticks when relevant.
  Kind: implement.
  Lanes: SubsystemA, SubsystemB.
```

Required pieces:

- **Status emoji** — first character after `- `.
- **Stable ID** — `[PROJ-NNNN]` immediately after the emoji.
- **Bold headline ending in a period** — stands alone as a
  one-line summary; this is what the dialog filters and the LLM
  agent reads first.
- **`Kind: <kind>.`** — declares the type of work. One of the
  values in §3.5.3. **Required as of v1.1** so the Roadmap
  viewer (and any tooling that consumes the file
  deterministically) can categorise without inferring from the
  surrounding section heading. The dominant Kind for a section
  may be inherited implicitly via a section-level convention,
  but the canonical bullet form carries the field explicitly
  to make every bullet self-describing.

  Two properties of the label, both settled by ANTS-4065 § 2.2:

  - **The trailer may be written INLINE**, trailing a prose sentence
    rather than on a continuation line of its own (`… not in this
    fold. Kind: doc-fix.`). Supported, not merely tolerated — 99
    bullets in this project write that shape against 1,435 own-line.
    The parser was anchored to a line start until 2026-08-09 and
    defaulted every one of them to `implement` in silence, which is
    the loss that spec exists to stop.
  - **The label is CASE-SENSITIVE**, reversing ANTS-3407 for this one
    field. Case tolerance was safe only while the anchor held the
    label to a line start; un-anchored, `… changed the kind: of work
    we do …` would parse as a declaration. `Lanes:` has never had the
    tolerance for the same reason. So a hand-typed `kind:` / `KIND:`
    no longer parses — accepted because once a project is on the
    roadmap store the render is the sole writer of this file.

  Two guards limit what un-anchoring admits. A label inside
  backticks does not declare anything (a bullet *quoting* `Kind:` is
  writing about the format), and where a bullet carries more than
  one match the **last** wins — that is the one the render authored,
  and taking the first would let stale prose overwrite the canonical
  value on every regeneration.

Optional pieces:

- **Body prose** — free-form, after the bold headline.
- **`Lanes: X, Y, Z`** — declares ownership; helps subagents
  find test files.
- **`Source: <source>`** — declares where the item came from,
  when the section heading doesn't already make that clear. See
  §3.5.3.
- **`Layman: <one-sentence summary>.`** — a non-technical
  one-line summary, written for a vibe-coder / non-programmer
  reader. When present, the Ants Roadmap dialog (ANTS-1154)
  shows this on the card face instead of the bold headline; the
  headline still appears when the card is expanded. Falls back
  to the bold headline if absent. Sits after the body prose,
  before `Kind:` / `Lanes:` / `Source:`. Case-insensitive label.
- **`Evidence: <path1>, <path2>`** — optional file paths (screenshots,
  logs, repros) that evidence the item — e.g. a bug diagnosed from a
  screenshot. Comma-separated; a comma or newline *inside* a path is
  folded to a space so one path can't break the single-line field.
  Rendered WITHOUT a trailing period (paths contain dots, so a sentence
  period would read as part of the last path). Written by `roadmap_log
  op:append` / `op:append_batch` via their `evidence: […]` arg and
  echoed by `roadmap_query` as an `evidence` array (omitted when the
  bullet has none) so a later session can re-locate the files.
  **Case-INsensitive label.** ANTS-3382 introduced it case-sensitive and
  ANTS-3407 gave it `CaseInsensitiveOption` for parity with `Layman:`;
  this line said "case-sensitive" until 2026-08-12 and the code had
  disagreed with it since. Verified against `rxEvidence()` in
  `src/roadmapparse.cpp`. `Kind:`, `Lanes:` and `Source:` are the
  case-SENSITIVE ones — the opposite grouping to what both this file and
  the global copy previously implied.
- **Sub-bullets** — for parametrised work (e.g. "implement for X
  / Y / Z").

#### 3.5.1 Stable IDs — `[PROJ-NNNN]`

The ID is a project-prefixed monotonic integer:

- **Prefix** — a short project tag of `[A-Za-z0-9_-]` that
  **contains at least one ASCII letter**. All-caps 4–6 letters is the
  convention (`ANTS`, `MYPRJ`, `ENGINE`, `RETRO`); a digit-led,
  letter-containing prefix is also accepted (`3D_E`), so a project whose
  scheme starts with a digit can be fetched/flipped by ID (ANTS-3492). A
  letter-free prefix (`2026`) is NOT an ID — that keeps a date/version
  bracket like `[2026-07]` from being mistaken for one. Multi-prefix repos
  (e.g. `Sh-`, `Ed-`, `Phase-`) are permitted under § 3.10.4; the
  `\[(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-\d+\]` regex
  accepts any letter-containing, dash-then-digit token.
- **Number** — zero-padded to 4 digits minimum (`0001`, `0042`,
  `1234`). Pad wider once a project crosses 9999.
- **Append-only** — once assigned, an ID never changes. It
  survives rewording, moving, status flips, and even being
  deleted (a deleted ID is *retired*; the next new bullet uses
  the next free number, not the deleted one).

On a project the roadmap store does not serve, and whose roadmap is not
pass-headings — the carrier table below states exactly which those are,
and it is not simply "has no store row" — the high-water mark lives in
`.roadmap-counter` at the project root: a one-line file with the highest
assigned integer. An absent file reads as 0 rather than failing. New IDs
increment this counter atomically, and concurrent sessions
read-modify-write under a brief flock so collisions are impossible.

**The counter is a derived, per-machine cache — NOT source (ANTS-3450).**
It is `.gitignore`d, not committed. Its true value is the highest
`PROJ-NNNN` id across the committed roadmap corpus — `ROADMAP.md` +
`CHANGELOG.md` + `docs/roadmap/*.md` (the archives that shipped/rotated
bullets migrate into). Every allocation *floors* to that committed
high-water mark (`RoadmapFoldIn::corpusHighWater`), so a stale, wiped, or
fresh-clone-absent counter can never reissue a live or migrated id **that the
corpus carries** — see the caveat below for the ids it does not — it is
recovered from committed content on first use. This removes a whole class
of "the counter bump got left out of the commit" drift: git can't drift a
file it doesn't track.

**Which carrier is authoritative during the store cutover (ANTS-3809).**
Projects move onto the roadmap store
([`roadmap-data-model.md`](roadmap-data-model.md)) one at a time, so the id
high-water lives in more than one place. **"Store-migrated" below means a
project has a store row *and* a roadmap this format's detector classifies as
`ants-v1`** — the pair `RoadmapSource::migratedProject()` tests. `ants-v1` is
the detector's name for a file conforming to this standard; its siblings are
`github-task-list` for a task list (§ 3.10.3) and `pass-headings` for § 3.10.5's
shape — the three format literals the store's `source_format` CHECK admits
beside `''` ("not recorded"), spelled as the detector emits them. The
detector answers on the § 3.1 marker where one is present and on a best-effort
parse where it is not, so a roadmap **SHOULD** carry that marker: without it
the classification — and therefore the carrier — is best-effort rather than
deterministic. Store-migration is **not**
§ 3.10.3's migration, which converts a GFM task list into this format and is a
fact about the file, not about the store; a project can be one without the
other.

| Project | High-water carrier | `.roadmap-counter` |
|---|---|---|
| **Store-migrated** — store row *and* `ants-v1` | the store's `id_high_water` row, per `(project, prefix)`, **floored to the committed corpus exactly as above** | neither read nor written; a stale file is simply left behind |
| **Pass-headings** (§ 3.10.5), store row or not | derived from the heading, never allocated | neither read nor written |
| **Everything else** — no store row, *or* a store row whose roadmap is not `ants-v1` | `.roadmap-counter`, exactly as above | read and written |

On a store-migrated project an allocation is
`max(idHighWater(project, prefix), corpusHighWater(root, prefix)) + 1`,
followed by `raiseIdHighWater()`. An absent `id_high_water` row is *not* an
error — it is the state of every project until its first store-side
allocation, and is read as 0, exactly as an absent counter file is above.

**Concurrent allocations are serialised by the store transaction, not by the
flock above.** The read-modify-write runs inside the `BEGIN IMMEDIATE` the
whole write op holds, so two sessions cannot interleave — the same guarantee
the counter's flock gives, reached by a different mechanism.

**This does not overturn ANTS-3450.** The store row is a second derived cache,
not a second source: it lives outside the repo (under `GenericDataLocation`),
so it is machine-local exactly as the gitignored counter is, and the corpus is
still what either one is recovered from — which is why the `max()` above keeps
the floor. Dropping it on the store path would re-open the very failure the
rule above closes: a fresh clone reissuing an id the committed corpus already
holds.

`id_strategy: "stable_prefix"` (a caller-supplied string id such as
`Ts20-SP6`) consults neither carrier and raises neither, on either path: a
stable string id is not a counter value, and seeding a counter from one
would corrupt the next counter-project allocation. Such an id need **not**
match the `-\d+` grammar above — that grammar governs the ids this scheme
*allocates*, not the ones a caller pins, and failing it does not make the id
malformed (`roadmap-data-model.md` § 7.1 states what it is worth instead).

This is the **interim** rule, named as such because the cutover is not
finished: [`roadmap-data-model.md`](roadmap-data-model.md) § 8 records that
once the store's export is published on a cadence (ANTS-3794), the export —
not the committed corpus — becomes the authoritative floor for a
store-migrated project. Until then the committed corpus above is that floor
for every project, store-migrated or not.

**And on a store-migrated project that floor has a hole, which is why the
interim is an interim.** `roadmap-data-model.md` § 7.5 keeps `internal` and
`dropped` items off the render by policy, so their ids appear in **no**
committed file — not `ROADMAP.md`, not an archive, not the CHANGELOG. A corpus
scan cannot see them, so on such a project the corpus is not a sufficient floor
on its own and an absent `id_high_water` row must **not** be treated as a safe
0. Only the store row covers those ids today; the export (ANTS-3794) is subject
to neither exclusion, which is the whole reason it supersedes both.

**Illustrative only — this is NOT the allocation path.** It shows the
counter's shape, not how an id is issued: real allocation goes through
`roadmap_log`, which takes the lock above and floors to the committed corpus.

```bash
# What the counter file holds -- NOT an allocator.
cat .roadmap-counter        # e.g. 42  ->  the last id issued was PROJ-0042
```

**Extended into an allocator — reading the counter, adding one, formatting the
result — it reissues live ids on a fresh clone**, where the gitignored counter
is absent, the read yields nothing and the next id is `PROJ-0001`: the exact
failure the two rules above promise is impossible. This block deliberately
does not show that form. (Adopted from the global copy 2026-08-12; this file
previously shipped the runnable version.)

#### 3.5.2 Insertion order vs numbering

This is the rule that everything else hangs on:

> **Execution order is positional. Numbering is identity.**

Items in a section are executed **top-to-bottom**, regardless of
their IDs. The ID identifies the bullet permanently; the
position in the file declares its priority. When new items are
inserted (e.g. a `/audit` finding):

1. **Insert at the position they should be tackled.** A
   CRITICAL audit finding goes near the top of the active
   release block (under the Tier-1 heading if one exists). A LOW
   finding goes lower. The author *chooses* the position based
   on priority.
2. **Assign the next free ID.** Don't shuffle existing IDs to
   keep the section monotonic — that's the anti-pattern this
   sub-spec prevents.
3. **Document the priority in the bullet body.** A line like
   `Priority: CRITICAL — security blocker` makes the position
   choice auditable.

This means a section's IDs may be **non-monotonic** in document
order (e.g. `0003, 0017, 0004, 0012`). That is correct and
expected. The agent reads the file top-to-bottom and works the
items in that order.

#### 3.5.3 Kinds and Sources

The numbering system itself is uniform — every actionable bullet
gets exactly one ID, regardless of what kind of work it
represents. But different kinds of work have different
follow-through (a documentation fix doesn't need a regression
test; an audit-fix does), and different sources need
traceability (a finding from a user report should remain
attributable years later). Two metadata fields cover this without
adding complexity to the bullet's surface form — `Kind:` required
as of v1.1 (§ 3.5), `Source:` optional.

**Recognised `Kind:` values:**

| Kind | Meaning | Follow-through |
|------|---------|----------------|
| `implement` | New code for a planned feature | tests + changelog + docs |
| `feature` | User-visible capability addition (alias for implement; preferred in UX-facing bullets) | tests + changelog + docs |
| `enhancement` | Incremental improvement to an existing feature | tests + changelog |
| `fix` | Code change to repair a bug | regression test + changelog |
| `audit-fix` | Code change in response to an audit finding | regression test + changelog (cite finding source) |
| `review-fix` | Code change in response to an indie-review or peer review | regression test + changelog (cite reviewer source) |
| `doc` | New / updated documentation, no code | changelog if user-facing |
| `doc-fix` | Documentation correction (typo, stale ref, drift) | no test, changelog optional |
| `refactor` | Code reshape with no behavior change | tests must still pass; usually no changelog |
| `test` | Test-only change (new spec, new fixture, harness improvement) | no changelog |
| `chore` | Housekeeping (deps, build flags, generated files) | no test, changelog optional |
| `release` | Version bump, packaging files, tag | drives the release skill |
| `perf` | Performance improvement (latency, throughput, memory) | benchmark or before/after in changelog |
| `security` | Security hardening, CVE fix, permission change | changelog + advisory if public |
| `investigate` | Triage / root-cause work that may not produce code | investigation note or roadmap annotation |
| `research` | Exploratory / feasibility work | journal artifact or decision doc |
| `accessibility` | A11y fix or improvement | changelog if user-facing |
| `optimize` | Resource or algorithmic optimisation (overlap with perf; prefer perf for latency) | changelog optional |
| `package` | Packaging, distribution, installer, Flatpak/AUR/Homebrew changes | release notes |
| `marketing` | Website, README, social, demo video, launch post | no test |
| `ux` | UX or interaction design work | design doc or mockup |

**Required as of v1.1** — every actionable bullet declares its
`Kind:` explicitly, even when the surrounding section makes the
default obvious. Section context is a hint for human readers;
machine consumers (the Roadmap dialog, the App-Build runner,
any tooling that filters / counts / reports by Kind) need the
field on every bullet so the parser stays simple and one-pass.
A backfill pass over the active roadmap is a `Kind: doc-fix`
item.

**Recognised `Source:` values:**

| Source | Meaning |
|--------|---------|
| `planned` | On the roadmap from project design (default; usually omitted) |
| `user-YYYY-MM-DD` | User report on date YYYY-MM-DD |
| `audit-YYYY-MM-DD` | `/audit` skill output on date YYYY-MM-DD |
| `code-quality-review-YYYY-MM-DD` | `/code-quality-review` skill output on date YYYY-MM-DD. **Adopted from the global standard 2026-08-12**; the old spelling `indie-review-YYYY-MM-DD` still parses and existing bullets keep it — nothing reads `Source:` values, so this is traceability, not a format break. Write the new one. |
| `debt-sweep-YYYY-MM-DD` | `/debt-sweep` skill output on date YYYY-MM-DD |
| `doc-review-YYYY-MM-DD` | Documentation review on date YYYY-MM-DD |
| `static-analysis` | cppcheck / clazy / semgrep / ruff / bandit ad-hoc |
| `regression` | Item was previously ✅ but a later change broke it |
| `external-CVE-NNNN-NNNN` | Public CVE / advisory triggering this work |
| `upstream-<dep>` | Driven by a dep / library upstream change |

Most `/debt-sweep` findings get fixed inline during the sweep
itself (the skill's "trivial" bucket goes straight into a
`chore: post-X.Y.Z debt sweep` commit) and never reach the
roadmap. Only items the user must rule on (the "behavioural"
bucket) or items deferred as out-of-scope land here. Use
`🧹 Debt-sweep fold-in (YYYY-MM-DD)` as the section heading and
`Source: debt-sweep-YYYY-MM-DD` if declared explicitly.

**A bullet with no `Kind:` / `Source:` reads as implementation work
for the planned roadmap (`Kind: implement`, `Source: planned`) — and
that is a reader-side fallback for pre-v1.1 bullets, not permission
to omit the field.** § 3.5 makes `Kind:` a required piece as of
v1.1; a newly authored bullet without one does not conform, even
though every reader will still classify it. The fallback exists
because the field was introduced against a corpus that predates it —
two in five items carry no `Kind:` — and dropping those items or
refusing to read them was never an option.

#### 3.5.4 LLM-agent execution contract

When an LLM agent (Claude Code, Codex, etc.) is told *"work the
roadmap"*, it MUST:

1. Read the file top-to-bottom.
2. Skip past `##` release blocks until it finds the **active
   release** (the lowest version `##` that contains any 📋 or 🚧
   items) — or, on a pre-1.0 phase-block file (§ 3.2), the
   **lowest-numbered `## P<NN>` block** containing any.
   **Numbered, not first in document order.** Live phase roadmaps
   interleave `## FP<NN>` fold-in and `## DS<NN>` debt-sweep blocks
   above and below the phase sequence — one corpus project opens
   with `FP03 FP04 FP05 FP06 FP01` and closes with `DS01 FP02`,
   wrapping `P01`…`P10` — so position does not track the sequence
   and reading top-to-bottom selects a fold-in as the active phase.
   Those blocks carry no phase number and are never selected: they
   are § 3.8 fold-ins hoisted to `##`, so treat each as an extra
   theme section of the active phase, worked in step 3 after that
   phase's own themes.
3. Within the active release, find the first non-✅ bullet under
   each `###` theme section, prioritising 🚧 over 📋.
4. Tackle bullets in document order — *not* in ID order.
5. When inserting new bullets (e.g. from an audit), follow
   §3.5.2.

Do **not** "jump around" by ID. Do **not** reorder existing
items to fit a perceived priority — let the human author make
priority decisions through positioning.

### 3.6 Current-work signaling

The Roadmap dialog marks a bullet as "currently being tackled"
using three signals OR'd together:

#### 3.6.1 Primary — 🚧 status emoji

Author flips the bullet's emoji from 📋 to 🚧 when starting, and
from 🚧 to ✅ when shipping. This is the **canonical,
author-controlled** signal — every other mechanism is an
augmenter.

**One bullet, one author.** A repository should have at most a
small handful of 🚧 bullets at any time (typical: 1–3). Many 🚧
bullets is a smell — either work is fragmented or the author has
stopped shipping.

#### 3.6.2 Secondary — `CHANGELOG.md` `[Unreleased]` block

The viewer reads the project's `CHANGELOG.md` for an
`[Unreleased]` section (Keep-a-Changelog convention; see §4).
Bullets in `[Unreleased]` are fuzzy-matched against ROADMAP
bullet headlines (lowercase, hyphens as spaces, punctuation
stripped). Matches get the highlight even if their emoji hasn't
been flipped to 🚧.

This catches the case where the author writes the changelog
entry before updating the roadmap.

#### 3.6.3 Tertiary — recent commit subjects

The last 5 non-merge / non-revert / non-release-bump commit
subjects on the current branch are fuzzy-matched against bullet
headlines. A match adds the highlight.

Useful for "I just committed this; mark it as in-progress before
I write the changelog" workflows.

### 3.7 Release blocks

A release block is a `##` heading naming a version + theme +
target date:

```markdown
## 0.7.0 — shell integration (target: 2026-06)

**Theme:** OSC 133 + trigger system + project-audit dashboard.
```

The `**Theme:**` line is optional but recommended — it gives
the filter dialog one-line context per release.

Released versions move from `(target: YYYY-MM)` to
`shipped (YYYY-MM-DD)`. The viewer treats released blocks as
read-only: items under them are expected to be ✅ and don't
appear in the 📋/🚧/💭 filters.

### 3.8 Findings fold-in subsections

When an external review produces new items — `/audit`,
`/code-quality-review`, a documentation review, a user bug report,
static-analysis run, an upstream advisory — fold them into a
dedicated `###` subsection inside the active release block, with
date and source stamped on the heading. The pattern is the same
regardless of where the finding came from; only the theme emoji
and heading wording change.

```markdown
### 🐛 Regressions reported post-0.7.55 (user, 2026-04-28)

- 📋 [ANTS-0512] **HIGH — Background-tasks button no longer shows up.**
  …

### 🔍 Audit fold-in (2026-04-28)

- 📋 [ANTS-0518] **CRITICAL — SARIF export not atomic.** …

### 🔍 Indie-review fold-in (2026-04-23)

- 📋 [ANTS-0521] **HIGH — TerminalGrid / TerminalWidget cohesion smell.**
  …

### 📚 Documentation review fold-in (2026-04-15)

- 📋 [ANTS-0530] **PLUGINS.md OSC 8 surface mismatches code.**
  Doc says `osc-8-handler`, code uses `osc8-handler`.
  Kind: doc-fix.
  Lanes: docs.

### 🐛 Static-analysis fold-in (2026-04-12)

- 📋 [ANTS-0535] **MEDIUM — cppcheck `nullPointerArithmetic`.** …

### 🧹 Debt-sweep fold-in (2026-04-28)

Trivial findings were fixed inline during the sweep — see
`chore: post-0.7.55 debt sweep` commit. The bullets below are
the "behavioural" findings the user opted to defer.

- 📋 [ANTS-0540] **`tests/features/vt_throughput/` invariant
  list grew but spec.md unchanged.** Kind: test. Lanes: tests.
- 📋 [ANTS-0541] **`README.md § Plugins` references removed
  `ants.fs.read`.** Kind: doc-fix. Lanes: docs.

### 📝 Cold-eyes 2026-04-30

> Docs reviewed: N. Loops to clean: L. Findings fixed: F.

- Flat bullets, no tier grouping (every finding was already
  fixed in-place by the cold-eyes orchestrator — this subsection
  is the audit trail, not a backlog).
- Use the `📝 Cold-eyes <YYYY-MM-DD>` heading (no parenthesised
  date suffix; the date is in the heading itself). The blockquote
  line gives reviewers a one-line summary.
```

Conventions for any findings fold-in:

- **Choose the theme emoji from §3.4.** 🐛 for bug-shaped
  findings, 🔍 for audit/review fold-ins as a whole, 📚 for doc
  reviews, 🔒 if security-only, 📦 if packaging.
- **Date-stamp the heading** — `(YYYY-MM-DD)`.
- **Source-stamp the heading** — `(user, …)`, `(audit, …)`,
  `(indie-review, …)`, `(static-analysis, …)`,
  `(doc-review, …)`, `(cppcheck, …)`, etc.
- **Severity in the headline** — `**CRITICAL — …**`,
  `**HIGH — …**`, `**MEDIUM — …**`, `**LOW — …**`.
- **Position by priority** — Tier-1 / CRITICAL items go above
  existing Tier-2 / HIGH items.
- **Kind/Source lines are usually inherited from the section**
  for readability — but the canonical bullet still carries
  `Kind:` explicitly on every actionable item (§3.5.3); section
  context is only a human hint, never a substitute for the
  required field.

### 3.9 Archive rotation

When a `ROADMAP.md` grows past ~150 KiB, split closed minors out
into per-minor archive files.

**The size figure is a review trigger, not a rotation event.** On a
versioned roadmap the only rotation event is a minor or major bump
(below) — a phase-block roadmap's occasion is phase closure instead,
owned by the phase bullet, and every unqualified mention of `/bump`
in this section is the versioned rule. So a file
that crosses 150 KiB part-way through a minor has nothing eligible
to rotate — every section under the open minor stays put — and it
stays over the threshold until that minor closes. Crossing it is
therefore a signal to check that rotation is still happening at
all, not a breach on its own. There is deliberately no within-minor
rotation: splitting an open minor would move bullets that are still
being edited, and § 3.6's ID stability is worth more than the
bytes.

The convention:

- Archives live at `<dir(ROADMAP.md)>/docs/roadmap/<MAJOR>.<MINOR>.md`
  — one file per closed minor version, named verbatim
  (`0.5.md`, `0.6.md`, `0.7.md`).
- File names follow the **case-sensitive regex**
  `^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.md$`. No leading `v`, no
  `roadmap-` prefix, no zero-padding (`0.7.md` not `00.07.md`), **no
  patch suffix** (`0.7.0.md` is rejected — archives are per-minor
  only). The alternation is what rejects the padding: a plainer
  `[0-9]+` accepts `00.07.md` and would leave the regex and the
  sentence beside it disagreeing, with a reader loading an archive the
  prose forbids.
- Tooling that reads archives sorts numerically by the
  `(major, minor)` integer tuple, descending — lexical sort breaks
  on minor 10 (`0.10` < `0.9` lexically). Numeric sort is the
  contract; the naming rule (above) is what makes it parseable.
- **A pre-1.0 roadmap using phase blocks (`## P01 — …`, § 3.2)
  rotates on a closed *phase*, exactly as a versioned one rotates
  on a closed minor.** Archives live at
  `<dir(ROADMAP.md)>/docs/roadmap/<designator>.md` with the
  designator copied **verbatim** from the heading — `P01.md`,
  `P07.5.md` — under the case-sensitive regex
  `^P[0-9]+(\.[0-9]+)?\.md$`. Verbatim is what keeps one name per
  block: the zero-padding width is the project's business, and
  copying it means `P01` can never also archive as `P1`.
  **Phase archives sort as a separate, strictly older class** —
  every version archive first, descending by `(major, minor)`, then
  every phase archive, descending by `(phase, sub)` with an absent
  `sub` reading as 0. The two cannot share one key space: `P01.md`
  and `1.0.md` both parse to `(1, 0)`. Two classes need no tiebreak,
  because § 3.2 makes phases pre-1.0 only and promotion one-way, so
  no phase can postdate any version block.

  **The occasion is the phase closing, not a bump.** A pre-1.0
  project on phase blocks may never bump a version at all, so
  keying rotation to `/bump` as the next bullet does would mean it
  never fires. A phase is closed when every actionable bullet under
  it is ✅ — no 📋, no 🚧 and **no 💭**, that last being live
  research-phase work (§ 3.3) which archiving would hide from
  `roadmap-query` and so from every agent working the file. **Every
  closed phase rotates except the highest-numbered one in the file,
  which always stays**: it is where new work is filed, and stating
  it that way also settles the all-closed file — no phase is active,
  and rotation still has a referent. The size rule at the head of
  this section applies unchanged.

  **`## FP<NN>` and `## DS<NN>` blocks do not rotate on their
  own.** They are § 3.8 fold-in subsections that some projects
  hoist to `##`; they carry no phase number, so there is nothing to
  name an archive after and no position saying which phase they
  closed with. File a fold-in where § 3.8 puts it — inside the
  block it was raised against — and it rotates with that block.
  **A project already carrying hoisted `##` fold-ins moves them back
  in order to archive them** — the heading's date and source (§ 3.8)
  say which block each was raised against. Until moved they stay in
  `ROADMAP.md`, which is a real cost rather than a formality:
  LocalWebServerManager carries seven in that shape, of seventeen
  `##` blocks.

  **No shipped reader accepts this name yet, and the failure is
  silent — so do not rotate a phase block by hand until they
  widen.** The rule is specified and unimplemented. Four sites
  enforce the version-only form: `parseArchiveFilename`
  (`src/roadmapdialog.cpp`), which makes the viewer **skip** a
  non-conforming entry with no message; `archiveNameRx()`
  (`src/roadmapmigrate.cpp`); `isPlaceableSourcePath`
  (`src/roadmapmigrateload.cpp`); and `rotate_minor`'s `kMinorRx`
  (`src/remotecontrol_roadmap_log.cpp`). A `P01.md` written today
  is read by none of them, so the block would leave `ROADMAP.md`
  and reach no reader. ANTS-4073 owns widening them.

  > Global `roadmap-format.md` § 3.9 said until 2026-08-13 that
  > phase roadmaps do not rotate and should promote to release
  > blocks instead. A corpus re-scan that day found four projects
  > live on phase blocks — LocalWebServerManager (17 blocks, 2881
  > lines), finbreak (14, 6472), Games_Hub (3, 556),
  > Ants_Projects_Hub_Website (2, 253) — with finbreak up 99 lines
  > on the 2026-08-12 count and 373 on the 2026-08-10 one. Promoting would have
  > all four rewrite a plan sequence (`P01 Bootstrap` … `P13
  > Packaging`) as a release sequence, which is a different
  > organising axis rather than a rename. The cost that answer was
  > avoiding — a second archive naming scheme — is one added regex
  > reusing the sort contract already stated. Decided here per
  > CFG-0069 and raised into the global copy (ANTS-4073).
- **On a versioned roadmap** rotation happens at `/bump` time on a
  minor or major bump only. Patch bumps don't rotate. The `/bump` recipe (`.claude/bump.json`
  on each project) owns the snip-and-create step. Rotation is
  content-preserving: every bullet under the closed minor's
  `## <closed>.0 — …` heading and its sub-headings moves to
  `docs/roadmap/<closed>.md` byte-identical, then the heading and
  bullets are removed from `ROADMAP.md`.
- The viewer (Ants Terminal's `RoadmapDialog`) reads archives only
  on demand — when the user picks the History preset or types in
  the search box. Default render stays cheap.
- The `roadmap-query` IPC verb (Ants ANTS-1117) reads only the
  current `ROADMAP.md`. Archives are dialog-only by contract.
  **The contract survives cutover, carried differently.** On a
  store-migrated project there is no "current file" to read, so
  archive scope stops being a consequence of which file is parsed
  and becomes an explicit choice by the caller: the read seam takes
  an include-archive flag and, when it is not set, returns only
  sections with no archive path. Same result set, chosen rather
  than implied.

Spec for the viewer's archive-load path:
[`tests/features/roadmap_viewer_archive/spec.md`](../../tests/features/roadmap_viewer_archive/spec.md)
(Ants project; the standard owns the layout, the spec owns the
viewer behaviour).

**On a store-migrated project (§ 3.5.1), rotation is a store write and the
snip-and-create step above does not apply.** Everything else here still holds:
the naming regex, the per-minor rule, the numeric sort, and the archives' place
in the committed corpus are unchanged, and the archives are still real files at
the same paths. What changes is who writes them. Both `ROADMAP.md` and
`docs/roadmap/*.md` are rendered from the store, which records per section the
file it belongs to — so rotating a closed minor means **reassigning that
minor's sections to the archive path and re-rendering**, after which both files
land with their content preserved. Snipping the markdown by hand instead is
discarded at the next render, silently and with no error, which is the failure
this paragraph exists to prevent. `roadmap-data-model.md` § 8 owns the
reasoning.

The operation is **`roadmap_log op:"rotate_minor"`** (Ants ANTS-4070), taking
the closed `<MAJOR>.<MINOR>` and nothing else. **It is implemented and it is
not reachable** — the handler exists, but the verb's `op` dispatch carries
only `append`, `append_batch`, `flip`, `flip_batch`, `annotate`,
`amend_body`, `create_section` and `bundle_row`, so a call reaches
`bad_op_combo` and none of the refusal codes below can fire. Do not build a
`/bump` recipe on it yet; ANTS-4081 owns the wiring, and § 4.3's
`retitle_section` is unreachable for the same reason and by the same commit.
Three things about it are part of this convention rather than that project's
implementation detail:

- **The archive path is derived, never passed** — `docs/roadmap/<M>.<N>.md`,
  relative to the **project root**, so the naming regex above stays stated in
  one place. A caller-supplied path could name a file the migration's own
  archive discovery then refuses to read back.
- **Sections are selected by TITLE, and only a release designator matches** —
  `<M>.<N>` optionally preceded by `v`, followed by end-of-title, a character
  that is neither a digit nor a `.`, or a `.` and a digit. That is what makes
  `## 0.70.0` not part of minor 0.7, and what keeps a two-minor signpost like
  `## 0.5.x and 0.6.x — archived` out of the archive it points at.
- **A moved section is re-slugged**, because the import prefixes every archive
  slug with its file's minor. Reassigning the path alone would leave the store
  holding a slug no re-import derives, and the next import would add a second
  section beside the first.

It refuses rather than guessing: a minor still holding an open item
(`minor_not_closed`), a move that would leave the live file with no sections or
free a slug a remaining section was disambiguated out of (`bad_args`), and a
project that is not store-migrated (`op_unsupported` — there, the snip-and-create
step above is still the answer).

**It does not enforce the rotation event.** This section says rotation happens on
a minor or major bump only, and the operation cannot see a version transition —
a minor holds zero open bullets routinely, just after a patch release. `/bump`
owns that check, because `/bump` is the only caller that knows a minor just
closed.

### 3.10 Compatibility with GFM task lists

The wider markdown ecosystem has a sibling convention — GitHub
Flavored Markdown (GFM) **task lists**:

```markdown
- [ ] Build login screen
- [x] Wire CI cache
```

GFM task lists are the canonical GitHub convention for ad-hoc
to-do tracking in markdown. **This spec's emoji-bullet format
extends GFM task lists, it does not replace them.** The
extensions are the parts that GFM doesn't model:

- A four-state taxonomy (✅ 🚧 📋 💭) instead of the GFM
  two-state checkbox (`[x]` / `[ ]`).
- Stable IDs (`[PROJ-NNNN]`) for cross-doc reference.
- Required `Kind:` metadata line per bullet (§ 3.5); optional
  `Source:` / `Layman:` metadata for provenance and
  non-technical readers.

#### 3.10.1 Semantic equivalence

| GFM      | This spec     | Meaning                |
|----------|---------------|------------------------|
| `[ ]`    | 📋            | Planned, not started.  |
| `[x]`    | ✅            | Done / shipped.        |
| *(none)* | 🚧            | In progress.           |
| *(none)* | 💭            | Idea / not yet planned.|

The two GFM states map cleanly to two of this spec's four.
GFM has no native syntax for "in progress" or "speculative" —
projects that need them on a GFM-task-list roadmap either
adopt the full emoji set or annotate with a prose prefix
(`- [ ] (WIP) Build login screen`). The emoji set is
strictly more expressive.

#### 3.10.2 Reader-side adapter mode

Ants Terminal's MCP verbs (`roadmap_query`, `roadmap_log`) and
the `RoadmapDialog` viewer can read GFM-task-list roadmaps
without requiring migration. See **ANTS-1428** for the adapter
implementation; the contract is that `[ ]` / `[x]` bullets are
surfaced through the same envelope shape as emoji bullets, with
the missing fields (`Kind:`, `Source:`, etc.) returned as
empty strings rather than parse errors. Projects that prefer
GFM stay on GFM; projects that adopt the full Ants format get
the extra surface.

#### 3.10.3 Migration

A project that wants the full emoji-bullet format from a
GFM-task-list starting point converts in five passes:

0. Add the § 3.1 format marker `<!-- ants-roadmap-format: 1 -->`, so
   the result classifies `ants-v1` deterministically rather than by
   best-effort parse — which is what selects the id carrier (§ 3.5.1).
1. Replace `- [x]` with `- ✅`, `- [ ]` with `- 📋`.
2. Assign stable IDs (`[PROJ-NNNN]`) bottom-up against a fresh
   `.roadmap-counter` (§ 3.5.1). **The counter is the carrier for
   this step even on a project that already has a store row**, because
   the file does not yet classify `ants-v1` and § 3.5.1's table puts
   "a store row whose roadmap is not `ants-v1`" on the counter. The
   carrier table hands over to `id_high_water` once step 0 lands, and
   the counter goes unread from then on.
3. Add `Kind:` and `Source:` lines under each bullet (§ 3.5.3).
4. Add `Layman:` summaries (§ 3.5 Bullet structure).
5. **On a project with a store row, re-run the store migration**, so
   its recorded `source_format` becomes `ants-v1`. Without this the
   store still records `github-task-list` while the live file
   classifies `ants-v1`, and `roadmap-data-model.md` § 4.1.1 refuses
   every subsequent store write on that mismatch. This step is what
   makes the conversion *sanctioned* rather than a dialect change
   behind the store's back.

**On a project that already has a store row, do the whole conversion
in one uncommitted working-tree edit and add the marker last.** Store
row plus `ants-v1` is the definition of store-migrated (§ 3.5.1), so
the moment the file classifies `ants-v1` the store owns it: from
there steps 1–4 are hand edits to a generated file, which
`roadmap-data-model.md` § 10 forbids and the next render discards
silently — while the store path is unusable too, its publish gate
being unmet until step 4 fills the `Layman:` lines in.

**Adding the marker last reduces that window but does not close
it**, because the marker is not the only route to `ants-v1`: the
detector falls back to a best-effort parse, and step 1 alone —
replacing `- [x]` / `- [ ]` with emoji bullets — can be enough for
that parse to classify the file. So the ordering rule is necessary
and not sufficient; what makes it safe is that no render runs against
a half-converted tree. Do not commit, and do not invoke a store write
or any `roadmap_log` op, between step 1 and step 0. A project with no
store row has no such window and may keep step 0 first.

The migration is reversible — write `[x]` / `[ ]` back, drop
the metadata, and the file is GFM again.

#### 3.10.4 Prefix conventions

This spec uses **one prefix per repo** (`ANTS-`, `VESTIGE-`,
…) by convention. Repos with multiple work streams sometimes
prefer **multi-prefix** schemes (`SH-`, `ED-`, `PHASE-`) for
lane visibility. Multi-prefix is permitted. Mixed-case prefixes
like `Sh-`, `Ed-`, `mame-curator-` parse, are fetched / flipped,
**and** are allocated fine — id handling is case-insensitive on
both the read and the write side. The one uppercase-only check in
the tooling is a narrow `op:flip` anchor helper, not id handling:

- **Id parsing — case-insensitive.** The `roadmap-query` parser
  accepts any letter-containing, dash-then-digit token
  (`(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-\d+`), so
  mixed-case prefixes **do** parse and can be fetched / flipped by
  id. Same regex family §3.5.1 documents — see the bullet-scan and
  `kIdIsh` patterns in `remotecontrol.cpp` and
  `RoadmapIndex::isCanonicalId`.
- **Id allocation — also case-insensitive.** The `roadmap_log`
  `id_prefix` argument (the counter-prefix override for
  `op:append` / `op:append_batch`) is validated by the *same*
  letter-containing, case-insensitive grammar (`kIdPrefixShape` in
  `remotecontrol.cpp`), which is deliberately looser than the
  helper below so a repo can pin a lowercase / mixed-case prefix
  (e.g. `mame-curator`). So a repo can mint new `Sh-` / `Ed-` ids
  directly.
- **The lone uppercase-only rule — `op:flip`'s `prefix_hint`.**
  That argument is validated `^[A-Z][A-Z0-9_-]{0,15}$` (`rxPrefix`
  in `remotecontrol.cpp`), but it is used *only* when injecting a
  caret anchor onto a GFM bullet that has no id — it never
  constrains id allocation.

The single-prefix rule is convention because it keeps
`.roadmap-counter` unambiguous: the file holds a single integer and
has no per-prefix form. **No per-prefix counter filename is defined,
by this standard or by any tool** — so a multi-prefix project that
is *not* store-migrated has no working counter carrier for its
second and later prefixes, and allocates them from the
committed-corpus high-water mark alone (§ 3.5.1). Do not invent a
name for one: two tools inventing different names each read zero for
the other's prefix, which reissues live IDs, and that is a worse
failure than the slower corpus scan.

That limitation is the counter carrier's alone: a store-migrated
project's `id_high_water` row is keyed per `(project, prefix)`
(§ 3.5.1), so multi-prefix is native there and needs no second file.
**A multi-prefix project is the strongest reason to cut over.**

#### 3.10.5 Heading-format roadmaps (`#### Pass N.M`)

Some projects (RetroDB-style) track work as `#### Pass N.M
<title>` level-4 headings with a `- **Status**: <word>` sub-bullet
instead of `- <emoji> [ID] **headline**` bullets (§ 3.5's order —
the ID sits immediately after the status emoji, never after the
headline). The reader classifies
these as `pass-headings` and synthesises a `PASS-<major>-<minor>
[-<sub>]` id per heading (**ANTS-1530**). Since **ANTS-2126**
`roadmap_log` also **writes** this format: `op:"append"` (needs a
`pass` arg, e.g. `"43.5"`), `op:"append_batch"`, `op:"flip"` /
`op:"flip_batch"` (locate by the synthesised `PASS-N-M` id or
`headline`), and `op:"annotate"`. Pass ids are derived from the
heading, never from `.roadmap-counter` (the counter is left
untouched). `op:"create_section"` is not yet supported and still
refuses `format_mismatch`.

### 3.11 ROADMAP anti-patterns

- ❌ Status emoji other than ✅ 🚧 📋 💭. Tools won't recognise
  them.
- ❌ Renumbering items when inserting. The whole point of stable
  IDs is to defeat this temptation.
- ❌ Multiple status emojis on one bullet (`✅ 📋 …`).
- ❌ Reordering bullets by ID. Position is priority; numerical
  order is not.
- ❌ More than ~3 🚧 bullets simultaneously.
- ❌ Mixing `[ ]` / `[x]` task-list syntax with the emoji
  status system on the same bullet (the formats coexist at
  file scope per § 3.10, but not at bullet scope).
- ❌ Reading or bumping `.roadmap-counter` on a store-migrated
  project (§ 3.5.1). Its carrier is the store's `id_high_water`
  row; the file is stale there by construction.
- ❌ Allocating from that row **without** the committed-corpus
  floor under it (§ 3.5.1). That is the silent drop which lets a
  fresh clone reissue a live ID.


## 4. CHANGELOG.md format spec

A conforming project keeps a Keep-a-Changelog-style
`CHANGELOG.md` at the repo root. The format is defined at
<https://keepachangelog.com/> and pinned here as a sub-spec.

### 4.1 Structure

```markdown
# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- New feature.

### Fixed
- Bug fix.

## [X.Y.Z] — YYYY-MM-DD

**Theme:** one-line summary of the release.

### Added
…

### Changed
…

### Fixed
…

### Removed
…

### Security
…

## [X.Y.Z-1] — YYYY-MM-DD
…
```

### 4.2 Conventions

- `[Unreleased]` block at the top, **always** — even if empty.
  The ROADMAP viewer reads it for current-work signaling per
  §3.6.2.
- Dated sections in **reverse chronological order**.
- `**Theme:**` line is one sentence; sets the release's
  character.
- Bullets categorical: Added / Changed / Fixed / Removed /
  Security. Don't invent new categories.
- Bullets terse — one line each. Body paragraphs go in commits.
- **Cite ROADMAP IDs** in bullets when applicable: `Added: live
  search filter (ANTS-1042).`. The bidirectional link helps
  readers move between the changelog and the roadmap.

### 4.3 Release flow with ROADMAP integration

When a release ships:

1. `[Unreleased]` block contents move to a new dated section
   `## [X.Y.Z] — YYYY-MM-DD`.
2. Empty `[Unreleased]` section is left at the top (with an
   empty-state hint or just the heading).
3. ROADMAP bullets that were 🚧 flip to ✅.
4. Released ROADMAP block changes from `(target: YYYY-MM)` to
   `shipped (YYYY-MM-DD)`.

The `/release` skill (if used) automates steps 1–4.

On a store-migrated project (§ 3.5.1), the four steps split three
ways. Steps 1–2 are unchanged: `CHANGELOG.md` is not generated from
the store, so it stays authored exactly as it is today. Step 3 is a
per-bullet status flip and goes through the store's write path rather
than a text edit. **Step 4 is neither** — it rewrites a release-block
`##` heading (§ 3.7), which is a *section title*, and editing the
rendered heading by hand is discarded at the next render, silently. It
has its own store operation, `roadmap_log op:"retitle_section"`
(Ants ANTS-4070), which takes the section's slug and the new title and
**recomputes the slug from it**: a slug is derived on import from the
heading, never round-tripped, so keeping the old one would make the
store disagree with what the next import derives. The envelope reports
`previous_slug` beside the new `slug` so a caller holding the old
address learns it moved.

Nothing automatically moves shipped items into the CHANGELOG on
either side of cutover, and nothing did before.

---

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
| 4 | 2026-08-13 | 2, cold — **this copy's first gate as a co-subject rather than as a cross-reference.** ANTS-4073's edit was gated on the global copy; loop 2 of that run put both copies in one packet as a tightly-coupled pair, genre pinned `standard` | **Q1 1 · Q3 1** (this file's share of a joint 16) | **Two fixed here, thirteen filed as ANTS-4140, and the split is scope rather than doubt.** Fixed: § 3.9 presented `roadmap_log op:"rotate_minor"` as the rotation operation while the phase paragraph sixty lines above discloses that the archive path is unimplemented — both lanes found it, the live `op` dispatch carries eight ops and `rotate_minor` is not among them, so its three documented refusal codes are unreachable and a `/bump` recipe built on the section always errors; the paragraph now says so and points at ANTS-4081. And the new phase bullet forward-referenced "§ 3.5.4 step 2's no-workable-item state", which **this** copy does not define — its absence is a declared divergence, so the all-closed case is now stated directly instead of by reference. **Filed, not fixed:** this copy has never been gated against the global one, and thirteen findings are its own pre-existing defects — an unwired `retitle_section` in § 4.3, § 3.10.4's `id_prefix` grammar omitting its 16-character cap, `rxPrefix` cited in `remotecontrol.cpp` when it lives in `remotecontrol_roadmap_log.cpp`, § 3.10.3 step 2 numbering against a fresh `.roadmap-counter` that § 3.5.1 of this same file measures as reissuing live ids, "marker last" placing step 5's store write inside the window forbidding store writes, § 3.5.1 reading an absent `id_high_water` row as 0 and forbidding it as a safe 0, `Priority:` prescribed here as a severity word and globally as a band `1`–`5`, and `**Evidence:**` declaring nothing because `rxEvidence` alone carries no bold alternation. Two of those are decisions. **In every one the global copy already holds the correct text, so the repair is to raise it here** — the CFG-0069 direction — and that is a gate of its own, which ANTS-4140 books. |
| 3 | 2026-08-09 | 3, cold — identical packet rebuilt from disk; dispatched only after the blast-radius sweep was re-run over loop 2's rows | **Q1 0 · Q2 5 · Q3 1** (this file's share of a joint 8) | **Exited at the 3-loop cap.** § 3.10.3 gained a **step 5**: its five passes never re-ran the store migration, so a project with a store row finished the conversion recording `github-task-list` against a file now classifying `ants-v1` — the exact mismatch `roadmap-data-model.md` § 4.1.1 refuses every later write on, with the procedure offering no remedy. § 3.5.1 still promised a committed-corpus floor that "can never reissue a live or migrated id", although § 7.5 keeps `internal` and `dropped` items off the render entirely, so their ids reach no committed file and an absent `id_high_water` row is not a safe 0 on a store-migrated project; the caveat now sits on both sides of the pair rather than only in the data model. § 3.10.5 glossed the emoji format as `- **headline** [ID]`, with the ID *after* the headline, where § 3.5 pins it immediately after the status emoji — a parser author following the gloss would scan mid-text for ids, which the data model's § 10 names an anti-pattern. And § 3's preamble — rewritten in loop 1 — said closed bullets carry "their IDs, which is what § 3.6.2's CHANGELOG matching and § 3.6.3's commit matching resolve against"; both of those sections **fuzzy-match headlines** (lowercase, hyphens as spaces, punctuation stripped), and it is § 4.2's CHANGELOG citations that resolve ids. **Filed rather than fixed:** ANTS-4073, because § 3.2 tells a pre-1.0 project to use `## P01 —` phase blocks while § 3.9 rotates only under a `## <minor>.0 —` heading and § 3.5.4 step 2 selects "the lowest version `##`" — so such a roadmap can never rotate, and choosing between scoping § 3.2 and defining phase-block rotation is a decision. |
| 2 | 2026-08-09 | 3, cold — identical packet rebuilt from disk after loop 1's edits | **Q1 1 · Q2 4** (this file's share of a joint 10) | **Three findings were against loop 1's own repairs.** The § 3.10.3 ordering rule was justified by "the marker is what makes the file classify `ants-v1`", which § 3.5.1 contradicts four hundred lines earlier: the detector falls back to a **best-effort parse**, and step 1 alone — replacing `- [x]` / `- [ ]` with emoji bullets — can be enough for it to classify. Marker-last therefore narrows the hand-over window without closing it; the rule now says so and adds what actually makes it safe (no commit, no store write, between step 1 and step 0). Step 2 four lines above still told a store-row project the counter goes unread, which under marker-last is exactly backwards — it is the carrier until the marker lands. **One pre-existing defect was a regex that does not do what the sentence beside it says**: § 3.9 published `^[0-9]+\.[0-9]+\.md$` while forbidding zero-padding, and `[0-9]+` accepts `00.07.md`. The shipped `archiveNameRx()` is `\A(0\|[1-9][0-9]*)\.(0\|[1-9][0-9]*)\.md\z`, which rejects it — so the code implemented the prose and the standard's own regex was wrong. Corrected, then executed against twelve names (`0.10.md`, `10.0.md`, `00.07.md`, `01.7.md`, `0.7.0.md`, `0.7.MD`, a trailing space and others) before it landed. Also recorded: § 3.5.1's carrier table has no comparison against the stored `source_format`, so a project whose dialect changes falls through to the counter row instead of refusing — the counterpart rule now lives in `roadmap-data-model.md` § 4.1.1. **Collateral caught by the post-fix structural check, not by a lane:** loop 1's new loop-log section was missing from § Contents. |
| 1 | 2026-08-09 | 3, cold — gated as a pair with [`roadmap-data-model.md`](roadmap-data-model.md), one shared byte-stable packet carrying the live corpus survey, the pass-headings status reader, archive discovery, render routing and the store DDL; genre pinned `standard` | **Q1 0 · Q2 5 · Q3 3** (this file's share of a joint 11) | **This standard's first gate.** It had never had one; the run was triggered by ANTS-4069 after an authoring edit corrected § 3.5.1's detector literal `gfm` → `github-task-list`, and by the § 9 decision pass that added the post-cutover paragraphs to §§ 3.9 and 4.3. **Two findings were against that new text**: § 4.3 claimed steps 3–4 both edit *bullets*, but step 4 rewrites a release-block `##` heading — a section title with no store operation, so a cut-over project cannot perform it (filed onto ANTS-4070); and § 3's preamble still said released work "moves out of the roadmap into the CHANGELOG", which contradicts `roadmap-data-model.md` § 7.5's rule that closed items are published and would have had a renderer author drop every ✅ item. **Five were pre-existing.** `Kind:` was simultaneously "Required as of v1.1" (§ 3.5) and "two optional metadata fields" whose absence "stays terse" (§ 3.5.3) — two lanes found it independently, and it decides whether a conformance checker rejects a bullet. § 3.10.3 put the format marker at step 0, which hands a store-row project to the store before its four hand-edit steps run, so all four are discarded at the next render while the store path refuses them for an unmet publish gate — the marker now goes last. § 3.9's ~150 KiB threshold reads as a breach at 0.7.104's 3.2 MB although no closed minor exists to rotate; it is now stated as a review trigger, with no within-minor rotation by design. § 3.10.4 required "one counter per prefix — one file each" while no per-prefix filename exists here or in any source file; inventing one would have two tools reading zero for each other's prefix and reissuing live IDs, so both standards now say such a project allocates from the committed-corpus floor alone. And § 3.9's `roadmap-query` bullet ("archives are dialog-only by contract") had no post-cutover answer — the contract survives, carried by the read seam's include-archive flag rather than by which file is parsed. |


