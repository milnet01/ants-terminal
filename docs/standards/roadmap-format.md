<!-- ants-roadmap-format-spec: 1 -->
# ROADMAP.md & CHANGELOG.md format spec (v1)

> Detailed format spec for the two files the Ants Terminal Roadmap
> dialog parses deterministically. Extracted from
> [`documentation.md`](documentation.md) so the documentation standard
> can stay short for projects that don't use the Ants viewer.
>
> Read this file when authoring a `ROADMAP.md` bullet, a `CHANGELOG.md`
> entry, or any tooling that consumes either format. Skip otherwise.

## 3. ROADMAP.md format spec

A shareable contract for `ROADMAP.md` files. Following this
sub-spec is **required** for any roadmap intended to render
correctly in the Ants Terminal Roadmap dialog or be parsed
deterministically by LLM agents.

The roadmap is the single place to track unshipped work. Released
work moves out of the roadmap into the CHANGELOG.

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
names of the form `roadmap-toc-N` based on heading position. For
stable cross-references, embed an explicit anchor:

```markdown
<a name="release-0-7-0"></a>
## 0.7.0 — shell integration (target: 2026-06)
```

Explicit anchors take precedence and survive heading edits.

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

Projects MAY introduce additional theme emojis; the viewer's
filter panel surfaces any emoji it sees in any `###` heading.

### 3.5 Bullet structure

```markdown
- 📋 [PROJ-0123] **One-line headline ending with a period.** Body
  spanning as many lines as needed; lines wrapped to roughly 70
  columns. Cite `file:line` in backticks when relevant. End with
  a `Lanes:` line declaring which subsystems own the work.
  Lanes: SubsystemA, SubsystemB.
```

Required pieces:

- **Status emoji** — first character after `- `.
- **Stable ID** — `[PROJ-NNNN]` immediately after the emoji.
- **Bold headline ending in a period** — stands alone as a
  one-line summary; this is what the dialog filters and the LLM
  agent reads first.
- **`Kind: <kind>.`** — declares the type of work. One of the
  ten values in §3.5.3. **Required as of v1.1** so the Roadmap
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
- **Sub-bullets** — for parametrised work (e.g. "implement for X
  / Y / Z").

#### 3.5.1 Stable IDs — `[PROJ-NNNN]`

The ID is a project-prefixed monotonic integer:

- **Prefix** — 4–6 ASCII letters, all caps. One per project.
  Pick something short and grep-friendly. Examples: `ANTS`,
  `MYPRJ`, `ENGINE`, `OBS`, `R5`.
- **Number** — zero-padded to 4 digits minimum (`0001`, `0042`,
  `1234`). Pad wider once a project crosses 9999.
- **Append-only** — once assigned, an ID never changes. It
  survives rewording, moving, status flips, and even being
  deleted (a deleted ID is *retired*; the next new bullet uses
  the next free number, not the deleted one).

The high-water mark lives in `.roadmap-counter` at the project
root — a one-line file with the highest assigned integer. New
IDs increment this counter atomically. Concurrent sessions
read-modify-write under a brief flock so collisions are
impossible. The counter file is checked into git so the next
session starts from the right number.

```bash
# Allocate the next ID:
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
attributable years later). Two optional metadata fields cover
this without adding complexity to the bullet's surface form.

**Recognised `Kind:` values:**

| Kind | Meaning | Follow-through |
|------|---------|----------------|
| `implement` | New code for a planned feature | tests + changelog + docs |
| `fix` | Code change to repair a bug | regression test + changelog |
| `audit-fix` | Code change in response to an audit finding | regression test + changelog (cite finding source) |
| `review-fix` | Code change in response to an indie-review or peer review | regression test + changelog (cite reviewer source) |
| `doc` | New / updated documentation, no code | changelog if user-facing |
| `doc-fix` | Documentation correction (typo, stale ref, drift) | no test, changelog optional |
| `refactor` | Code reshape with no behavior change | tests must still pass; usually no changelog |
| `test` | Test-only change (new spec, new fixture, harness improvement) | no changelog |
| `chore` | Housekeeping (deps, build flags, generated files) | no test, changelog optional |
| `release` | Version bump, packaging files, tag | drives the release skill |

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

A bullet with no `Kind:` / `Source:` is implementation work for
the planned roadmap (`Kind: implement`, `Source: planned`).
That's the overwhelming majority case, so the format stays terse
for it.

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
- **Kind/Source lines are usually inherited from the section.**

### 3.9 ROADMAP anti-patterns

- ❌ Status emoji other than ✅ 🚧 📋 💭. Tools won't recognise
  them.
- ❌ Renumbering items when inserting. The whole point of stable
  IDs is to defeat this temptation.
- ❌ Multiple status emojis on one bullet (`✅ 📋 …`).
- ❌ Reordering bullets by ID. Position is priority; numerical
  order is not.
- ❌ More than ~3 🚧 bullets simultaneously.
- ❌ Mixing `[ ]` / `[x]` task-list syntax with the emoji
  status system.


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


