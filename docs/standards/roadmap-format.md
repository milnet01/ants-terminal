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

## 3. ROADMAP.md format spec

A shareable contract for `ROADMAP.md` files. Following this
sub-spec is **required** for any roadmap intended to render
correctly in the Ants Terminal Roadmap dialog or be parsed
deterministically by LLM agents.

The roadmap is the single place to track unshipped work, and a
release writes its own account of what shipped into the CHANGELOG.

**That does not mean a closed bullet leaves the roadmap when it
ships.** Closed items stay in `ROADMAP.md` — carrying their IDs,
which is what § 3.6.2's CHANGELOG matching and § 3.6.3's commit
matching resolve against — until § 3.9 rotates a *closed minor* out
into an archive. Rotation is the size-management mechanism, not the
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
| `##` | Release block (post-1.0) **or** phase block (pre-1.0) | `## 0.7.0 — shell integration` / `## P01 — Bootstrap` |
| `###` | Theme group within a release/phase | `### 🎨 Features` |
| `####` | Optional subgroup | `#### Tier 1 — ship-this-week` |

The Roadmap dialog treats `##` as a top-level boundary (release
or phase), `###` as the theme filter, `####` as a fold-out.
Pre-1.0 projects use phase blocks (`## P01 — Bootstrap`) since
there's no real version to anchor to yet; phase blocks promote
naturally to release blocks once the project ships 1.0 (the work
under `P01` becomes the body of `## 1.0.0 — initial release`).

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
  Case-sensitive label (ANTS-3382).
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
fresh-clone-absent counter can never reissue a live or migrated id — it is
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

```bash
# Allocate the next ID — illustrative, and for the counter carrier only.
# A real allocator takes the flock above, floors to the committed corpus, and
# treats an absent counter file as 0 rather than failing on the `cat`.
echo $(($(cat .roadmap-counter) + 1)) > .roadmap-counter
printf "PROJ-%04d\n" $(cat .roadmap-counter)
```

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
| `indie-review-YYYY-MM-DD` | `/indie-review` skill output on date YYYY-MM-DD |
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
   items).
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
`/indie-review`, a documentation review, a user bug report,
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

**The size figure is a review trigger, not a rotation event.** The
only rotation event is a minor or major bump (below), so a file
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
  `^[0-9]+\.[0-9]+\.md$`. No leading `v`, no `roadmap-` prefix, no
  zero-padding (`0.7.md` not `00.07.md`), **no patch suffix**
  (`0.7.0.md` is rejected — archives are per-minor only).
- Tooling that reads archives sorts numerically by the
  `(major, minor)` integer tuple, descending — lexical sort breaks
  on minor 10 (`0.10` < `0.9` lexically). Numeric sort is the
  contract; the naming rule (above) is what makes it parseable.
- Rotation happens at `/bump` time on a minor or major bump only.
  Patch bumps don't rotate. The `/bump` recipe (`.claude/bump.json`
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
   `.roadmap-counter` (§ 3.5.1) — unless the project already has a
   store row, in which case converting to this format makes it
   store-migrated and § 3.5.1's carrier table applies from that
   moment, leaving the counter unread.
3. Add `Kind:` and `Source:` lines under each bullet (§ 3.5.3).
4. Add `Layman:` summaries (§ 3.5 Bullet structure).

**On a project that already has a store row, do step 0 LAST.** The
marker is what makes the file classify `ants-v1`, and store row plus
`ants-v1` is the definition of store-migrated (§ 3.5.1) — so adding
it first hands the file to the store before steps 1–4 have run. From
that moment those steps are hand edits to a generated file:
`roadmap-data-model.md` § 10 forbids them and the next render
discards them silently, while the store path cannot be used either,
because its publish gate is unmet until step 4 fills the `Layman:`
lines in. Running 1–4 as text edits and adding the marker at the end
converts the file first and hands over once, which is the order that
works. A project with no store row is unaffected and may keep step 0
first.

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
instead of `- **headline** [ID]` bullets. The reader classifies
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
`##` heading (§ 3.7), which is a *section title*, and no store
operation for changing one is defined yet; it is owed alongside
rotation (Ants ANTS-4070), which moves whole sections for the same
reason. Until it exists, a cut-over project cannot perform step 4 at
all: editing the rendered heading by hand is discarded at the next
render, silently.

Nothing automatically moves shipped items into the CHANGELOG on
either side of cutover, and nothing did before.

---

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-09 | 3, cold — gated as a pair with [`roadmap-data-model.md`](roadmap-data-model.md), one shared byte-stable packet carrying the live corpus survey, the pass-headings status reader, archive discovery, render routing and the store DDL; genre pinned `standard` | **Q1 0 · Q2 5 · Q3 3** (this file's share of a joint 11) | **This standard's first gate.** It had never had one; the run was triggered by ANTS-4069 after an authoring edit corrected § 3.5.1's detector literal `gfm` → `github-task-list`, and by the § 9 decision pass that added the post-cutover paragraphs to §§ 3.9 and 4.3. **Two findings were against that new text**: § 4.3 claimed steps 3–4 both edit *bullets*, but step 4 rewrites a release-block `##` heading — a section title with no store operation, so a cut-over project cannot perform it (filed onto ANTS-4070); and § 3's preamble still said released work "moves out of the roadmap into the CHANGELOG", which contradicts `roadmap-data-model.md` § 7.5's rule that closed items are published and would have had a renderer author drop every ✅ item. **Five were pre-existing.** `Kind:` was simultaneously "Required as of v1.1" (§ 3.5) and "two optional metadata fields" whose absence "stays terse" (§ 3.5.3) — two lanes found it independently, and it decides whether a conformance checker rejects a bullet. § 3.10.3 put the format marker at step 0, which hands a store-row project to the store before its four hand-edit steps run, so all four are discarded at the next render while the store path refuses them for an unmet publish gate — the marker now goes last. § 3.9's ~150 KiB threshold reads as a breach at 0.7.104's 3.2 MB although no closed minor exists to rotate; it is now stated as a review trigger, with no within-minor rotation by design. § 3.10.4 required "one counter per prefix — one file each" while no per-prefix filename exists here or in any source file; inventing one would have two tools reading zero for each other's prefix and reissuing live IDs, so both standards now say such a project allocates from the committed-corpus floor alone. And § 3.9's `roadmap-query` bullet ("archives are dialog-only by contract") had no post-cutover answer — the contract survives, carried by the read seam's include-archive flag rather than by which file is parsed. |


