# ANTS-4127 — resolve the test surface a spec cites, instead of reading it

**Status:** spec draft (2026-08-12).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-4127 (split out of ANTS-4108 at spec time;
scope re-decided 2026-08-12 by the user — see § 2.2).
**Pairs with:** ANTS-3662 (`spec_lint`) — this extends that engine.
**Composes with:** ANTS-4108 (`spec_conformance`) — this inherits its
FINDING / CANDIDATE / invisible taxonomy, not its code. § 2.1 says why the
code lands elsewhere.

**Format note.** Structure follows the project standard
(`docs/standards/specs.md`), as ANTS-4108 does and for the same reason: the
corpus and `spec_query`'s parse contract are built on it. Global
`~/.claude/standards/spec-format.md` § 0 was applied as the authoring
checklist on top.

## 1. Problem

A spec's `*Test:*` clause is the claim that an invariant is locked by
something real. **Nothing checks that the thing it names exists.**

Three tools read these clauses and each stops short:

1. **`spec_lint`** (`src/speclint.cpp`) raises `invariant_no_test` when a
   clause is *absent* and `command_test_no_expectation` when a command
   clause states no expected output. Neither looks at what a present clause
   names.
2. **`doc_citations`** resolves `path:line` references. A clause naming
   `tests/features/audit_since_baseline` carries no line number and is not
   a citation in that sense, so it is never harvested.
3. **`spec_conformance`** (ANTS-4108) runs *pattern* artefacts. Its § 2.2
   states plainly that fixture defects produce nothing at all there, "not
   even a CANDIDATE".

So a spec can name a test directory that has never existed, and every gate
passes it. **It has.** Measured over the corpus on 2026-08-12:

```
# distinct tests/features/<name> named anywhere in docs/specs/
grep -ohE 'tests/features/[a-z0-9_]+' docs/specs/*.md | sort -u | wc -l     → 266
# of those, absent from disk (loop over the same list, test -d)             → 26
```

Twenty-three of the twenty-six are real; the other three (`audit_`,
`remote_control_`, `roadmap_`) are truncation artefacts of the measuring
regex above, which stops at the `_` of a `tests/features/audit_*` wildcard.
That artefact is not incidental — § 2.4 makes avoiding it an invariant,
because the shipped extractor must not reproduce a defect its own spec's
measurement had.

Two of the twenty-three sit in specs whose own **Status** says the work
shipped:

| Spec | Status line says | Cites | On disk |
|---|---|---|---|
| ANTS-1111 | `v1 shipped 2026-05-13 in 0.7.88` | `tests/features/audit_since_baseline`, `…/audit_allow_widening` | no |
| ANTS-3504 | `Implemented (2026-07-12) — cold-eyes clean` | `tests/features/feedback_ship_date` | no |

Neither is a rename: the names appear nowhere under `tests/`
(`grep -rl <name> tests/` → empty for all three). ANTS-3504's Status
records a clean cold-eyes pass. **A judgement review read the clause and
passed it, because reading a clause is not resolving it** — the same
asymmetry ANTS-4108 § 1 documents for patterns, in a second class.

**Layman:** specs say "this rule is checked by that test over there".
Sometimes there is no test over there. Nobody notices, because checking
means going and looking, and reviewers read instead.

## 2. Surface

### 2.1 A finding kind in `spec_lint`, not a mode of `spec_conformance`

The ROADMAP bullet placed this in `spec_conformance`, on the premise that
it would *execute* fixtures. § 2.2 drops that premise, and the home moves
with it:

- `spec_lint` **already parses the exact clause** — the
  `invariant_no_test` / `command_test_no_expectation` branch walks the ids
  gathered from **both** invariant forms, the `- **INV-N**` bullet and the
  `| INV-N | … |` table row (`bulletAnchorRe` and `rowAnchorRe`, merged into
  one `allIds` set). Resolving what the clause names is one more check in a
  walk that already happens, which is the smallest correct change — and it
  inherits both forms for free, which a new walk would have to re-earn.
- `spec_conformance`'s identity is *running artefacts* (ANTS-4108 § 2.1).
  Resolving a citation runs nothing, and filing it there would make its
  name false.
- Both are read-only and start no subprocess, so the posture is unchanged
  either way. Only the walk is shared, and `spec_lint` is where it lives.

What is genuinely inherited from ANTS-4108 is its **taxonomy** — FINDING,
CANDIDATE, and silently invisible — which § 2.5 extends rather than
restates.

### 2.2 Scope decision — citations are resolved, fixtures are not executed

**This check resolves what a clause names. It executes nothing.** The
ROADMAP bullet proposed running a spec's fenced fixtures under a sandbox,
and named two questions that had to be settled first: which interpreter,
and what sandbox. **Rejected 2026-08-12 (user decision)** — both questions
are answered by dropping the premise that raised them. The measurement
behind the call is § 6.

The consequence, stated plainly rather than buried: **a fixture that runs
but tests nothing is still invisible.** This check proves a cited test
*exists and is wired to run*; it never proves the test can fail. That
remains the honest gap, and § 9 records it as `nothing`.

### 2.3 Inputs and envelope

No new verb and no new arguments. `spec_lint`'s existing `path` /
`max_findings` / `etag_match` surface is unchanged; two `kind` values join
its `findings[]`, and one counter joins its envelope:

```jsonc
{ "kind": "test_surface_absent",       // FINDING   — § 2.5
  "line": 412,                         // the INV-N bullet's line, per ANTS-3662
  "invariant": "INV-5",
  "surface": "tests/features/audit_since_baseline",
  "spec_status": "shipped" }           // § 2.5's lowercased FIRST WORD, not the bucket

{ "kind": "test_surface_unresolved",   // CANDIDATE — § 2.5
  "line": 88,
  "invariant": "INV-2",
  "surface": "tests/features/doc_lint",
  "spec_status": null }                // Status line absent or empty — an explicit
                                       // null, never an omitted key
```

`spec_status` is the lowercased first word of the `**Status:**` value, and
is **JSON `null`** — not `""`, not absent — when the line is missing or
empty. 53 specs are in that state (§ 2.5), so a consumer grouping by this
field would silently drop a fifth of the corpus if the key could vanish.

**`surfaces_resolved` counts distinct SURFACES, not clauses**, so a clause
naming two present directories contributes two. The field is the
denominator without which two zero-finding runs are indistinguishable — but
zero is also what a *skipped* run reports, so the count never carries that
distinction alone:

`surfaces_checked` is a boolean, false exactly when § 2.6's injected sets
were empty and the checks skipped. It is always emitted and never inferred
from a zero count — the same contract, for the same reason, as
`Result::sectionsChecked`, whose header comment says it "is always reported
and never inferred from an empty findings list".

### 2.4 Extraction contract

A clause's surface is harvested only in the one form the corpus already
uses at scale: a `tests/features/<name>` path.

```regex pcre2
tests/features/([a-z0-9]+(?:_[a-z0-9]+)*)(?!\w|\*)
```

| input | expected |
|---|---|
| `tests/features/spec_conformance/` | `spec_conformance` |
| `tests/features/audit_run_cache, and` | `audit_run_cache` |
| `tests/features/audit_*` | no match |
| `tests/features/remote_control_*` | no match |

The trailing `(?!\w|\*)` is the whole of the wildcard rule, and it is there
because the § 1 measurement got this wrong: a naive `[a-z0-9_]+` reads
`tests/features/audit_*` as a directory named `audit_` and reports it
absent. A spec legitimately writes a wildcard when it means a family of
tests, so that reading manufactures a finding against a correct spec — the
outcome ANTS-4108 § 2.6 calls worse than not checking at all.

**A clause naming no such path is invisible** — no finding, no candidate,
no refusal. **That is 986 of 1026 clauses today**: invisibility is measured
against the extraction pattern itself, not against a proxy, because every
other proxy undercounts it.

```
# every count below EXCLUDES this document, which is itself in the corpus
# it measures — see § 2.4b
grep -ohE '\*Test:\*.*' docs/specs/*.md --exclude='ANTS-4127-*' | wc -l    → 1026
grep -ohE '\*Test:\*.*' docs/specs/*.md --exclude='ANTS-4127-*' \
  | grep -cP 'tests/features/([a-z0-9]+(?:_[a-z0-9]+)*)(?!\w|\*)'          →   40
# invisible = 1026 − 40                                                    →  986
```

**40, not 260.** An earlier draft of this section measured the invisible set
as "clauses naming neither a `tests/` path nor a backticked identifier"
(766) — a proxy that counts a clause as *visible* when it merely mentions a
backticked test name or a `tests/unit/…` path, neither of which § 2.4
harvests. The pattern is the only honest denominator, and it moves the
figure by 220 clauses.

The invisible 986 are prose surfaces — "a fixture asserting…",
"source-scrape over…", a manual recipe. Prose is a legitimate test surface
(`~/.claude/skills/write-spec/references/drafting-rules.md`
§ *Clauses with nothing to run*), so reporting on it would fire on three
quarters of every spec in the corpus and be right about none of them.
Silence here is a decision, not an omission.

### 2.4b This document is inside the corpus it measures

Every figure in § 1, § 2.4 and § 2.5 was measured before this file existed,
and this file then joined `docs/specs/`. It carries nine `*Test:*` clauses
of its own plus several prose mentions of the literal token, so the
unqualified commands now return **1039** where the spec says 1026 — the
number was right when taken and the corpus moved under it.

Hence the `--exclude='ANTS-4127-*'` on every count: the figures describe
**the corpus this checker would be pointed at, not counting the document
proposing it**, and the commands as written reproduce them. A reader who
drops the flag gets a different number and should.

The same reflexivity is why § 2.4's fence is a live case rather than an
illustration: `spec_conformance` executes it on every run over this file
(4 cases, 0 findings). ANTS-4108 § 2.4a hit the mirror image — its
illustration would have become three false cases had it not been nested
inside a four-backtick fence. A spec about checking specs is always inside
its own subject, and the choice is only whether that is stated.

### 2.5 Classification — the spec's own Status decides, and it decides conservatively

The same absent directory is a defect or a forward reference depending on
whether the spec claims to have shipped. So the bucket is chosen by the
spec's `**Status:**` line, and **only a confidently-shipped status yields a
FINDING**:

| Status first word | Bucket when the surface is absent |
|---|---|
| `shipped`, `implemented`, `v1` | **FINDING** `test_surface_absent` |
| `spec`, `draft`, `accepted`, `in`, `implementing`, `ready` | **CANDIDATE** `test_surface_unresolved` |
| `superseded`, `considered` | invisible — the work was abandoned on purpose |
| absent, empty, or anything else | **CANDIDATE** `test_surface_unresolved` |

Measured with

```
grep -h '^\*\*Status:\*\*' docs/specs/*.md \
  | sed -E 's/^\*\*Status:\*\* *//; s/^\*+//' \
  | awk '{print tolower($1)}' | sed 's/[^a-z0-9].*//' | sort | uniq -c
```

**twelve** distinct first words across the 187 of 240 specs that carry the
line, plus three whose Status value is present but empty — so **53 specs
carry no Status line at all** and three more carry an empty one. The table
above names eleven of the twelve; the twelfth (`amended`, 1 spec) falls
through to the catch-all row, which is the row doing the real work. An
unrecognised or absent word is therefore the common case, not the exotic
one, and it falls to CANDIDATE — never to FINDING, and never to a guess. The vocabulary is wider than
`specs.md` § 5.6's three-state lifecycle; this table describes the corpus
that exists, and § 10 does not propose narrowing it, because a status
rewrite across 187 files is not this spec's job.

### 2.6 Wiring — existing on disk is not the same as running

A directory that exists can still hold a test that never runs, which is the
project's own documented trap (`CLAUDE.md`, *Feature-conformance*: "Building
the wrong target succeeds silently and runs the old binary… a green run
reads as success"). So a resolved surface is checked twice:

1. the directory exists under `tests/features/`;
2. at least one `test_*.cpp` inside it is named in `CMakeLists.txt`.

Failing (2) with (1) satisfied is a FINDING regardless of Status — a test
source on disk and in no bundle compiles nowhere and runs never, and no
spec lifecycle makes that intended.

**Neither fact is gathered by the engine, and this is a hard constraint
rather than a preference.** `SpecLint::check()` takes the document's *text*
and a `relPath` "only carried onto findings"; `src/speclint.h` states the
engine is Qt6::Core-only, "never opens the document under review", and
"cannot find or read that file either". So both filesystem facts arrive the
way `Options::requiredSections` and `Options::siblingInvNumbers` already
do — gathered by the verb layer and **injected**:

```cpp
// joins SpecLint::Options, beside requiredSections / siblingInvNumbers
QSet<QString> existingTestDirs;   // tests/features/<name> present on disk
QSet<QString> wiredTestDirs;      // …and holding a test_*.cpp named in CMakeLists.txt
```

**Each set gates its own check, independently, and empty always means skip
rather than fail** — the contract `requiredSections` already uses, for the
same reason: a check against an empty set condemns everything it reads.

| Injected state | Effect |
|---|---|
| `existingTestDirs` empty | **both** checks skip (the verb layer supplied nothing) |
| `existingTestDirs` non-empty, `wiredTestDirs` empty | check (1) runs; **check (2) skips** |
| both non-empty | both run |

The middle row is the one that matters: an unreadable or moved
`CMakeLists.txt` yields an empty `wiredTestDirs`, and without this row
INV-6 — which is deliberately Status-proof — would file a FINDING against
**every** resolved surface in the corpus from a single failed file read.
A parse failure must never present as 266 defects.

**This second check has zero yield today, and that is stated rather than
discovered later.** The two sets are identical:

```
ls tests/features/*/test_*.cpp | sort -u                                  → 515
grep -oE 'tests/features/[a-z0-9_]+/test_[a-z0-9_]+\.cpp' CMakeLists.txt \
  | sort -u                                                               → 515
comm -3 <(the first) <(the second)                                        → empty
```

It ships anyway because it is nearly free once (1) has resolved the
directory, and because the trap it guards is one CLAUDE.md documents as
recurring. A check with no current findings is not the same as a check with
nothing to find — but the distinction is only honest if the number is on
the record, which is what ANTS-4108 § 2.4a learned the expensive way.

### 2.7 Where the code lives

| File | Role |
|---|---|
| `src/speclint.cpp` | the two new finding kinds, in the existing `*Test:*` walk — text only, no filesystem |
| `src/speclint.h` | `Options` gains `existingTestDirs` / `wiredTestDirs`; `Result` gains `surfacesResolved` |
| `src/remotecontrol_docs.cpp` | `RemoteControl::cmdSpecLint` gathers both sets and injects them, as the sibling helper `specLintRequiredSections` already does for the section list |
| `tests/features/spec_lint/` | engine lane — extraction, classification, wiring, injected-set handling |
| `tests/features/spec_lint_verb/` | verb lane — the gathering step and the new `kind` values reaching the envelope |

No new TU, no new library, no new dependency. `src/specconformance.{h,cpp}`
is **not touched**, which keeps ANTS-4108 INV-8's source-scrape true by
construction rather than by care.

### 2.8 Caps and the trust boundary

Unchanged from `spec_lint`'s: the specs are in your own repository and no
subprocess is started. The engine's own boundary is unchanged too — it
still opens nothing.

The one new cost is in the verb layer: one directory scan of
`tests/features/` plus one read of `CMakeLists.txt` per call, both done
once and shared across every spec in a directory walk rather than per
document. `max_findings` bounds the reported set exactly as it does for
every other kind.

## 3. Invariants

- **INV-1** — a test clause naming `tests/features/<name>` yields one
  resolution attempt per **distinct** name in that clause. *Test:*
  `tests/features/spec_lint/` — a fixture whose clause names two distinct
  directories asserts two attempts, and one naming the same directory twice
  asserts one. *Breaks when:* the harvest stops at the first match, which
  leaves the second surface of any multi-surface clause unchecked.
- **INV-2** — a `tests/features/<name>*` **wildcard** yields no resolution
  attempt and no finding of any kind. *Test:* § 2.4's fence-and-table, run
  by `spec_conformance` over this document, plus a `spec_lint` fixture
  whose clause names `tests/features/audit_*` asserting zero findings.
  *Breaks when:* the trailing `(?!\w|\*)` is dropped, which reports a
  directory named `audit_` absent and files a finding against a correct
  spec.
- **INV-3** — an absent surface in a spec whose Status first word is
  `shipped`, `implemented` or `v1` is a FINDING; in every other recognised
  draft state it is a CANDIDATE. *Test:* two fixtures differing **only** in
  their Status line, asserting `test_surface_absent` and
  `test_surface_unresolved` respectively. *Breaks when:* the two collapse
  into one bucket, which either files 23 findings against specs nobody has
  implemented yet or files none against the two that shipped.
- **INV-4** — a spec with **no** `**Status:**` line, or one whose first
  word is unrecognised, classifies as CANDIDATE and never as FINDING.
  *Test:* a fixture with no Status line asserts `test_surface_unresolved`;
  a second carrying `**Status:** banana (2026-01-01).` asserts the same.
  *Breaks when:* an unparsed status defaults to shipped — which would fire
  against the 53 Status-less specs measured in § 2.5.
- **INV-5** — a spec whose Status first word is `superseded` or
  `considered` is skipped **before either check runs**, so it yields
  nothing at all: no finding, no candidate, from an absent surface or an
  unwired one. *Test:* two fixtures carrying `**Status:** superseded
  (2026-07-28) — merged into ANTS-3663.`, one citing an absent surface and
  one citing a present-but-unwired surface; both assert an empty
  `findings[]`. *Breaks when:* the skip is applied per-check rather than
  per-spec, which lets INV-6 fire on abandoned work and reports it as drift
  forever — training readers to ignore the kind.
- **INV-6** — for a spec INV-5 did not skip, a resolved directory present
  in `existingTestDirs` but absent from `wiredTestDirs` is a FINDING
  **whatever its live Status** — `spec draft` included. *Test:* a
  `spec draft` fixture citing a directory injected into the first set and
  not the second asserts one finding. *Breaks when:* the wiring check is
  gated on the live lifecycle like § 2.5's absence check — a test on disk
  and in no bundle runs never, and no draft state makes that intended.
- **INV-7** — `surfaces_resolved` counts distinct **surfaces** that
  resolved and were found, not the clauses carrying them, and is emitted
  even when zero. *Test:* a spec with one clause naming two present
  directories and a second clause naming one absent directory asserts
  `surfaces_resolved == 2` alongside one finding — the multi-surface clause
  is what separates the two readings, which a one-per-clause fixture cannot.
  *Breaks when:* the counter is per-clause, so a field named for surfaces
  reports something else; or it is omitted on a clean run, which makes
  "checked everything, found nothing" and "harvested nothing" identical.
- **INV-8** — the engine opens no file: both filesystem sets arrive through
  `Options`, and nothing is written or executed anywhere. *Test:*
  source-grep over `src/speclint.cpp` and `src/speclint.h` for
  `\bQProcess\b|system\(|popen|fork\(|QFile\b|QDir\b`, **over non-comment
  text only**, asserting zero hits; plus a hash of the fixture directory and
  `docs/specs/` before and after a run. The comment exclusion is not
  hygiene: `src/speclint.cpp:40` already reads ``// A fixed vocabulary, not
  a shape test: `QProcess` and …``, so the naive grep returns 1 against
  correct code and the likely repair is weakening the pattern.
  *Breaks when:* the engine reaches for the filesystem directly, which is
  the constraint `src/speclint.h` states twice and the reason
  `requiredSections` is injected rather than read.
- **INV-9** — each injected set gates its own check, and empty means skip:
  an empty `existingTestDirs` skips both checks, an empty `wiredTestDirs`
  with a non-empty `existingTestDirs` skips only the wiring check. *Test:*
  one `shipped` spec citing `tests/features/absent_one`, run three ways —
  (a) both sets empty → **zero** findings; (b) `existingTestDirs` non-empty
  but **not containing** that name, `wiredTestDirs` empty → **one**
  `test_surface_absent`; (c) `existingTestDirs` containing it,
  `wiredTestDirs` empty → **zero**. (a) against (b) is the whole test: same
  document, same absent surface, and the only difference is whether the set
  was empty. Three arms all asserting zero would pass against an engine
  that never ran either check. *Breaks when:* "not in the set" is read as
  "not on disk", which turns one failed `CMakeLists.txt` read into a
  FINDING against every resolved surface in the corpus.
- **INV-10** — `surfaces_checked` is false exactly when INV-9 skipped, and
  is emitted on every run. *Test:* INV-9's arm (a) asserts
  `surfaces_checked == false` with `surfaces_resolved == 0`; arm (c)
  asserts `surfaces_checked == true` with `surfaces_resolved == 1`. The
  pair is chosen so **neither field alone separates them** — a reading that
  infers the boolean from the counter passes arm (a) and fails arm (c).
  *Breaks when:* the boolean is inferred from the counter, the defect
  `Result::sectionsChecked` exists to prevent — its header comment says it
  "is always reported and never inferred from an empty findings list".

## 4. RAM / build cost

No new build target, no new library, no state held between calls. Two
`QSet<QString>` of `tests/features/<name>` directory names, built once per
`spec_lint` call in the verb layer and discarded with it. The population is
the directories **on disk** — `ls -d tests/features/*/ | wc -l` → **499**
today — not the 266 distinct names the specs happen to cite, which is a
different and partly disjoint set (26 of those 266 are not on disk at all).
499 names at ~40 bytes ≈ 20 KiB per set. Both sets grow with the test
suite, not with the number of specs walked, and are built once per call
rather than once per document.

## 5. Failure modes

- **A spec names a test that exists under another layout** (`tests/unit/…`).
  Not harvested, so invisible. Accepted — `tests/features/` is this
  project's convention and the only form with corpus scale behind it.
- **A directory is renamed and every citing spec updated but one.** Caught
  as a FINDING or a CANDIDATE by Status. This is the intended yield.
- **A spec cites a directory it creates in the same commit.** A CANDIDATE
  while the Status is a draft state; becomes a FINDING only once the Status
  says shipped, by which time the test should exist.

## 6. Alternatives considered (and rejected)

- **Execute the fenced fixtures under an OS sandbox** — the ROADMAP
  bullet's original design. Rejected on measurement: `docs/` carries
  **one** `python` fence and **442** `cpp` fences across 151 files
  (`grep -rcE '^```(python|py)\b' docs/` summed, and the same for
  `^```(cpp|c\+\+)\b`), and the `cpp` fences are signatures and excerpts,
  not self-contained programs. So it would need a new authoring convention
  nothing uses, to run an interpreter the corpus invokes once, behind a
  sandbox and an external dependency — for a defect class this corpus
  cannot currently exhibit. The cite-a-real-test shape needs no new
  convention: **266 distinct `tests/features/<name>` directories are
  already named in prose across the corpus** — a count of directory names,
  not of specs, the corpus holding 240 specs in total.
- **A new verb rather than a `spec_lint` kind** — rejected per § 2.1. The
  clause walk already exists; a second walk over the same bullets is
  duplication with a maintenance cost and no user-visible gain.
- **Report every absent surface as a FINDING, ignoring Status** — rejected:
  it files 23 findings today, of which 2 are defects and 21 are forward
  references or abandoned work. A check whose first run is 91% noise is one
  nobody runs twice.

## 7. Out of scope

- **Executing fenced fixtures** — a **permanent exclusion** as of the
  2026-08-12 decision, not a deferral, and therefore carrying no follow-up
  id. § 6 records the measurement. ANTS-4108 § 6 deferred this to
  ANTS-4127; that line is discharged here by decision rather than by
  delivery, which § 10 lists as a required edit to the parent.
- **Proving a cited test can fail** — a permanent exclusion at this
  boundary: it needs a build and a mutation, which is a test-harness job
  and not a read-only document check. The project convention
  (`specs.md` § 3.6) keeps asking the author for it.
- **Python `re` as an engine** — ANTS-4108 § 6 defers this to ANTS-4127 as
  its *second* bullet, on the ground that it "requires an out-of-process
  runner". It is a **permanent exclusion** here for the same reason
  fixture execution is, and carries no follow-up id: the runner it needs is
  the sandbox § 6 rejected. ANTS-4108's INV-5 continues to refuse an
  unrecognised engine rather than substituting one, which is the correct
  standing behaviour and needs nothing from this spec.
- **Resolving prose test surfaces** — § 2.4; 986 of 1026 clauses, and
  natural-language resolution is a judgement instrument, which is
  `review-contract`.
- **Normalising the Status vocabulary** — 187 files, and § 2.5 is designed
  to tolerate the spread rather than to require the cleanup.

## 8. Tests

Engine lane: `tests/features/spec_lint/`, label `features;fast`, covering
INV-1..7, INV-9 and INV-10 — every one drivable by injecting `Options`,
which is what keeping the engine filesystem-free buys. Verb lane:
`tests/features/spec_lint_verb/`, same labels, for the gathering step
(a real `tests/features/` scan and `CMakeLists.txt` parse) and for the new
`kind` values and `surfaces_resolved` reaching the envelope.
**INV-8 is a source-scrape plus a hash, not a runtime case.** Each test is
verified to fail against pre-implementation source before the
implementation lands, per the project test convention.

Both directories already exist and are already wired into a bundle
(`grep -c 'tests/features/spec_lint' CMakeLists.txt` → 2), so this spec
adds sources to targets that are already built rather than adding a target.
Per the project test convention, `ctest -N -R SpecLint` before and after is
the check that the new cases actually appear.

## 9. What checks this

| Invariant | Checked by |
|---|---|
| INV-1..7, INV-9, INV-10 | `tests/features/spec_lint/` |
| INV-2 (the pattern itself) | `spec_conformance` over § 2.4's fence-and-table, per `specs.md` § 3.5.1 |
| INV-3..5, INV-7, INV-10 reaching the envelope; the verb-layer gathering step | `tests/features/spec_lint_verb/` |
| INV-8 | source-grep over `src/speclint.{h,cpp}` + a before/after hash |
| Whether a resolved test actually *exercises* its invariant | **nothing** — § 2.2; existing and wired is not the same as able to fail |
| Whether a prose test surface names anything real | **nothing** — § 2.4 leaves 986 of 1026 clauses invisible by design |
| Whether the Status line tells the truth | **nothing** — § 2.5 classifies on a claim the spec makes about itself |
| That the figures here stay true as the corpus grows | **nothing** — § 2.4b; they are `--exclude`'d hand measurements, not an output of any test |

Four `nothing` rows. The first is the one this spec was originally asked to
close; it is recorded here rather than argued away. The fourth is the one
this document keeps re-earning — every count in it moved once already, when
the document itself joined the corpus.

## 10. Cross-doc impact

- `docs/standards/specs.md` — § 3.6 gains one line: a `*Test:*` clause
  naming a feature test writes the `tests/features/<name>` path, because
  that is the form now checked. No existing clause is invalidated.
- `docs/specs/ANTS-4108-spec-conformance-verb.md` — § 6 defers **two**
  bullets to ANTS-4127, and both need amending or the parent keeps
  promising work nobody will do: *"Executing fenced fixtures"* and
  *"Python `re` as an engine — deferred, same id"*. § 7 above closes both
  as permanent exclusions; record that in the parent rather than leaving
  either pointing at a shipped id.
- `src/speclint.cpp` — the comment above the `command_test_no_expectation`
  branch says "fenced FIXTURES have none at all until ANTS-4127 settles an
  interpreter and sandbox". ANTS-4127 settles neither; amend it in the same
  commit, or the code asserts a future this spec has cancelled.
- `docs/subsystems.md` — the `spec_lint` entry gains the two kinds.
- `ROADMAP.md` ANTS-4127 — **two** reversals to annotate on accept, not
  one. The stated home (`spec_conformance`) is superseded by § 2.1; and the
  bullet's **headline** still promises to "execute a spec's fenced
  FIXTURES", which § 2.2 and § 7 make a permanent exclusion. Annotating the
  home alone leaves the ROADMAP advertising, under this id, the work this
  spec cancelled.
- `CHANGELOG.md` — on ship.

## 11. Cold-eyes loop log

| Loop | Lanes | Q1 | Q2 | Q3 | Q4 | Verified / dismissed | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2 cold, spec genre | 7 | 4 | 4 | 1 | 16 / 0 | all fixed |

**Loop 1 (2026-08-12).** 12 of the 16 came from the two lanes (2 raised by
both, merged on defect identity); 4 were found by the orchestrator — two
while building the context packet, one while verifying a lane's arithmetic,
one while writing a fix.

- **[Q1] The engine cannot do what § 2.6 asked it to.** Found building the
  packet, before a lane was spent: `src/speclint.h` states twice that
  `SpecLint::check()` is handed the document's *text* and "cannot find or
  read that file either", yet § 2.6/§ 2.8 had it `stat`-ing directories and
  parsing `CMakeLists.txt`. Re-shaped onto the injection contract
  `requiredSections` already uses — which is also what made every invariant
  drivable from `Options`.
- **[Q1] Every count in the document stopped reproducing when the document
  was saved.** The figures were measured before this file joined
  `docs/specs/`; it carries nine `*Test:*` clauses of its own, so the quoted
  commands returned 1039 against a stated 1026. Fixed with
  `--exclude='ANTS-4127-*'` on every count and § 2.4b stating the
  reflexivity outright. Neither lane could see this — both were briefed with
  the pre-existing measurements.
- **[Q1] The invisible-clause figure was measured by proxy.** Both lanes
  independently found 766 understated it. Verifying showed the honest
  denominator is the extraction pattern itself: 986 of 1026, not 766 — the
  proxy counted a clause as visible for merely mentioning a backticked name
  or a `tests/unit/…` path.
- **[Q2] INV-5 and INV-6 contradicted each other.** A `superseded` spec
  citing a present-but-unwired directory was covered by both, oppositely.
  Resolved by skipping abandoned specs *before* either check rather than
  per-check.
- **[Q4] The fix for INV-9 shipped a tautology, caught while writing it.**
  The first draft asserted zero findings in all three arms — which passes
  against an engine that never runs either check. Re-cut so arm (a) and arm
  (b) differ only in whether the set was empty.
- Also fixed: `surfaces_resolved` was defined over clauses but named for
  surfaces (Q3); `spec_status` had no defined value for the 53 Status-less
  specs (Q3); `existingTestDirs` non-empty with `wiredTestDirs` empty was
  unspecified and would have filed a corpus-wide false-positive run from one
  failed file read (Q3); INV-8's source-grep returns 1 today against correct
  code, on a comment in `src/speclint.cpp:40` (Q1); § 2.1 described the walk
  as bullet-only when it covers the table form too (Q1); the § 4 memory
  budget used the cited-name population (266) where the injected set is the
  on-disk one (499) (Q1); § 10 annotated the ROADMAP bullet's *home* but not
  its headline, which still promises fixture execution (Q2); and ANTS-4108
  § 6 defers **two** bullets to this id, of which only one was accounted for
  (Q2).

The document grew 452 → 558 lines this loop, against the delete-first rule.
Two invariants and one subsection are genuine Q3/Q1 gaps that added where
nothing stood; the remainder replaced text rather than sitting beside it.

**Not machine-checked, and not to be read as clean:** `spec_lint` returns
`sections_checked: false` — no format standard in this project ships a
`<!-- required-sections -->` block, so required-section structure was
verified by hand against `docs/standards/specs.md` § 3 + § 4, not by a tool.
