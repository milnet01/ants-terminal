# ANTS-4108 — run a spec's own patterns against the examples beside them

**Status:** accepted (2026-08-12) — 26 findings verified and fixed across
three cold review loops; the gate hit its 3-loop cap **without a clean
pass**, so this is accepted-with-caveat, not converged. Review closed in
favour of building test-first (§ 10 carries the evidence and the reasoning).
§ 1, § 2.1, § 2.2, § 5 and § 6 are settled and produced no findings after
loop 1. **Treat § 2.3–§ 2.6 as provisional** — the envelope and extraction
taxonomy were still being designed inside the review, and the first
fixtures should be trusted over the prose where they disagree.
**Kind:** implement.
**Source:** ROADMAP.md ANTS-4108 (cross-session-feedback-2026-08-11, Local
Web Server Manager).
**Pairs with:** ANTS-3662 (`spec_lint`) — the structural half of the same
gate. This verb is the executable half; § 2.1 states why it is a separate
verb and not a mode of that one.

**Format note.** This project carries a full spec standard
(`docs/standards/specs.md`, v1 2026-05-21) predating the 2026-08-08 change
that made `~/.claude/standards/spec-format.md` authoritative with projects
stating deltas. The structure below follows the **project** standard,
because the corpus (`ls docs/specs/ | wc -l` → 240, this spec included) and
`spec_query`'s parse contract are built on it; the global standard's § 0
*Before you commit —
the ten* was applied as the authoring checklist on top. Reconciling the two
into a deltas-only override is a corpus-wide job, not this spec's.

## 1. Problem

A spec routinely **prescribes an executable artefact** — a regex, a range
check, a size cap, a precedence order, a fixture — and states in prose what
that artefact does. When the prose is wrong, every downstream reader
inherits the error, because the implementer builds what the prose says.

Nothing in the current gate executes those artefacts.

1. **`spec_lint` deliberately does not.** `src/speclint.cpp`, in the branch
   that raises `command_test_no_expectation`, records the decision in
   comment: *"this engine runs no subprocess — /write-spec Step 3 owns
   executing a clause, at write time, where a failure is free (INV-4)."*
   The check therefore flags a test clause that states no expectation, and
   stops short of finding out whether the expectation would hold.
2. **The author-side step it delegates to does not reliably happen.**
   `/write-spec` Step 3 requires running every `*Test:*` clause twice as it
   is written. It is an instruction to a reader, and the reporting session's
   measurement is that readers do not carry it out — see below.
3. **Cold review does not catch this class either, and that is the
   evidence.** Per the ANTS-4108 bullet: three `/cold-eyes` loops, six
   strong-model lanes and 75 verified findings read the prescribed patterns
   and passed them. Running the same patterns surfaced four defects in about
   two minutes, of which the bullet records these three:
   - `re.search(r"\d{1,5}(?![0-9])", " 123456")` returns `23456`, not a
     refusal — a lookahead does not reject a longer number under *search*,
     because the engine advances the start position.
   - A performance fixture returned before the regex under test ever ran:
     green by construction, and green under its own prescribed mutation.
   - A quoted cost of 0.24 µs was timing that early return against a real
     77 µs.

The asymmetry is the point. A judgement review always has one more
dimension with something to say, so it converges slowly and never proves
absence. **A pattern either matches its stated example or it does not** —
that answer is cheap, total, and needs no language model.

**Layman:** specs often contain a search pattern plus a small table of
"this input should give this answer". Nobody actually runs them; when the
pattern is wrong, everything built from it is wrong. This runs them.

## 2. Surface

### 2.1 A new verb, not a `spec_lint` mode

`spec_lint`'s contract states that it runs no subprocess and executes
nothing — the decision quoted in § 1, item 1, from `src/speclint.cpp`
itself. Folding execution into it would falsify a published
contract that another spec's invariants rest on, so `spec_conformance` is a
separate verb. `spec_lint` keeps raising `command_test_no_expectation` as a
CANDIDATE — a clause that states no expectation is precisely a clause this
verb cannot check, so the two are complementary rather than overlapping.

### 2.2 Scope decision — patterns are executed, arbitrary blocks are not

**This verb executes PATTERNS against declared inputs. It does not execute
fenced code.** The ANTS-4108 bullet proposed `exec`-ing each fenced block in
a restricted namespace. That is rejected here (§ 5, alternatives) on two
grounds:

- **Trust.** A pattern applied to a string cannot reach the filesystem or
  the network whatever it contains; the worst it can do is spend CPU. An
  `exec` of arbitrary block text can do anything the process can, and
  in-process interpreter sandboxes for that job are not sound.
- **Coverage.** Of the four defects above, the first is a pattern
  conformance failure this verb catches. The second and third are *fixture
  reachability* and *timing* failures, which need the fixture's own code
  run — a strictly larger and riskier feature. Splitting them keeps the
  cheap, total half shippable now.

The consequence is stated plainly rather than buried: **this verb catches
one of the reporter's four defects and surfaces none of the others.** A
fixture lives in a `python` or `cpp` fence, which § 2.6 makes invisible to
this verb — so the reachability and timing defects produce nothing at all
here, not even a CANDIDATE. The fixture-execution half is § 6, and it is a
permanent exclusion as of 2026-08-12 rather than work waiting on ANTS-4127 —
that id shipped the citation half instead (`test_surface_absent` and its two
siblings), which resolves what a clause NAMES and still executes nothing.

### 2.3 Inputs and envelope

```jsonc
// request
{ "path": "docs/specs/ANTS-4070-rotation-and-section-title.md",  // required
  "caller_cwd": "/abs/project/root",                             // required
  "max_cases": 200,        // optional, range [1, 1000], default 200;
                           // out of range refuses the call (bad_args) —
                           // it is not silently clamped (§ 2.5)
  "etag_match": "<etag>" } // optional, 304 short-circuit

// response
{ "ok": true,
  "path": "docs/specs/ANTS-4070-...md",
  "cases_run": 12,
  "findings":     [ { "kind": "mismatch", "line": 88, "pattern": "\\d{1,5}(?![0-9])",
                      "engine": "pcre2", "input": " 123456",
                      "expected": "no match", "actual": "23456" } ],
  "candidates":   [ { "kind": "pattern_without_expectation", "line": 143,
                      "pattern": "^#{1,6}\\s" } ],   // kinds: § 2.5
  "refusals":     [ { "line": 210, "code": "unsupported_engine" } ],
  "observations": [ { "kind": "timing", "line": 88, "micros": 77 } ],
  "truncated": false,
  "etag": "<hex>" }
```

**`line` is always the line an author must edit**, which resolves it for
every bucket without a second rule:

| Entry | `line` |
|---|---|
| `findings[]`, `observations[]`, a per-row refusal (`too_large` on an input) | the expectation ROW |
| `candidates[]` kind `malformed_expectation_cell` | the offending ROW |
| `candidates[]` kinds `pattern_without_expectation` / `engine_not_declared`, and a fence-level refusal (`unsupported_engine`, `too_large` on a pattern) | the FENCE's opening line |

`observations[]` carries **one entry per case**, on the same line as the
finding it would have produced — a per-fence timing could not attribute a
slow row.

Conforms to [`docs/standards/mcp-tools.md`](../standards/mcp-tools.md) —
response-wrap, `caller_cwd` + `CallerCwdContract`, path validation under the
project root, and the refusal taxonomy in
[`mcp-error-codes.md`](../standards/mcp-error-codes.md). Read-only: the verb
never writes, so it has no state-routing surface. `path` is **required**: this
verb executes one document, so unlike `spec_lint` there is no tree walk to
default to. The emitted `path` is project-relative — the envelope crosses the
wire, where an absolute path leaks the caller's home directory.

**Two deviations from that checklist, both forced by `observations[]`** and
recorded here because a reader would otherwise take them for oversights
(2026-08-12, wiring pass — see § 10 row `3-impl-2`):

- **The ETag 304 is handler-local, against step 7.** That step has the
  dispatcher hash the whole response text and forbids the handler from emitting
  an etag. This envelope carries one measured-microsecond row per case, so that
  hash differs on every run: the 304 could never fire and INV-9 would be
  unsatisfiable. The engine hashes the envelope minus `observations` and
  `RemoteControl::specConformanceBuildResponse` compares it. The verb is
  therefore deliberately **absent** from `isEtagSupportedTool`, and
  `tests/features/spec_conformance_verb/` asserts that absence — wiring both
  mechanisms would overwrite the stable etag with a timing-sensitive one and
  kill the cache silently.
- **`fields=` (step 8, optional) was declined — no longer, and it cannot be
  (ANTS-4524, 2026-08-25).** Projection is universal now, so there is nothing
  to decline. The hazard stands and is unchanged: central projection is skipped
  only on a *central* 304, a handler-local one is invisible to it, and a caller
  passing `etag_match` and `fields` together would have `unchanged` projected
  out of its own 304 response. What moved is the guard — `mcp::projectFields`
  returns any envelope carrying `unchanged:true` whole, so it covers this verb
  without this verb doing anything. Only the ETag deviation above is still a
  deviation.

### 2.4 Extraction contract

A **case** is a pattern plus one expectation row. Both must be adjacent in
the same section, so that authoring one is a local act:

````markdown
```regex pcre2
^(?:export\s++)?(?:default\s++)?(?:const|let|var)\s++([A-Za-z_$][\w$]*)\s*=
```

| input | expected |
|---|---|
| `const CSS = ` | `CSS` |
| `export const CLIENT_JS = ` | `CLIENT_JS` |
| `  const local = ` | no match |
````

- The fence's **info string declares the engine** (`regex pcre2`). A fence
  tagged `regex` with no engine is a CANDIDATE, never a guess (§ 2.5).
- **THREE ways a block is invisible — no case, no candidate, no refusal.**
  These are the extractor's silent modes, and they are grouped here because
  silence is the one outcome an author cannot debug from the envelope.
  `docs/standards/specs.md` § 3.5.1 restates exactly this list conformer-side,
  and nothing else from this section, for that reason.
  1. **The fence must open at column 0.** `fenceDelimLen()` counts backticks
     from index 0, so an indented fence returns 0 and the line is skipped.
     This is the likeliest author error, since attaching a fenced block to an
     `- **INV-N**` bullet in GFM means indenting it.
     *Test:* `Inv1IndentedFenceIsNotScanned`.
  2. **The info string's FIRST word must be exactly `regex`.** ` ```regexp `,
     ` ```pcre2 ` and every other near-miss are not ours — the engine check
     below never runs, so this is NOT `unsupported_engine`.
     *Test:* `Inv1NonRegexFirstWordIsInvisible`.
  3. **Only TOP-LEVEL fences are scanned.** The extractor never descends into
     a fence opened by a longer delimiter. The example immediately below is
     the reason: it is a four-backtick `markdown` fence *containing* a
     `regex pcre2` fence and its table, so a naive line scan would extract
     three cases from this spec's own illustration and report any
     deliberately-wrong example as a real FINDING. A spec quoting another
     spec's pattern invariant inherits the same silence.
     *Test:* `Inv1NestedFenceIsNotScanned`.
- The expectation table is the **first** GFM table after the fence with
  exactly the columns `input` and `expected`. The search skips blank lines
  and stops at the next fence, the next heading, **or any other prose line** —
  so a sentence between fence and table demotes the case to a
  `pattern_without_expectation` CANDIDATE. That is reported, not silent.
  Anything else is not an expectation table.
- `expected` is either the `no match` sentinel, or the text of **capture
  group 1** when the pattern has **at least one** capturing group (group 1
  regardless of how many follow), else the **first** match under search
  semantics.
- **The sentinel is recognised BEFORE backtick stripping**, so bare
  `no match` is the sentinel and `` `no match` `` is the literal string.
  Otherwise a pattern expected to match that text could not be written.
  Backticks are stripped after, so a literal can carry leading or trailing
  spaces visibly.
- **There are THREE outcomes, not two, because INV-3 turns on telling them
  apart.** Qt returns a *null* string for a capture group that did not
  participate and an *empty* one for a group that participated and matched
  nothing, so a two-way encoding collapses two different states:

  | Outcome | Written as |
  |---|---|
  | the pattern did not match | `no match` |
  | matched; group 1 did not participate (e.g. `a(b)?` on `a`) | `no capture` |
  | matched; group 1 captured the empty string (e.g. `a(b*)` on `a`) | an empty backtick pair `` `` `` |

  A bare empty cell is malformed and yields a CANDIDATE, never a silent
  reading of any of the three.

  **Discriminate on whether group 1 *participated*, never by comparing
  the captured string.** Qt returns a null `QString` for a
  non-participating group and an empty one for a zero-length capture, and
  `QString() == QString("")` is **true** — so an implementer reaching for
  `==` or `isEmpty()` collapses rows two and three of that table and INV-3
  fails for a reason the encoding cannot express.

  **`QRegularExpressionMatch::hasCaptured()` says exactly this and is the
  wrong call here: it is Qt 6.3+, and this project's floor is Qt 6.2**
  (`dependencies.md` § 4, enforced by `ci.yml`'s `qt62-baseline` job).
  The first implementation used it, compiled on this host's Qt, passed
  the full suite, and failed the baseline job with *"no member named
  `hasCaptured`"* — the exact class that job exists to catch. Spell out
  its two clauses instead: `m.lastCapturedIndex() >= 1 &&
  m.capturedStart(1) != -1`, since `capturedStart()` is `-1` precisely
  when the group did not participate. Both have been available since
  Qt 5.

### 2.4a The convention is NEW, and nothing uses it yet

Found by building the engine and pointing it at the corpus (2026-08-12),
not by reading:

```
grep -rn -e '^```regex' docs/   →  1 hit, in this spec
grep -rlE '^\|\s*input\s*\|\s*expected\s*\|' docs/  →  1 file, this spec
```

That single hit is § 2.4's illustration, nested inside a four-backtick
fence, which the extractor correctly ignores. **So against today's corpus
this verb reports nothing at all.**

Two consequences the rest of this document must be read against:

1. **Its value is prospective.** It checks patterns authors write in the
   fence-plus-table form from now on; it does not retrofit itself onto the
   239 existing specs. § 9 lists the `docs/standards/specs.md` amendment
   that makes the convention the recommended way to state a pattern
   invariant — that amendment is the precondition for this verb finding
   anything, not a nicety.
2. **The reporting session's defects were in a different form.** Its
   evidence (§ 1) is `re.search(r"\d{1,5}(?![0-9])", " 123456")` — a Python
   *call*, in a Python fence. This verb's extractor would not have found
   it. It catches that defect only once the pattern is restated as a
   `regex pcre2` fence with a table beside it. Recognising the call form
   directly is **ANTS-4128**.

Neither is a reason not to ship: a pattern nobody can check is exactly what
§ 2.5's CANDIDATE bucket is for, and the convention has to exist before it
can be adopted. But a reader who took § 1's evidence as a promise of
immediate yield would be wrong, so it is stated here rather than left to be
discovered.

### 2.5 Result taxonomy

| Bucket | Meaning | Example |
|---|---|---|
| FINDING | a row ran and disagreed | `\d{1,5}(?![0-9])` on `" 123456"` yields `23456`, expected no match |
| CANDIDATE | a prescribed artefact nothing can check | see the three `kind` values below |
| OBSERVATION | a measured fact, never a verdict | per-pattern match time in µs |

A candidate's `kind` is one of exactly three, because the author action
differs for each: `pattern_without_expectation` (a `regex` fence with no
table), `engine_not_declared` (a bare ` ```regex ` fence), and
`malformed_expectation_cell` (a bare empty `expected` cell). Collapsing
them loses the distinction between "nobody stated an expectation" and
"the table is broken".

A CANDIDATE says **nobody stated what this pattern should do**. Its scope
is a `regex` fence and nothing else — it is not a partial answer to the
fixture defects, which § 2.2 states this verb does not surface at all.

**A refusal is PER CASE, and per FENCE where the fence yields no case.**
An engine or pattern defect is a property of the fence, so it emits **one**
refusal row however many expectation rows sit beneath it — including zero,
which is how INV-5's table-less `regex python` fixture emits a refusal with
no case at all. An input defect is a property of one row and emits one row.
**A case emits at most one refusal**, under this precedence: engine →
pattern cap → expectation table → input cap. Without the order, a
`regex python` fence carrying a 600-byte pattern satisfies both
`unsupported_engine` and `too_large`, and a fixture asserting
`refusals[0].code` is green or red by build order.

The run continues either way; the other cases in the file still produce
their findings. Only a malformed
*request* — an unreadable `path`, a `max_cases` outside its range — refuses
the whole call. The alternative was tried on paper and rejected: a
corpus-wide sweep in which one stray fence blinds an entire file is a sweep
that reports clean for the wrong reason. `cases_run` counts cases actually
executed, so a refused case is excluded from it.

### 2.6 Engines

**This table ranges over fences whose info string's FIRST word is `regex`.**
A fence tagged anything else (` ```cpp `, ` ```jsonc `, ` ```markdown `) is
not a case, is not a candidate, and is not a refusal — it is invisible to
this verb. Every spec in the corpus carries such fences, this one included,
so a rule that refused them would refuse nearly every real document.

| Fence tag | Engine | Notes |
|---|---|---|
| `regex pcre2` | `QRegularExpression` | Qt's PCRE2; no new dependency |
| `regex` (no engine) | — | CANDIDATE, never a guess and never a refusal (§ 2.4) |
| `regex <other>` | — | refused `unsupported_engine`; never silently substituted |

`unsupported_engine` is the one new code this verb adds, named to match the
taxonomy's existing `unsupported_format`. The cap refusal reuses `too_large`
rather than minting a second (INV-7).

The check precedence that keeps an unsupported-engine fence from also
counting as a candidate is stated once, in § 2.5. **A fence yields at most
one candidate** under that same order: a bare ` ```regex ` fence with no
table is `engine_not_declared`, and the table check is never reached — so
it is one candidate, not two.

**A substitute engine is worse than no engine.** The two agree on the
reported case — verified, not assumed: `python3 -c "import re;
print(re.search(r'\d{1,5}(?![0-9])', ' 123456').group(0))"` and
`printf ' 123456' | grep -oP '\d{1,5}(?![0-9])'` both return `23456`. They
are still not the same engine, and the defaults differ where it matters:
Qt leaves `QRegularExpression::UseUnicodePropertiesOption` **off** by
default — a Qt header symbol, not a project one: the option is `0x0040`
and the constructor takes `PatternOptions options = NoPatternOption`
(`0x0000`), both at `/usr/include/qt6/QtCore/qregularexpression.h:35,42,53`
on this host — so `\d` is ASCII-only, while
Python 3's `\d` matches Unicode digits unless `re.ASCII` is passed. A
pattern written against one and run under the other can therefore return a
*confident wrong answer about a spec that is correct* — the one outcome
worse than not checking at all. So the tag selects the engine or the case
is refused; Python support is § 6.

*(Author's note, kept because it is this spec's own evidence: the first
draft of this paragraph claimed Python and PCRE2 differ on named-group
syntax, `(?P<n>…)` against `(?<n>…)`. Running it —
`printf 'abc' | grep -oP '(?P<w>a)bc'` — showed PCRE2 accepts the Python
form, so the claim was false. It was a prescribed-artefact claim that read
fine and was wrong, which is the exact class § 1 is about.)*

### 2.7 Where the code lives

| File | Role |
|---|---|
| `src/specconformance.{h,cpp}` | pure engine: extract → run → classify. Joins `ants_core_lib` beside `src/speclint.cpp` |
| `src/remotecontrol_docs.cpp` | `cmdSpecConformance` handler |
| `src/claudeintegration.cpp` | `tools/list` schema + `tools/call` dispatch |
| `src/mainwindow.cpp` | provider lambda (`rcDelegate`, which forwards the whole arg object — no per-verb allowlist to drop `max_cases` through, the ANTS-3420 class) |
| `tests/features/spec_conformance/` | engine lane, 10 fixtures |
| `tests/features/spec_conformance_verb/` | verb lane, 3 fixtures |

### 2.8 Caps and the trust boundary

The specs this verb reads are **in your own repository**. That is the same
trust boundary as building the project: a hostile file there has easier
routes than a regex. So the caps below are defence against an *accident* —
a runaway pattern an author wrote by mistake — and the verb explicitly does
**not** claim to withstand a deliberately catastrophic pattern.

| Cap | Value | Reason |
|---|---|---|
| pattern length | 512 bytes | same class of guard as `fileoutline.cpp`'s `kMaxLineBytes`, which caps a scanned line at **1024** for the same backtracking reason; half that here because a pattern is shorter than a source line |
| input length | 512 bytes | an expectation row is an example, not a corpus |
| cases per file | `max_cases`, default 200 | bounds total work per call |

**Honest limit, stated because `QRegularExpression` exposes no match
timeout:** a pathological pattern within these caps can still spend
significant CPU, and this verb has no way to interrupt it. § 3 INV-7 tests
the caps; nothing tests interruption, because nothing implements it. This
row is deliberately `nothing` in § 8.

## 3. Invariants

- **INV-1** — a fence declaring an engine, followed by an expectation table
  with `input`/`expected` columns, yields one case per row. *Test:*
  `tests/features/spec_conformance/` — fixture with a 3-row table asserts
  `cases_run == 3`. *Breaks when:* the table scan stops at the first row, or
  looks past the next heading and adopts an unrelated table.
- **INV-2** — a row whose actual result differs from `expected` is a FINDING
  carrying pattern, input, expected and actual. *Test:* the reporter's own
  case — `\d{1,5}(?![0-9])` against `" 123456"` with `expected: no match`
  produces one finding whose `actual` is `23456`. *Breaks when:* the runner
  uses an anchored match rather than a search, which makes the defect that
  motivated this verb invisible to it.
- **INV-3** — `expected: no match` is satisfied only by zero matches.
  *Test:* three rows, one per § 2.4 outcome, and the last two are the ones
  that matter. (a) `no match` against a pattern that matches non-emptily is
  reported as a finding. (b) `a(b*)` against input `a` — group 1
  participates and captures the empty string — is NOT reported against an
  empty backtick pair, and IS reported against `no match`. (c) `a(b)?`
  against input `a` — group 1 does not participate — is NOT reported
  against `no capture`, and IS reported against the empty backtick pair.
  *Breaks when:* any two of the three outcomes are conflated. Row (a) alone
  reaches none of that, and (b) alone cannot separate a null capture from an
  empty one — which is the distinction Qt actually returns.
- **INV-4** — a fence with a declared engine and NO expectation table is a
  CANDIDATE, never silently dropped and never a finding. *Test:* fixture
  with a bare `regex pcre2` fence asserts one candidate, zero findings.
  *Breaks when:* extraction requires a table to emit anything, which makes
  an unchecked pattern indistinguishable from an absent one.
- **INV-5** — an unrecognised engine tag is refused `unsupported_engine`
  and no pattern is run under a substitute; the refusal is per case.
  *Test:* two fixtures. One containing **only** a `regex python` fence
  asserts `refusals[0].code == "unsupported_engine"`, `cases_run == 0`
  **and `candidates == []`** — the last is what pins the § 2.6 ordering,
  since that fence also has no expectation table.
  One containing that fence **plus two valid `regex pcre2` cases** asserts
  `ok: true`, `cases_run == 2` and one refusal row — the leg that catches a
  call-level abort. *Breaks when:* the tag is ignored and the pattern runs
  under PCRE2, which returns a confident wrong verdict on a correct spec
  (§ 2.6); or one bad fence aborts the file, so a sweep reports clean
  because it stopped.
- **INV-6** — the verb writes nothing. *Test:* hash every file under the
  **fixture directory and `docs/specs/`** before and after a run over a
  fixture containing findings; assert equality. *Breaks when:* a future
  autofix is added here rather than in an author-side tool. The hash is
  scoped rather than taken over the project root, because `build*/` and
  `.git/` are written by the harness running the test — a root-wide hash
  fails whatever the verb does, which falsifies nothing.
- **INV-7** — the § 2.8 caps are enforced and reported, not silently
  applied. *Test:* a 600-byte pattern and a 600-byte input each yield a
  per-case `refusals[]` row with code `too_large` — the taxonomy's existing
  code for an input resource over a cap, not a new one minted here; a file
  with 201 cases at `max_cases: 200` returns `truncated: true` with
  `cases_run == 200`.
  *Breaks when:* the cap clamps quietly, so a partial run reads as a
  complete one.
- **INV-8** — the conformance ENGINE runs no subprocess and loads no
  interpreter, so `spec_lint`'s no-execution contract stays true of the
  pair. *Test:* source-grep over `src/specconformance.h` and
  `src/specconformance.cpp` — the files this change creates — for
  `\bQProcess\b|system\(|popen|fork\(|lua_newstate|luaL_newstate|Py_Initialize`,
  asserting zero hits. *Breaks when:* Python support (§ 6) is added here
  instead of behind its own decision.
  **Scope, and it is the whole point of the clause:** the grep covers the
  new engine only, NOT the shared TUs § 2.7 also lists. Under the regex
  above, `grep -cE` (matching **lines**) returns 33 for
  `src/mainwindow.cpp`, 1 for `src/claudeintegration.cpp` and 0 for
  `src/remotecontrol_docs.cpp` — 34 lines already there for unrelated
  verbs. A scrape over "every file § 2.7 names" would assert zero against
  those 34: red for every possible implementation, and therefore falsifying
  nothing. `\b` on `QProcess` keeps `QProcessEnvironment` from counting,
  which is most of the difference between this figure and the same grep
  without it.
- **INV-9** — two runs over an unchanged file return envelopes that are
  byte-identical **except for `observations`**, and a matching `etag_match`
  short-circuits. *Test:* run twice, compare the serialised envelope with
  `observations` elided; then pass the returned etag and assert
  `unchanged: true`. *Breaks when:* the etag is computed over the whole
  envelope — measured µs differ per run, so every call would be a cache
  miss. The etag is therefore computed over the envelope minus
  `observations`. Full byte-identity is **not** claimed: § 2.3 emits
  per-run timings on purpose, and an invariant demanding both would be
  unsatisfiable by any correct implementation.

## 4. RAM / build cost

Per call: one spec file, plus at most `max_cases` × (512 + 512) bytes of
extracted case text ≈ 200 KiB at the default. The largest spec in the corpus
is 122,121 bytes (`ls -S docs/specs/ | head -1` → `ANTS-3636.md`; `wc -c` →
122121), so the file itself is the smaller half of a worst-case call.
Nothing is retained between calls; no cache, so no eviction policy is
needed. Build cost: one new TU in `ants_core_lib` (`add_library(ants_core_lib
STATIC …)`, CMakeLists.txt, where `src/speclint.cpp` also sits), no new
external library.

## 5. Alternatives considered

- **A mode on `spec_lint`** — rejected: falsifies that verb's published
  no-subprocess/no-execution contract (§ 2.1).
- **`exec` of fenced blocks in a restricted namespace** (the bullet's own
  proposal) — rejected for v1 on trust and scope (§ 2.2). It is the only
  route to the fixture-reachability and timing defects, so it is deferred
  rather than refused outright (§ 6).
- **Run patterns under the Lua sandbox** (`src/luaengine.cpp`, which already
  has a 5-library allowlist and an instruction-count plus wall-clock
  watchdog, ANTS-1172) — rejected: Lua patterns are not regexes, so the
  engine cannot run the artefacts specs actually prescribe. Its watchdog is
  the right shape for a future interpreter lane and is noted for § 6.

## 6. Out of scope

- **Executing fenced fixtures** — the reachability and timing defects, the
  second and third of the three listed in § 1. **No longer deferred: a
  permanent exclusion, closed by decision on 2026-08-12 and carrying no
  follow-up id.** It was tracked by **ANTS-4127**, which settled the
  interpreter-and-sandbox question by dropping the premise that raised it —
  `docs/` holds one `python` fence against 442 illustrative `cpp` ones, so
  the runner would be built for a defect class this corpus cannot exhibit
  (ANTS-4127 § 6). ANTS-4127 shipped the citation half instead: it resolves
  what a `*Test:*` clause names, and executes nothing.
- **Python `re` as an engine** — a **permanent exclusion** for the same
  reason, also closed 2026-08-12 with no follow-up id: the out-of-process
  runner it requires is the sandbox above. INV-5 continues to refuse an
  unrecognised engine rather than approximate one, which is the correct
  standing behaviour and needs nothing further.
- **Prose quality, or whether an invariant is well chosen** — a permanent
  exclusion. That is `review-contract`'s judgement half, and this verb exists
  precisely because the two are different instruments.
- **Interrupting a running match** — a permanent exclusion at this trust
  boundary (§ 2.8); Qt exposes no match timeout and the input is your own
  repository.

## 7. Tests

Feature test: `tests/features/spec_conformance/`, label `features;fast`,
covering INV-1..7 and INV-9. **INV-8 is a source-scrape, not a runtime
case** — no running test can observe the absence of an interpreter.
Verb-layer lane: `tests/features/spec_conformance_verb/`, same bundle and
labels — the handler-local ETag 304 (INV-9's second half, which no engine
test can reach), the project-relative `path`, and the source-scrape of the
schema, dispatch and provider wiring per the `mcp-tools.md` checklist.
Each test is verified to fail
against pre-implementation source before the implementation lands, per the
project test convention.

## 8. What checks this

| Invariant | Checked by |
|---|---|
| INV-1..7, INV-9 (etag stable across runs) | `tests/features/spec_conformance/` |
| INV-9 (a matching `etag_match` short-circuits) | `tests/features/spec_conformance_verb/` — the 304 is handler-local, so the engine lane cannot reach it |
| INV-8 | source-grep for the seven subprocess/interpreter tokens over `src/specconformance.{h,cpp}` only — the shared TUs already carry 34 such lines for other verbs |
| A pattern's engine matching the one its artefact will really run under | **nothing** — the fence tag is the author's claim, and no check ties it to the consuming code |
| A catastrophic pattern within the caps | **nothing** — § 2.8, no interruption exists |
| Whether an expectation row states the *right* expectation | **nothing** — a wrong expectation and a wrong pattern are indistinguishable from inside |

Three `nothing` rows. The third is inherent: this verb proves a pattern and
its stated example agree, never that either is what the author meant.

## 9. Cross-doc impact

- `CLAUDE.md` — MCP verb catalogue pointer; `tool_info {catalog:true}`
  picks the verb up automatically.
- `docs/standards/specs.md` — § 3.5 gains the fence-plus-table convention as
  the recommended way to state a pattern invariant.
- `CHANGELOG.md` — on ship.
- `src/speclint.cpp` — the comment delegating execution to `/write-spec`
  Step 3 is amended to point at this verb.

## 10. Cold-eyes loop log

| Loop | Lanes | Q1 | Q2 | Q3 | Q4 | Verified / dismissed | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2 cold, spec genre | 1 | 4 | 2 | 2 | 9 / 0 | all fixed |
| 2 | 2 cold, spec genre | 0 | 2 | 4 | 2 | 8 / 0 | all fixed |
| 3 | 2 cold, spec genre | 1 | 2 | 6 | 0 | 9 / 0 | all fixed; **cap reached, not converged** |
| 3-impl | none — implementation, no reviewer dispatched | 1 | 0 | 0 | 0 | 1 / 0 | § 2.4a added |
| 3-impl-2 | none — MCP wiring, no reviewer dispatched | 0 | 2 | 0 | 0 | 2 / 0 | § 2.3 deviations stated |

**Loop 1 (2026-08-12).** Both lanes independently found the same five
defects, which is what two rolls of a cold read are for.

- **[Q1]** § 2.4's worked example was itself the class this verb exists to
  catch. The fence pasted a pattern beginning `^\s*` while its expectation
  table was written for the column-0 form the example is meant to
  illustrate, so the row `` `  const local = ` `` → `no match` was false —
  verified by running it: `printf '  const local = ' | grep -oP '^\s*…'`
  matches and captures `local`. Fixed by restoring the column-0 anchor and
  re-running all three rows.
- **[Q2]** § 2.6's catch-all row refused every fence it did not recognise,
  including `cpp`/`jsonc` fences that every spec carries, and contradicted
  § 2.4's rule that a bare `regex` fence is a CANDIDATE. Scoped to
  `regex*` fences and given an explicit bare-`regex` row.
- **[Q2]** INV-9 demanded byte-identical envelopes while § 2.3 emits
  per-run timings — unsatisfiable by any correct implementation. Scoped to
  exclude `observations`, and the etag subset stated.
- **[Q2]** `truncated` was asserted by INV-7 but absent from the § 2.3
  envelope; added, along with the `refusals[]` array.
- **[Q2]** § 7 enumerated INV-8 into the feature test while § 8 and INV-8
  itself assigned it to a source-scrape. § 7 corrected.
- **[Q3]** Refusal granularity was undefined — one bad fence aborting the
  file, or skipping the case, were both buildable. Now stated: per case,
  with only a malformed request refusing the call.
- **[Q3]** The encoding for an empty capture was undefined while INV-3
  turns on exactly that distinction. Now stated.
- **[Q4]** INV-6 hashed the whole project root, which `build*/` and `.git/`
  churn during any test run — it would fail regardless of the verb. Scoped.
- **[Q4]** INV-8 claimed "no subprocess **and no interpreter**" while its
  test grepped one file for `QProcess` alone; `luaL_newstate`,
  `Py_Initialize`, `system(`, `popen` and `fork(` all passed it. Grep set
  and file set widened.

Author-side during the fix pass: the cap refusal was about to mint a new
`cap_exceeded` code, corrected to the taxonomy's existing `too_large`
(`mcp-error-codes.md`); a `§ 1.1` self-citation that resolved to nothing was
made explicit; and the "four defects" count was reconciled with the three
the bullet actually enumerates.

Lane spend: 99.4k and 96.5k input tokens against a ~60k/turn budget — over,
because the packet carries eight code windows and the document is long for
its genre.

**Loop 2 (2026-08-12).** Both lanes again converged, and **three of the
eight findings were collateral from loop 1's own fixes** — the pattern the
review procedure warns about, arriving on schedule.

- **[Q4]** INV-8's widened grep (a loop-1 fix) became *unreachable*: § 2.7
  names `src/mainwindow.cpp` and `src/claudeintegration.cpp`, which already
  carry 35 and 6 hits of the token set for unrelated verbs. Asserting zero
  where 41 exist is red for every possible implementation, so it falsified
  nothing. Scoped to the two files this change creates, and `\b`-anchored so
  `QProcessEnvironment` stops counting. *Loop-1 collateral.*
- **[Q2]** § 2.3 said `max_cases` is clamped; § 2.5 (a loop-1 fix) said an
  out-of-range value refuses the call. Both were buildable. Now: refuses.
  *Loop-1 collateral.*
- **[Q2]** § 2.2 claimed the three uncaught defects surface "as CANDIDATEs"
  while § 2.6 (a loop-1 fix) made non-`regex` fences invisible. § 2.2 now
  says plainly that this verb surfaces none of them. *Loop-1 collateral.*
- **[Q3]** Only one candidate `kind` was ever named while three causes
  produce candidates; all three enumerated, since the author's next action
  differs per kind.
- **[Q3]** An unsupported-engine fence with no table satisfied INV-4 and
  INV-5 at once. § 2.6 now fixes the order (engine before table) and INV-5
  asserts `candidates == []`.
- **[Q3]** `line` was emitted in three buckets and defined in none.
- **[Q3]** `expected` did not say which group a multi-group pattern yields,
  and the `no match` sentinel was indistinguishable from the literal after
  backtick stripping.
- **[Q4]** INV-3 named the empty-capture conflation as its break, but its
  test used a pattern that never captures emptily — the break was
  unreachable by its own fixture. Second row added.

A lane also challenged § 2.6's load-bearing premise (that Qt leaves
`UseUnicodePropertiesOption` off). Verified rather than defended: the
constructor takes `PatternOptions options = NoPatternOption` — citation
tightened to the three header lines that show it.

Lane spend: 108.5k and 105.1k input tokens. Document grew 394 → 479 lines
across the fix pass, which is the cost this review's consolidation rule
exists to watch.

**Loop 3 (2026-08-12) — the cap. Nine findings, all fixed, NOT converged.**

- **[Q1]** Both lanes caught the same thing, and it is loop 2's own: the
  INV-8 scope paragraph cited "35 and 6 hits … 41" while prescribing a
  `\b`-anchored regex. Under the regex it actually prescribes the figures
  are 33, 1 and 0 — 34 lines. The paragraph measured one regex and
  prescribed another, **and its own closing sentence about `\b` excluding
  `QProcessEnvironment` was the proof**. Corrected in both places.
- **[Q2]** § 2.2's loop-2 rewrite ("surfaces none of the others") left
  § 2.5 and INV-4 still crediting CANDIDATEs with "two of the four reported
  defects". Deleted rather than reconciled.
- **[Q2]** The `line` rule sent every candidate to the fence's line, but
  `malformed_expectation_cell` has a row to point at. Replaced with a table
  covering all five entry kinds.
- **[Q3]** A refusal was "per case", but INV-5's fixture is a fence with
  **zero** cases — so its own asserted `refusals[0]` had no case to attach
  to. Now per case, and per fence where the fence yields none.
- **[Q3]** Only one ordering rule existed (engine before table), leaving a
  `regex python` fence with a 600-byte pattern satisfying two codes at once.
  Full precedence stated: engine → pattern cap → table → input cap, at most
  one refusal per case.
- **[Q3]** A bare ` ```regex ` fence with no table satisfied two candidate
  kinds; now at most one candidate per fence, engine check winning.
- **[Q3]** `observations[]` had no cardinality and no `line` rule.
- **[Q3]** The empty-capture encoding conflated a *non-participating* group
  with a *zero-length* one. Qt distinguishes them, but `QString() ==
  QString("")` is **true**, so the spec now names
  `QRegularExpressionMatch::hasCaptured(1)` as the discriminator — an
  implementer reaching for `==` would have collapsed two of the three
  outcomes INV-3 exists to separate.
- **[Q3]** Fence **nesting** was never specified, and § 2.4's own worked
  example is a four-backtick `markdown` fence containing a `regex` fence:
  a naive scanner run on this very spec would extract three cases from an
  illustration. Only top-level fences are scanned.

**Why the cap bound, since that is evidence about this document.** Not
size: 30,930 bytes against a corpus median of 20,676 and a 90th percentile
of 55,794 (`ls -l docs/specs/*.md`), and the two specs that needed nine and
eleven loops were over a thousand lines. The cause is that **the response
envelope and extraction taxonomy were being designed inside the review**.
Every loop's fixes added surface — new fields, new `kind` values, new
ordering rules — and that new surface is what the next cold read found: 3
of loop 2's 8 findings and at least 4 of loop 3's 9 landed on text an
earlier loop had added. § 1, § 2.1, § 2.2, § 5 and § 6 — the problem, the
scope decision and the alternatives — produced no findings after loop 1 and
are settled.

**Recommendation, following ANTS-4070's precedent:** close the review and
build test-first. Every unresolved item is envelope detail that a fixture
pins in seconds and a cold read argues about for a loop — which is this
spec's own thesis (§ 1) turned on itself.

**Row `3-impl` (2026-08-12) — implementation, NO reviewer dispatched.**
The engine was built test-first and its ten fixtures pass. Building it
surfaced one [Q1] defect that three cold loops and six lanes could not,
because it needs a corpus rather than a reader: **nothing in this
repository uses the convention § 2.4 defines** — one `regex` fence exists
and it is § 2.4's own nested illustration — so the verb reports nothing
against today's corpus, and the reporting session's evidence was a Python
`re.search` CALL that this extractor does not recognise at all. § 2.4a now
states both, and ANTS-4128 carries the fix.

**A re-gate of that amendment is owed and deliberately not run.** Step 8
asks for one; the addition is a stated limitation rather than a changed
contract, the gate had already bound its cap on this document, and the
standing recommendation is to trust fixtures over further cold reads here.
Recorded as a decision so it is not mistaken for an oversight.

**Row `3-impl-2` (2026-08-12) — MCP wiring, NO reviewer dispatched.**
The verb is wired and its three verb-layer fixtures pass. Wiring it surfaced
two [Q2] contradictions between § 2.3 and the standard it claims conformance
with, and both have the same cause — `observations[]`, which no earlier loop
traced past the etag:

- § 2.3 claimed ETag-304 per `mcp-tools.md` step 7, which computes the etag
  over the **whole** response. INV-9 already knew the timings had to be
  excluded, and loop 1 scoped the etag for exactly that reason; what nobody
  followed through was that the central mechanism therefore **cannot** be the
  one used. Under it the 304 never fires, which makes INV-9 unsatisfiable —
  the same defect class loop 2 caught in INV-8.
- § 2.3 claimed `fields=` as well. Central projection is skipped on a central
  304 only, so a handler-local 304 would have `unchanged` projected out of it.
  Declined.

Both are now stated in § 2.3 with the reason, and the `isEtagSupportedTool`
absence — the thing that keeps the two mechanisms from being wired at once —
is asserted by a fixture rather than left to a comment.

**A re-gate of these amendments is owed and deliberately not run**, on the
same grounds as the row above: the additions state which of two documented
mechanisms this verb uses and why the other is unavailable, the gate had
already bound its cap on this document, and the fixtures now pin the choice.
Recorded as a decision so it is not mistaken for an oversight.
