# ANTS-4108 — run a spec's own patterns against the examples beside them

**Status:** spec draft (2026-08-12).
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
one of the reporter's four defects directly, and makes the other three
visible only as CANDIDATEs** (a prescribed artefact with no expectation
beside it, § 2.5). The fixture-execution half is § 6.

### 2.3 Inputs and envelope

```jsonc
// request
{ "path": "docs/specs/ANTS-4070-rotation-and-section-title.md",  // required
  "caller_cwd": "/abs/project/root",                             // required
  "max_cases": 200,        // optional, clamp [1, 1000], default 200
  "etag_match": "<etag>" } // optional, 304 short-circuit

// response
{ "ok": true,
  "path": "docs/specs/ANTS-4070-...md",
  "cases_run": 12,
  "findings":     [ { "kind": "mismatch", "line": 88, "pattern": "\\d{1,5}(?![0-9])",
                      "engine": "pcre2", "input": " 123456",
                      "expected": "no match", "actual": "23456" } ],
  "candidates":   [ { "kind": "pattern_without_expectation", "line": 143,
                      "pattern": "^#{1,6}\\s" } ],
  "refusals":     [ { "line": 210, "code": "unsupported_engine" } ],
  "observations": [ { "kind": "timing", "line": 88, "micros": 77 } ],
  "truncated": false,
  "etag": "<hex>" }
```

Conforms to [`docs/standards/mcp-tools.md`](../standards/mcp-tools.md) —
response-wrap, `caller_cwd` + `CallerCwdContract`, path validation under the
project root, ETag-304, `fields=`, and the refusal taxonomy in
[`mcp-error-codes.md`](../standards/mcp-error-codes.md). Read-only: the verb
never writes, so it has no state-routing surface.

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
- The expectation table is the **first** GFM table after the fence with
  exactly the columns `input` and `expected`, before the next fence or
  heading. Anything else is not an expectation table.
- `expected` is either `no match`, or the text of **capture group 1** when
  the pattern has one, else the **first** match under search semantics.
  Backticks around a cell are stripped so a literal can carry leading or
  trailing spaces visibly.
- **The empty result has an encoding, because INV-3 turns on it.** An empty
  backtick pair (`` `` ``) means *matched, captured the empty string*. A
  bare empty cell is malformed and yields a CANDIDATE, never a silent
  reading of either. Without this rule "matched but captured nothing" and
  "did not match" are indistinguishable in the table.

### 2.5 Result taxonomy

| Bucket | Meaning | Example |
|---|---|---|
| FINDING | a row ran and disagreed | `\d{1,5}(?![0-9])` on `" 123456"` yields `23456`, expected no match |
| CANDIDATE | a prescribed artefact nothing can check | a `regex` fence with no expectation table, or no declared engine |
| OBSERVATION | a measured fact, never a verdict | per-pattern match time in µs |

CANDIDATEs are the verb's answer to the defects it cannot execute: it
cannot tell whether a fixture is reachable, but it can say **nobody stated
what this pattern should do**, which is what produced two of the reported
defects.

**A refusal is PER CASE, never per call.** An unsupported engine or an
over-cap pattern adds a row to `refusals[]` and the run continues; the
other cases in the file still produce their findings. Only a malformed
*request* — an unreadable `path`, a `max_cases` outside its clamp — refuses
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

**A substitute engine is worse than no engine.** The two agree on the
reported case — verified, not assumed: `python3 -c "import re;
print(re.search(r'\d{1,5}(?![0-9])', ' 123456').group(0))"` and
`printf ' 123456' | grep -oP '\d{1,5}(?![0-9])'` both return `23456`. They
are still not the same engine, and the defaults differ where it matters:
Qt leaves `QRegularExpression::UseUnicodePropertiesOption` — a Qt header
symbol, not a project one (`/usr/include/qt6/QtCore/qregularexpression.h:42`
on this host) — **off** by default, so `\d` is ASCII-only, while
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
| `src/mainwindow.cpp` | provider lambda |

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
  *Test:* same fixture, one row expecting `no match` against a pattern that
  matches, asserted as a finding. *Breaks when:* an empty capture group is
  conflated with no match — a pattern that matches but captures nothing then
  reads as a refusal.
- **INV-4** — a fence with a declared engine and NO expectation table is a
  CANDIDATE, never silently dropped and never a finding. *Test:* fixture
  with a bare `regex pcre2` fence asserts one candidate, zero findings.
  *Breaks when:* extraction requires a table to emit anything, which makes
  an unchecked pattern indistinguishable from an absent one — the state that
  produced two of the four reported defects.
- **INV-5** — an unrecognised engine tag is refused `unsupported_engine`
  and no pattern is run under a substitute; the refusal is per case.
  *Test:* two fixtures. One containing **only** a `regex python` fence
  asserts `refusals[0].code == "unsupported_engine"` and `cases_run == 0`.
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
- **INV-8** — this verb runs no subprocess and loads no interpreter, so
  `spec_lint`'s no-execution contract stays true of the pair. *Test:*
  source-grep across **every file § 2.7 names** for
  `QProcess|system\(|popen|fork\(|lua_newstate|luaL_newstate|Py_Initialize`,
  asserting zero hits. *Breaks when:* Python support (§ 6) is added here
  instead of behind its own decision. A grep for `QProcess` alone on a
  single TU was the first draft's test and could not falsify the
  interpreter half of its own claim — every one of the other six tokens
  passed it.
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
  second and third of the three listed in § 1 — deferred, tracked by
  **ANTS-4127**. Needs a decision on
  an interpreter and a real sandbox; `luaengine.cpp`'s watchdog is the
  in-repo precedent to start from.
- **Python `re` as an engine** — deferred, same id. Requires an
  out-of-process runner; until it exists, INV-5 refuses rather than
  approximates.
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
Source-scrape also covers the schema, dispatch and provider wiring per the
`mcp-tools.md` checklist. Each test is verified to fail
against pre-implementation source before the implementation lands, per the
project test convention.

## 8. What checks this

| Invariant | Checked by |
|---|---|
| INV-1..7, INV-9 | `tests/features/spec_conformance/` |
| INV-8 | source-grep for the seven subprocess/interpreter tokens across every file § 2.7 names |
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
