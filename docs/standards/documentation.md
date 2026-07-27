<!-- ants-doc-standards: 1 -->
# Documentation Standards — v1

Documentation contract for this project. Pairs with
[coding](coding.md), [testing](testing.md), [commits](commits.md);
see the [index](README.md). Governs `Kind: doc` / `doc-fix`
bullets. ROADMAP.md and CHANGELOG.md format details live in a
separate sub-spec at [`roadmap-format.md`](roadmap-format.md).


## 1. Principles

### 1.1 Six-month test

A reader six months from now should be able to use the doc
without the author present. If the doc says "see the recent
change", that won't be true in six months — replace with a
durable reference (`src/foo.cpp` + section name).

### 1.2 Show, don't claim

Examples beat prose. A README that *shows* the command + expected
output beats one that *describes* what the command does. Code
blocks should be runnable as-is.

### 1.3 Date format — ISO 8601

`YYYY-MM-DD`. No `Apr 28 2026`, no `28/04/2026`, no relative dates
(`yesterday`, `last week`) in committed docs. Relative dates rot.

### 1.4 Don't reference what isn't shipped

Doc lands when the feature lands. Forward-references to unshipped
features go in `ROADMAP.md`, not `README.md` or contract docs.

### 1.5 One source of truth per fact

Don't repeat the install steps in README + INSTALL + CONTRIBUTING
+ SETUP. Pick the canonical home; cross-link from the others.

This applies **within** a document as much as across them. A limit,
default, or constant stated in five sections will be wrong in at least
one of them after the first edit. State it once — in one table — and
reference that table everywhere else. Repetition is where
contradictions come from, and a reviewer finds them one at a time.

### 1.6 Concise over complete-sounding

The shortest form that stays unambiguous. Length is not thoroughness:
a longer document has more places for two statements to disagree, and
every extra sentence is one more thing that has to stay true.

- **Show the shape; don't narrate it.** A request/response format, a
  struct, a set of limits, a status list — those are schemas, tables
  and code blocks. Prose walking the reader through a table they can
  already see is deleted, not shortened.
- **Cut what adds no constraint.** If removing a sentence changes
  nothing a reader would do differently, it was commentary.
- **Put rationale where it acts.** "Here is the case a naive
  implementation gets wrong" belongs with the test that catches it,
  not inside the contract it defends.
- **Compare against a sibling.** A doc several times the size of its
  nearest equivalent in the same project is over-built until it can
  name the extra surface it covers.

### 1.7 Cite symbols, not line numbers

A line number points into a file and changes whenever anything above
it changes. A symbol name changes only when the thing itself changes.
Cite the symbol.

- **Canonical form** — `src/markdownscan.cpp::fenceMask()` for a
  function; `claude.mcp_enabled` in `config.json` for a key; "the
  `JOB_POOLS` block in `CMakeLists.txt`" for a build directive. Prose
  reads fine too: "the `op:detect` block in `cmdProjectSettings`".
- **A line number may accompany a symbol, never replace it.** Spell an
  approximate hint `~:N`, so a reader knows it is a convenience and a
  drifted hint is not a defect.
- **No symbol to name?** Cite the nearest stable named container plus a
  distinctive quoted token. The test is grep-ability: if one search
  finds it after the file moves, the citation holds.
- **Exempt — historical records.** Line numbers inside a review loop
  log, a `Resolved (date):` note, or a dated measurement are frozen
  records of what was true then. Leave them; never re-sync them. Only
  *live* citations — claims about how the code works now — are governed
  by this rule.

### 1.8 Mark teaching examples so they don't read as rot

A doc that *explains* citations has to write ones that point nowhere —
`src/foo.cpp:12` in a grammar table, `src/gone.cpp:1` in a fixture. To a
checker those are indistinguishable from real rot. Mark the passage:

```
<!-- doc-examples: begin -->
...prose whose file references are illustrations...
<!-- doc-examples: end -->
```

- **Alone on its line**, up to three leading spaces, lowercase keyword.
  A marker inside a fenced block is sample text, not syntax — which is
  what lets this section show the marker without opening a region.
- **Honouring it is each verb's own policy.** `doc_citations` skips a
  marked region entirely (ANTS-3659); another checker may not, the same
  way `doc_integrity` and `doc_citations` already share one code-span
  scanner and invert what they do with it.
- **Mark the passages that are about notation, not the whole file.** A
  region drawn wide enough to quiet every complaint also hides real
  rot; `examples_suppressed` in the response is what makes an
  over-broad one visible, so treat a surprising count as a defect.
- **The § 1.7 exemption above extends to this**: a cold-eyes loop log
  is a frozen record, so its citations are not live claims and are
  worth marking rather than checking.
- **Never infer "example" from a name that looks like a placeholder.**
  This repo has real files with generic names; a heuristic that guesses
  suppresses real rot silently. The marker is explicit for that reason.

Grounded in three hand-repairs of the same drift (ANTS-3470, ANTS-3596,
ANTS-3641): ANTS-3470 found that the one citation which survived the
refactor was the one naming its symbol chain.


## 2. Project-level files

### 2.1 README.md

Required sections, in order:

1. **Masthead** — project name, one-line description, badges
   (build, license, version).
2. **Current version** — single line: `Current version: X.Y.Z`
   with links to CHANGELOG, ROADMAP, and any companion docs.
3. **Features** — bulleted list of headline capabilities.
4. **Install** — one-line install for each supported platform.
5. **Quickstart** — minimal command sequence to use the project.
6. **Plugin / extension** (if applicable) — link to the plugin
   author contract.
7. **Documentation** — links to `docs/`, including the four
   standards docs.
8. **License** — single line + link.

Avoid: a TOC for a short README; "About" / "Why" sections without
content; broken screenshot links.

### 2.2 CLAUDE.md

For projects worked on with Claude Code: the project-specific
instructions Claude should follow. Lives at the repo root.
Typical contents:

- Module map (one line per major subsystem).
- Build instructions.
- Testing instructions.
- Conventions specific to this codebase.
- Key design decisions that aren't obvious from reading the code.

Keep it terse — the global `~/.claude/CLAUDE.md` covers
machine-wide rules; this file only covers project-specific ones.

### 2.3 LICENSE / COPYING / NOTICE

Standard files at the repo root. Use the SPDX-tagged canonical
license text — don't paraphrase.

### 2.4 SECURITY.md

For projects that accept external bug reports: disclosure policy,
contact email, GPG key (if used), supported-version table.

### 2.5 CODE_OF_CONDUCT.md

Contributor Covenant 2.1 verbatim is the default. Don't write
your own unless the project has a specific reason.

### 2.6 CONTRIBUTING.md (optional)

For projects accepting external contributors: build steps, test
expectations, how to file issues, how to propose features. Should
link to all four standards docs in this folder.


## 3. ROADMAP.md and CHANGELOG.md formats

The detailed format specs for both files — used by the Ants
Terminal Roadmap dialog and any tooling that consumes them
deterministically — live in
[`roadmap-format.md`](roadmap-format.md) (split out for
token efficiency; only relevant when authoring those files).

The high-level rules:

- `ROADMAP.md` is the single place to track unshipped work;
  shipped work moves to `CHANGELOG.md`.
- `ROADMAP.md` uses status emojis (✅🚧📋💭), theme emojis, and
  stable per-bullet IDs (`<project>-NNNN` from
  `.roadmap-counter`) plus phase IDs (`P##` — the canonical
  prefix; see `roadmap-format.md` § 3.2 for the heading format).
- `CHANGELOG.md` follows
  [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) with
  an `[Unreleased]` block at the top.

For details — including the format-version comment, theme
emoji set, current-work signalling rules, bullet contract, and
release flow — read [`roadmap-format.md`](roadmap-format.md).

## 4. API / contract docs

For any project that exposes an API, a plugin contract, or a
machine-readable surface (`PLUGINS.md`, `API.md`,
`openapi.yaml`):

- **Document every public symbol.** If a function is exported,
  it's part of the contract.
- **Include the version it was added in.** Helps consumers know
  what they can rely on. Example: `Added in 0.6.5`.
- **Show input + output examples.** Type signatures alone aren't
  enough.
- **Mark deprecation explicitly.** `Deprecated since X.Y.Z; use
  Foo instead.`
- **Provide a migration path** for any deprecated / removed
  surface.


## 5. In-code documentation

Defer to [coding § 3](coding.md). Default is no comments; only
WHY non-obvious things need them. Don't write multi-paragraph
docstrings.


## 6. Screenshots

- **Path** — `docs/screenshots/` or `assets/screenshots/`.
- **Filename** — `<feature>-<state>.png`
  (`terminal-tabs-active.png`, not `Screenshot 2026-04-28.png`).
- **Format** — PNG for UI, JPG for photographic content.
- **Caption** every screenshot in the surrounding prose.
- **Replace, don't accumulate.** When the feature changes, swap
  the screenshot. Don't pile up `_old` / `_v2` versions.


## 7. Accessibility

User-visible status and category labels reach screen readers
(Orca / NVDA / VoiceOver) only via the text that the application
stores in its document model — **not** via HTML metadata.

- For Qt 6 `QTextBrowser` / `QTextDocument` content, the
  accessibility surface is the rendered plain text returned by
  `QAccessibleTextInterface::text(int, int)` — which is the
  document's `toPlainText()`. HTML `aria-*`, `alt`, `role`,
  `title` attributes are stripped by Qt's HTML parser and never
  reach AT-SPI / IAccessible2.
- `QTextCharFormat` / `QTextFormat` have no per-fragment
  accessible-name property, and `QTextImageFormat::setName`
  applies only to image fragments. There is no Qt-level escape
  hatch for text-fragment accessibility metadata in
  QTextDocument.
- Therefore: **render the labels you want spoken.** If a
  decorative emoji ("✅") would otherwise be announced as its
  CLDR name ("white heavy check mark"), emit a short text label
  inline alongside it (`<span>shipped</span>`) so the
  plain-text projection carries the right word.
- For QWidget subclasses (QLabel, QPushButton, QCheckBox, …),
  `setAccessibleName()` is the right path — screen readers speak
  the accessibleName in preference to the visible text. Pair
  with `setAccessibleDescription()` for longer context that
  shouldn't go on the visible label.
- Reference implementations in this codebase:
  `src/claudestatuswidgets.cpp` (status-bar chips),
  `src/commandpalette.cpp` (palette search box), and
  `src/roadmapdialog.cpp` (Roadmap card status labels and filter
  checkboxes, per ANTS-1235).


## 8. Markdown style

- ATX headings (`# `, `## `, `### `) — never setext (`====`).
- One blank line before/after headings.
- Tables for structured data, fenced code blocks for code.
- Line wrap at ~70–80 columns for readability in `git diff`.
  Don't force-wrap inside code blocks or tables.
- Links: `[text](url)` not `<url>`, unless the URL itself is
  meant as the visible text.
- Lists: `- ` for bullets, `1. ` for numbered. Don't mix `*` and
  `-` in one file.
- Inline code: backticks for filenames, function names, CLI
  flags.


## 9. Doc reviews

Schedule periodic doc reviews independent from code reviews —
the two drift independently. A doc review surfaces:

- Stale CLI flag references.
- Screenshots showing the previous version's UI.
- "Recent change" / "yesterday" relative dates.
- Sections that document a feature that was removed.
- Cross-references to renamed files / functions.
- Live `file:line` citations that have drifted (§ 1.7) — check the
  cited line still *says* what the doc claims. Existence is not
  enough: code moves, and the line that replaces it still exists.
- ROADMAP / CHANGELOG bullets whose claims don't match the
  shipped code.

Findings from a doc review fold into the ROADMAP under
`### 📚 Documentation review fold-in (YYYY-MM-DD)` per [`roadmap-format.md` § 3.8](roadmap-format.md).


## 10. Anti-patterns

- ❌ Lorem ipsum or placeholder text in committed docs.
- ❌ Screenshots that show the previous version's UI.
- ❌ "We" / "I" — use second person ("the user", "you").
- ❌ Markdown that doesn't render correctly on GitHub (test it).
- ❌ Documentation for a feature that hasn't shipped (goes in
  ROADMAP.md instead).
- ❌ Stale CLI flag references — sweep every doc when a flag
  changes.
- ❌ Relative dates in committed docs (`recently`, `last week`).
- ❌ A README so long a new contributor bounces off the page.
- ❌ A bare `file:line` as the only pointer to live code — name the
  symbol (§ 1.7).
- ❌ Prose that restates the table, schema, or code block above it
  (§ 1.6).
- ❌ The same limit, default, or constant written out in more than one
  place (§ 1.5) — the copies drift apart, and only one of them is
  right.
