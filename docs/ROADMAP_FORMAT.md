<!-- ants-roadmap-format: 1 -->
# ROADMAP_FORMAT — v1

A shareable contract for `ROADMAP.md` files. Following this spec is
**opt-in but strongly encouraged** for any project shared with Claude
Code (or any LLM-driven workflow): adherence means tools (the Ants
Terminal Roadmap dialog, LLM agents, future automation) can parse the
file deterministically — and the implementation order is unambiguous.

This spec is intended to be readable by both humans and LLMs. A
project-template author can drop it into a new repo unchanged, and a
sibling Claude Code session can be told *"follow
`docs/ROADMAP_FORMAT.md`"* and produce a roadmap that renders
correctly in the Ants Roadmap viewer the first time.

---

## 1. Why a format

Roadmaps drift. Without a contract:

- Status emojis vary between sections (📋 vs ☐ vs `[ ]`).
- Insertion order is ambiguous — when a new audit finding lands, does
  it go at the top? At the bottom? Renumbered into existing items?
- "What is currently being worked on?" is invisible to anyone who
  isn't the author.
- Numbering schemes break under insertion (e.g. items 1–10 then a new
  audit finding becomes 11, even though it's higher priority than 3).

This spec settles those questions once. Conforming files get
deterministic rendering, stable cross-references, and a clear
execution order that LLM agents can follow without "jumping around".

---

## 2. File header

A conforming file declares the format version with an HTML comment in
the **first five lines**:

```markdown
<!-- ants-roadmap-format: 1 -->
# MyProject — Roadmap
```

Parsers look for the marker; if absent, they fall back to best-effort
parsing. Conforming files render with a `(format v1)` badge in the
Roadmap dialog footer.

---

## 3. Heading hierarchy

| Level | Use | Example |
|-------|-----|---------|
| `#` | File title (one per file) | `# MyProject — Roadmap` |
| `##` | Release block | `## 0.7.0 — shell integration (target: 2026-06)` |
| `###` | Theme group within a release | `### 🎨 Features` |
| `####` | Optional subgroup | `#### Tier 1 — ship-this-week` |

The Roadmap dialog treats:
- `##` as a release boundary (entries collapse/expand by release).
- `###` as the theme filter (per-section).
- `####` as a fold-out within a release.

**Headings are addressable.** The viewer auto-generates anchor names
of the form `roadmap-toc-N` based on heading position. For stable
cross-references, embed an explicit anchor:

```markdown
<a name="release-0-7-0"></a>
## 0.7.0 — shell integration (target: 2026-06)
```

Explicit anchors take precedence and survive heading edits.

---

## 4. Status emojis

Every actionable bullet starts with one of four status emojis:

| Emoji | Meaning |
|-------|---------|
| ✅ | Done / shipped |
| 🚧 | In progress (being tackled now) |
| 📋 | Planned (next up) |
| 💭 | Considered (research phase; scope or feasibility uncertain) |

Plain narration bullets without a status emoji are allowed but won't
match any status filter — they render as context-only.

**Status transitions** follow `💭 → 📋 → 🚧 → ✅`. A bullet can skip
🚧 if the work is small enough to ship in one commit, but the
expectation is "💭 means we don't know yet, 📋 means it's queued, 🚧
means I'm doing it right now, ✅ means it's shipped."

---

## 5. Theme emojis

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

Projects MAY introduce additional theme emojis; the viewer's filter
panel will surface any emoji it sees in any `###` heading.

---

## 6. Bullet structure

```markdown
- 📋 [PROJ-0123] **One-line headline ending with a period.** Body
  spanning as many lines as needed; lines wrapped to roughly 70
  columns. Cite `file:line` in backticks when relevant. End with a
  `Lanes:` line declaring which subsystems own the work.
  Lanes: SubsystemA, SubsystemB.
```

Required pieces:
- **Status emoji** — first character after `- `.
- **Stable ID** — `[PROJ-NNNN]` immediately after the emoji.
- **Bold headline ending in a period** — stands alone as a one-line
  summary; this is what the dialog filters and the LLM agent reads
  first.

Optional pieces:
- **Body prose** — free-form, after the bold headline.
- **`Lanes: X, Y, Z`** — declares ownership; helps subagents find
  test files.
- **`Kind: <kind>`** — declares the type of work, when it's not
  obvious from the section context. See §6.3.
- **`Source: <source>`** — declares where the item came from, when
  the section heading doesn't already make that clear. See §6.3.
- **Sub-bullets** — for parametrised work (e.g. "implement for X / Y
  / Z").

### 6.1 Stable IDs — `[PROJ-NNNN]`

The ID is a project-prefixed monotonic integer:

- **Prefix** — 4–6 ASCII letters, all caps. One per project. Pick
  something short and grep-friendly. Examples: `ANTS`, `MYPRJ`,
  `ENGINE`, `OBS`, `R5`.
- **Number** — zero-padded to 4 digits minimum (`0001`, `0042`,
  `1234`). Pad wider once a project crosses 9999.
- **Append-only** — once assigned, an ID never changes. It survives
  rewording, moving, status flips, and even being deleted (a deleted
  ID is *retired*; the next new bullet uses the next free number, not
  the deleted one).

The high-water mark lives in `.roadmap-counter` at the project root —
a one-line file with the highest assigned integer. New IDs increment
this counter atomically. Concurrent sessions read-modify-write under
a brief flock so collisions are impossible. The counter file is
checked into git so the next session starts from the right number.

```bash
# Allocate the next ID:
echo $(($(cat .roadmap-counter) + 1)) > .roadmap-counter
printf "PROJ-%04d\n" $(cat .roadmap-counter)
```

### 6.2 Insertion order vs numbering

This is the rule that everything else hangs on:

> **Execution order is positional. Numbering is identity.**

Items in a section are executed **top-to-bottom**, regardless of
their IDs. The ID identifies the bullet permanently; the position in
the file declares its priority. When new items are inserted (e.g. a
`/audit` finding):

1. **Insert at the position they should be tackled.** A
   CRITICAL audit finding goes near the top of the active release
   block (under the Tier-1 heading if one exists). A LOW finding
   goes lower. The author *chooses* the position based on priority.
2. **Assign the next free ID.** Don't shuffle existing IDs to keep
   the section monotonic — that's the anti-pattern this spec
   prevents.
3. **Document the priority in the bullet body.** A line like
   `Priority: CRITICAL — security blocker` makes the position
   choice auditable.

This means a section's IDs may be **non-monotonic** in document order
(e.g. `0003, 0017, 0004, 0012`). That is correct and expected. The
agent reads the file top-to-bottom and works the items in that
order.

### 6.3 Kinds and Sources

The numbering system itself is uniform — every actionable bullet
gets exactly one ID, regardless of what kind of work it represents.
But different kinds of work have different follow-through (a
documentation fix doesn't need a regression test; an audit-fix
does), and different sources need traceability (a finding that came
from a user report should remain attributable years later). Two
optional metadata fields cover this without adding complexity to
the bullet's surface form.

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

If a bullet's kind is obvious from the section context (e.g. a
bullet under `### 🔍 Audit fold-in (2026-04-28)` is implicitly
`audit-fix`), the `Kind:` line MAY be omitted. If the bullet does
something atypical for its section (a `doc-fix` filed under 🐛
Regressions because the doc bug *manifests* as a user-visible
issue), declare the kind explicitly.

**Recognised `Source:` values:**

| Source | Meaning |
|--------|---------|
| `planned` | On the roadmap from project design (default; usually omitted) |
| `user-YYYY-MM-DD` | User report on date YYYY-MM-DD |
| `audit-YYYY-MM-DD` | `/audit` skill output on date YYYY-MM-DD |
| `indie-review-YYYY-MM-DD` | `/indie-review` skill output on date YYYY-MM-DD |
| `doc-review-YYYY-MM-DD` | Documentation review on date YYYY-MM-DD |
| `static-analysis` | cppcheck / clazy / semgrep / ruff / bandit ad-hoc |
| `regression` | Item was previously ✅ but a later change broke it |
| `external-CVE-NNNN-NNNN` | Public CVE / advisory triggering this work |
| `upstream-<dep>` | Driven by a dep / library upstream change |

Like `Kind:`, `Source:` MAY be omitted if it's obvious from the
section heading (which is the canonical way per §9). The fields
exist for items where the heading-based inheritance isn't enough.

**Example with explicit metadata:**

```markdown
- 📋 [PROJ-0234] **Fix typo in OSC 8 contract section.** PLUGINS.md
  references `osc-8-handler` as the API name; the actual symbol is
  `osc8-handler`. Update three callsites in the doc.
  Kind: doc-fix.
  Source: doc-review-2026-04-15.
  Lanes: docs.
```

A bullet with no `Kind:` / `Source:` is implementation work for the
planned roadmap (kind=`implement`, source=`planned`). That's the
overwhelming majority case, so the format stays terse for it.

### 6.4 LLM-agent execution contract

When an LLM agent (Claude Code, Codex, etc.) is told *"work the
roadmap"*, it MUST:

1. Read the file top-to-bottom.
2. Skip past `##` release blocks until it finds the **active release**
   (the lowest version `##` that contains any 📋 or 🚧 items).
3. Within the active release, find the first non-✅ bullet under each
   `###` theme section, prioritising 🚧 over 📋.
4. Tackle bullets in document order — *not* in ID order.
5. When inserting new bullets (e.g. from an audit), follow §6.2.

Do **not** "jump around" by ID. Do **not** reorder existing items to
fit a perceived priority — let the human author make priority
decisions through positioning.

---

## 7. Current-work signaling

The viewer marks a bullet as "currently being tackled" using three
signals OR'd together:

### 7.1 Primary — 🚧 status emoji

Author flips the bullet's emoji from 📋 to 🚧 when starting, and from
🚧 to ✅ when shipping. This is the **canonical, author-controlled**
signal — every other mechanism is an augmenter.

**One bullet, one author.** A repository should have at most a small
handful of 🚧 bullets at any time (typical: 1–3). Many 🚧 bullets is
a smell — either work is fragmented or the author has stopped
shipping.

### 7.2 Secondary — `CHANGELOG.md` `[Unreleased]` block

The viewer reads the project's `CHANGELOG.md` for an `[Unreleased]`
section (Keep-a-Changelog convention). Bullets in `[Unreleased]` are
fuzzy-matched against ROADMAP bullet headlines (lowercase, hyphens
as spaces, punctuation stripped). Matches get the highlight even if
their emoji hasn't been flipped to 🚧.

This catches the case where the author writes the changelog entry
before updating the roadmap.

```markdown
# CHANGELOG.md

## [Unreleased]

### Added
- Live search — incremental filter as the user types.
- Per-tab session memory.

### Fixed
- HIGH — Search escapes regex metachars.
```

The corresponding ROADMAP bullets get auto-highlighted.

### 7.3 Tertiary — recent commit subjects

The last 5 non-merge / non-revert / non-release-bump commit subjects
on the current branch are fuzzy-matched against bullet headlines. A
match adds the highlight.

Useful for "I just committed this; mark it as in-progress before I
write the changelog" workflows.

### 7.4 The combination

A bullet is "currently being tackled" iff **any** of (🚧, in
`[Unreleased]`, in last-5 commit subjects) matches. The dialog draws
a yellow left-border highlight on every matching bullet. The author
controls the primary signal explicitly; the secondary and tertiary
signals act as fall-backs so the surface stays accurate even when
the author hasn't synced everything by hand yet.

---

## 8. Release blocks

A release block is a `##` heading naming a version + theme + target
date:

```markdown
## 0.7.0 — shell integration (target: 2026-06)

**Theme:** OSC 133 + trigger system + project-audit dashboard.
```

The `**Theme:**` line is optional but recommended — it gives the
filter dialog one-line context per release.

Released versions move from `(target: YYYY-MM)` to
`shipped (YYYY-MM-DD)`. The viewer treats released blocks as
read-only: items under them are expected to be ✅ and don't appear in
the 📋/🚧/💭 filters.

---

## 9. Findings fold-in subsections

When an external review produces new items — `/audit`,
`/indie-review`, a documentation review, a user bug report,
static-analysis run, an upstream advisory — fold them into a
dedicated `###` subsection inside the active release block, with
date and source stamped on the heading. The pattern is the same
regardless of where the finding came from; only the theme emoji and
heading wording change.

```markdown
### 🐛 Regressions reported post-0.7.55 (user, 2026-04-28)

- 📋 [ANTS-0512] **HIGH — Background-tasks button no longer shows up.**
  Locked-in invariant from 0.7.32+ is passing in CI but the button
  is missing in the running binary. Triage path:
  `git log --oneline -- src/mainwindow.cpp | head -20`. Lanes:
  MainWindow, ClaudeBgTasks.

### 🔍 Audit fold-in (2026-04-28)

- 📋 [ANTS-0518] **CRITICAL — SARIF export not atomic.**
  `auditdialog.cpp:3530` raw `QFile::Truncate` may leak partial
  reports on crash. Switch to `QSaveFile + commit()`.
  Lanes: AuditDialog.

### 🔍 Indie-review fold-in (2026-04-23)

- 📋 [ANTS-0521] **HIGH — TerminalGrid / TerminalWidget cohesion smell.**
  Cross-cutting flag from 4 lanes — too much grid mutation in the
  widget. Refactor to push pixel-only concerns into the widget.
  Lanes: TerminalGrid, TerminalWidget.

### 📚 Documentation review fold-in (2026-04-15)

- 📋 [ANTS-0530] **PLUGINS.md OSC 8 surface mismatches code.**
  Doc says `osc-8-handler`, code uses `osc8-handler`. Three
  callsites. Kind: doc-fix.
  Lanes: docs.

### 🐛 Static-analysis fold-in (2026-04-12)

- 📋 [ANTS-0535] **MEDIUM — cppcheck `nullPointerArithmetic`.**
  `terminalgrid.cpp:1402` — `cells[col].combining + offset` when
  `combining` may be nullptr on freshly-cleared rows. Guard with
  null check.
  Lanes: TerminalGrid.
```

Conventions for any findings fold-in:
- **Choose the theme emoji from §5.** 🐛 for bug-shaped findings,
  🔍 for audit/review fold-ins as a whole, 📚 for doc reviews, 🔒
  if the finding is security-only, 📦 if it's packaging.
- **Date-stamp the heading** — `(YYYY-MM-DD)` so the source of the
  finding is visible.
- **Source-stamp the heading** — `(user, ...)`, `(audit, ...)`,
  `(indie-review, ...)`, `(static-analysis, ...)`,
  `(doc-review, ...)`, `(cppcheck, ...)`, etc. — whatever names the
  finding's actual origin.
- **Severity in the headline** — `**CRITICAL — ...**`,
  `**HIGH — ...**`, `**MEDIUM — ...**`, `**LOW — ...**` matches
  audit-tool taxonomies. Doc-fix bullets typically don't carry a
  severity tag (the headline alone is enough).
- **Position by priority** — Tier-1 / CRITICAL items go above
  existing Tier-2 / HIGH items in the release. Don't append blindly
  to the end.
- **Kind/Source lines are usually inherited from the section.** A
  bullet under `🔍 Audit fold-in (2026-04-28)` defaults to
  `Kind: audit-fix`, `Source: audit-2026-04-28` — only declare them
  on the bullet if the work shape is atypical for its section.

---

## 10. The CHANGELOG.md companion

A conforming project keeps a Keep-a-Changelog-style `CHANGELOG.md`
sibling file with an `[Unreleased]` section at the top:

```markdown
# Changelog

## [Unreleased]

### Added
- New feature ...

### Fixed
- Bug fix ...

## [0.7.55] — 2026-04-28

...
```

The roadmap viewer reads `[Unreleased]` for the §7.2 current-work
signal. When a release ships, the entry moves from `[Unreleased]` to
a dated section, and the corresponding ROADMAP bullets flip from 🚧
to ✅.

---

## 11. Anti-patterns

Things conforming files do **not** do:

- ❌ Use emoji other than ✅ 🚧 📋 💭 for status. Tools won't recognise
  them.
- ❌ Renumber items when inserting. The whole point of stable IDs is
  to defeat this temptation.
- ❌ Use multiple status emojis on one bullet (`✅ 📋 ...`). One per
  bullet, period.
- ❌ Reorder bullets by ID. Position is priority; numerical order is
  not.
- ❌ Have more than ~3 🚧 bullets simultaneously. Pick a thing,
  finish it, move on.
- ❌ Write a release block without `**Theme:**` (encouraged) or the
  target date.
- ❌ Mix `[ ]` / `[x]` task-list syntax with the emoji status system.
  Pick one (this spec mandates emojis).

---

## 12. Minimal example

```markdown
<!-- ants-roadmap-format: 1 -->
# MyProject — Roadmap

> Current version: 0.5.0. See [CHANGELOG.md](CHANGELOG.md) for shipped
> work; this file covers what's planned.

**Legend**

- ✅ Done · 🚧 In progress · 📋 Planned · 💭 Considered

---

## 0.5.0 — shipped (2026-04-15)

### 🎨 Features

- ✅ [MYPRJ-0001] **Dark mode.** Adds a system-aware theme switcher.
  Lanes: Settings, MainWindow.

## 0.6.0 — search bundle (target: 2026-05)

**Theme:** make the existing data discoverable — full-text search,
inline filtering, escaped-regex search.

### 🎨 Features

- 🚧 [MYPRJ-0002] **Live search.** Incremental filter as the user
  types. Lanes: SearchBar.
- 📋 [MYPRJ-0003] **Tabbed history.** Per-tab session memory. Lanes:
  SessionManager.

### 🔍 Audit fold-in (2026-04-28)

- 📋 [MYPRJ-0017] **HIGH — Search escapes regex metachars.** Audit
  finding 2026-04-28 — current search treats `.` as wildcard
  unintentionally. Priority: HIGH — visible from the search bar.
  Lanes: SearchBar.

### 🔒 Security

- 📋 [MYPRJ-0018] **CRITICAL — XSS in search results.** Audit
  finding 2026-04-28 — unescaped HTML in result snippets. Priority:
  CRITICAL — bypassable from any indexed page. Lanes: SearchBar.

### 📚 Documentation review fold-in (2026-04-25)

- 📋 [MYPRJ-0019] **README install steps stale.** Doc says
  `apt install foo`; package was renamed to `foo-cli` upstream.
  Update three references. Kind: doc-fix.
  Lanes: docs.
```

Notice:
- ID 0017 was inserted between 0002 and 0003 (in the 🎨 Features
  list) because the audit finding belongs in that section and is
  higher priority than 0003.
- ID 0018 was added under 🔒 Security — a different theme group, so
  it gets its own section.
- ID 0019 is a doc-fix from a documentation review — same
  numbering policy, different theme (📚) and different `Kind:`.
  No regression test required because the work is doc-only.
- Neither shuffled existing IDs.
- The 🚧 bullet (0002) is currently being tackled; the rest are
  queued in document order.

---

## 13. Migration from non-conforming roadmaps

A project adopting v1 mid-life can migrate incrementally:

1. **Add the `<!-- ants-roadmap-format: 1 -->` header.**
2. **Initialise `.roadmap-counter`** to a number larger than any ID
   the project might already use (e.g. `1000` if you want to start
   issuing IDs from `PROJ-1001` going forward).
3. **Assign IDs to new bullets only.** Existing ✅ items don't need
   IDs retroactively — leave them as-is. Existing 📋 / 🚧 / 💭 items
   SHOULD get IDs as they're touched.
4. **Confirm status emojis.** Replace any non-canonical markers
   (`[ ]`, ☐, `TODO:`) with the four canonical emojis.
5. **Confirm heading hierarchy.** The viewer expects `##` for
   release blocks, `###` for theme groups.

Partial conformance still renders — the viewer falls back to
best-effort for anything it doesn't recognise.

---

## 14. Versioning of this spec

This is **v1**. Future revisions will increment the format-marker
number (`<!-- ants-roadmap-format: 2 -->`). The Ants Terminal
Roadmap dialog will support all known versions; projects can pin to
v1 indefinitely if v2 introduces unwanted complexity.

Backwards-incompatible changes (renaming a status emoji, changing
the ID syntax) require a major version bump. Additive changes (a new
theme emoji, a new optional metadata field) stay on the current
version.
