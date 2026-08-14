# ANTS-3368 — `co_change_family`: the grouped edit-site checklist for a settings-backed field

**Status:** accepted (2026-08-14), review-contract loops 1 + 2 folded, cap reached.
**Kind:** feature.
**Source:** ROADMAP.md ANTS-3368 (cc-feedback-2026-06-30, Vestige Sug-A, 6 consecutive slices).
**Pairs with:** ANTS-2156 (`similar_code include_bodies` — the exemplar's *body*; this verb is the exemplar's *edit sites*).

## 1. Problem

Adding a settings-backed field is a lockstep fan-out across ~10 files, and
the agent's job is to mirror an existing field rather than invent one. The
reporter (Vestige) hit this six consecutive times and named the cost: the
existing verbs find the stem and miss everything derived from it.

That is not a tuning gap, it is the matching rule — and the two closest verbs
fail differently, which is why neither can be extended into this one.
`SymbolQuery::findCaller()` (`src/symbolquery.cpp`) anchors on
`"\\b" + s + "\\s*\\("` — a word boundary *and* a required following paren —
so it sees call sites only, and `_` being a word character means it cannot
match inside `m_claudeMcpEnabled` at all.

Worked from this repo's own `claude.mcp_enabled` family. Under `src/`, the
family spans **11** files and a whole-word search for `claudeMcpEnabled`
returns **6** of them:

```
rg -il 'claude[_.-]?mcp|mcp[_.-]?enabled' src/ | wc -l   # 11
rg -w  -l 'claudeMcpEnabled'              src/ | wc -l   # 6
```

The 5 files it misses are `claudeintegration.cpp`, `mainwindow.h`,
`mcpprojection.h`, `remotecontrol.h` and `settingsdialog.h` — the last of
them holding the editor widget's member declaration.

**File-level presence hides site-level absence, which is the sharper half.**
`src/claudeintegration.h` is in the found set, but only because a *comment*
says `Config::claudeMcpEnabled()`; the two things an author actually has to
copy in that file — the apply sink `setMcpEnabled()` and its mirror member
`m_mcpEnabled` — match nothing. A file list cannot express that, which is why
this verb returns sites. Site by site:

| Site | Form | Whole-word `claudeMcpEnabled` finds it? |
|---|---|---|
| `Config::claudeMcpEnabled()` (`src/config.h`) | bare | yes |
| `Config::setClaudeMcpEnabled()` (`src/config.h`) | prefix `set` | no |
| `"claude.mcp_enabled"` (`src/config.cpp`) | string literal, snake | no |
| `m_claudeMcpEnabled` (`src/settingsdialog.h`) | prefix `m_` | no |
| `ClaudeIntegration::setMcpEnabled()` (`src/claudeintegration.h`) | truncated + prefix | no |
| `m_mcpEnabled` (`src/claudeintegration.h`) | truncated + prefix | no |

Three consequences, in the order they cost time:

1. **The JSON string key is invisible to a symbol search.** `"claude.mcp_enabled"`
   is not an identifier; it is text inside a quote, in a different casing
   convention from every C++ name in its own family.
2. **Derived names are affixed, so whole-word matching is exactly wrong.**
   `setX`, `m_X`, `XChanged` all *contain* the stem and none *is* the stem.
3. **A partial stem still names the same field.** The apply sink here is
   `setMcpEnabled`, not `setClaudeMcpEnabled` — the family is held together
   by a shared run of words, not by a shared full name.

`FindSources::findSources()` (`src/findsources.cpp`) is the closer of the two,
and its limits are not the whole-word one. It lowercases the file's bytes and
runs `QByteArray::indexOf` for each case variant of the token, so it matches on
**substring** and does reach `setClaudeMcpEnabled`. Three things stop it being
this verb, and only the third is a tuning matter:

1. **It returns ranked files, not sites** — `FileHit{path, score, role,
   evidence}`, where `role` is `impl`/`header`/`test` derived from the path. An
   edit checklist needs the line.
2. **It walks the declared source/test roots only** (`src/` + `tests/` by
   default), so a config key's `docs/` and `CLAUDE.md` mentions are outside it
   by construction.
3. **Its variant set is separator-blind.** `variantsForToken()` yields
   `claudemcpenabled` and `claude_mcp_enabled` — neither a substring of
   `"claude.mcp_enabled"`. Consequence 1 defeats it too, for its own reason.

**Layman:** When you add a setting by copying an existing one, this lists
every place you have to touch — including the easy-to-miss text key in the
config file and the helper names the compiler makes up from it.

## 2. Surface

### 2.1 Scope decision — a new verb, reusing the existing scanner

ANTS-2156 resolved its ask by extending `similar_code` rather than adding a
verb, and that precedent was tested here first. It does not carry:
`find_sources` takes free text and returns scored files; this takes one
identifier and returns grouped line-level sites with a per-site role. The
input, the matching rule and the response shape all differ, so a mode flag
would be a second verb sharing a name.

What *is* reused, rather than rebuilt:

- **The ripgrep plumbing** — `rcRunRg()` in `src/remotecontrol_workspace.cpp`
  (argv, working dir, millisecond budget, `rg_failed`). It is file-local to an
  anonymous namespace, so the handler is appended to that same TU, whose stated
  subject is "Workspace and code index verbs".

  **This is a stated divergence from `mcp-tools.md`'s TU rule, not an oversight
  of it.** That rule's cheap position is a *new* handler TU appended last; here
  the handler joins TU 6 of 12. What the rule exists to prevent is a
  test-driven seam sharing a TU with its handler, which drags `RemoteControl`
  → `MainWindow` into everything that links it and broke `test_core` when it
  was measured (ANTS-3855). That requirement is met in full by
  `src/cochangefamily.{h,cpp}` being its own TU under `ants_core_lib` (INV-10);
  the new-handler-TU half is not load-bearing once it is. Joining an existing
  TU is also strictly cheaper: a thirteenth TU would renumber every `TU N/M`
  head marker in the sibling `remotecontrol*.cpp` files, which
  `RcTuSplit.TuOrdinalMarkersAscend` asserts. **An implementer must not create
  `src/remotecontrol_cochangefamily.cpp`** — that is the diff this paragraph
  exists to prevent.
- **The refusal taxonomy** — `caller_cwd_required`, `bad_args`, `no_project`,
  `rg_failed`, all already in `mcp-error-codes.md`. No new code is minted, and `bad_path`
  is absent because the verb takes **no path-typed argument**: `stem`,
  `stems`, `min_run` and `max_sites` are the whole surface, so
  `mcp-tools.md` step 4 has nothing to validate. The scan root comes from
  `caller_cwd` through `ants::resolveCallerCwdRoot`, as step 3 requires.

### 2.2 The matching rule

A **stem** is one exemplar name: an identifier (`claudeMcpEnabled`) or a
config key (`claude.mcp_enabled`). Both reduce to the same word sequence.

```
splitWords("claude.mcp_enabled")  -> [claude, mcp, enabled]
splitWords("claudeMcpEnabled")    -> [claude, mcp, enabled]
splitWords("m_claudeMcpEnabled")  -> [m, claude, mcp, enabled]
splitWords("setMcpEnabled")       -> [set, mcp, enabled]
splitWords("MCP_ENABLED")         -> [mcp, enabled]
```

A candidate is a **site** when the longest run of stem words it shares is at
least `min_run` long, where the run must be **contiguous in both sequences** —
consecutive in the stem's words *and* consecutive in the candidate's. The run
is what holds a family together, and it is what survives an affix:
`setMcpEnabled` shares the run `[mcp, enabled]` with the stem even though it
shares no whole name with it.

**Both halves of "contiguous" are load-bearing.** Under the looser reading —
consecutive in the stem, merely present in order in the candidate —
`mcpTraceEnabled` would score `[mcp, enabled]` and become a site at the
default `min_run`. It would also be unreachable, because the scan pattern
below never matches it: the filter would be looser than the search, and the
sites it admitted could never be found.

### 2.2.1 From an `rg` match to a candidate

The scan pattern finds *text*; the filter needs a *name*. The widening step
between them decides `name`, `run`, `run_len` and `role`, so it is pinned
rather than left to the implementer:

- **Inside a string literal**, the candidate is the literal's contents.
  `"claude.mcp_enabled"` matched at `claude.mcp` widens to the whole key, which
  `splitWords` reads as `[claude, mcp, enabled]` — run 3, `role: json_key`.
- **Outside one**, the candidate is the maximal surrounding `[A-Za-z0-9_]`
  token. `m_claudeMcpEnabled` matched at `claudeMcp` widens to the whole
  identifier — `[m, claude, mcp, enabled]`, run 3, `role: member`.

Without this the same match yields a different `name` and a different `role`
per implementer, and the two examples in § 2.5 are not reproducible.

**The pattern is derived from `min_run`, because the two have to agree.** At
`min_run >= 2` it is the case-insensitive alternation of the stem's adjacent
word **pairs**, joined by an optional single separator. At `min_run == 1` each
single stem word is alternated as well. A pairs-only pattern cannot produce a
one-word match, so a scan that ignored `min_run` would make `min_run: 1`
return byte-identical results to the default — and the wide net the reporter
needs for `audioLod` (one word shared with `lodEnabled`) would be unreachable
at every setting.

A single-word alternation is the expensive one, which is why it is opt-in: for
a stem whose words are common in the tree it approaches a full-text search for
that word, bounded only by `max_sites`.

### 2.2.2 Scan scope

**The scan is rooted at `caller_cwd` and covers the whole repository, minus
whatever `.gitignore` excludes** — `rg`'s own default. It does **not** consult
`.ants/project.json`'s `source_roots` / `test_roots`.

That is a deliberate divergence from `find_sources`, and it is the decision
this verb turns on: a config key's `docs/`, `CLAUDE.md` and spec mentions are
co-change sites, and the declared source roots exclude every one of them. The
cost is real and § 7 states it — a repo-wide scan for `claudeMcpEnabled`
matches 74 files here against 11 under `src/`, most of the difference being
prose. `max_sites` and INV-7 are what keep that bounded.

### 2.3 Roles

`role` is assigned from the **lexical shape of the site**, in this precedence
order, first match wins:

| `role` | Condition | Example |
|---|---|---|
| `json_key` | the match lies inside a string literal | `"claude.mcp_enabled"` |
| `member` | the identifier begins `m_` | `m_claudeMcpEnabled` |
| `mutator` | the identifier begins `set` (before the run) | `setClaudeMcpEnabled` |
| `signal` | the identifier ends `Changed` | `mcpEnabledChanged` |
| `type` | the identifier is PascalCase and the line opens `struct`/`class`/`enum` | `struct McpEnabledState` |
| `reference` | anything else | `if (!m_config.claudeMcpEnabled())` |

**The role is lexical, never semantic**, and the vocabulary above is the whole
of it. The roadmap bullet named "apply sink" and "editor widget" — those are
not recoverable from a token's shape (an apply sink is a mutator on a class
that happens not to be the settings store), and guessing them needs type
resolution this scanner does not have. A caller reads the *path* for that:
`src/settingsdialog.cpp` is the editor, `src/claudeintegration.h` is the sink.
Emitting a confident wrong `apply_sink` is worse than emitting `mutator` and
a path.

### 2.4 Request

```json
{ "caller_cwd": "/abs/project",
  "stems": ["claudeMcpEnabled", "mcpEnabled"],
  "min_run": 2,
  "max_sites": 200,
  "etag_match": "…",
  "fields": ["files"] }
```

| Arg | Type | Default | Meaning |
|---|---|---|---|
| `caller_cwd` | string | — | **Required** contract (step 2 of `mcp-tools.md`). |
| `stem` | string | — | One exemplar name. Sugar for a one-element `stems`. |
| `stems` | string[] | — | The group; the union of each stem's families. |
| `min_run` | int | `min(2, stem_words)` | Shortest accepted run. Applied **per stem** against that stem's own word count, and clamped to `1 … stem_words` per stem. |
| `max_sites` | int | 200 | Site cap; clamped to `1 … 1000`, never refused. |
| `etag_match` | string | — | `makeEtagMatchProp()`; a match short-circuits to `{ok, unchanged, etag}`. |
| `fields` | string[] | — | `makeFieldsProp()`; narrows to named top-level fields. |

`inputSchema.required` is `["caller_cwd"]` alone — "one of `stem`/`stems`" is
not expressible in JSON Schema `required[]`, so it is a runtime check
refusing `bad_args`. **`stems` wins when both are sent**, rather than merging;
a caller sending both has made a mistake, and picking the richer argument
makes that mistake visible in the echoed `stems` rather than silently
doubling the family.

### 2.5 Response

**One shape, whatever was asked.** The per-stem fields are always keyed by
stem and the per-site `stem` is always present, so a one-stem call and a
six-stem call parse identically and no caller branches on which form it sent.

```json
{ "ok": true,
  "stems": ["claudeMcpEnabled"],
  "stem_words": { "claudeMcpEnabled": ["claude", "mcp", "enabled"] },
  "min_run":    { "claudeMcpEnabled": 2 },
  "files": [
    { "path": "src/config.cpp",
      "sites": [
        { "line": 602, "stem": "claudeMcpEnabled",
          "name": "claude.mcp_enabled", "role": "json_key",
          "run": ["claude", "mcp", "enabled"], "run_len": 3,
          "text": "    return m_data.value(\"claude.mcp_enabled\").toBool(true);" } ] } ],
  "files_count": 11,
  "sites_count": 41,
  "truncated": false }
```

**One row per source line, never one per stem.** Stem families overlap — § 2.4's
own example sends `claudeMcpEnabled` and `mcpEnabled`, whose families are
nearly identical — so a site matching several stems is emitted **once**, owned
by the stem with the longest run, ties broken by position in `stems[]`. Emitting
per stem would double `sites_count`, repeat every line in `files[].sites[]`, and
make INV-6's ordering depend on which duplicate was read first.

**`etag` is absent on purpose.** The dispatcher injects it
(`applyEtagPattern`); `mcp-tools.md` step 7 says the handler must not emit it,
so it appears on the wire and never in `cmdCoChangeFamily`'s return.

Totals above are illustrative — the example elides every file but one. What
makes the real figure vary is § 7's second open question: a repo-wide scan for
this stem matches 74 files, most of them prose under `docs/specs/`.

### 2.6 Files

| File | Change |
|---|---|
| `src/cochangefamily.h` / `.cpp` | **New.** The pure seam: `splitWords`, `widenToCandidate` (§ 2.2.1), `longestRun`, `scanPattern`, `classifyRole`. Own TU under `ants_core_lib` SOURCES, beside `src/findsources.cpp`. No Qt Widgets, no `RemoteControl`, no `MainWindow`. |
| `src/remotecontrol_workspace.cpp` | `RemoteControl::cmdCoChangeFamily()` appended; calls the seam + `rcRunRg()`. |
| `src/remotecontrol.h` | Handler declaration. |
| `src/mainwindow.cpp` | `registerToolProvider("co_change_family", CallerCwdContract::Required, …)`. |
| `src/claudeintegration.cpp` | `tools/list` schema (incl. `makeEtagMatchProp()` + `makeFieldsProp()` — the entry alone leaves the arg undeclared, so the dispatcher drops it into `ignored_args` and the 304 is silently unreachable); a `detail` sibling, since the wire `description` would otherwise blow `mcp-tools.md` step 11's ~800 B budget; `selection_hint`; prefix-tag bucket; `callerCwdContractFor`; `isEtagSupportedTool`. **No `encoding` prop**: the columnar repack is for a top-level array of flat objects, and `files[]` carries a nested `sites[]` per row. |
| `src/mcpprojection.cpp` | `isFieldProjectionTool` — the `fields=` opt-in lives here, not in `claudeintegration.cpp`. |
| `CMakeLists.txt` | `src/cochangefamily.cpp` into `ants_core_lib`; the feature test into `test_claude`. |

## 3. Invariants

- **INV-1** — `splitWords()` splits on `_`, `-` and `.`, and on every
  lower→upper boundary; lowercases each part; drops empty parts. The five
  forms in § 2.2 produce exactly the sequences shown there. *Test:*
  `tests/features/co_change_family/` case `SplitWordsFormsAgree`.
- **INV-2** — at `min_run >= 2` the scan pattern is the case-insensitive
  alternation of the stem's adjacent word pairs joined by an optional single
  `_`, `.` or `-`. At `min_run == 1`, and for a one-word stem, each single
  stem word is alternated as well — so `min_run: 1` widens the scan and not
  merely the filter. *Test:* the pattern block below, run by
  `spec_conformance`, plus case `ScanPatternWidensWithMinRun`.

```regex pcre2
(?i)(?:claude[_.\-]?mcp|mcp[_.\-]?enabled)
```

| input | expected |
|---|---|
| `bool m_claudeMcpEnabled` | `claudeMcp` |
| `"claude.mcp_enabled"` | `claude.mcp` |
| `void setMcpEnabled(bool)` | `McpEnabled` |
| `MCP_ENABLED` | `MCP_ENABLED` |
| `mcpTrace` | no match |

- **INV-3** — a candidate becomes a site only when its longest run of stem
  words, **contiguous in the stem's sequence and in the candidate's alike**,
  is `>= min_run`. `min_run` is resolved **per stem** against
  that stem's own word count: it defaults to `min(2, stem_words)` and is
  clamped to `1 … stem_words`. A value outside that range is clamped, never
  refused, so a group mixing a one-word and a three-word stem resolves to a
  different effective `min_run` for each and the response echoes both. *Test:*
  case `MinRunIsPerStemAndClamps`.
- **INV-4** — a run whose every word is a stopword is dropped whatever its
  length. The list is frozen here: `get`, `set`, `is`, `has`, `on`, `off`,
  `enabled`, `disabled`, `value`, `data`, `flag`, `count`, `size`, `index`,
  `name`, `type`, `mode`, `m`, `p`. *Test:* case `StopwordOnlyRunsDropped`.
- **INV-5** — every emitted `role` is one of the six values in § 2.3, assigned
  in that precedence order. No seventh value is ever emitted. *Test:* case
  `RoleVocabularyIsClosed`, asserting against the enumerated set.
- **INV-6** — one site row per `(path, line)`, whatever the stem count: a line
  matching several stems is emitted once, owned by the stem with the longest
  run, ties broken by position in `stems[]`. `files[]` is ordered by each
  file's maximum `run_len` descending, ties broken by `path` ascending;
  `sites[]` within a file is ordered by `line` ascending. *Test:* case
  `OrderingIsDeterministic`, run with two overlapping stems so the dedup and
  the ordering are asserted together.
- **INV-7** — `truncated: true` accompanies every partial answer, and no site
  is dropped without it. Three things make an answer partial and all three set
  it: the site count exceeding `max_sites` (itself clamped to `1 … 1000`,
  never refused); `rg` exhausting its millisecond budget, in which case the
  sites already parsed are returned rather than discarded; and `rg` being
  hard-killed after that budget. **`rg_failed` is reserved for a scanner that
  did not run** — `startFailed` or a non-zero exit that is not "no matches" —
  so a caller can distinguish "incomplete" from "no answer". **When the cap
  binds, the sites retained are those with the highest `run_len`, ties by
  `path` then `line`** — never the first N in scan order. Which sites you get
  is the verb's entire product, so a cap that kept walk order would drop a
  header's exact-run site in favour of a doc's weak one. *Test:* case
  `PartialAnswersAreFlagged`, asserting both the flag and the retained set.
- **INV-8** — *withdrawn — 2026-08-14, the count it defined was uncomputable.*
  It promised a `weak_matches_available` field counting candidates rejected by
  `min_run` alone. The pairs-only scan pattern never produces a one-word
  candidate, so at the default `min_run` there was nothing to count, and the
  `min_run: 1` re-run it advertised would have returned identical results.
  INV-2 widens the scan with `min_run` instead. Number retained, never
  reflowed: `specs.md` § 5.5 makes invariant ids permanent, so the gap in this
  document's sequence is correct.
- **INV-9** — refusals, all with existing codes: an empty `caller_cwd` →
  `caller_cwd_required` (at the dispatcher, before the handler runs); a
  non-empty `caller_cwd` that does not canonicalise to a directory →
  `no_project`; a `stem` not matching `^[A-Za-z0-9_.\-]+$`, or
  yielding zero words after `splitWords`, or **all of whose own words are
  INV-4 stopwords** → `bad_args`. The last case is a refusal rather than an
  empty result because every run it could form would be dropped by INV-4, and
  a silent `sites_count: 0` reads as "this field is touched nowhere" — a
  confident wrong answer. *Test:* case `RefusalCodes`.
- **INV-10** — `src/cochangefamily.cpp` references no symbol from
  `RemoteControl`, `MainWindow` or `ClaudeIntegration`. *Test:* case
  `SeamTuHasNoChromeSymbols`, a source-grep over the TU. The `test_core` link
  is deliberately **not** claimed as a second surface: § 2.6 wires the feature
  test into `test_claude` only, so no `test_core` object references the seam
  and that link would pass whatever the TU contained.
- **INV-11** — the verb appears in `tools/list` with
  `inputSchema.type == "object"` and `additionalProperties == false`, is
  registered with `CallerCwdContract::Required` at both the registration site
  and `callerCwdContractFor`, and is listed in `isEtagSupportedTool` and
  `isFieldProjectionTool`. *Test:* case `RegistrationAndSchema`.
- **INV-12** — every word derived from a stem is regex-quoted
  (`QRegularExpression::escape`) before assembly into the alternation, so no
  stem can inject pattern syntax into the `rg` argv. This is the trust
  boundary the verb crosses (`specs.md` § 5.4): caller-supplied text — on this
  surface, LLM output arriving over IPC — reaching a subprocess that reads the
  filesystem. **Under INV-9 this is defence in depth, not the primary gate**,
  and the honest statement of why it is still an invariant is that it makes
  the guarantee independent of INV-9: the charset admits `.` and `-`, but
  `splitWords` consumes both as separators, so a word reaching the alternation
  is alphanumeric and carries no metacharacter today. A later loosening of the
  charset must not silently become an injection. *Test:* case
  `StemCannotInjectPattern`, driving the assembler directly with a word
  containing `.*` and asserting the emitted pattern matches that literal
  sequence.
- **INV-13** — an `rg` match is widened to a candidate by § 2.2.1's rule: to
  the string literal's contents when the match lies inside one, otherwise to
  the maximal surrounding `[A-Za-z0-9_]` token. The widened candidate is what
  `name` reports and what `splitWords` reads, so `run`, `run_len` and `role`
  all derive from it and not from the raw match span. *Test:* case
  `MatchWidensToCandidate`, asserting `"claude.mcp_enabled"` matched at
  `claude.mcp` yields `name: "claude.mcp_enabled"`, `run_len: 3`,
  `role: json_key`, and `m_claudeMcpEnabled` matched at `claudeMcp` yields
  `name: "m_claudeMcpEnabled"`, `run_len: 3`, `role: member`.
- **INV-14** — the scan is rooted at the `caller_cwd` root and covers the whole
  repository subject to `.gitignore`; `.ants/project.json`'s `source_roots` and
  `test_roots` are **not** consulted, so a site in `docs/` or `CLAUDE.md` is
  returned like any other. *Test:* case `ScanIgnoresDeclaredSourceRoots`,
  against a fixture whose `project.json` declares a root that excludes a file
  the stem appears in, asserting the site is still returned.

## 4. RAM / build cost

No new build target and no new external library. `src/cochangefamily.cpp`
joins the existing `ants_core_lib`; the handler joins an existing RC TU, so
the RC TU count and its ordinal markers are unchanged.

Memory is bounded by the response cap, not by the tree, and INV-7's retention
rule is what makes the two compatible. `rg` streams; the handler keeps a
bounded min-heap of `max_sites` records keyed by `run_len` (ties `path`, then
`line`), evicting the weakest as it parses. So the highest-`run_len` sites
survive the cap **without** holding every candidate — the naive reading, "sort
at the end", would need the whole repo-wide candidate set resident and is what
this sentence exists to rule out.

Each record carries one line of matched text clipped to 512 bytes — the same
clip `workspace_search` applies (`kDefaultMaxMatchBytes`). Worst case ≈ 1000 ×
~600 B ≈ 600 KB, transient, freed with the response. No cache, no
process-lifetime state, so nothing to evict.

## 5. Out of scope

- **Semantic roles ("apply sink", "editor widget").** Permanent exclusion, not
  deferred — § 2.3 gives the reason. No follow-up id.
- **Suggesting the new field's names.** Permanent exclusion, no id: the verb
  reports where the *exemplar* is touched, and writing the mirror is the
  agent's job. A generator would have to know the target's type and defaults,
  which is a different verb and not one this spec is deferring.
- **Cross-repo families.** Permanent exclusion: the scan is rooted at one
  `caller_cwd`, as every project-scoped verb is.
- **Frequency-ranked weak matches** — ranking a one-word run by how common
  that word is in the tree, so a distinctive `lod` outranks a ubiquitous
  `claude`. Permanent exclusion for this spec, not deferred, so it carries no
  id: `min_run: 1` already reaches those sites (INV-2), and whether the
  *ordering* is worth a frequency pass is a question only real use answers.
  § 7's first open question is what would reopen it, and reopening it means a
  new item, not a promise made here.

## 6. Tests

Feature test: `tests/features/co_change_family/`, compiled into the
**`test_claude`** bundle (beside `tests/features/mcp_find_sources/`; it is not
a standalone target). Label `features;fast`. One case per live invariant, as
named in § 3: **INV-1 … INV-7 and INV-9 … INV-14, thirteen cases.** INV-8 is
withdrawn and has no case.

Two project conventions apply and are not optional here:

- **Verify each case fails against pre-fix source first** — for a new verb
  that means stubbing the seam to return empty and watching the assertions
  (not the compile) fail.
- **Build `test_claude` specifically and check `ctest -N -R co_change` moved.**
  Building the wrong target succeeds silently and runs the old binary, so a
  green suite would read as success with the new test never compiled.

The `spec_conformance` verb runs INV-2's pattern block against its table
directly out of this document. That block is the `min_run >= 2` form; the C++
case `ScanPatternWidensWithMinRun` is what covers the other half, asserting
the generator emits that exact pattern at `min_run: 2` and a strictly wider
alternation at `min_run: 1`.

## 7. Open questions

- **Is `min(2, stem_words)` the right default?** It is tight: it returns the
  reporter's `setLodEnabled` and `"lod_enabled"` and drops their `audioLod`,
  which `min_run: 1` reaches at the cost of a much wider scan (INV-2). The
  spec cannot settle this, because the answer is a frequency property of each
  caller's tree. **What makes it answerable in use rather than in review:** a
  caller who re-runs at `min_run: 1` and finds the extra sites useful has
  shown the default wrong. If that becomes the habit, the default flips and
  frequency ranking (§ 5) is what makes the wide net readable.
- **Is the repo-wide scope affordable?** It is decided, not open — § 2.2.2 and
  INV-14 settle it, and the reason is that the `docs/` sites are the ones the
  verb exists to surface. What is open is the cost: 74 matched files against
  11 under `src/`, and a repo-wide `rg` over this tree took over two minutes
  once during drafting on a warm cache, which is far outside a verb's budget.
  INV-7's cap bounds the *response*, not the scan. If the scan itself proves
  too slow, the answer is a scope argument defaulting to repo-wide — not a
  reversal of the default, which would put the verb back where `find_sources`
  already is.

## 8. Cross-doc impact

| Doc | Change |
|---|---|
| `docs/standards/mcp-behavioural-notes.md` | Per-verb note: the matching rule, the role vocabulary, and that `min_run` widens the scan rather than only the filter. |
| `CHANGELOG.md` | Entry under Added on the shipping release. |
| `ROADMAP.md` | ANTS-3368 flipped on ship, with the resolution line. |
| `Ants_Terminal_Ants_MCP_Feedback.md` | The Vestige finding's `**Proposed ID:**` slot closes when it ships. |
| `mcp-error-codes.md` | **No change** — every refusal reuses an existing code. |

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-14 | 2, cold; genre pinned `spec`; one byte-stable shared packet carrying the `findsources` / `symbolquery` / `rcRunRg` windows, the `claude.mcp_enabled` family as it exists, and the `specs.md` / `mcp-tools.md` excerpts | **Q1 2 · Q2 3 · Q3 5 · Q4 1** (11 verified / 1 dismissed) | **This document's first gate; all 11 fixed, no deferred tail.** **[Q2] The escape hatch the design rested on was inert, and one lane found it alone.** § 2.5 promised `weak_matches_available` so a caller could tell the tight default was lossy and re-run at `min_run: 1` — but INV-2's scan pattern was the alternation of adjacent word *pairs*, which cannot return a one-word match, so every candidate already had `run_len >= 2`, the count was permanently 0, and `min_run: 1` would have returned byte-identical results. The reporter's own `audioLod` case was unreachable at every setting. Fixed by deleting the field (INV-8 withdrawn) and making INV-2 derive the pattern *from* `min_run`, so the argument widens the scan rather than only the filter. **[Q1] The § 1 premise was false about one of the two verbs it indicted.** It said `findCaller()` and `findSources()` "both match an identifier as a **whole word**"; `findSources` lowercases the file's bytes and runs `QByteArray::indexOf` per case variant, so it matches on substring and *does* reach `setClaudeMcpEnabled`. Surfaced as a lane's open question, confirmed by the orchestrator against `findsources.cpp`. § 1 now states each verb's real limit separately — `findCaller` anchors on `\b<sym>\s*\(` and cannot see a member reference at all; `findSources` is file-granular, walks `src/`+`tests/` only, and its variants are separator-blind to the dotted key. **[Q3] The verb crossed a trust boundary with no invariant, which `specs.md` § 5.4 requires** — a caller-supplied stem was interpolated into an `rg` pattern with no charset check and no escaping, so `a.*b` would have surfaced as `rg_failed` rather than `bad_args`. Now INV-9 (charset) + INV-12 (escape). **[Q3] Multi-stem mode invented its own wire shape:** the arg table said each site carries the `stem` that matched it, the response example had no such key and singular `stem`/`min_run`, and nothing said which wins when both args are sent. Now one shape always — per-stem maps, per-site `stem`, `stems` wins. **[Q2] `etag_match`/`fields` appeared in the request example but in neither the arg table nor § 2.6's prop factories** (`additionalProperties == false` would have rejected the document's own example). **[Q1] `etag` was shown as a handler-returned field**, which `mcp-tools.md` step 7 forbids. **[Q4] INV-10 claimed "the `test_core` link itself" as a test surface** although § 2.6 wires the test into `test_claude` only, so nothing pulls the seam's object into that link and the clause could never fail; the grep is now the whole surface, and the doc says why. Plus three underspecified response behaviours both lanes raised as open questions and the orchestrator promoted (**[Q3]** ×3): what a timed-out scan returns (now `truncated`, with `rg_failed` reserved for a scanner that did not run), whether an over-ceiling `max_sites` clamps or refuses (clamps), and that an all-stopword stem such as `isEnabled` would have returned a silent empty result (now `bad_args`). **Dismissed as immaterial:** an illustrative `files_count: 12` contradicting § 1's measured 11 — true, but it changes nothing built; the block was rewritten for the shape fixes anyway and the totals are now marked illustrative against a measured repo-wide 74. **Collateral, caught by 4c:** the INV-8 tombstone was first written `*withdrawn. …*`, which `speclint.cpp`'s literal `^\*withdrawn — (.+?)\*` does not match, so the withdrawal re-fired `invariant_no_test`; rewritten to the corpus form. **Resolved, not findings:** § 4's "the same clip `workspace_search` applies" is exact (`kDefaultMaxMatchBytes` = 512), and `mcp-tools.md` step 4 has nothing to validate here because the verb takes no path-typed argument — stated in § 2.1 so the next reader does not re-ask. |
| 2 | 2026-08-14 | 2, cold; identical packet rebuilt from disk after loop 1's edits, with the `findSources` substring fact and `speclint.cpp`'s tombstone anchor added as verified source facts | **Q2 2 · Q3 4** (6 verified / 0 unverified) | **Cap reached (2 for a spec); all six fixed, no deferred tail.** The loop found no false claim — every Q1 in the document now holds — and instead found four places where a capable implementer would have had to invent something the wire binds to. **[Q3] The step from an `rg` match to a candidate was never specified**, and `name`, `run`, `run_len` and `role` all derive from it: INV-2's own table says the match for `"claude.mcp_enabled"` is `claude.mcp`, while § 2.5 emits `name: "claude.mcp_enabled"` and `run_len: 3`, with nothing in between. New § 2.2.1 + INV-13 pin the widening (string-literal contents inside a literal, maximal `[A-Za-z0-9_]` token outside one), which is also the first thing making § 2.5's two examples reproducible. **[Q3] "Contiguous run" was unanchored** — contiguous in the stem, in the candidate, or both. The loose reading makes `mcpTraceEnabled` a site at the default `min_run` *and* unreachable, because the pairs pattern never matches it: the filter would have been looser than the search. Now "contiguous in both sequences", mirrored in INV-3. **[Q3] The repo-wide scan scope was decided only inside § 7 Open questions**, with § 2, § 2.6 and every invariant silent — so an implementer reading the Surface alongside § 2.1's `find_sources` comparison would have scoped to the declared source roots and lost the `docs/` sites the verb exists to surface. Moved to § 2.2.2 + INV-14; § 7 now carries the cost rather than the fork. **[Q3] A line matching two stems had no stated arity**, and § 2.4's own example sends two stems with near-identical families — one builder doubles `sites_count`, another picks an arbitrary owner. INV-6 now pins one row per `(path, line)`, owned by the longest run, ties by `stems[]` order. **[Q2] Which sites survive the cap was unspecified, and the two candidate readings collided with § 4's memory bound** — INV-6's global `run_len` ordering implies holding every candidate, § 4 promised at most `max_sites` records. Both lanes reached this independently from opposite ends. INV-7 now retains the highest `run_len`, and § 4 states the bounded min-heap that satisfies both, explicitly ruling out "sort at the end". **[Q2] § 2.1's TU choice contradicted `mcp-tools.md` without saying so:** the standard's cheap position is a *new* handler TU appended last, this spec appends to TU 6 of 12. Verified the divergence is correct — `remotecontrol_workspace.cpp` is in `ANTS_RC_SOURCES_REL`, the seam requirement is met by `cochangefamily.{h,cpp}` being its own TU, and a thirteenth TU would renumber every `TU N/M` marker that `RcTuSplit.TuOrdinalMarkersAscend` asserts — so § 2.1 now states the divergence and forbids the new-TU diff by name. **Two of the six landed on text loop 1 wrote** (INV-7's partial-answer clause, § 2.5's per-site `stem`), which is the fix-pass-generates-defects pattern rather than an unsettled contract; the other four were in the original draft. **Dismissed:** `rg`'s millisecond budget is never pinned — true, and a local choice the builder settles, with nothing else binding to it. **Fixed but not tallied:** § 5's "Suggesting the new field's names" declared neither a permanent exclusion nor an id, which `specs.md` § 4 requires; a lane correctly declined to file it as a finding since it answers none of the four questions. **Why the cap binding is the right exit here:** every remaining risk in this document is about what the scan *costs* at repo scope, and § 7 says a repo-wide `rg` over this tree once took over two minutes. No cold read settles that — running the verb does. |
| impl | 2026-08-14 | **none — no reviewer was dispatched.** Written by the implementing session from what the build and the suite proved, per `write-spec` Step 8 | — (not a review loop; findings here came from a compiler and a test run, not a cold read) | **The contract survived implementation with three corrections, all recording what was built rather than changing direction.** **§ 2.1 and INV-9 named three refusal codes and the handler needs four**: a non-empty `caller_cwd` that does not canonicalise has no home in `caller_cwd_required` (the dispatcher has already passed it) or `bad_args` (the caller's arguments are fine), so it refuses `no_project` — already in the taxonomy, and the code `find_definition` uses for the same condition. **§ 2.6 omitted the `detail` sibling**: `mcp-tools.md` step 11 caps the wire `description` at ~800 B, and this verb's contract surface does not fit, so the per-arg prose moved to `detail` and `tool_info` serves it. **§ 2.6 also put `isFieldProjectionTool` in `claudeintegration.cpp`, where it is not** — it lives in `src/mcpprojection.cpp`, which is now its own row. **One design decision was reversed by building it:** an `encoding` prop was added for parity with every sibling read verb and then removed, because the columnar repack is defined over a top-level array of FLAT objects and `files[]` carries a nested `sites[]` per row — the spec never declared it, and adding an undesigned surface to satisfy a sibling's shape is what § 2.1's scope decision exists to prevent. **Two sibling tests needed repair and only one was a real count change.** `McpProjection.Inv10SchemaDeclaresFields` pins the number of `makeFieldsProp()` call sites; this verb legitimately adds the fourteenth, so the guard was bumped with provenance. `McpTestResults.WiringContract` was a **false failure**: it read `kindForName` through a fixed 8000-character window in which `"test_results"` began at offset 7987, so a three-line insertion elsewhere in the file sliced the token in half and the test reported a branch missing that was still present. Re-anchored to `slurpFunctionBody`, the brace-matched helper this repo wrote for exactly that failure mode (ANTS-1348), rather than widening the budget — which would only move the next occurrence. **Verified:** 13 new cases red against a stubbed seam (12 failing on assertions, the 13th an absence check a stub satisfies by construction), then green; full suite 3452/3452 with 4 pre-existing disabled. **NOT verified:** the verb has never been invoked over a live MCP socket — the running Ants instance predates it, so every claim about its envelope rests on the seam's unit tests and the wiring scrapes, not on a round trip. |
