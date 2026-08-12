<!-- ants-specs-standards: 1 -->
# Spec-authoring standard

**Status:** v1 (2026-05-21).
**Applies to:** every spec under `docs/specs/` — the
`<ID>-<topic>.md` shape § 2 requires, and the bare `<ID>.md` of
legacy files.

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

Write a spec when the work has a **non-obvious contract** — invariants
a future change could silently break, a data shape other code depends
on, a security boundary, or a multi-file design.

Skip the formal spec (a regression test is more useful) when the work
is mechanical: a typo, a one-line fix, a menu entry, a dependency bump.
A `spec.md` under `tests/features/<name>/` (the feature-conformance
contract) is the right home for a small, single-invariant behaviour;
a top-level spec under `docs/specs/` is for designs big enough that
the contract spans files.

**If you are two files in and it is still spreading, stop and write the
spec.** That is the correction for a call that went the wrong way, and
it is cheaper than finishing it wrong. There is deliberately no "when
unsure, write the spec" rule — that instruction biased every borderline
call toward a document plus its full review gate. `spec-format.md` § 1
owns the five triggers and the skip test; read it rather than deciding
from this paragraph.

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

## 3. Required structure

Every spec has, in this order:

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
- Number them `INV-1`, `INV-2`, … and **never renumber** once the spec
  is referenced elsewhere — invariants are cited by id from CHANGELOG,
  CLAUDE.md, and other specs. Add `INV-14`, don't reflow.

#### 3.5.1 Pattern invariants — state them so they can be run

When an invariant's claim is about a **regex pattern**, state the
pattern as a fence tagged with its engine and put an `| input |
expected |` table directly beneath it. In that form the
`spec_conformance` verb *runs* the pattern against every row, and a
pattern that disagrees with its own example comes back as a finding
instead of surviving a cold read. The example below is nested inside a
four-backtick fence so this standard does not itself become a case:

````markdown
```regex pcre2
^- \*\*\[(ANTS-\d+)\]\*\*
```

| input | expected |
|---|---|
| `- **[ANTS-4108]** ships the verb` | `ANTS-4108` |
| `  - **[ANTS-1]** indented` | no match |
````

- **Start the fence at column 0, with only blank lines between it and
  the table.** The fence and its table are siblings of the `INV-N`
  bullet, never indented children of it. This is the one part of the
  extraction contract restated here, because it is the only one that
  fails *silently*: an indented fence is not a fence, so the case is
  never run and is reported as nothing at all — no finding, no
  candidate, no refusal. Prose between fence and table is reported, as
  a candidate.
- **Tag the engine.** `regex pcre2` is `QRegularExpression`, Qt's PCRE2.
  A bare ` ```regex ` fence is reported as a candidate and any other
  engine is refused — never silently run under a substitute, because a
  pattern written for one flavour and run under another can return a
  confident wrong answer about a spec that is correct.
- **`expected` has three outcomes, not two**, because a group that did
  not participate and a group that captured nothing are different
  states: bare `no match`, bare `no capture`, or the expected text in
  backticks (an empty backtick pair for a zero-length capture). A bare
  empty cell is malformed. The sentinels are read *before* backticks are
  stripped, so `` `no match` `` is the literal string.
- `expected` is capture group 1 when the pattern has any capturing
  group, else the whole match, under search semantics — not anchored
  unless you anchor it.

This is a recommendation, not a requirement: an invariant whose pattern
has no meaningful example, or whose real subject is the surrounding
code rather than the pattern, stays prose. The full extraction contract
is `docs/specs/ANTS-4108-spec-conformance-verb.md` § 2.4; do not restate
it here.

### 3.6 Tests

A `## N. Tests` section naming the feature-test directory
(`tests/features/<name>/`), which invariants each test covers, the
test label (`features;fast`), and — per the project test convention —
the requirement to **verify the test fails against pre-fix code**
before the fix is restored. For designs with a live-API or manual
component, give the manual recipe as a subsection.

## 4. Recommended sections

Add these when they carry weight; omit when they would be empty:

- **§ RAM / build cost** — required for any feature that holds state or
  adds a build target. State the memory budget and eviction policy at
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
- **§ Cold-eyes loop log** — see § 5.7.

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
([`documentation.md` § 1.5](documentation.md)). Two
yardsticks: a spec several times longer than a sibling spec covering
comparable surface, or several times longer than the code it specifies,
is over-built until it names the extra surface it covers. Length is not
just a reading cost — every restatement of a fact is one more place for
the next review loop to find a contradiction, so an over-long spec
actively delays its own convergence.

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

### 5.6 Status lifecycle

`spec draft` → `accepted` (design signed off, ready to implement) →
`shipped X.Y.Z`. A shipped spec is a historical record; later behaviour
changes get their own spec that supersedes it (`**Superseded by:**`).

### 5.7 Cold-eyes loop log

Specs in this project run through the `review-contract` skill before
implementation, looped until it reports convergence — never ship a
first-draft spec. The skill owns the procedure and the definition of
convergence; this standard does not restate them.

Record each loop's findings + resolutions in a `## Cold-eyes loop log`
section and reflect progress in the **Status** line. **The section name
is frozen deliberately**: it is the heading `check-doc-facts` looks for
and the one every gated document in this corpus already carries, so
renaming it here would strand all of them. The log is the evidence the
loop happened and the audit trail for why a contract reads the way it
does.

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

## 6. Tests

Feature test: `tests/features/<name>/`. Covers INV-1..N. Label
`features;fast`. Verify each test fails against pre-fix source first.

## 7. Cross-doc impact

<CLAUDE.md / CHANGELOG / README / sibling specs touched this release>
```

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 2, cold; genre pinned `standard`; one byte-stable shared packet carrying the `spec_conformance` engine windows and the `documentation.md` / `roadmap-format.md` / ANTS-4108 excerpts | **Q1 1 · Q2 3 · Q3 1** (5 verified / 0 unverified) | **This standard's first gate**, triggered by the § 3.5.1 amendment ANTS-4108 § 9 had left undone. **One finding was against that new text:** § 3.5.1 never said the fence must start at **column 0**, and that is the one part of the extraction contract whose breach is *silent* — `fenceDelimLen()` counts backticks from index 0, so an indented fence returns 0 and is skipped with no finding, no candidate and no refusal. A conformer writing the natural GFM rendering — fence and table indented under their `- **INV-N**` bullet — gets a spec that renders correctly and is never run. ANTS-4108 § 2.4 is silent on it too and no test covers it (filed: ANTS-4130). **Four were pre-existing.** § 1 still carried "When unsure, write the spec", which `~/.claude/standards/spec-format.md` § 1 deleted on 2026-08-08 for biasing every borderline call toward a document *plus its full review gate*; the two standards biased opposite ways at exactly the point of doubt, and this one now carries the two-files-in escape hatch instead. § 5.7 told authors to run a "cold-eyes review loop … until a clean pass", naming a skill that no longer exists (replaced by `review-contract`) and a bar stricter than the one the skill defines — `spec-format.md` § 6 gives the skill sole ownership of convergence. The `## Cold-eyes loop log` heading is kept and the freeze is now stated: it is what `check-doc-facts` looks for and what every gated document in the corpus already carries. And the **Applies to:** line named only the bare `ANTS-NNNN.md` shape while § 2 *requires* `<ID>-<topic>.md` on new specs, so a conformer writing the mandated shape could read the standard as not governing their file. **Dismissed:** the remaining `cold-eyes` mentions (§§ 3.2, 4) name the frozen section and the live in-Ants engine (`src/coldeyesengine.h`, the `cold_eyes_*` verbs), not a command. **Surfaced, not fixed:** whether this file should become a deltas-only `spec-format-overrides.md` rather than a full parallel standard — a scope decision above this review. |
