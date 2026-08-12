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
  `invariant_no_test` / `command_test_no_expectation` branch walks every
  `- **INV-N** … *Test:* …` bullet. Resolving what the clause names is one
  more check in a walk that already happens, which is the smallest correct
  change.
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
  "spec_status": "shipped" }           // the bucket § 2.5 classified on

{ "kind": "test_surface_unresolved",   // CANDIDATE — § 2.5
  "line": 88,
  "invariant": "INV-2",
  "surface": "tests/features/doc_lint",
  "spec_status": "accepted" }
```

`surfaces_resolved` counts the clauses that named a resolvable surface and
found it — the denominator without which two zero-finding runs are
indistinguishable (one checked nothing, one checked everything).

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
no refusal. That is 766 of 1026 clauses today:

```
grep -ohE '\*Test:\*.*' docs/specs/*.md | wc -l                          → 1026
grep -ohE '\*Test:\*.*' docs/specs/*.md | grep -vE 'tests/' \
  | grep -cvE '`[A-Za-z_][A-Za-z0-9_]*`'                                 →  766
```

They are prose surfaces — "a fixture asserting…", "source-scrape over…", a
manual recipe. Prose is a legitimate test surface
(`~/.claude/skills/write-spec/references/drafting-rules.md`
§ *Clauses with nothing to run*), so reporting on it would fire on three
quarters of every spec in the corpus and be right about none of them.
Silence here is a decision, not an omission.

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
| `src/speclint.cpp` | the two new finding kinds, in the existing `*Test:*` walk |
| `src/speclint.h` | `Result` gains `surfacesResolved` |
| `tests/features/spec_lint/` | engine lane — extraction, classification, wiring |
| `tests/features/spec_lint_verb/` | verb lane — the new `kind` values reaching the envelope |

No new TU, no new library, no new dependency. `src/specconformance.{h,cpp}`
is **not touched**, which keeps ANTS-4108 INV-8's source-scrape true by
construction rather than by care.

### 2.8 Caps and the trust boundary

Unchanged from `spec_lint`'s: the specs are in your own repository, the
check reads them and stats directories, and it runs no subprocess. The one
new cost is a `stat` per resolved surface — 266 across the whole corpus
today, bounded by `max_findings` in the same way every other kind is.

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
  `considered` yields nothing at all: no finding, no candidate. *Test:*
  fixture carrying `**Status:** superseded (2026-07-28) — merged into
  ANTS-3663.` and an absent surface asserts an empty `findings[]`.
  *Breaks when:* abandoned work is reported as drift forever, which trains
  readers to ignore the kind.
- **INV-6** — a resolved directory holding no `test_*.cpp` named in
  `CMakeLists.txt` is a FINDING **whatever** the spec's Status. *Test:* a
  fixture directory containing a `test_*.cpp` deliberately absent from
  `CMakeLists.txt`, cited by a `spec draft` spec, asserts one finding.
  *Breaks when:* the wiring check is gated on Status like § 2.5's absence
  check — this is the one case where a draft lifecycle excuses nothing.
- **INV-7** — `surfaces_resolved` counts clauses whose surface resolved and
  was found, and is emitted even when it is zero. *Test:* a run over a spec
  with one present and one absent surface asserts `surfaces_resolved == 1`
  alongside one finding. *Breaks when:* the counter is omitted on a clean
  run, which makes "checked everything, found nothing" and "harvested
  nothing" the same envelope.
- **INV-8** — the check reads and stats only; it writes nothing and starts
  no subprocess. *Test:* source-grep over `src/speclint.cpp` and
  `src/speclint.h` for
  `\bQProcess\b|system\(|popen|fork\(|QFile::remove`, asserting zero hits;
  plus a hash of the fixture directory and `docs/specs/` before and after a
  run. *Breaks when:* an autofix is added here rather than in an
  author-side tool.

## 4. RAM / build cost

No new build target, no new library, no state held between calls. One
`QSet<QString>` of the `CMakeLists.txt` test-source paths, built once per
`spec_lint` call and discarded with it: 515 paths at ~48 bytes ≈ 25 KiB,
bounded by the file it is parsed from.

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
  convention: 266 specs already name `tests/features/<name>` in prose.
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
- **Resolving prose test surfaces** — § 2.4; 766 of 1026 clauses, and
  natural-language resolution is a judgement instrument, which is
  `review-contract`.
- **Normalising the Status vocabulary** — 187 files, and § 2.5 is designed
  to tolerate the spread rather than to require the cleanup.

## 8. Tests

Engine lane: `tests/features/spec_lint/`, label `features;fast`, covering
INV-1..7. Verb lane: `tests/features/spec_lint_verb/`, same labels, for the
new `kind` values and `surfaces_resolved` reaching the envelope.
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
| INV-1..7 | `tests/features/spec_lint/` |
| INV-2 (the pattern itself) | `spec_conformance` over § 2.4's fence-and-table, per `specs.md` § 3.5.1 |
| INV-3..5, INV-7 reaching the envelope | `tests/features/spec_lint_verb/` |
| INV-8 | source-grep over `src/speclint.{h,cpp}` + a before/after hash |
| Whether a resolved test actually *exercises* its invariant | **nothing** — § 2.2; existing and wired is not the same as able to fail |
| Whether a prose test surface names anything real | **nothing** — § 2.4 leaves 766 of 1026 clauses invisible by design |
| Whether the Status line tells the truth | **nothing** — § 2.5 classifies on a claim the spec makes about itself |

Three `nothing` rows. The first is the one this spec was originally asked
to close; it is recorded here rather than argued away.

## 10. Cross-doc impact

- `docs/standards/specs.md` — § 3.6 gains one line: a `*Test:*` clause
  naming a feature test writes the `tests/features/<name>` path, because
  that is the form now checked. No existing clause is invalidated.
- `docs/specs/ANTS-4108-spec-conformance-verb.md` — § 6's first bullet
  defers fixture execution to ANTS-4127; amend it to record that ANTS-4127
  closed the line by decision, so the parent does not keep promising work
  nobody will do.
- `docs/subsystems.md` — the `spec_lint` entry gains the two kinds.
- `ROADMAP.md` ANTS-4127 — the bullet's stated home (`spec_conformance`) is
  superseded by § 2.1; annotate on accept.
- `CHANGELOG.md` — on ship.

## 11. Cold-eyes loop log

| Loop | Lanes | Q1 | Q2 | Q3 | Q4 | Verified / dismissed | Outcome |
|---|---|---|---|---|---|---|---|
| — | — | — | — | — | — | — | not yet reviewed |
