# ANTS-4127 — resolve the test surface a spec cites, instead of reading it

**Status:** implemented (2026-08-12) — shipped in `src/speclint.{h,cpp}` +
`src/remotecontrol_docs.cpp`, 11 test rows across both lanes, full suite
3410/3410. Accepted the same day after three cold review loops: 37 findings
verified and fixed, 1 dismissed; **the gate hit its 3-loop cap without a clean
pass**, so this was accepted-with-caveat, not converged. The tail is empty and
§ 11 records why the cap bound. § 1, § 2.1, § 2.2 and § 6 produced no findings
after loop 1 and are settled; **treat § 2.3–§ 2.5's counts as the most-revised
part** — the invisible-clause figure alone was wrong in three consecutive loops.
**Kind:** feature.
**Source:** ROADMAP.md ANTS-4127 (split out of ANTS-4108 at spec time;
scope re-decided 2026-08-12 by the user — see § 2.2).
**Pairs with:** ANTS-3662 (`spec_lint`) — this extends that engine.
**Composes with:** ANTS-4108 (`spec_conformance`) — this inherits two of its
three taxonomy buckets, not its code. § 2.1 says which, and why the code
lands elsewhere.

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
# distinct tests/features/<name> named anywhere in docs/specs/.
# --exclude drops THIS document, which cites example directories of its
# own and would otherwise inflate both figures — see § 2.4b.
grep -ohE 'tests/features/[a-z0-9_]+' docs/specs/*.md \
  --exclude='ANTS-4127-*' | sort -u | wc -l                                 → 266
# of those, absent from disk (loop over the same list, test -d)             → 26
```

Twenty-three of the twenty-six are real; the other three (`audit_`,
`remote_control_`, `roadmap_`) are truncation artefacts of the measuring
regex above, which stops at the `_` of a `tests/features/audit_*` wildcard.
That artefact is not incidental — § 2.4 makes avoiding it an invariant,
because the shipped extractor must not reproduce a defect its own spec's
measurement had.

**Three of them** — in **two** specs, ANTS-1111 citing two — sit in specs
whose own **Status** says the work shipped. The two counts differ because a
finding is emitted per cited surface, not per spec, and § 6's yield figures
are the surface count:

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

What is genuinely inherited from ANTS-4108 is its **taxonomy**. That is
three named buckets — FINDING, CANDIDATE and **OBSERVATION** ("a measured
fact, never a verdict") — plus the silent case its § 2.4 defines separately,
where a block yields no bucket at all. This spec uses two of the three and
**deliberately declines OBSERVATION**: `spec_lint` has no `observations[]`
array, and `surfaces_resolved` / `surfaces_checked` are envelope fields
rather than per-case measurements, so there is nothing for the bucket to
hold. § 2.5 extends the two it uses rather than restating them.

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
`max_findings` / `etag_match` surface is unchanged; **three** `kind` values
join its `findings[]`, and **two** fields join its envelope
(`surfaces_resolved`, `surfaces_checked`):

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

{ "kind": "test_surface_unwired",      // FINDING   — § 2.6 check (2)
  "line": 201,                         // the directory EXISTS; no test_*.cpp of
  "invariant": "INV-3",                // its own is named in CMakeLists.txt, so
  "surface": "tests/features/orphaned",// it compiles nowhere and runs never
  "spec_status": "spec" }              // populated as above, but NOT gated on it
```

**Three kinds, not two.** `test_surface_unwired` is a distinct outcome from
`test_surface_absent` and must not borrow its name: the directory is
present, so a consumer reading "absent" would go looking for a missing
directory that is right there. It carries the same fields, and — unlike the
other two — its bucket does not depend on `spec_status` (§ 2.6).

`spec_status` is the **normalised** first word of the `**Status:**` value,
and normalisation is three steps, not one — the corpus needs all three and
§ 2.5's own measuring pipeline performs all three:

1. strip leading `*` (a Status value may open bold: `**considered / shelved
   (2026-07-19, user decision)`);
2. lowercase;
3. truncate at the first character outside `[a-z0-9]` (values carry
   trailing punctuation: `accepted (2026-05-27),` → `accepted`).

"First word" alone yields `**considered` or `accepted,`, which match no row
in § 2.5's table and fall to the catch-all — filing a CANDIDATE where INV-3
requires a FINDING, silently, because the catch-all absorbs them. The value
is **JSON `null`** — not `""`, not absent — when the line is missing or
empty. **56 specs are in that state** (53 carrying no Status line, 3
carrying an empty one — § 2.5), reached by two different paths and
deliberately collapsed to one value; a consumer grouping by this field
would silently drop nearly a quarter of the corpus if the key could vanish.

**`surfaces_resolved` counts distinct SURFACES, not clauses**, so a clause
naming two present directories contributes two. **Distinctness is scoped
per document**: a directory cited by two invariants of the same spec counts
**once**, and a directory-walk total is the sum over documents, so the same
directory cited by three specs contributes three. Without that scope two
implementations report different figures for one corpus and both satisfy
every other clause here. The field is the denominator without which two
zero-finding runs are indistinguishable — but zero is also what a *skipped*
run reports, so the count never carries that distinction alone:

`surfaces_checked` is a boolean, **false exactly when `existingTestDirs`
was empty** — the state in which neither check ran. It is **true** when
check (1) ran, including § 2.6's middle row where check (2) skipped for an
empty `wiredTestDirs`: the flag reports whether resolution happened at all,
not whether every check fired. It is always emitted and never inferred from
a zero count — the same contract, for the same reason, as
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
no refusal. **That is 881 of 1018 clauses today.**

**The measurement must be clause-aware, because a clause is not a line.**
Invariant bullets hard-wrap, so the path routinely sits on a continuation
line while `*Test:*` sits on the one above — INV-1 in this very document is
that shape. The extractor runs over the parser's joined `test_surface`
string, so any line-scoped count understates what it will harvest:

```
# continuation lines joined first; every count EXCLUDES this document,
# which is itself in the corpus it measures — see § 2.4b
for f in $(ls docs/specs/*.md | grep -v ANTS-4127); do
  perl -0777 -pe 's/\n[ \t]{2,}/ /g' "$f"; done > /tmp/joined.txt
grep -ohE '\*Test:\*.*' /tmp/joined.txt | wc -l                            → 1018
grep -ohE '\*Test:\*.*' /tmp/joined.txt \
  | grep -cP 'tests/features/([a-z0-9]+(?:_[a-z0-9]+)*)(?!\w|\*)'          →  137
# invisible = 1018 − 137                                                   →  881
```

**137, not 40 — and not 766 either.** Two earlier drafts of this section got
this wrong in opposite directions, and both are recorded because the shape
of each error outlived it. The first measured invisibility by *proxy*
("clauses naming neither a `tests/` path nor a backticked identifier",
766), which counts a clause visible for merely mentioning a backticked
name. The second measured against the real pattern but **line-scoped**
(40), which counts a wrapped clause invisible. Only the joined form
measures what the engine sees.

The invisible 881 are prose surfaces — "a fixture asserting…",
"source-scrape over…", a manual recipe. Prose is a legitimate test surface
(`~/.claude/skills/write-spec/references/drafting-rules.md`
§ *Clauses with nothing to run*), so reporting on it would fire on three
quarters of every spec in the corpus and be right about none of them.
Silence here is a decision, not an omission.

### 2.4b This document is inside the corpus it measures

Every figure in § 1, § 2.4 and § 2.5 was measured before this file existed,
and this file then joined `docs/specs/`. It carries **ten** `*Test:*`
clauses of its own — one per invariant — plus several prose mentions of the
literal token, so the unqualified commands now return **1039** where § 2.4
says **1018**: the number was right when taken and the corpus moved under
it.

Hence the `--exclude='ANTS-4127-*'` on every count: the figures describe
**the corpus this checker would be pointed at, not counting the document
proposing it**, and the commands as written reproduce them. A reader who
drops the flag gets a different number and should.

It reaches further than the counts. This document names example directories
— `tests/features/absent_one` and `…/orphaned` in § 2.3 and INV-9 — that do
not exist and are not meant to, so without the flag § 1's own figures read
268 and 28 rather than 266 and 26. **A spec about citing real tests cannot
avoid citing unreal ones**, which is precisely why the exclusion is stated
rather than assumed.

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
grep -h '^\*\*Status:\*\*' docs/specs/*.md --exclude='ANTS-4127-*' \
  | sed -E 's/^\*\*Status:\*\* *//; s/^\*+//' \
  | awk '{print tolower($1)}' | sed 's/[^a-z0-9].*//' | sort | uniq -c
```

**twelve** distinct first words across the 187 of 240 specs that carry the
line (240 and 187 both excluding this document, per § 2.4b — it is itself a
spec with a Status line, so the unexcluded figures are 241 and 188), plus
three whose Status value is present but empty — so **53 specs
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

Failing (2) with (1) satisfied is a `test_surface_unwired` FINDING
**whatever its live Status — for any spec § 2.5 did not make invisible**.
A test source on disk and in no bundle compiles nowhere and runs never, and
no live lifecycle state makes that intended; a `superseded` or `considered`
spec, by contrast, is skipped before this check is reached (INV-5), so the
carve-out is the same one INV-6 carries and not a second rule.

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

| Injected state | Effect | `surfaces_checked` |
|---|---|---|
| `existingTestDirs` empty | **both** checks skip (the verb layer supplied nothing) | `false` |
| `existingTestDirs` non-empty, `wiredTestDirs` empty | check (1) runs; **check (2) skips** | `true` |
| both non-empty | both run | `true` |

**`wiredTestDirs` is a subset of `existingTestDirs` by construction** — the
verb layer derives it by filtering the directories it just scanned, so a
name in the second set and not the first is not a state the gatherer can
produce. The engine does not validate the invariant and has no behaviour
defined for its breach; stating it here is what keeps that from being an
undefined case an implementer must invent an answer for.

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
| `src/speclint.cpp` | the three new finding kinds, in the existing `*Test:*` walk — text only, no filesystem |
| `src/speclint.h` | `Options` gains `existingTestDirs` / `wiredTestDirs`; `Result` gains `surfacesResolved` **and** `surfacesChecked` |
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
  resolution attempt per **distinct** name in that clause, and a clause
  wrapped across lines is harvested whole. *Test:*
  `tests/features/spec_lint/` — a fixture whose clause names two distinct
  present directories asserts `surfaces_resolved == 2`; one naming the same
  directory twice asserts `1`; one whose path sits on a continuation line
  after a `*Test:*` ending its own line asserts `1`, not `0`. *Breaks
  when:* the harvest stops at the first match, leaving the second surface of
  a multi-surface clause unchecked; or it scans line-wise, which misses
  every wrapped clause — the error that put § 2.4's own yield at 40 instead
  of 137. **"Attempts" is not an emitted quantity**, so the assertion is
  written against `surfaces_resolved`; counting findings instead would pass
  against an engine that harvests once and errors twice. A fifth fixture
  pins § 2.3's **per-document** distinctness, which no other clause
  reaches: two *separate* invariants of one spec citing the same present
  directory assert `surfaces_resolved == 1`. Without it an engine
  deduplicating per clause passes every other test here and still reports a
  different total for the same corpus.
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
  into one bucket, which either files 20 findings against specs nobody has
  implemented yet or files none against the 3 surfaces that shipped.
- **INV-4** — a spec with **no** `**Status:**` line, or one whose first
  word is unrecognised, classifies as CANDIDATE and never as FINDING.
  *Test:* a fixture with no Status line asserts `test_surface_unresolved`;
  a second carrying `**Status:** banana (2026-01-01).` asserts the same.
  Two further fixtures pin § 2.3's normalisation, and each fails a
  different one of its three steps: `**Status:** **shipped** (2026-01-01).`
  and `**Status:** shipped, then amended.` must both classify as
  `shipped` and yield `test_surface_absent` — an engine taking the literal
  first word reads `**shipped**` and `shipped,`, matches no § 2.5 row, and
  files a CANDIDATE.
  *Breaks when:* an unparsed status defaults to shipped — which would fire
  against the 53 Status-less specs measured in § 2.5.
- **INV-5** — a spec whose Status first word is `superseded` or
  `considered` is skipped **before either check runs**, so it yields
  nothing at all: no finding, no candidate, from an absent surface or an
  unwired one. *Test:* two fixtures carrying `**Status:** superseded
  (2026-07-28) — merged into ANTS-3663.`, one citing an absent surface and
  one citing a present-but-unwired surface; both assert an empty
  `findings[]` **and `surfaces_resolved == 0`** — a skipped spec resolves
  nothing, so it contributes nothing to the counter, and a directory walk
  over the same corpus therefore reports one denominator rather than two.
  *Breaks when:* the skip is applied per-check rather than per-spec, which
  lets INV-6 fire on abandoned work and reports it as drift forever —
  training readers to ignore the kind; or the spec is skipped for findings
  but still counted, which makes the § 2.3 denominator depend on how many
  abandoned specs a walk happened to cross.
- **INV-6** — for a spec INV-5 did not skip, a resolved directory present
  in `existingTestDirs` but absent from `wiredTestDirs` is a
  `test_surface_unwired` FINDING **whatever its live Status** — `spec draft`
  included. *Test:* a `spec draft` fixture citing a directory injected into
  the first set and not the second asserts one finding whose `kind` is
  `test_surface_unwired`, **not** `test_surface_absent` — the directory is
  present, and conflating the two sends a reader hunting for it. *Breaks when:* the wiring check is
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
  is emitted on every run. *Test:* all three of INV-9's arms — (a) asserts
  `surfaces_checked == false` with `surfaces_resolved == 0`; **(b) asserts
  `true` with `surfaces_resolved == 0`**, the surface being absent so
  nothing resolved, yet check (1) having run; (c) asserts `true` with
  `surfaces_resolved == 1`.
  **Arm (b) is the only falsifier of the three.** Both defects this
  invariant guards against survive (a) and (c): an implementation reading
  `surfaces_checked = (surfaces_resolved > 0)` gives false/0 at (a) and
  true/1 at (c) and passes both, and so does one setting the flag false
  whenever *any* check skipped. Only (b) — flag `true` while the counter is
  `0` — separates the correct contract from either, so a suite asserting
  (a) and (c) alone tests nothing this invariant exists for.
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
  (`grep -rhcE '^```(python|py)\b' docs/ --exclude='ANTS-4127-*'` summed,
  and the same for `^```(cpp|c\+\+)\b`; without the exclusion the figures
  read 443 across 152, because § 2.6 of this document is itself a `cpp`
  fence — § 2.4b), and the `cpp` fences are signatures and excerpts,
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
  the corpus holds **25** (spec, absent-surface) pairs today, of which
  **3** are defects; the other 22 are forward references or abandoned work.
  A check whose first run is 88% noise is one nobody runs twice. Under
  § 2.5 the same corpus yields 3 FINDINGs, 20 CANDIDATEs and 2 invisible —
  the classification is the whole difference between a usable first run and
  an ignored one. (Counted per pair, not per distinct directory name: the
  23 real absent names of § 1 produce 25 pairs because
  `claude_statusbar_extraction` and `doc_lint_fix` are each cited by two
  specs. 23 is also exactly what § 2.4's pattern yields, the three wildcard
  artefacts being excluded by construction — INV-2 holding on live data
  rather than on a fixture.)

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
- **Resolving prose test surfaces** — § 2.4; 881 of 1018 clauses, and
  natural-language resolution is a judgement instrument, which is
  `review-contract`.
- **Normalising the Status vocabulary** — 187 files, and § 2.5 is designed
  to tolerate the spread rather than to require the cleanup.

## 8. Tests

Engine lane: `tests/features/spec_lint/`, label `features;fast`, covering
INV-1..7, INV-9 and INV-10 — every one drivable by injecting `Options`,
which is what keeping the engine filesystem-free buys. Verb lane:
`tests/features/spec_lint_verb/`, same labels, for the gathering step
(a real `tests/features/` scan and `CMakeLists.txt` parse) and for the
three new `kind` values and **both** envelope fields —
`surfaces_resolved` *and* `surfaces_checked` — reaching the wire.
`surfaces_checked` is the one § 2.3 says is "always emitted and never
inferred", so a verb lane that never asserts it leaves that claim untested
at the only layer where it is observable.
**INV-8 has two arms and only one of them is static:** the token
source-scrape runs no engine, while the write-check *does* run `spec_lint`
over a fixture and hashes the tree either side of it — a claim that nothing
is written cannot be settled without writing nothing during something. The
hash arm therefore lives in the engine lane like the rest. Each test is
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
| Whether a prose test surface names anything real | **nothing** — § 2.4 leaves 881 of 1018 clauses invisible by design |
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
- `docs/subsystems.md` — the `spec_lint` entry gains the three kinds.
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
| 2 | 2 cold, spec genre | 3 | 3 | 4 | 1 | 11 / 0 | all fixed; + 4 collateral in own sweep |
| 3 | 2 cold, spec genre | 3 | 3 | 2 | 2 | 10 / 1 | all fixed; **cap reached, not converged**; empty tail |

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
**This is not specific to this spec: the check has never run on any spec in
this corpus**, and the missing block is tracked as **ANTS-3829** (with
**ANTS-4080** for the global-standard half). Until one of those lands, every
`spec_lint` clean result in this project is silent about section structure,
and a gate reporting it as settled is reporting something nobody measured.

**Loop 2 (2026-08-12).** 8 of the 11 came from the two lanes (3 raised by
both); 3 more surfaced from lane open questions the orchestrator resolved.
A fourth open question — whether `tests/features/spec_lint_verb/` is really
wired, since the quoted `grep -c` pattern is a prefix of it — resolved
**clean** at `CMakeLists.txt:1499-1500`, and is recorded here because a
question that came back sound is evidence the packet held.

- **[Q1] The yield measurement was line-scoped, and a clause is not a
  line.** Invariant bullets hard-wrap, so `grep -ohE '\*Test:\*.*'` stops
  before a path on the continuation line — and INV-1 in this document is
  exactly that shape. Re-measured clause-aware: **137 harvestable of 1018,
  so 881 invisible**, against the 40/986 loop 1 had settled on. The engine
  reads the parser's joined `test_surface`, so 137 is what it will see.
  **This is the second time this one figure has been wrong in two loops**,
  by two unrelated mechanisms; § 2.4 now records both, because the next
  person to re-measure it needs to know which traps are already known.
- **[Q3] The wiring check had no `kind`.** § 2.6 and INV-6 created a third
  outcome — a directory that exists but is in no bundle — and § 2.3 defined
  only two names for it. An implementer would have emitted
  `test_surface_absent` for a directory that is right there. Now
  `test_surface_unwired`, with its own example.
- **[Q2] Three of loop 2's findings were loop 1's own collateral**, and
  they are the pattern worth naming: § 2.6 still said "regardless of
  Status" after INV-6 gained its INV-5 carve-out; § 2.7's `Result` row
  still listed one new member after § 2.3 grew a second; and § 2.3's null
  population said 53 where § 2.5 had already split it into 53 + 3 = 56.
  Each fix was correct and each left its other half behind.
- Also fixed: `surfaces_checked` was undefined for the one row § 2.6 calls
  "the one that matters" — `existingTestDirs` non-empty, `wiredTestDirs`
  empty — and INV-10 tested only the two arms either side of it (Q2);
  "distinct surfaces" never said distinct *within what*, so two conforming
  engines could report different totals for one corpus (Q3); INV-1 asserted
  "attempts", which the envelope does not emit (Q4); § 2.1 described
  ANTS-4108's taxonomy as "FINDING, CANDIDATE, and silently invisible" when
  it has three named buckets including OBSERVATION (Q1); and
  `wiredTestDirs ⊆ existingTestDirs` was never stated (Q3).

**Collateral caught in this loop's own sweep, not by a lane (4).** The
header still described the inherited taxonomy the way § 2.1 no longer did;
§ 2.3's prose still said "two `kind` values … one counter"; and **two count
commands lacked the `--exclude` § 2.4b requires** — § 1's, which now reads
268/28 because this document cites two deliberately-unreal example
directories, and § 2.5's, which now reads 241/188 because this document is
itself a spec carrying a Status line. The reflexivity § 2.4b describes is
not a one-time correction; it re-arms on every edit that adds an example.

**Loop 3 (2026-08-12) — the cap.** 10 verified, 1 dismissed. Three lane
open questions resolved **clean** (ANTS-4108 § 2.2's "not even a CANDIDATE"
wording; `tests/features/spec_lint_verb/` really is wired; `QFile`/`QDir`/
`system(`/`popen`/`fork(` appear nowhere in `src/speclint.{h,cpp}`, so
INV-8's comment carve-out is load-bearing for `QProcess` alone).

- **[Q4] INV-10's own falsifier was misattributed** — the clause claimed
  arms (a) and (c) ensure "neither field alone separates them", but an
  implementation reading `surfaces_checked = (surfaces_resolved > 0)`
  returns false/0 at (a) and true/1 at (c) and **passes both**. Arm (b) is
  the only falsifier. A test author trusting the sentence would have shipped
  a suite that could not catch the defect the invariant exists for — the
  same green-by-construction shape § 1 is about, in this document's own
  contract, at the third loop.
- **[Q2] The first-run yield was counted per spec where findings are
  emitted per surface.** § 1's table lists three absent surfaces in two
  shipped-status specs (ANTS-1111 cites two), so § 6's "2 defects / 21
  noise" was wrong on both halves. Re-measured per (spec, surface) pair:
  **25 pairs — 3 FINDINGs, 20 CANDIDATEs, 2 invisible**, so the rejected
  alternative is 88% noise, not 91%.
- **[Q3] `spec_status` normalisation was under-specified.** § 2.3 said
  "lowercased first word"; § 2.5's own pipeline also strips leading `*` and
  truncates at the first non-`[a-z0-9]`. Without all three, `**considered`
  and `accepted,` match no row and fall to the catch-all — a CANDIDATE
  where INV-3 requires a FINDING, silently. INV-4 gained a fixture per step.
- **[Q4] Per-document distinctness had no test.** § 2.3 pinned the scope in
  loop 2 and no clause reached it; an engine deduplicating per clause passed
  every invariant and still reported a different corpus total.
- Also fixed: "two new finding kinds" survived in § 2.7 and § 10 after § 2.3
  grew a third (Q2); § 2.4b cited a figure (1026) the document no longer
  states (Q1); § 8's verb lane omitted `surfaces_checked`, the one field
  § 2.3 calls "always emitted" (Q2); "nine `*Test:*` clauses of its own"
  where there are ten (Q1); INV-5 was silent on what a skipped spec
  contributes to `surfaces_resolved` (Q3); and § 6's `cpp`-fence count
  needed the `--exclude` too, this document's own § 2.6 snippet having made
  it 443/152 (Q1).

**Dismissed (1).** A lane reported the document's invariant count as
disagreeing with the briefing packet's "9". The document carries ten and
`spec_query` returns ten — the **packet** was stale, written before INV-10
existed. Recorded rather than dropped, and the lane was right to dispute a
packet fact rather than defer to it. **Collateral (1):** a blank-line
paragraph added inside INV-10 split the bullet, so `spec_query` parsed the
new prose as part of the *body* and left the test surface truncated —
caught by re-running the parse check after the fix, not by reading.

**Why the cap bound, and it is not size.** At 781 lines this spec sits
above the corpus median (392) but below the 90th percentile (822), with 32
of 241 specs larger; the documents that historically needed nine and eleven
loops were over 1000. The cause is that this document is unusually
**measurement-dense** — nearly every section rests on a corpus count, and
each count carried a distinct population trap: proxy versus pattern, line
versus clause, spec versus surface, and the corpus changing because this
file joined it. The single figure "how many clauses are invisible" was
wrong in three consecutive loops by three unrelated mechanisms. Those are
Q1 defects a cold read surfaces one at a time, and splitting the document
would not have found them faster — the measurements are the contract.
**The tail is empty: every finding from all three loops is fixed.**
