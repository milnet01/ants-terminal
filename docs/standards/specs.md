<!-- ants-specs-standards: 1 -->
# Spec-authoring standard

**Status:** v1 (2026-05-21).
**Applies to:** every spec under `docs/specs/` — the
`<ID>-<topic>.md` shape § 2 requires, and the bare `<ID>.md` of
legacy files.

**Why this is a full standard and not a `spec-format-overrides.md`.**
`~/.claude/standards/spec-format.md` says it is the only copy and that a
project needing to differ writes deltas only. That rule targets projects
carrying a *verbatim copy* that silently forked; this file is not one.
The two prescribe different documents — the global standard's section
set (Goal, Scope decisions, Design, Failure modes, Alternatives
considered, What checks this, …) is not this one's (§§ 3-4), and it
names the design section *Surface* — so the "deltas" would be almost
the whole file. It is also **executed, not just read**:
`src/speclint.cpp`, `src/specparse.h` and `src/speclog.h` implement the
rules below, and the `spec_lint`, `mcp_spec_query` and
`spec_parse_test_surface` feature tests lock them. Read
the global standard for anything this file does not cover; where they
differ on the *shape* of a spec under `docs/specs/`, this file wins.
That applies to the in-repo mirror at `docs/standards/spec-format.md`
too, which is the same document.
**The one exception is § 1** — whether a spec is needed at all is
`spec-format.md` § 1's call, not this file's.

A *spec* is the implementation contract for one ROADMAP item. It sits
between the one-paragraph ROADMAP bullet (the *what* and *why*) and the
code (the *how*, in detail). It is the document a reviewer reads to
answer "is this design right?" before any code exists, and the document
a future maintainer reads to answer "why is the code shaped this way?".

This standard codifies the format the corpus already follows so authors
stop reverse-engineering it from old specs. It also keeps specs
parseable by the `spec_query` MCP tool (§ 6).

---

## 1. When a spec is required

**`spec-format.md` § 1 owns this question** — its triggers, its skip
test, and the escape hatch for a call that went the wrong way. Read it
rather than deciding from this file.

What this file adds is where the contract lives: a `spec.md` under
`tests/features/<name>/` (the feature-conformance contract) is the right
home for a small, single-invariant behaviour, and `docs/specs/` is for
contracts that span subsystems. **§§ 2-7 bind the second only.** A
feature-test contract follows `tests/features/README.md`, carries no
loop log, and is not gated. A default `spec_lint` run walks `docs/specs/`
and never reaches it; pointed at one directly it reports the whole
required set as absent, which is not a defect in that file.

## 2. File + naming

- One spec per file at `docs/specs/<ID>-<topic>.md`, where `<ID>` matches
  `^ANTS-[0-9]+$` (the stable ROADMAP id from `.roadmap-counter`) and
  `<topic>` is two to four kebab-case words.
- **The topic slug is required on new specs** (user decision, 2026-07-30).
  A directory of bare ids is navigable only by someone holding the id;
  people remember names. `spec_query` resolves `<id>-*.md` as well as
  `<id>.md` (ANTS-3356), so both shapes are addressable by id and the
  slug costs nothing mechanically.
- Bare `<ID>.md` remains valid for the specs already written in that
  shape. Renaming them is a corpus-wide sweep with its own dry-run
  script, tracked by **ANTS-3755**; do not rename them piecemeal, which
  would leave the corpus half-converted with no record of which half.
- The spec elaborates exactly one ROADMAP bullet. Cross-cutting work
  that spans several ids gets one spec per id, cross-referenced in the
  header, or one umbrella spec whose header lists the ids it covers.
  **An umbrella spec's filename takes the id it primarily elaborates**;
  `**Covers:**` (§ 3) lists them all. Only the filename id is addressable
  by `spec_query`, and no tool reads `**Covers:**`, so each other id
  needs its own ROADMAP bullet to name the spec.

## 3. Required structure

Every spec opens with these, in this order. They are not the whole
required set — § 4 carries two more: **§ RAM / build cost** when its
trigger applies, and **§ Cold-eyes loop log** always.

The block below is that rule in the form `spec_lint`'s `missing_section`
check reads (ANTS-4345). It **adds no obligation** — every entry is
already required by this section or by § 4, and it is deliberately a
*subset* of them:

- **§ 3.1 Title and § 3.2 Header block are absent** because they are an
  `#` heading and bold key-value lines, not `##` sections, so the check
  cannot address them. `spec_query` already fails loudly on both.
- **§ RAM / build cost is absent** because § 4 requires it *conditionally*
  ("whenever its trigger applies"), and a flat list cannot carry a
  condition. Listing it would convert a conditional rule into an
  unconditional one, which is a change this standard has not made.
- **The entries carry no section numbers, on purpose.** This standard
  does not fix them — § 4's optional sections are inserted wherever they
  read best, which shifts every heading after them — so the check matches
  these by name at whatever number a spec gives them. A standard whose
  numbering *is* part of a section's identity writes the numbers in and
  gets an exact match instead; the global `spec-format.md` does exactly
  that.

<!-- required-sections -->
```
## Problem
## Surface
## Invariants
## Tests
## Cold-eyes loop log
```

**This block is the one the check reads here.** `spec_lint` tries
`docs/standards/specs.md` before the mirrored `spec-format.md` beside it
and stops at the first file carrying a block, so a project's own standard
wins — which is what that standard's own § 3 says should happen
(ANTS-4895). The response's `sections_source` names the file used.

**Expect a real backlog over `docs/specs/` even so.** Much of the corpus
predates this standard (v1, 2026-05-21); § 2 already records that.
`spec_lint` measures the current figure — this document does not restate
it, because a count here reads as present tense and rots.

A spec that cannot conform — a build plan or a ledger parked in
`docs/specs/`, or a pre-standard file nobody is rewriting — may exempt
itself with `<!-- spec-lint: no-required-sections -->` on its own line
outside any fence, which suppresses every `missing_section` finding for
that document. It is not a licence for a new spec.


### 3.1 Title (H1)

```
# ANTS-NNNN — <short imperative title>
```

The H1 is what `spec_query` returns as `title`. Keep it one line;
backtick code identifiers (`audit_run`, `LlmClient`).

### 3.2 Header block

Bold key-value lines immediately under the H1, blank-line separated
from § 1:

```
**Status:** spec draft (YYYY-MM-DD).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-NNNN (<provenance>).
```

- **Status** (required, parsed by `spec_query`) — lifecycle:
  `spec draft (DATE)` → `accepted (DATE)` → `shipped X.Y.Z (DATE)`.
  Append cold-eyes progress inline as it happens, e.g.
  `spec draft, cold-eyes loops 1 + 2 folded (DATE)` — see § 5.7.
- **Kind** (required, parsed by `spec_query`) — the same taxonomy as
  the ROADMAP `Kind:` field (`implement`, `fix`, `refactor`, …; see
  `roadmap-format.md` § 3.5.3).
- **Source** (required) — provenance: the ROADMAP id plus where the
  ask came from (`user-request-DATE`, `indie-review-DATE lane-N`, an
  incident, a dependency surfaced while sizing another item).
- **Optional relationship lines** when they apply: `**Blocker for:**`,
  `**Blocked by:**`, `**Pairs with:**`, `**Composes with:**`,
  `**Supersedes:** / **Superseded by:**`, and `**Covers:**`. Use these so
  the dependency graph lives in the docs, not only in someone's head.
  `**Covers:**` is the umbrella form of § 2 — one spec whose header lists
  every ROADMAP id it is the contract for, used when two ids share one
  surface and would otherwise own two documents that must agree forever.
  The ids stay separate on the ROADMAP; only the document merges.

### 3.3 § 1. Problem

What is broken or missing, and why it matters *now*. Ground every
claim about current behaviour in a symbol reference
(`src/foo.cpp::bar()`, per [`documentation.md` § 1.7](documentation.md))
checked against live source — the spec-side application of the global
verify-before-stating rule. State the consequences as a numbered list
when there is more than one — it makes the invariants in § 3.5 easier
to trace back.

### 3.4 § 2. Surface

The design. This is the largest section; break it into `### 2.1`,
`### 2.2`, … subsections by concern — directory layout, data shapes,
API additions, algorithms. Conventions:

- Show new structs / function signatures in fenced ` ```cpp ` blocks,
  written in current-idiom Qt6/C++20 (per `coding.md`).
- Show file layouts, JSON schemas, and wire formats as fenced blocks.
- Algorithms may be pseudocode in a fenced block when prose is unclear.
- Name the **exact** files, functions, libs, and config keys you will
  add or touch. "A helper in the audit lib" is not a contract; 
  `src/auditcache.cpp` joining `ants_audit_lib` is.

### 3.5 Invariants

A section (`## N. Invariants`) of numbered, testable contracts. **Use
the bullet form** — it is what the overwhelming majority of specs use
and what reviewers expect:

```
- **INV-1** — <one testable claim>. *Test:* <test surface>.
- **INV-2** — <…>. *Test:* <…>.
```

The GFM table form (`| INV-1 | claim | test surface |`) is also parsed
by `spec_query` and acceptable, but the bullet form is the default;
don't introduce a table without reason.

Rules for invariants:

- Each is **independently testable** — it names a condition a test can
  assert and a failure a test can trigger. "The code is clean" is not
  an invariant; "`reaper` deletes only files named in dropped manifest
  entries; foreign files survive" is.
- Each carries a **test surface** (the `*Test:*` clause) — the feature
  test, source-grep, or manual recipe that locks it. An invariant with
  no test surface is a wish, not a contract.
- **A command clause states what the command should return.** Where the
  clause holds a code span opening with a command word — `grep`, `rg`,
  `ctest`, `sed` and the rest of a closed list `spec_lint` owns — text
  carrying a letter or digit must follow the
  last such span — `` `grep -c foo src/` `` alone is reported by
  `spec_lint`'s `command_test_no_expectation`, where "3 hits" after it
  is not. Trailing punctuation does not count. The corpus habit of an
  arrow (`→ 3 hits`) reads well and is not required.
- Number them `INV-1`, `INV-2`, … and **never renumber** once the spec
  is referenced elsewhere — invariants are cited by id from CHANGELOG,
  CLAUDE.md, and other specs. Add `INV-14`, don't reflow.

#### 3.5.1 Pattern invariants — state them so they can be run

When an invariant's claim is about a **regex pattern**, state the
pattern as a fence tagged with its engine and put an `| input |
expected |` table directly beneath it. In that form the
`spec_conformance` verb *runs* the pattern against each row — within
its per-run case cap, which reports `truncated` rather than stopping
silently, and skipping any pattern or input over 512 bytes as a
`too_large` refusal — and a pattern that disagrees with its own
example comes back as a finding instead of surviving a cold read. The
example below is nested inside a four-backtick fence so this standard
does not itself become a case:

````markdown
```regex pcre2
^- \*\*\[(ANTS-\d+)\]\*\*
```

| input | expected |
|---|---|
| `- **[ANTS-4108]** ships the verb` | `ANTS-4108` |
| `  - **[ANTS-1]** indented` | no match |
````

**Three mistakes make the block invisible — no finding, no candidate,
no refusal.** They are stated here because they are the ones no reader
can detect; everything else below is reported back to you:

- **Start the fence at column 0**, with only blank lines between it and
  the table. The fence and its table are siblings of the `INV-N`
  bullet, never indented children of it — an indented fence is not a
  fence. (Prose between fence and table *is* reported, as a candidate.)
- **The first word of the info string must be exactly `regex`.** A
  fence tagged ` ```regexp `, ` ```pcre2 ` or any other near-miss is not
  a case at all. This is the typo's failure mode.
- **Only top-level fences are scanned.** A `regex` fence nested inside a
  longer fence is never seen — which is exactly why the example above,
  wrapped in four backticks, is safe to write here. A spec quoting
  another spec's pattern invariant inherits the same silence.

The rules below are *reported* failures. They are restated from the
extraction contract because a candidate or refusal tells you something
is wrong without telling you what to write instead:

- **Tag the engine.** `regex pcre2` is `QRegularExpression`, Qt's PCRE2.
  Given a correct `regex` first word, a bare ` ```regex ` fence is
  reported as a candidate and any other engine is refused — never
  silently run under a substitute, because a pattern written for one
  flavour and run under another can return a confident wrong answer
  about a spec that is correct.
- **`expected` has three outcomes, not two**, because a group that did
  not participate and a group that captured nothing are different
  states: bare `no match`, bare `no capture`, or the expected text in
  backticks (an empty backtick pair for a zero-length capture). A bare
  empty cell is malformed. The sentinels are read *before* backticks are
  stripped, so `` `no match` `` is the literal string.
- `expected` is capture group 1 when the pattern has any capturing
  group, else the whole match, under search semantics — not anchored
  unless you anchor it.

**Run `spec_conformance` on your own draft once written** — the same
habit § 6 asks for with `spec_query`, and the only way to catch the
invisible mistakes above, which by definition no reader can see. A
pattern invariant that comes back as a candidate, a refusal, or nothing
at all is not yet in the runnable form.

This is a recommendation, not a requirement: an invariant whose pattern
has no meaningful example, or whose real subject is the surrounding
code rather than the pattern, stays prose. `docs/specs/ANTS-4108-spec-conformance-verb.md`
§ 2.4 is the full extraction contract and the authority for anything the
bullets above do not cover.

### 3.6 Tests

A `## N. Tests` section naming the feature-test directory
(`tests/features/<name>/`), which invariants each test covers, the
test label (`features;fast`), and — per the project test convention —
the requirement to **verify the test fails against pre-fix code**
before the fix is restored. For designs with a live-API or manual
component, give the manual recipe as a subsection.

A `*Test:*` clause naming a feature test writes the
`tests/features/<name>` path, because that is the form `spec_lint`
resolves against disk (ANTS-4127). Prose remains a legitimate surface
and is left alone; a wildcard (`tests/features/audit_*`) is understood
as a family and is not resolved. No existing clause is invalidated.

## 4. Recommended sections

Add these when they carry weight; omit when they would be empty — with
two exceptions, marked below: **§ RAM / build cost** whenever its
trigger applies, and **§ Cold-eyes loop log** on every spec without
exception.

- **§ RAM / build cost** — **required** for any feature that holds state
  or adds a build target. State the memory budget and eviction policy at
  design time (the project's standing rule: no unbounded growth ships
  without a named cap). Note new external libraries (prefer none).
- **§ Out of scope** — the spec's edges, stated so they read as decisions
  rather than omissions. Two kinds of line live here and they carry
  different obligations:
  - **Deferred work** — something that will be done, later. Names the
    follow-up ROADMAP id that will carry it. A deferred line with no id
    is a promise nobody is holding.
  - **A permanent exclusion** — a boundary this spec will not cross at
    all ("prose quality", "cross-repo comparison", "semantic
    similarity"). These have no follow-up id *by definition*, and
    inventing one to satisfy a format rule files work nobody intends to
    do. Give the reason instead.

  Most lines in this corpus are the second kind. Where a line's status is
  genuinely unclear — deferred, or excluded? — say which, because that
  ambiguity is the thing the section exists to remove.
- **§ Migration / compatibility** — for changes to an existing
  on-disk format, schema, or public contract: how old data / callers
  are handled.
- **§ Cross-doc impact** — which docs change in the same release
  (CLAUDE.md module map, CHANGELOG, README, PLUGINS.md, sibling specs).
- **§ Open questions** — unresolved design forks, so a reviewer knows
  where judgment is still needed.
- **§ Cold-eyes loop log** — **required on every spec.** The gate always
  runs (§ 5.7), so this section is never empty, and its heading is what
  `check-doc-facts` looks for. See § 5.7.

## 5. Conventions

### 5.1 Grounding

Every file path, symbol name, constant, and version-specific behaviour
in the spec is backed by a grep/Read against current source — not
recall. A spec built on an unverified assumption is the most expensive
class of mistake: it is discovered wrong on the next pass and forces a
rewrite. Cite what you verified by symbol rather than by line
([`documentation.md` § 1.7](documentation.md)) — a line number verified
today is stale two commits later.

### 5.2 Layman line

Where a spec describes user-facing behaviour, include a one-sentence
plain-English **Layman:** gloss (same convention as `roadmap-format.md`
§ 3.5) so a non-technical reader can follow the *what*.

### 5.3 Idiom + brevity

Code in specs follows `coding.md`: current Qt6/C++20 idioms, shortest
correct form, no scaffolding for hypothetical futures. A spec proposing
250 lines where 50 suffice is over-designed before a line is written.

The same gate applies to the spec itself, per
[`documentation.md` § 1.6](documentation.md). Show request/response
shapes, structs and limits as schemas, tables and fenced blocks — never
as paragraphs narrating them — and state each limit once
([`documentation.md` § 1.5](documentation.md)).

**Length is not itself the defect, and the sibling-size yardstick is
withdrawn** (2026-08-12). This section used to say a spec several times
longer than a comparable sibling, or than the code it specifies, was
over-built until it named the extra surface. Global `documentation.md`
§ 2.8 is the owner and says the opposite: a longer document is worth a
look, not automatically wrong, and "a length target invites deleting
whatever is easiest to delete." Apply § 2.8's test instead — look for a
duplicated fact, prose narrating a table, or a decision never made. If
none is present, the length is the subject's.

What survives is the *reason* the old rule was reaching for: every
restatement of a fact is one more place for the next review loop to find
a contradiction, so a spec that repeats itself delays its own
convergence. That is the duplicated-fact case of § 2.8, and it is caught
by deleting the duplicate — never by cutting to a size.

### 5.4 Security boundaries

If the work crosses a trust boundary (network, filesystem, user input,
LLM output, IPC), the spec states the boundary and the defence as an
invariant. Re-state existing project defences the design must preserve
(path validation, secret redaction, scheme allowlists, fence-hardening)
rather than silently relying on them.

### 5.5 Invariant immutability

Invariant ids are stable handles. Once a spec is referenced from
CHANGELOG / CLAUDE.md / another spec, its `INV-N` ids never change
meaning. Amend by adding a new invariant or annotating the old one
(`INV-7 amended by ANTS-NNNN`), never by reflowing the list.

**Retire one with a tombstone.** `spec-format.md` § 3.7 owns the
`*withdrawn — …*` form and its load-bearing dash; `*moved to ANTS-NNNN*`
is the other form `spec_lint` accepts, as the body's opening text.
Either way the id stays and still counts in the sequence, so retiring
one opens no gap, and a tombstoned invariant owes no test surface.

**A spec carrying a subset of a parent's ids keeps them**, holes
included. Where the holes sit below your lowest id, declare the floor
with `<!-- invariant-id-base: N -->`; interior holes are reported as
candidates for a reader to triage.

### 5.6 Status lifecycle

`spec draft` → `accepted` (design signed off, ready to implement) →
`shipped X.Y.Z`. A shipped spec is a historical record; later behaviour
changes get their own spec that supersedes it (`**Superseded by:**`).

### 5.7 Cold-eyes loop log

Specs in this project run through the `review-contract` skill before
implementation, looped until it reports convergence or its loop cap
binds — at the cap the remaining findings are filed and the spec ships.
Never ship a first-draft spec. The skill owns the procedure, the cap and
the definition of convergence; this standard does not restate them.

Record each loop's findings + resolutions in a `## Cold-eyes loop log`
section and reflect progress in the **Status** line. **The section name
is frozen deliberately**: it is the heading `check-doc-facts` looks for
and the one every gated document in this corpus already carries, so
renaming it here would strand all of them. The log is the evidence the
loop happened and the audit trail for why a contract reads the way it
does.

**One table shape.** The log is a GFM table whose first header cell is
exactly `Loop` and whose last column is the prose outcome:

```
| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
```

One count per question, each in its own column, so the counts can be
read without parsing prose. `review-contract`'s genre overlays decide
which questions are asked; where a genre does not ask one, its cell is
`n/a` rather than `0` — an unasked question and a question that found
nothing are different facts. Keep the outcome last: that is the cell
`spec_lint`'s `loop_row_no_outcome` reads, and it is what lets the check
work across the older shapes too.

**Findings go in the outcome cell and nowhere else.** Restating them
under per-loop headings puts each count in two places, and two places
drift.

**The rules above bind the rows, not their location.** Rows normally sit
in the section. Where it names a record instead — `documentation.md`
§ 9.1's form, which this standard itself uses — the section keeps its
heading and a one-line pointer, and the rows go to `docs/reviews/`.

**This binds new logs, and the rules above bind table logs only.** A
landed row is never edited, so a log already running keeps the shape it
has — a heading-form log stays heading-form, and a table keeps the
columns its header already has.

## 6. Machine-readability (`spec_query`)

The `spec_query` MCP tool parses specs into
`{id, title, status, kind, invariants:[{id, body, test_surface?}]}`.
To stay parseable, keep:

- the H1 as `# ANTS-NNNN — title`,
- the `**Status:**` and `**Kind:**` header lines as the first bold
  key-values,
- invariants in the bullet form `- **INV-N** — body` (or the GFM
  table form). A `*Test:*` / trailing `— test surface` clause is
  surfaced as `test_surface`.

Authors should `spec_query` their own draft once written to confirm it
parses (title, status, kind, and all invariants come back).

## 7. Skeleton

Copy this to start a new spec:

```markdown
# ANTS-NNNN — <short title>

**Status:** spec draft (YYYY-MM-DD).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-NNNN (<provenance>).

## 1. Problem

<what is broken/missing, grounded in `path::symbol()`; why now>

## 2. Surface

### 2.1 <concern>

<structs / signatures / layout in fenced blocks; exact files + libs>

## 3. Invariants

- **INV-1** — <testable claim>. *Test:* <feature test / grep / recipe>.
- **INV-2** — <…>. *Test:* <…>.

## 4. RAM / build cost

<memory budget + eviction policy; new build targets; external libs>

## 5. Out of scope

- <deferred item> — tracked by ANTS-MMMM.
- <permanent exclusion> — not done at all, because <reason>. No id;
  most lines here are this kind (§ 4).

## 6. Tests

Feature test: `tests/features/<name>/`. Covers INV-1..N. Label
`features;fast`. Verify each test fails against pre-fix source first.

## 7. Cross-doc impact

<CLAUDE.md / CHANGELOG / README / sibling specs touched this release>

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|

<one row per review loop; written as the loops happen, never back-filled>
```

## What checks this

| Rule | What catches a breach |
|---|---|
| § 2 the `<ID>-<topic>.md` filename shape | **nothing** — `spec_query` resolves the bare `<ID>.md` too, so a missing slug is invisible to it; the corpus sweep is ANTS-3755 |
| § 3 the required sections are present | **`Partial:`** `spec_lint` `missing_section`, a required-section presence check reading § 3's own block. It tests presence, never ORDER, though § 3 says "in this order" — **nothing** catches a spec whose sections are all present and out of sequence |
| § 3.2 the header block stays parseable | **`Partial:`** `spec_query`, a parse check that fails loudly on a broken header — but nothing runs it. § 6 asks the author to |
| § 3.5 every invariant carries a test surface | `spec_lint` `invariant_no_test`, a per-invariant clause-presence check. A tombstone is exempt, so a MALFORMED one — the dash in `*withdrawn — …*` is load-bearing — surfaces here as an untested invariant rather than as itself |
| § 3.5 a command test clause states a result | `spec_lint` `command_test_no_expectation`, a candidate check over the clause's code spans |
| § 3.5 / § 5.5 ids are never renumbered | **`Partial:`** `spec_lint` `invariant_id_gap`, a candidate id-sequence check. It sees a HOLE; a list renumbered contiguously leaves none, so **nothing** catches the renumbering this rule actually forbids |
| § 3.5.1 a pattern invariant is runnable | **`Partial:`** `spec_conformance`, a pattern-execution check reporting a pattern that disagrees with its own example. The three mistakes § 3.5.1 calls invisible are reported by **nothing**, and nothing makes an author run the verb |
| § 3.6 a named test surface resolves | `spec_lint` `test_surface_absent` / `test_surface_unresolved` / `test_surface_unwired`, disk- and CMake-resolution checks, gated on a `tests/features/<name>/` directory being named. Prose surfaces and wildcards are left alone by design |
| § 5.1 every path, symbol and constant is grounded | **`Partial:`** `check-doc-facts` `paths` and `symbols`, resolution checks emitting candidates. Whether a symbol that RESOLVES supports the claim made about it is **nothing mechanical** — the review gate's cold lanes |
| § 5.2 the Layman line | **nothing** — no check knows whether a given spec describes user-facing behaviour, which is what triggers the rule |
| § 5.3 brevity, § 5.4 security boundaries | **nothing mechanical** — the review gate's cold lanes, and the reader of the resulting document |
| § 5.6 the status lifecycle | **nothing** — no check reads `**Status:**` against the lifecycle. `spec_lint` reads the value only to bucket § 3.6's surface findings |
| § 5.7 the loop log exists | `spec_lint` `missing_section`, via § 3's block, which lists it |
| § 5.7 every loop row records an outcome | `spec_lint` `loop_row_no_outcome`, an emptiness check on the row's LAST cell, read by position |
| § 5.7 the table's columns | **nothing** — no check reads the header cells. `loop_row_no_outcome` matches the first cell `Loop` and then counts from the end, which is what lets it work across the older shapes |
| § 5.7 a landed row is never edited, and the log is not back-filled | **nothing** — both show only in the commit diff, and no check reads it |
| § 5.7 the gate ran at all | **nothing** — the log is the only evidence, and the session that ran the gate writes it |

## Cold-eyes loop log

Moved to [`docs/reviews/specs-review-log.md`](../reviews/specs-review-log.md).
A standard carries rules; its review history is read far less often and
was the larger half of this file.
