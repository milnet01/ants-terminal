<!-- ants-doc-standards: 2 -->
# Documentation Standards — Ants Terminal deltas

> **The standard itself is `~/.claude/standards/documentation.md`. Read it
> there.** This file carries only what is specific to *this* project.

**Why this file is a delta (2026-08-12).** It was a `/start-app` copy that had
drifted since 2026-07-27, and it had come to contradict the global standard on
document length — it treated a doc larger than its nearest sibling as
over-built until proven otherwise, where global § 2.8 says a longer document
is worth a look but not automatically wrong, and that "a length target invites
deleting whatever is easiest to delete."

**The section numbers below are deliberately NOT renumbered.** Other
documents cite `§ 1.5`, `§ 1.6`, `§ 1.7`, `§ 1.8`, `§ 7` and `#9-doc-reviews`
by anchor. Sections whose content moved to the global standard are kept as
pointers so those links keep resolving.

## Where the rules actually live

| You want | Read |
|---|---|
| What kind of document is this | global § 1 |
| One source of truth per fact; fix the home, never copy | global § 2.1 |
| Verify against the source, don't recall | global § 2.2 |
| Durable references — cite symbols, not line numbers | global § 2.3 |
| Show, don't claim | global § 2.4 |
| Concision | global § 2.5 |
| The six-month test | global `coding.md` § 1.4 |
| ISO 8601 dates | global § 2.6 |
| Don't reference what isn't shipped | global § 2.7 |
| Document length | global § 2.8 |
| The `## What checks this` requirement | global § 2.9 |
| Folder layout, naming, capitalisation | global § 3 |
| CHANGELOG format | global `changelog-format.md` — **but see § 3 below; in this project that spec is `roadmap-format.md` § 4** |
| README, CLAUDE.md, LICENSE, SECURITY.md, CONTRIBUTING | global § 5 |
| API / contract docs | global § 6 |
| Screenshots and videos — **`docs/screenshots/`, not `assets/`** | global § 7 |
| Markdown style | global § 8 |
| Mechanical checks, the review gate, escalation | global § 9 |
| Anti-patterns | global § 10 |

**Sections below are kept at their original numbers so inbound links keep
resolving.** One whose content moved to the global standard is a pointer, not
a deletion.

### 1.1–1.4 Six-month test · show don't claim · ISO 8601 dates · don't reference what isn't shipped

→ global `coding.md` § 1.4 (the six-month test), then global § 2.4, § 2.6
and § 2.7 respectively. The six-month test is **not** in the global
documentation standard; it is a coding-standard rule.

### 1.5 One source of truth per fact

→ global § 2.1. It is stronger than the text this file used to carry: a rule
stated in a second document is a **pointer, never a copy**, and where a fact
already lives in two places you have a consolidation to do, not an edit. This
file is the result of applying that rule to itself.

### 1.6 Concision

→ global § 2.5 for the rule, and **global § 2.8 for length**.

This section previously told you to compare a document against its nearest
sibling and treat a multiple of its size as over-built. That is not the
global position and it is withdrawn. Global § 2.8: check for the three named
causes — a duplicated fact, prose narrating a table, decisions never made —
and "if none of those is present, the length is the subject's, and cutting it
would lose something."

### 1.7 Cite symbols, not line numbers

→ global § 2.3.

Cite the symbol (`src/markdownscan.cpp::fenceMask()`), not the line. This
project's own history is the argument: `dependencies.md` § 6 carried `(L…)`
hints and **every one of them had drifted** by 2026-07-30.

> Corrected 2026-08-12: this section used to permit an approximate line hint
> alongside the symbol, spelled `~:N`. Global § 10 lists a `path:line`
> citation as an anti-pattern without that carve-out. Don't write the hint.

### 1.8 Mark teaching examples so they don't read as rot

**Project-local — this documents an Ants MCP contract and has no global
counterpart.**

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
- **The § 1.7 exemption above extends to this**: a review loop log is a
  frozen record, so its citations are not live claims and are worth
  marking rather than checking.
- **Never infer "example" from a name that looks like a placeholder.**
  This repo has real files with generic names; a heuristic that guesses
  suppresses real rot silently. The marker is explicit for that reason.

Grounded in three hand-repairs of the same drift (ANTS-3470, ANTS-3596,
ANTS-3641): ANTS-3470 found that the one citation which survived the
refactor was the one naming its symbol chain.

## 2. Project-level files

→ global § 5 (README, CLAUDE.md, LICENSE, SECURITY.md, CODE_OF_CONDUCT,
CONTRIBUTING). Global § 5.1's README section list is the ordered one. The badges and
plugin sections this file used to require are not in it — but a
**where-the-rest-of-the-documentation-is** section is, so do not trim that.

## 3. ROADMAP.md and CHANGELOG.md formats

**Project-local routing.** Both formats live in
[`roadmap-format.md`](roadmap-format.md) — the ROADMAP spec in § 3 and the
**CHANGELOG spec in § 4**. This project has no `changelog-format.md`; global
split its changelog spec into one, and this project has not. Follow
`roadmap-format.md` § 4 here.

The high-level rules:

- `ROADMAP.md` is the single place to track unshipped work. Shipped work is
  recorded in `CHANGELOG.md`, but the ✅ bullet **stays in `ROADMAP.md`**
  until archive rotation moves it byte-identically to
  `docs/roadmap/<MAJOR>.<MINOR>.md` (`roadmap-format.md` § 3.9). Nothing
  moves shipped items out automatically; deleting them at release time
  leaves rotation with nothing to archive.
- `ROADMAP.md` uses status emojis (✅🚧📋💭), theme emojis, and stable
  per-bullet IDs (`ANTS-NNNN`, allocated by `roadmap_log`) plus phase IDs
  (`P##` — see `roadmap-format.md` § 3.2 for the heading format).
- `CHANGELOG.md` follows
  [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) with an
  `[Unreleased]` block at the top. Its categories include **`Deprecated`** —
  the `changelog_log` verb's own enum carries it.

Write CHANGELOG entries with `mcp__ants__changelog_log`, not by hand.

## 4. API / contract docs

→ global § 6.

## 5. In-code documentation

→ global `coding.md` § 3 (Comments). A one-line documentation comment saying
what a caller gets is not the "comment" that rule discourages — it is the
contract.

## 6. Screenshots

→ global § 7. **`docs/screenshots/`, and recordings in `docs/videos/`.** This
file previously also permitted `assets/screenshots/`, which does not exist in
this repo and is not a location global allows.

## 7. Accessibility

**Project-local — Qt-specific, and the global standards tree carries no
accessibility content at all.**

User-visible status and category labels reach screen readers (Orca / NVDA /
VoiceOver) only via the text that the application stores in its document
model — **not** via HTML metadata.

- For Qt 6 `QTextBrowser` / `QTextDocument` content, the accessibility
  surface is the rendered plain text returned by
  `QAccessibleTextInterface::text(int, int)` — which is the document's
  `toPlainText()`. HTML `aria-*`, `alt`, `role`, `title` attributes are
  stripped by Qt's HTML parser and never reach AT-SPI / IAccessible2.
- `QTextCharFormat` / `QTextFormat` have no per-fragment accessible-name
  property, and `QTextImageFormat::setName` applies only to image fragments.
  There is no Qt-level escape hatch for text-fragment accessibility metadata
  in QTextDocument.
- Therefore: **render the labels you want spoken.** If a decorative emoji
  ("✅") would otherwise be announced as its CLDR name ("white heavy check
  mark"), emit a short text label inline alongside it (`<span>shipped</span>`)
  so the plain-text projection carries the right word.
- For QWidget subclasses (QLabel, QPushButton, QCheckBox, …),
  `setAccessibleName()` is the right path — screen readers speak the
  accessibleName in preference to the visible text. Pair with
  `setAccessibleDescription()` for longer context that shouldn't go on the
  visible label.
- Reference implementations in this codebase: `src/claudestatuswidgets.cpp`
  (status-bar chips), `src/commandpalette.cpp` (palette search box), and
  `src/roadmapdialog.cpp` (Roadmap card status labels and filter checkboxes,
  per ANTS-1235).

## 8. Markdown style

→ global § 8.

## 9. Doc reviews

→ global § 9 owns the mechanical checks, the review gate and escalation. Run
`check-doc-facts` for the deterministic half and `review-contract` for the
cold read.

**Project-local:** findings fold into the ROADMAP under
`### 📚 Documentation review fold-in (YYYY-MM-DD)`, per
[`roadmap-format.md` § 3.8](roadmap-format.md).

## 10. Anti-patterns

→ global § 10. Two it names that this project has committed, and that the
delta rewrite exists to undo: **repeating a global rule in a project
document**, and **a `path:line` citation**.

## What checks this

`mcp__ants__doc_citations` (honours § 1.8's markers),
`mcp__ants__doc_integrity`, `mcp__ants__doc_symbols` and
`mcp__ants__doc_dedup`, plus the `check-doc-facts` skill. § 7 is **partial**: the widget half — `setAccessibleName` /
`setAccessibleDescription` on chrome controls — is asserted by
`tests/features/a11y_chrome_names/` (label `features`). **Nothing** checks the
QTextDocument plain-text projection that the rest of § 7 is about.

## Review loop log

| Loop | Date | Lanes | Q1/Q2/Q3/Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 1 (cold, general-purpose) | Q1 3 · Q2 2 · Q3 0 · Q4 n/a | 5 verified, 5 fixed, 0 dismissed. Routed the six-month test to global documentation § 2.6, which is ISO 8601 dates — the rule is `coding.md` § 1.4; claimed global § 5.1 omits a documentation section when it requires one; said shipped work "moves to CHANGELOG" when the ✅ bullet stays in ROADMAP until archive rotation, so following it would empty the per-minor archives; claimed nothing checks § 7 when `tests/features/a11y_chrome_names/` covers its widget half. Surfaced, not fixed: `specs.md` § 5.3 still prescribes the sibling-size rule § 1.6 withdraws. |
