# Review loop log — `docs/standards/specs.md`

Moved out of the standard so the rules are not paid for alongside their
review history. A standard is read on every consultation; this is read
when someone asks what a past loop already settled.

**Rows are never edited once landed.** They record what each pass found,
including where a later pass corrected it — a correction goes in the
current row, not by rewriting an old one. That is the whole reason the
log is kept: so a trap already paid for is not re-engaged.

## Cold-eyes loop log
```

**Expect a real backlog over `docs/specs/`, and it is not evidence the
list is wrong.** Most of the corpus predates this standard (v1,
2026-05-21); § 2 already records that. `spec_lint` measures the current
figure — this document does not restate it, because a count here reads as
present tense and rots.


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
`spec_conformance` verb *runs* the pattern against each row — within
its per-run case cap, and skipping any pattern or input over 512 bytes
as a `too_large` refusal — and a pattern that disagrees with its own
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
habit § 6 asks for with `spec_query`, and the only way to catch the two
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
- <permanent exclusion> — not done at all, because <reason>. No id;
  most lines here are this kind (§ 4).

## 6. Tests

Feature test: `tests/features/<name>/`. Covers INV-1..N. Label
`features;fast`. Verify each test fails against pre-fix source first.

## 7. Cross-doc impact

<CLAUDE.md / CHANGELOG / README / sibling specs touched this release>

## Cold-eyes loop log

<one row per review loop; written as the loops happen, never back-filled>
```

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
| 3 | 2026-08-12 | 2, cold; packet rebuilt from disk after loop 2's edits and the § 0 header paragraph | **Q1 1 · Q2 4** (5 verified / 0 unverified) | **Cap reached; all five fixed, no deferred tail.** Both lanes independently found the same two, which is what two rolls are for. **[Q1] There is a THIRD silent failure mode**, and loop 2's own repair denied it: `run()` resumes past a fence's close, so a `regex` fence nested inside a longer fence is never seen — which is precisely the mechanism § 3.5.1 relies on four lines earlier to keep its own example from becoming a case. An author quoting another spec's pattern invariant would get total silence and work a two-item checklist that cannot explain it. Now three, with the connection to the example stated. **[Q2] Loop 2 made `§ Cold-eyes loop log` required on every spec but never touched § 7's skeleton**, which ends at `## 7. Cross-doc impact` — so an author doing what § 7 says ("Copy this to start a new spec") shipped a document failing the `check-doc-facts` heading test they had just been told was mandatory. Skeleton amended; § 4's preamble no longer calls it trigger-conditional, since it has no trigger. **[Q2] Loop 2's block claimed "nothing else from the extraction contract is" restated while restating four further § 2.4 rules below it**, and closed with "do not restate it here" — a future editor syncing against ANTS-4108 would have deleted the only statement of the three-outcome encoding an author ever sees. Both sentences scoped. **[Q2] The § 0 header added this loop said "this file wins" while § 1 defers the is-a-spec-needed decision to `spec-format.md` § 1** — the two disagree on whether a borderline item gets a document at all. Precedence now carved to *shape*, with § 1 named as the exception. **[Q2] The skeleton's `Out of scope` showed only the deferred+id form** although § 4 says permanent exclusions are the common kind and inventing an id for one is forbidden; both forms now appear. **Resolved, not findings:** the 512-byte cap is `>`, so "over 512 bytes" is exact; and the § 0 claims about `speclint.cpp` / `specparse.h` / `speclog.h` and the three feature tests were verified by the orchestrator. **Why the cap bound, since that is evidence about this document:** three of these five landed on text *this gate* wrote in loops 1-2, which is the fix-pass-generates-defects pattern rather than a contract that was never settled. The pre-existing defect rate fell 4 → 2 → 2 across the three loops. |
| 2 | 2026-08-12 | 2, cold; identical packet rebuilt from disk after loop 1's edits, with the change-hint paragraph neutralised so no section was flagged | **Q1 2 · Q2 2 · Q3 1** (5 verified / 0 unverified) | **One finding was against loop 1's own repair.** Loop 1 added "it is the only one that fails *silently*" about an indented fence; that is false. `run()` skips any fence whose info string's FIRST word is not exactly `regex`, so ` ```regexp ` — the typo's failure mode — is equally invisible: no finding, no candidate, no refusal. The bullet now names both, and the engine bullet no longer implies a mis-tag is always reported. **A second was against the § 3.5.1 amendment itself:** it promised the verb "runs the pattern against every row" with no mention of `kMaxPatternBytes` / `kMaxInputBytes` (512, a `too_large` refusal rather than a run) or `Options::maxCases` (200, ceiling 1000, sets `truncated`) — the same false confidence the section exists to prevent. Qualified. **A third: the section stated a rule with no check** — it never told an author to run the verb, though § 6 does exactly that for `spec_query`, and the two invisible failures above are by definition undetectable by reading. It now does. **Two were pre-existing**, both in § 4: its opener makes the whole list optional-on-judgement while its `§ RAM / build cost` entry is required on a stated trigger, and `§ Cold-eyes loop log` sat in a "Recommended" list although the gate always runs and `check-doc-facts` looks for the heading — so an author could omit a section and fail a mechanical check they were told was optional. Both marked required, with § 3 amended not to read as the exhaustive required set. **Resolved, not a finding:** a lane asked whether table cells are trimmed after backtick-stripping, which would make this standard's own `no match` row a mismatch. `test_spec_conformance.cpp:59` already asserts `` | `  const local = ` | no match | `` against a `^`-anchored pattern and is green, so leading spaces inside backticks survive. |
| 1 | 2026-08-12 | 2, cold; genre pinned `standard`; one byte-stable shared packet carrying the `spec_conformance` engine windows and the `documentation.md` / `roadmap-format.md` / ANTS-4108 excerpts | **Q1 1 · Q2 3 · Q3 1** (5 verified / 0 unverified) | **This standard's first gate**, triggered by the § 3.5.1 amendment ANTS-4108 § 9 had left undone. **One finding was against that new text:** § 3.5.1 never said the fence must start at **column 0**, and that is the one part of the extraction contract whose breach is *silent* — `fenceDelimLen()` counts backticks from index 0, so an indented fence returns 0 and is skipped with no finding, no candidate and no refusal. A conformer writing the natural GFM rendering — fence and table indented under their `- **INV-N**` bullet — gets a spec that renders correctly and is never run. ANTS-4108 § 2.4 is silent on it too and no test covers it (filed: ANTS-4130). **Four were pre-existing.** § 1 still carried "When unsure, write the spec", which `~/.claude/standards/spec-format.md` § 1 deleted on 2026-08-08 for biasing every borderline call toward a document *plus its full review gate*; the two standards biased opposite ways at exactly the point of doubt, and this one now carries the two-files-in escape hatch instead. § 5.7 told authors to run a "cold-eyes review loop … until a clean pass", naming a skill that no longer exists (replaced by `review-contract`) and a bar stricter than the one the skill defines — `spec-format.md` § 6 gives the skill sole ownership of convergence. The `## Cold-eyes loop log` heading is kept and the freeze is now stated: it is what `check-doc-facts` looks for and what every gated document in the corpus already carries. And the **Applies to:** line named only the bare `ANTS-NNNN.md` shape while § 2 *requires* `<ID>-<topic>.md` on new specs, so a conformer writing the mandated shape could read the standard as not governing their file. **Dismissed:** the remaining `cold-eyes` mentions (§§ 3.2, 4) name the frozen section and the live in-Ants engine (`src/coldeyesengine.h`, the `cold_eyes_*` verbs), not a command. **Surfaced, not fixed:** whether this file should become a deltas-only `spec-format-overrides.md` rather than a full parallel standard — a scope decision above this review. |
