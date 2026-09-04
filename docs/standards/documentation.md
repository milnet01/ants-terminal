<!-- ants-doc-standards: 3 -->
# Documentation Standards — Ants Terminal deltas

> **The standard itself is `~/.claude/standards/documentation.md`.** This half
> of the file carries only what is specific to *this* project.
>
> **The owner is mirrored verbatim below the divider**, between the
> `MIRROR BEGIN` / `MIRROR END` markers, because this repo is public and an
> outside reader cannot open a path inside a private home directory. **Do not
> edit that half.** A correction goes upstream, then
> `tools/check-standard-mirrors.sh --write` re-copies it down;
> `tools/hooks/pre-commit` refuses a commit whose mirror has drifted from its
> owner (ANTS-4133).

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

**The mirrored half is checked.** `tools/hooks/pre-commit` runs
`tools/check-standard-mirrors.sh`, which fails the commit if the text between
the MIRROR markers no longer matches `~/.claude/standards/documentation.md`.
It skips on a checkout with no global standards tree — an outside
contributor's, or CI's — since there is then nothing to compare against.

## Review loop log

| Loop | Date | Lanes | Q1/Q2/Q3/Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 1 (cold, general-purpose) | Q1 3 · Q2 2 · Q3 0 · Q4 n/a | 5 verified, 5 fixed, 0 dismissed. Routed the six-month test to global documentation § 2.6, which is ISO 8601 dates — the rule is `coding.md` § 1.4; claimed global § 5.1 omits a documentation section when it requires one; said shipped work "moves to CHANGELOG" when the ✅ bullet stays in ROADMAP until archive rotation, so following it would empty the per-minor archives; claimed nothing checks § 7 when `tests/features/a11y_chrome_names/` covers its widget half. Surfaced, not fixed: `specs.md` § 5.3 still prescribes the sibling-size rule § 1.6 withdraws. |

---

<!-- MIRROR BEGIN ~/.claude/standards/documentation.md -->
# Documentation Standards — v1

**Status:** v1 (2026-08-08).

**Rule history: [`docs/history/documentation.md`](../docs/history/documentation.md).**

**Purpose: so that someone looking for an answer finds exactly one
document that has it, and can trust what it says.**

Every rule here traces to that sentence. The two failures it prevents
are *not finding it* and *finding it and being misled* — and the second
is worse, because a wrong document is believed.

**Scope: every document a human reads.** In practice that means the
markdown in a project, but the subject is documents, not a file
extension. Rules for one *kind* of document live in that kind's own
standard (§4); this file holds what is true of all of them.

Governs `Kind: doc` / `doc-fix`.

---

## 1. What kind of document is this

Answer this first. It decides what belongs in the document, what may be
left out, and how it gets reviewed — and almost every documentation
mistake is a document being written as the wrong kind.

| Kind | What it is for | So it must |
|---|---|---|
| **Contract** — design, spec, plan, ADR, standard | something is built *from* it or *under* it | be complete enough that an implementer never has to invent a required behaviour |
| **Ledger** — roadmap, changelog | to record what is planned and what shipped | be true about the code, and stay true |
| **Instructions** — README, INSTALL, CONTRIBUTING, runbook | so a reader can *do* the thing | work when followed literally, on a clean machine |
| **Reference** — API docs, tables, glossaries | to be looked up, not read | resolve — every name in it must exist |
| **Agent rules** — `CLAUDE.md`, skills | to instruct an agent | be followable, and not contradict each other |

**The kinds have genuinely different obligations, and borrowing one
kind's expectations for another is expensive.** A standard has no
acceptance criteria and no delivery date — that absence is its *correct*
shape, not an omission. A ledger that is beautifully written and untrue
has failed completely. A README nobody has run is unverified however
carefully it reads.

**An ADR is a decision frozen in time** — context, choice, reasoning,
cost. It is never edited to reflect a change of mind; a changed decision
gets a new ADR that supersedes it.

Full reasoning: `../docs/decisions/ADR-0001-documentation-families.md`.

## 2. Rules for every document

### 2.1 One fact, one home

State each limit, count, name and date **once**. Everywhere else points
at it.

This is not tidiness. A fact written in four places gets corrected in
one, and the other three are now wrong — and they read exactly as
confidently as the correct one. It is the single largest driver of
review cost, because each stale copy becomes a later finding.

**A concept has one name, too.** One thing with two names is the
beginning of two things.

**Correcting a rule: find its home before you write, not after.** This
half is addressed to whoever is *fixing* rather than authoring, and it
is the one most often missed — a defect is reported where it was
**visible**, which is not necessarily where the rule **lives**. Grep the
subject first. One home, here → edit in place. One home, elsewhere →
fix it there and leave a pointer. **More than one → you have a
consolidation, not an edit**: fix the owning copy and delete the rest —
**except a machinery cell, below** —
because correcting one of three leaves the document saying three
things where it previously said one wrong thing consistently.

Measured 2026-08-11 over five review loops on two standards: eight
defects were introduced by the repairs, and **five were this**.

**Two reasons that grep comes back empty on a copy that is there.** Both
are mechanical, and an empty result reads exactly like one home. **Search
with whitespace-and-blockquote normalisation** — `workspace_search` with
`match_wrapped: true`, or any search that ignores line breaks **and
leading blockquote markers** — because
these documents hard-wrap, so a phrase spanning a line break matches
nowhere and the search returns a false all-clear; measured 2026-08-24, a
search for one instruction reported no copy while copies existed. **The
`pre-commit` hook's survivor class shares that blindness** — it matches a
deleted line whole, so a differently-wrapped copy passes it (§ 9.0) — which
is why this search is still owed after a clean hook run. **And the second copy is often not a sentence**: a
table row, a trigger cell, a stop-condition bullet. It states the rule in
a few words, shares no phrasing with the prose, and is what a fix to the
prose leaves standing. So **search the subject too** — the tool name, the
flag, the section name. Both searches, not one: the phrase finds a
restatement, the subject finds a cell that shares no phrasing with it.
**A cell whose text is machinery — a trigger, a stop condition — is
updated in the same change as the owning prose**, never deleted and never
replaced by a pointer: deleting it disables the trigger. That is the first
exception to both rules above, and it is narrow — a cell a run *executes*,
not a cell that merely restates a rule.

**The second is stated by §9.0: a cell restating the enumeration a hook
owns.** That is a restatement, so the narrow test above does not reach it, and
§9.0 requires it kept rather than pointed at — updated in the same change that
edits the hook. Without this the two sections prescribe opposite edits on one
discovery.

**A rule stated in a second document is a pointer, never a copy** — and
this is the half most often missed, because the rule above reads as being
about facts. A second document that restates an obligation has taken
custody of it: the next person to narrow the rule narrows one copy, and
the two disagree from that moment with nothing to announce it. Name the
owning document and the section, and stop — **except the two machinery
cells above**. Measured 2026-08-11: a
transition rule restated in a second standard was narrowed there and left
unscoped in its home, and the contradiction survived a full review loop
because neither passage cited the other.

**A "What checks this" row about machinery another document owns cites
that document, never restates its coverage.** This is the same rule
applied to the one place it kept being missed. **"Another document" means one
that states its own coverage answer — a sibling standard, a format standard.
A check catalogue is machinery, not an answer**, so a row naming a
`check-doc-facts` check writes the named-check form § 2.9 requires and does
not point. It is worth stating
separately because such a row does not read like a copy — it reads like
a helpful summary, written once, by whoever wrote *this* document, about
machinery owned by a *different* one. Nothing re-reads it when the
machinery changes, so it goes stale silently and in the most damaging
direction: a reader trusts a green run the row promised and the check
was never there. Say which document owns the answer and stop; a row may
still say **nothing**, because that is a claim about this standard's own
coverage.

**The test is whether the other document STATES THE COVERAGE ANSWER, not
whether it owns the subject** — and getting that wrong turns a pointer into a worse
answer than the copy it replaced. A cold read the same day caught one:
`releases.md`'s row for *a changelog entry exists for this version* was
pointed at `changelog-format.md`, which owns the changelog's shape and
correctly answers **nothing** — while the rule is in fact caught, by the
release tool refusing to bump without a dated section. The pointer told a
reader nothing checks this when something does. **Where the check lives in a
third place, name the check here**; point only when the cited document is
where the answer actually is.

### 2.2 Verify, don't recall

Every path, symbol, constant, flag and version-specific behaviour is
backed by reading the current source. Not memory, not inference from how
it probably works.

Writing from recall is the most expensive class of documentation
mistake, precisely because the result is indistinguishable from the
truth until someone acts on it.

**A verbatim quotation of another document names its source in the same
sentence.** *"…"* alone is not enough: quote marks in prose mark emphasis,
labels, coined phrases and other people's words far more often than
quotation, so an unattributed quote cannot be told from any of them — by a
reader or by a check. Measured 2026-08-11 across this repo: of the long
italic-quoted fragments, **the ones that were real quotations of another
document were heavily outnumbered by the ones that were not** — emphasis,
labels and coined phrases. That is why a check to *find* quotations could not
ship: nothing in the form distinguished the real ones. **A check that
re-verifies an already-attributed quotation is a different instrument and does
exist** — the table's row for this rule says which part it covers.

Naming the source is what makes the class checkable at all — a quotation
that carries its source can be re-verified mechanically when the source
changes, which is the one defect no review of either document can find.

**A pointer you write is one you just opened.** Naming a section —
`§ 3.5.2`, *"see the drift table"* — asserts that it says the thing you
are attributing to it. That the section *exists* is checkable and not
enough: a citation can resolve perfectly and still point at a section
about something else. Measured 2026-08-11: two citations written into
one standard during a fix pass named a section that exists and does not
carry the rule attributed to it, and a third named a section the
document does not have at all.

**A negative claim is a search you ran.** *"Nothing checks this"*, *"no
skill does X"*, *"the field is never set"* — each asserts an absence across a
body of files, so run the search before writing the sentence. These
concentrate in a `## What checks this` table (§2.9), which is built almost
entirely of them. Measured 2026-08-11: **nothing yet** was written into such
a row while a `pre-commit` hook was already blocking on that exact class — a
hook the same author had written days earlier. A negative you cannot search
for is not a sentence you can write.

**Adding a value to an existing set means listing what already constrains the
set.** A new status, field, category or flag inherits every limit already
placed on its kind, and those limits are usually stated somewhere else.
Measured 2026-08-11: a `Waiting-on:` field parked items on an existing
status, and a cap on how many items may hold that status already existed in
another section — so four items obeying the new rule breached the old one.

### 2.3 Don't cite anything that goes stale without announcing it

**Line numbers.** `src/vault.py::derive_key()`, not `src/vault.py:39-49`.
A line number is stale two commits later and nothing announces it. The
prose form — "around line 786" — is caught by nothing at all.

**Counts of a population that changes.** "Seventeen skills", "98
occurrences", "23 files", "the table has 17 rows". Every one is a census
taken on a day, written as if permanent. It goes wrong the next time
anything is added, and it goes wrong *silently* — the sentence still
reads fine.

**A count spelled as a word is still a count**, and this is the form that
gets past the rule, because it does not look like a number: *both*
defects, *three* reports, *two of this document's three* review loops.
Enumerating a set
that can grow is the tell, whatever it is spelled with. Write *these*,
*each*, or the shape.

**The cost is not the wrong number, it is where the attention goes.**
A stale count is the easiest thing in a document to spot and the least
important thing to fix, so it is what a review finds. Reviews then spend
themselves correcting arithmetic while the claim that would actually
mislead an implementer goes unread. A document full of numbers is a
document that trains its reviewers to check numbers.

**Say the shape instead of the size.** "Every skill has one purpose" does
not rot; "nineteen skills each have one purpose" rots the moment there is
a twentieth — and the rule was never about nineteen. If the number is
genuinely the point, name the command that produces it and let the reader
run it, rather than pasting today's answer.

**Two kinds of number are not census counts and stay.**

- **Structural** — "a spec and its plan are *two* files", "the review
  asks *four* questions". These state a design, not a measurement. They
  change only when the design changes, which is a deliberate edit.
- **Dated historical** — "measured 2026-08-08: 23 files, none with a
  What-checks-this table". A record of what was observed on a day cannot
  go stale, because it was never a claim about now. It must read as past
  tense and carry its date; without one it is a census wearing a
  disguise.

**The test, for all three:** *would this number change under the document,
without anyone touching it?* Structural — no; it changes only when someone
deliberately edits the rule, and that same edit updates the sentence.
Historical — no; the past does not move. Census — **yes, silently, the next
time anything is added.** Only the third is the problem, and *silently* is the
whole of why.

**Asking instead "would a different number mean the document is wrong?" does
not discriminate** — it answers *yes* for a structural number and *yes* for a
census, which is the pair the rule has to separate.

### 2.4 Show, don't claim

An example beats a description. Show the command and its output rather
than explaining what the command does. Code blocks are runnable as
written.

### 2.5 Say it as a structure, not a paragraph

Shapes, keys, limits, states and options go in tables and fenced blocks.
Prose narrating a structure restates it; a table states it once and can
be checked.

### 2.6 Dates are absolute

`YYYY-MM-DD`. Never `last week`, `recently`, `the current version`, or
`the recent change` — relative references rot silently, and are true
right up until they are quietly false.

### 2.7 Don't document what hasn't shipped

The document lands when the thing lands. Intent belongs in the roadmap.
A README describing a feature that does not exist is not optimism, it is
a false claim with a date on it.

**Scope: documentation that describes behaviour to a reader** — a
README, a guide, reference material, an interface description. **A spec
or a plan is exempt, and is meant to be written first.** It is a
contract for whoever builds the thing, not a description of something
that exists, and getting it written before the build is the whole point
of [spec-format.md](spec-format.md). The test is who the document is
for: a reader learning what the software does, or a builder learning
what to make.

### 2.8 Length is a symptom, not a verdict

A document much longer than its siblings, or much longer than what it
describes, is **worth a look** — not automatically wrong.

What to look for, in order: the same fact stated in several places
(§2.1); prose narrating what a table would state (§2.5); decisions
written out at length that nobody actually made.

**If none of those is present, the length is the subject's, and cutting
it would lose something.** A length target invites deleting whatever is
easiest to delete, which is rarely what should go.

**Everything above in this section is an authoring obligation, never a
review question**
(§9.4). **The delete-first rules below are not** — those fire at a review's
fix pass, and `review-contract` 4a-min cites them there.

**Writing is the other half, and there the bias runs the other way: if it
fits in one paragraph, write one.** Not a target — a consequence. Every
sentence is a claim that can be false, can contradict another sentence, and
has to stay true as the subject moves. Measured across two review runs on
this machine, fix passes added about twice what they removed, and the added
text is where the next loop's findings landed. So the rule is *say it once,
as briefly as it can be said correctly*, and §2.8 above is what stops that
becoming a licence to cut the subject itself.

**Delete first, write second.** Where a correction can be made by deleting,
that is the whole edit:

1. **A claim that is false is deleted.** Write a replacement only if its
   absence leaves a gap a reader would fill by inventing something.
2. **A claim that is over-broad is narrowed by deletion**, not by a
   qualifying sentence set beside it.
3. **Never summarise a structure you are standing next to.** Worked case: an
   unscoped sentence under a four-row table was corrected with a two-case
   grouping the table did not have. The table already said it; deleting the
   sentence was the fix.
4. **Do not annotate the removal's reasoning.** The commit message is where an
   argument for a change is made; the document is where the change is made.
   Where the reasoning must survive in the document, name a check rather than
   describe one. **What this does NOT forbid is a dated record of what the rule
   previously said** — "this read X until 2026-08-14" is § 2.3's *dated
   historical* form, it is past tense, it carries its date, and it cannot go
   stale. **Where the document names a history file — a record it keeps for
   rule pedigree, never its loop log, whose rows `review-contract` alone
   writes — that record is written there and the rule keeps only its
   instruction.** A document naming none keeps its pedigree inline. Same
   ground as § 9.1: a
   document read in full whenever it is consulted pays for its pedigree on
   every read, and the pedigree is not what the reader came for. **Move a
   clause only when deleting it changes nothing a run would do** —
   `skills/_shared/provenance-split.md` owns that test, and a dated
   measurement the rule's strength rests on is scope and stays. It moves
   verbatim or not at all.
5. **A note naming a broken passage is not a fix — change the passage.**
   Where a correction records that some *other* line is wrong, that line
   changes in the same edit, or the note documents the defect instead of
   repairing it. **It reads as a fix and answers the same search**, which is
   what makes it expensive: a later reviewer greps the marker, finds the note,
   and stops. Measured 2026-08-15 (ROADMAP CFG-0129) on `security.md` § 3 — a
   gate loop added *"which found the opener and the bullet prescribing
   opposite code"* and left the bullet prescribing it. Five of six cold lanes
   across two later loops confirmed that section sound, never reading the
   bullet the note was about. **This is item 4's limit**: a dated record of
   what a rule *used to say* is fine, and a note about a line you did not
   change is not.

### 2.9 A rule with no check is a wish

Whether a rule holds is settled by whether something cheap catches it
failing — not by how firmly it is written.

**Every standard, reference and spec carries a `## What checks this`
section**: one table, each rule against what catches a breach. **In a
standard or a reference it is the last content section, with the loop log
after it** — a loop log is a record rather than content. **A spec's order is
`spec-format.md` § 3's block**, which puts `Cross-doc impact` between the two
and appends § 4's sections after the log. (A spec numbers it, per the
note below. `spec-format.md` § 3.12 delegates **all** of the table's rules
back here — the cell forms, which rules are owed a row, and why a wrong row is
worse than a missing one — and shows a worked spec table. **Its one addition
is spec-side: rows are keyed by `INV-N`, or by a section number for a
non-invariant rule.**)

Each right-hand cell says one of four things and never blurs them: a
**named check**; **`nothing`** in bold plus why; **`Partial:`** a named check
plus the part it does **not** cover; or **a pointer** to the document that
owns the answer, which § 2.1 requires wherever the ANSWER belongs to
another document — never merely the machinery, which § 2.1 settles above. **Where a pointer and `Partial:` both apply — another
document's coverage ANSWER, covering part of the rule — write the `Partial:`
and cite that document inside it.** § 2.1 forbids restating another document's
coverage; it does not forbid naming the half nobody covers. **Where two catchers
together still leave part of the rule uncovered, write the `Partial:` and
name both inside it** — the named half may be more than one check. **Where
they cover it between them, one plain named-check row names both**, because
`Partial:` has no uncovered half to state.

Name the check and say what kind it is — `` `check-doc-facts` `paths`, a
path-resolution check `` — rather than either alone. **The rule read "name the
kind of check rather than today's tool" until 2026-08-14 and this document's
own table broke it twice**, naming `` `paths` `` and the `pre-commit` hook's
survivor class; half the standards set modelled each answer. A bare kind is
unactionable — nobody can run "a path-resolution check" — and a bare tool name
goes stale when the tool is replaced, which is what the old rule was protecting
and is why the kind is still required.

**The `Partial:` form is required, not permitted, wherever coverage is
incomplete** — § 9.0 owns the reasoning, and a partial check recorded as a
plain named check is the row that reads as coverage and is not.

**A row is owed by every rule whose enforcement a reader would otherwise
have to guess at** — not by every numbered section. A rule the surrounding
prose already settles needs no row, and a table padded to one-row-per-heading
buries the `nothing` rows that are the point. **The two doubts resolve
opposite ways:** unsure what *catches* a rule you have decided is owed a row
— write it, and say **nothing**. Unsure whether the rule is owed a row at all
— leave it out. § 10's anti-pattern is the first case, not the second.

**A wrong row is worse than a missing one**, because the table's whole
value is being trustable without re-deriving it.

An unchecked rule recorded as unchecked gets fixed. An unchecked rule
left silent reads as covered. **The `nothing` rows are the document's
honest error budget — watch their share of the table fall.** The share,
never the raw count: splitting a blurred row into two is what the cell-form
rule above requires, and it raises the count while moving nothing.

In a standard or reference this section is **unnumbered**, so adding it
to an existing document renumbers nothing and every cross-reference
still resolves. Same for a trailing `## Cold-eyes loop log`. In a spec
it is numbered like every other section.

## 3. Where documents live

**One kind per folder.** A document's kind should be guessable from its
path, so that anything deciding how to treat it — a reader, a review, a
tool — can tell without opening it.

```
README.md            what this is, and how to start using it
CLAUDE.md            project-specific agent instructions
CHANGELOG.md         what shipped, when
ROADMAP.md           what is planned, in progress, shipped
SECURITY.md          trust boundaries and disclosure policy
LICENSE

docs/
  discovery.md       what this is for — root, because there is one
  design.md          the whole-app design — root, because there is one
  specs/             <ID>-<topic>.md   one feature's contract
  plans/             <ID>-<topic>.md   build steps for that contract
  decisions/         ADR-NNNN-<topic>.md
  standards/         this project's overrides and its own standards
  screenshots/
  videos/            screen recordings and demos
```

### 3.1 Naming a spec or plan

```
<roadmap-ID>-<topic>.md          CFG-0001-spec-authoring.md
```

**Both halves are load-bearing, for different readers.**

- **The ID ties the document to one roadmap item**, so anyone can get
  from the document to why it exists, and from a roadmap bullet to its
  contract. It is also what tooling routes on.
- **The topic makes a directory listing readable by a human.** A folder
  of bare IDs tells you nothing about which file you want, and you end
  up opening several.

Drop either half and one of those readers is stranded. Hyphens rather
than spaces, so every command touching the file does not need quoting.

The same ID appears on the spec and on its plan — one item, two
documents, each named for it.

### 3.2 A spec and its plan are separate files

They answer different questions and go stale at different rates — a
contract outlives the steps that satisfied it. Design decisions live in
the spec, never the plan; build steps live in the plan, never the spec.
A plan that argues for its approach has become a second spec, and the
two will disagree.

`spec-format.md` owns their format. Write both with `write-spec`.

### 3.3 Capitalisation of filenames

Three forms, and which one applies is decided by *how the file is found*,
not by how important it is.

| Form | For | Examples |
|------|-----|----------|
| `SHOUTING.md` | Documents an outside reader looks for **by name**, without being told they exist | `README.md`, `CHANGELOG.md`, `LICENSE`, `SECURITY.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `ROADMAP.md`, `CLAUDE.md` |
| `lowercase-kebab.md` | Everything else — standards, guides, references, notes. Found by reading a directory or following a link | `coding.md`, `spec-format.md`, `roadmap-format.md`, `foundation.md` |
| `<PREFIX>-NNNN-topic.md` | Specs, plans and ADRs. §3.1 owns it; the ID keeps its own casing | `CFG-0001-spec-authoring.md`, `ADR-0001-documentation-families.md` |

**The uppercase set is closed, and it is closed on purpose.** It is the
list the wider ecosystem already agreed on — GitHub renders those names
specially, package tooling looks for them, and a stranger cloning a repo
types `README` without checking. A file gets to shout because *other
people's tools* expect it to, never because its author felt it mattered.
That is what stops the set growing: "this is important" is not a reason,
and it is the only reason anyone ever offers.

**So a new document is lowercase unless it is on that list.** The
question to ask is not *is this a major document?* but *would someone
look for this file without being told it exists?* A charter, a design
note, a standard — all read because something pointed at them. All
lowercase.

Hyphens, never underscores or spaces, so nothing needs quoting
(`CODE_OF_CONDUCT` is the ecosystem's spelling, not ours, and is the one
exception) — **and an embedded roadmap ID is a second exception,
spelled however its project spells it.** `roadmap-format.md` §3.5.1
deliberately admits prefixes containing an underscore, and IDs that are
not `<PREFIX>-NNNN` at all, and says a checker built from it must not
reject them; a filename rule that overrode that would make a conforming
project unable to name its own specs. The ID is copied, never
transliterated — a filename that turns `3D_E-0007` into `3d-e-0007` no
longer contains the ID, so searching for the ID stops finding the spec.

## 4. Per-kind format standards

This file holds what is true of every document. What is true of one kind
lives with that kind — the same split as `coding.md` and `languages/`.

| Kind | Owned by |
|---|---|
| Specs and plans | [spec-format.md](spec-format.md) |
| Roadmap | [roadmap-format.md](roadmap-format.md) |
| Changelog | [changelog-format.md](changelog-format.md) |
| Tests and their contracts | [testing.md](testing.md) |

Where this file and a per-kind standard both speak, the per-kind one
wins for that kind.

**A new standard starts from
[skeletons/standard-skeleton.md](skeletons/standard-skeleton.md), which is a
template and NOT a per-kind standard — the precedence below does not reach
it.** A template that disagrees with a standard is a
template to fix.

**"Both speak" means the per-kind standard NARROWS or REPLACES a rule for
its kind — never that it restates one.** § 2.1 forbids the restatement, so
the two rules cannot collide over one passage: a per-kind standard says
"in a spec this section is numbered", cites the section it is narrowing, and
stops. **A rule found stated twice is § 2.1's problem, not this
precedence clause's** — delete the copy, do not arbitrate between them.

## 5. Documents a project must have

### 5.1 README.md

The first thing anyone reads, and often the only thing. In order:
project name and one-line description; current version with links to
changelog and roadmap; what it does; how to install it; the shortest
sequence that makes it do something useful; where the rest of the
documentation is; licence.

Avoid a table of contents on a short README, an "About" section with no
content, and any screenshot link that does not resolve.

### 5.2 CLAUDE.md

Project-specific agent instructions, at the repo root: how to build, how
to test, the module map, conventions particular to this codebase, and
design decisions that are not visible from reading the code.

**Only what is project-specific.** Machine-wide rules live in the global
`CLAUDE.md`, and repeating them here creates a second home that will
disagree (§2.1).

### 5.3 SECURITY.md

For anything that accepts external reports: the project's trust
boundaries (`security.md` §1), how to report a vulnerability, and which
versions receive fixes.

### 5.4 LICENSE, CODE_OF_CONDUCT, CONTRIBUTING

Canonical text, unparaphrased, for the first two. `CONTRIBUTING.md`
where a project accepts outside contributions: how to build, what is
expected of a change, and where the standards are.

## 6. Contract and API documentation

For any exposed surface — an API, a plugin contract, a file format:

- **Every public symbol is documented.** If it is exported, it is part
  of the contract whether it is documented or not.
- **Record the version each was added in**, so a consumer knows what
  they can rely on.
- **Show input and output.** A type signature alone leaves the shape to
  be guessed.
- **Mark deprecation explicitly, and give the migration path.** A
  deprecation with no replacement named is a dead end.

## 7. Screenshots and videos

Screenshots live in `docs/screenshots/`, recordings in `docs/videos/`.

- **Named for what they show and its state** — never for the date they
  were taken. `export-dialog-error.png` can be found; `Screenshot
  2026-04-28.png` cannot.
- **Captioned in the surrounding prose**, so the reader knows what they
  are looking at and why it is there.
- **Replaced, not accumulated.** When the interface changes, swap the
  file; do not leave `_old` and `_v2` beside it. Two images of the same
  screen with no way to tell which is current is worse than one stale
  one.

**An image or recording of an interface that no longer exists is a false
claim** — and one that nothing mechanical will ever catch, because the
file resolves perfectly. Only a person who knows the current interface
can see it.

Videos carry a specific risk screenshots do not: **they are expensive to
re-record, so they rot further before anyone replaces them.** Prefer a
recording of something unlikely to change soon, keep it short enough to
redo, and say in the caption which version it shows — so a reader can
tell whether to trust it even when it is out of date.

## 8. Markdown style

- ATX headings (`## `), never setext underlines. One blank line either
  side.
- Tables for structured data, fenced blocks for code.
- Wrap prose at roughly 70–80 columns so a diff shows what changed;
  never force-wrap inside a code block or table.
- One bullet marker per file — do not mix.
- Backticks for filenames, symbols and flags.

## 9. Review discipline

### 9.0 Run the mechanical checks while writing, not at the gate

**A document is not finished until the deterministic checks have run
over it** — links, paths, quoted fragments, census counts, required
sections, `§` pointers. Not once it reaches a reviewer: **before it is called
done.**

**`check-doc-facts` is what runs them**, and naming it here is the point of
this paragraph: the `pre-commit` hook below **overlaps that list rather than
being a subset of it**. `path` sits on the list above, against `paths`. **Tool
grants overlap too** — `links` emits `ungranted_tool` on a skill calling a
verb its own grant list omits, where the hook's class also reaches commands
and agent files and matches the call's shape. **`section` sits on it
nowhere.** It fires on a prose `` `<doc>.md` § N.N `` pointer; `links`' anchor
check reads a markdown `[t](#slug)`, `paths` resolves an asserted path, and
`section-exists` excludes a pointer into one named file. So a `§` pointer is
caught by this hook or by nothing, and by nothing at all in a scaffolded
project, whose skeleton omits the class. The rest — deleted text surviving
elsewhere, citations to renamed paths, unresolved provenance ids, and
**every** table's row shape — are this repository's own. **`check-doc-facts`
has no general table check.** Its `what-checks-this` check takes the one table
its own name gives it, so the hook's `table` class overlaps nothing. It read
*"a blocking subset of that list"* until 2026-08-14 (ROADMAP CFG-0107), which
invited the reverse error: treating a clean `check-doc-facts` run as covering
the hook, and so dropping the survivor class, which the § 2.1 row below names
as its only catcher across documents. A commit that passes **this
repository's** hook has had **every blocking class the table below lists** run
over it — and its links, quoted fragments, census counts and required sections
not checked at all — **and, where the staged file is under `docs/` or
`skeleton/files/`, not the classes whose `Skips` column excludes it either.**
**A project scaffolded from the skeleton has fewer**: that copy carries
`survivor` and a `link` class, for the reason given below. **`link` is not
`path`** — the paragraph below the table says how they differ. **Passing the
hook is not this rule satisfied**, and a session that reports otherwise has
read the second half of this section and not the first.

**The cost difference is the whole rule.** A mechanical defect caught
while writing costs one edit. The same defect caught at the gate costs a
dispatch, a verification, a fix, and possibly another loop — and it
occupies a reader who could have been finding something no tool can.

**This binds to the act of writing, not to a skill being invoked.** An
authoring skill running its checking counterpart is the same rule
arriving by a convenient route; a hand-written document gets no such
route, and hand-written is how most standards are produced. Measured
2026-08-10: `foundation.md` went to a cold review with six mechanically
decidable defects in it, one of which an existing check would have
caught outright — nothing had run the checks, because nothing had
required them.

**This rule now has a check, and a new project gets it too.** A
`pre-commit` hook (`.githooks/pre-commit`, enabled via `core.hooksPath`,
shipped in `skeleton/files/`) runs the decidable half in **blocking** classes
only.

**Read the table, not a paragraph.** Three loops of cold review each found an
error in this section and few anywhere else, because classes described in prose
drift — one of them is always slightly wrong, and a different one each time.
**That is also why no sentence here counts them.** A prose count beside a table
is a second copy of the same fact (§2.1), and it goes stale the moment a class
is added.
Three properties vary per class and none of them is optional, so an omission
has to show as an empty cell rather than as a sentence nobody wrote.

| Class | Fires on | Skips | Reads |
|---|---|---|---|
| **path** | a `~/.claude/…` path anywhere in the file, or a markdown link target beginning `./` or `../`, that resolves neither from the repo root nor relative to the staged file's own directory. A shell brace set (`a/{x,y}.cpp`) is expanded and **every** member must resolve | **a staged file under `docs/` or `skeleton/files/`**; a line carrying a `YYYY-MM-DD` date; placeholders (`<…>`, `{{…}}`, `*`); a truncated path; **a backticked bare path with no `~/.claude/` prefix, which it never extracts at all** | the staged file |
| **survivor** | text of 45+ characters this change deletes that still exists in another tracked `*.md` | **`docs/`**, and **`ROADMAP.md` and `CHANGELOG.md`**, which are record genres the search never enters; the file being edited; **a line the same change adds back to that same file, which a whole-file reflow does to every line**; a hit in a file the same commit *added* the text to; **a copy this change replaces with a POINTER — the same change adds, to the file the text left, a line naming the file the copy survives in**; **text this change moves to a record under `docs/`, where the same change also adds to the file the text left a line naming that record — both artefacts required, because the search never looks inside `docs/` and the destination is therefore never a hit**; hits across the `draft/`-versus-live boundary; fragments under 45 characters; **a copy wrapped differently from the deleted line, which the class matches whole-line** | the tracked tree, working copy — **not** `--cached` |
| **allowed-tools** | a skill, command or agent file instructing an `mcp__ants__*` verb its own grant list omits. **It matches the verb's call SHAPE — backticked with arguments, or near a call word — and cannot see the SUBJECT**, so a sentence describing some OTHER artefact making the call fires identically; the remedy is to break the shape, since deciding the subject is not mechanical (CFG-0214) | **a staged file under `docs/` or `skeleton/files/`**; `review-agent-rules` itself; `*/templates/*`; **a file with no grant line at all**, absent meaning unrestricted, so nothing can be breached; a sentence that negates the call (*never*, *not*, *don't*, *cannot*, *without*, *no*) | the staged file, plus the grant list in its own or its skill's frontmatter |
| **citation** | a document citing a path this change renames or deletes, where no candidate resolves | **`docs/` and `draft/`**, and **`ROADMAP.md` and `CHANGELOG.md`**, which are record genres; removed paths under `*/templates/`; the removed file itself | the tracked index, `--cached` — **a staged document is searched like any other**, the removed file being the only one skipped. The class exists for documents *not* in the commit, which is its reason rather than its scope |
| **section** | `` `<doc>.md` § N.N `` naming a heading that document does not have | **a staged file under `docs/` or `skeleton/files/`**; a line carrying a `YYYY-MM-DD` date; `*/templates/*`; a document name resolving to zero files or to several; a letter-suffixed id (`§ 1a`), which names an item rather than a heading | the staged file, plus the tracked `*.md` list |
| **roadmap-id** | a **parenthesised** provenance marker — `(CFG-0108)`, `(ROADMAP CFG-0108)` — whose id has no bullet in `ROADMAP.md`. The prefix is sniffed from the file, never hardcoded | a **bare** id in prose, which it never extracts at all; `ROADMAP.md` itself; a checkout with no `ROADMAP.md` or no ids, where it skips silently | the staged file, plus the bullet ids in `ROADMAP.md` |
| **table** | a table row whose unescaped-pipe count differs from its header row's | anything inside a fence, which is a specimen; an escaped `\|`, masked before counting; a `\|` line that is not part of a table | the staged file only |

**Two different things are both called a `docs/` skip, and conflating them
misreads the table.** A class either declines to *run on* a staged file, or
runs and declines to *search inside* a directory.

- **The staged-file skip:** the `Skips` column carries it, spelled *a staged
  file under `docs/` or `skeleton/files/`*. Records describe a tree as it was
  on a date, and templates prescribe paths for a project that does not exist
  yet.
- **The search-scope skip:** `survivor` and `citation` never look *inside*
  `docs/` (and `citation` also skips `draft/`). `survivor` takes its needles
  from the staged diff and then searches the tree, so the first skip governs
  where it LOOKS rather than whether it runs.

Two independent cold readers landed here in one loop.

**The `docs/` exclusions are the largest hole here, and not one a blocking
check can close.** Between them the survivor and citation classes never look
inside `docs/` — every spec, plan and ADR — so a rule copied into a spec is
invisible to the check §2.1 leans on. **Both search-scope classes skip the
record genres too**, `ROADMAP.md` and `CHANGELOG.md`; the `Skips` column is
the enumeration. `docs/` is where a rule is legitimately
*quoted* rather than restated, so a class that fired there would fire
constantly and correctly-but-uselessly, which is the advisory channel again.

**What the table cannot carry — the arguments — is below it.**

**The path class is narrower than "a broken link", in both directions**, and
its row says how: a repo-relative link target with no leading dot is not
caught however broken it is, while a `~/.claude/…` path in bare prose is.

**Writing that sentence tripped the check.** The first draft illustrated the
dot-relative form with a literal example, which the hook read as an asserted
path and blocked — the same shape as the census warning firing on § 2.3's own
examples. **A document describing a path check will contain paths that assert
nothing**, and that is a fourth shape beside the three below. It is left
unhandled on purpose: the scope rule that would admit it cannot be written
without admitting real breaches too, and rewording costs one sentence.

**Every class the table lists except `survivor` is absent from the skeleton,
and each is absent on purpose.** `allowed-tools` (CFG-0065) generalises
nowhere, since grant lists are a Claude Code convention rather than a
documentation one. `citation` (CFG-0067), `section` (CFG-0023), `roadmap-id`
and `table` (both CFG-0129) are out for CFG-0028's reason: each is measured
here and nowhere else, and an unmeasured check shipped to every new project is
the warning channel in a different costume. **`path` is absent too**, and it
is the one with a substitute rather than a gap — the skeleton ships the weaker
`link` class in its place.

**The skeleton copy carries `survivor` and a `link` class of its own.** The
`link` class fires on a markdown link target that does not resolve. **It is
not the `path` class above**, and the paragraph three above says how the two
differ.

**`allowed-tools` settles something about this section itself: a check may
cover part of a class, and is worth shipping when it does.** Only two shapes
are decidable — a call written with its arguments, and a call word immediately
before the verb — so it takes those and leaves the rest to
`review-agent-rules`. **The price of a partial check is saying which part**,
in the place a reader would otherwise assume coverage; that is what the
`Skips` column is for.

**`section` shipped on precision rather than yield**, which is the opposite of
the evidence behind `allowed-tools` and `citation`: it caught nothing on the
history it was replayed over. A dead `§` pointer renders perfectly and fails
only for the reader who follows it, so a class that never fires is still worth
its cost. Each class's own comment in `.githooks/pre-commit` records its
measurement, at the point it is implemented.

**The table above is the home of the class list and of the `Fires` column. The
hook owns the exclusion enumeration — the `Skips` and `Reads` properties — as
the artefact that executes it, and the hook's header says so outright — *"this
list is the enumeration"*, *"this list is the home BECAUSE it is the one that
executes"*. It carries each class's exclusions and its measurement, and
`survivor` is not the only class with a list.** The cells above
restate that enumeration for a reader, and are updated in the same change that
edits the hook. **So adding or removing an exclusion is an edit to
`.githooks/pre-commit` first and to the cell second; adding or removing a
*class* is an edit here first.**

Decided 2026-08-20 by the user (CFG-0176), reversing half of the 2026-08-14
settlement recorded below. Two artefacts each declared themselves the
enumeration — this sentence, and the hook header's *"this list is the
enumeration"* — so a maintainer adding a seventh `survivor` exclusion could
follow either and leave the other stale. An enumeration nothing executes is the
one that goes stale, which is why the executing side won.

**What the 2026-08-14 settlement still holds is the class list**, and its
reasoning is why the split falls where it does. That header said this section
delegates the hook's scope to it — all of it, the class list included — which
would have made the code the home of a rule — so
someone adding a class would update the header, believe the rule updated, and
leave this table stale. Settled here 2026-08-14 (ROADMAP CFG-0107), and the
header was corrected the same day to say so. **It was incomplete in the same
place**: it called the survivor class's exclusions *"two, both measured,
neither optional"* while the implementation carried a third, `docs/` — the one
the `survivor` row above lists. Both were fixed together, which is the shape to
expect: a header claiming to own a scope drifts from it, and the drift shows up
as an undercount.

**There is no advisory channel, and that absence is the more important
half of the hook.** Measured over the 113 tracked documents: 48 hits, none actionable, the
dominant class a threshold rather than a population, four of them §2.3's
own examples of the rule. It fired on most commits made that day and not
one was acted on — while sharing a channel with the path check, which
works. **A warning nobody acts on is worse than no check**, because it
teaches its reader to skip the channel both arrive on. A check added
here blocks, or it does not go in.

**A path has the same three shapes a number does, and the hook learned the
third the hard way.** Current (checkable), *prescribed* (a template telling a
project where to put its own file — out of scope), and **dated historical**
("opened 2026-08-08 as `X`; moved since"), which was true when written and is
not a claim about now. The hook blocked on one of those on 2026-08-10 and the
scope rule was widened rather than the commit bypassed — its own message
demands that order, because one `--no-verify` is how a gate dies. A line
carrying a `YYYY-MM-DD` date is now out of scope for the path check. **That is
not `counts`' exemption** — §2.3 exempts a number only where it reads as past
tense *and* carries its date, so a present-tense count with a date beside it is
still a census. **This hook has no count check of
its own** for the comparison to reach — its warning was deleted a paragraph
ago. Which classes carry the date exemption is the table's `Skips` column.

**The backticked-bare-path gap the table records is deliberate**, not an
oversight: a document names such a path both to *assert* it exists here and to
*prescribe* where a project should put its own, and nothing in the form
distinguishes them. Widening the check to cover it reintroduces the noise that
made an earlier version unusable.

**The same checks re-run after a fix, over what the fix produced.** A
fix that adds a structure — a table, a section, a list — is checked as
that structure, not merely swept for stale references. In the same run,
a repair introduced a table whose first row named a check that does not
exist; `check-doc-facts`' `what-checks-this` check resolves the names a cell
gives for things that exist — a skill, a hook, a command, a verb — and it
was never pointed at the new table. **It does not resolve a sub-check name
inside `check-doc-facts`**, which is how a cell of `spec-format.md`'s own table naming a check
dropped on 2026-08-10 survived a week — along with two citations of it
sitting outside any table, which this check never covered at all. That check takes
only the `What checks this` table, so this works for that one structure
and for no other.

### 9.1 The gate

**Every contract document runs through a cold read before the work it
governs starts** — a spec, plan, ADR, design doc or standard, reviewed
by someone with no memory of writing it, looped until a pass finds
nothing that changes what gets built — **or until the review instrument's
loop cap binds**, at which point the remaining findings are filed and the
document ships. `review-contract` owns the cap and its numbers.

Before implementation, not after. A contract is what the implementer
builds from; if it is wrong, the implementation is wrong by
construction. Reviewing afterwards costs both.

Two requirements this standard adds:

- **The loop log is written as the loops happen**, in a `## Cold-eyes
  loop log` section, after the What-checks-this table **in a standard or a
  reference; a spec's position is §2.9's**. Back-filling destroys
  the audit trail, which is the only evidence the review was real.
  **Where that section names a record, the rows are written there and the
  section keeps the heading and a one-line pointer.** These records live
  under `docs/reviews/`.
  § 2.9 calls a loop log a record rather than content, and a document read in
  full whenever it is consulted pays for that record on every read. The
  heading stays because contents tables link to its anchor and
  `check-doc-facts`' `mandated-section` requires the section; the `loop-log`
  tally check fires only on a section carrying a table, so a pointer fails
  nothing.
- **The tally balances.** Eight findings against six outcomes is a row
  where two findings were dropped without anyone deciding to.

### 9.2 Sweep shipped documents separately

Docs and code drift apart on their own schedules, so a documentation
sweep is its own activity, not a step inside a code review. What it
looks for: flags that changed, screenshots of an interface that no
longer exists, relative dates, sections describing removed features,
cross-references to renamed things, ledger claims the code contradicts.

### 9.3 Escalate a repeated class into a check

**When the same *class* of defect is caught twice, it stops being a
review finding and becomes a mechanical check.** Not an item on a
checklist someone must remember — a check that runs.

Catchers, cheapest first:

| Catcher | Cost | For |
|---|---|---|
| a deterministic check | seconds, every run | anything countable or greppable |
| a checklist with a fixed trigger | a minute, when triggered | judgement a script cannot make |
| a cold reader | a review pass | contradictions, gaps, a wrong approach |
| the user | a bug report | what the first three missed |

**A defect paid for at cold-reader prices that a grep could have caught
is a process failure, not a review success.**

### 9.4 Prose is outside every review here

**Wording, register, tone, presentation ordering and examples are nobody's
job** — presentation ordering never meaning a position a rule mandates,
which is a conformance breach and not a preference. **Nothing checks a
mandated position**: `mandated-section` tests that such a section is
PRESENT, which is what this document's own table credits it with. No
review here asks whether a document is well written, and a finding whose
only remedy is *reword this* is out of scope however true it is.

The reason is that the question has no bar. Nobody can be caught being
wrong about it, so the dimension never comes back clean. `review-contract`
ran fifteen dimensions until 2026-08-08 and did not converge: six could
never come back clean, their findings became edits, and the edits broke
things that were true. `skills/review-contract/references/rationale.md`
holds the measurement.

**Length is excluded too, and §2.8 owns why.** *Make this shorter* is not
a finding. §2.8's three symptoms are authoring rules, not review questions:
duplication is §2.1's and is **deliberately unowned at review**, and a review
sees two copies only when they disagree. `skill-method.md` owns the
companion rule — *there is no size budget, and nothing may stand in for one*
— and it bans every proxy, not only a literal ceiling.

**A review that excludes prose points here rather than restating why**
(§2.1). Nothing on this machine owns prose work today, so a real prose
defect is repaired by hand, outside every gate — §2.8's length rule and
`spec-format.md` §5.3 are authoring obligations, never review questions.

## 10. Anti-patterns

- ❌ A fact stated in more than one document — except the two cells §2.1
  keeps in both places on purpose: a machinery cell a run executes, and a
  cell restating an enumeration a hook owns.
- ❌ A `path:line` citation.
- ❌ A claim written from memory of how the code works.
- ❌ Relative dates in a committed document.
- ❌ Documentation for something that has not shipped — behaviour
  described to a reader, not a spec or plan written for a builder
  (§2.7).
- ❌ A screenshot of an interface that no longer exists.
- ❌ Placeholder text left in.
- ❌ A rule with nothing checking it and no row admitting so.
- ❌ A plan that argues for its approach.
- ❌ Repeating a global rule in a project document.

## What checks this

| Rule | What catches a breach |
|---|---|
| §2.3 no `path:line` citations | **`Partial:`** `check-doc-facts` `paths`, a path-resolution check, flags a `path:line` that fails to resolve, which catches it incidentally rather than as a style breach — a `path:line` whose file half resolves passes. **Nothing** catches the prose form, "around line 786", which §2.3 says outright |
| §2.2 a quotation names its source | **`Partial:`** `check-doc-facts` `quotes`, a quotation-drift check, re-verifies an *attributed* quotation when its source changes. **Nothing** catches an unattributed one, which is the form the rule exists to prevent — that is why the rule is written as an authoring obligation |
| §2.3 no census counts | **`Partial:`** `check-doc-facts` `counts`, a census-count check, which names §2.3 as its own authority; the `pre-commit` hook has no count check of its own (§9.0), so a census reaches a commit uncaught. Added 2026-08-20; §2.3's larger half had no row while its `path:line` half did |
| §2.9 cell forms | **`Partial:`** `check-doc-facts` `what-checks-this`, a cell-form and name-resolution check over this table. **Nothing** resolves a sub-check name inside `check-doc-facts` (§9.0), which is most of what these cells name |
| §2.2 cited symbols exist | `check-doc-facts` `symbols`, a symbol-resolution check against current source |
| §2.9 required sections present | **`Partial:`** `check-doc-facts` `sections`, a required-section check against the format standard, **for a spec only** — it reads a spec's machine-readable block, and a standard's or reference's requirement is prose, which only `mandated-section` reaches. Both test PRESENCE; **nothing** checks that a mandated section is in the mandated PLACE, though §2.9 requires `## What checks this` last and the loop log after it (added 2026-08-20, when §9.4 credited a position test that does not exist). This row named `structure` until 2026-08-17; that check is fences, id gaps and parse |
| §9.1 loop log present, tally balances | **`Partial:`** `check-doc-facts` `loop-log`, a tally check over the log's own rows, plus `mandated-section`, a required-section check, for the log's presence. The `loop-log` check does not test presence; this row credited it with both until 2026-08-17. It credited `sections` for presence until 2026-08-20, and `sections` reads a *spec's* machine-readable block and skips a standard, whose requirement is stated in prose — which is exactly what `mandated-section` exists for. **The rows-in-the-reference half is caught by nothing** — a row written into a section that points elsewhere reads as conforming to the sentence before it, and no check compares the two |
| §9.0 checks run before done | **`Partial:`** the `pre-commit` hook blocks this repository's classes at commit time — after *done*, not before it, which is what the rule asks. **Nothing** catches a document called done without `check-doc-facts` having run |
| `roadmap-format.md` / `changelog-format.md` parse | see each of those documents' own § What checks this. §4 routes to them and states no parsing rule of its own, so it states no *coverage* of its own either (§2.1). This row read "the parsers that read them" until 2026-08-14, where `changelog-format.md` says nothing validates a `CHANGELOG.md` at rest |
| §2.6 absolute dates | **nothing** — both forms are greppable, so this is a check worth adding |
| §3.1 / §3.3 filename form | **nothing** — the ID prefix and the `SHOUTING.md` / `lowercase-kebab.md` split are both greppable, so this is a check worth adding |
| §2.1 one fact one home | **`Partial:`** the `pre-commit` hook's survivor class (§9.0) blocks when a change deletes text that still exists in another tracked `*.md` **outside `docs/`, `ROADMAP.md` and `CHANGELOG.md`** — so it catches the copy you *edited past*, only at the moment you edit it, **never a copy living in a spec, plan or ADR**, and **never a copy wrapped differently from the deleted line**, which the class matches whole-line. `check-doc-facts` `enumeration-parity`, an enumeration-parity check, covers a second slice — two lists of the same set inside ONE document — as CANDIDATES rather than findings. Nothing finds a rule sitting in two documents that nobody is touching: a cold read sees "these two passages disagree" within one document only, and **a rule copied into a second document is invisible to a single-document review** unless that document is in the review's packet. `review-contract-set` reaches that half only once the two copies **disagree** — its Q1 asks whether two documents contradict each other, so a faithful copy is caught by **nothing**, which is what § 9.4 states. And it is a review, not a check, so this row stays `Partial:` |
| §2.2 the claim is *true* | **nothing** — resolution proves a symbol exists, not that the description of it is right |
| §2.7 nothing documented before it ships | **nothing** — forward-reference versus defect is a judgement |
| §2.8 length | **nothing** — an earned length cannot be told from an unearned one, which is the whole of the rule. |
| §2.2 a negative claim was searched | **nothing** — the search that would settle it is exactly the one the author skipped |
| §2.8 delete first, write second | **nothing** — visible only as a later review loop's findings landing in added text |
| §9.0 the hook's exclusion enumeration and the `Skips` cells agree | **nothing** — no check reads a shell comment against a prose cell, and the two have drifted in both directions |
| §2.8 pedigree goes to the history file, where the document names one | **nothing** — a dated clause written inline reads as conforming to the sentence permitting it, and no check can tell a document that names no such record from one ignoring it. The provenance-split test it rests on is not checkable either: a clause moved correctly and one moved wrongly produce the same diff |
| §7 screenshots current | **nothing** — a reader, or a user noticing |
| §9.3 repeated classes become checks | **nothing** — a habit, evidenced only by the `nothing` rows' *share* of the table falling over time (§2.9, which rules out the raw count by name) |
| §9.4 prose is outside every review | **nothing** — `review-skill` asks of each rule it enumerates what that rule must not fire on, so a skill that OMITS the exclusion offers it no rule to fire on. A prose finding raised inside a run is caught only by the parent dismissing it case by case, which is a review pass |

The `nothing` rows are this standard's honest error budget. **§2.2's *the
claim is true* row is the largest hole in it**: a checker can prove that
`derive_key()` exists, and nothing can prove that what this document
says about it is true.

## Cold-eyes loop log

Rows live in [`docs/reviews/documentation-loop-log.md`](../docs/reviews/documentation-loop-log.md).
`documentation.md` § 9.1 owns the form.
<!-- MIRROR END -->
