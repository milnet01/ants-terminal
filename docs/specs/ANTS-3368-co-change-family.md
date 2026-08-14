# ANTS-3368 — `co_change_family`: the grouped edit-site checklist for a settings-backed field

**Status:** spec draft (2026-08-14).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-3368 (cc-feedback-2026-06-30, Vestige Sug-A, 6 consecutive slices).
**Pairs with:** ANTS-2156 (`similar_code include_bodies` — the exemplar's *body*; this verb is the exemplar's *edit sites*).

## 1. Problem

Adding a settings-backed field is a lockstep fan-out across ~10 files, and
the agent's job is to mirror an existing field rather than invent one. The
reporter (Vestige) hit this six consecutive times and named the cost: the
existing verbs find the stem and miss everything derived from it.

That is not a tuning gap, it is the matching rule. `SymbolQuery::findCaller()`
(`src/symbolquery.cpp`) and `FindSources::findSources()`
(`src/findsources.cpp`) both match an identifier as a **whole word**, and
every co-change site is the stem wearing an affix.

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

`find_sources` already expands casing variants
(`FindSources::variantsForToken()`), which is why it is the closest existing
verb — but it returns ranked **files** with prose evidence, not the line-level
sites an edit checklist needs, and it walks only the declared source/test
roots, so the `docs/` and `CLAUDE.md` mentions of a config key are outside it.

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
  (argv, working dir, millisecond budget, `rg_failed`). The handler is
  appended to that same TU, whose stated subject is "Workspace and code index
  verbs", so nothing is promoted out of file scope and no TU ordinal marker
  moves.
- **The refusal taxonomy** — `caller_cwd_required`, `bad_args`, `rg_failed`,
  all already in `mcp-error-codes.md`. No new code is minted.

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

A candidate is a **site** when the longest **contiguous run** of the stem's
words appearing in the candidate's words is at least `min_run` long. The run
is what holds a family together, and it is what survives an affix:
`setMcpEnabled` shares the run `[mcp, enabled]` with the stem even though it
shares no whole name with it.

The scan pattern handed to `rg` is the case-insensitive alternation of the
stem's **adjacent word pairs**, joined by an optional single separator. It is
a cheap over-approximation — it finds candidates, and `splitWords` on the
matched identifier does the real filtering in process.

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
  "stem": "claudeMcpEnabled",
  "stems": ["claudeMcpEnabled", "mcpEnabled"],
  "min_run": 2,
  "max_sites": 200,
  "etag_match": "…",
  "fields": ["files"] }
```

| Arg | Type | Default | Meaning |
|---|---|---|---|
| `caller_cwd` | string | — | **Required** contract (step 2 of `mcp-tools.md`). |
| `stem` | string | — | One exemplar name. Required unless `stems` is given. |
| `stems` | string[] | — | Multi-field group; the union of each stem's families. Each site carries the `stem` that matched it. |
| `min_run` | int | `min(2, stem_words)` | Shortest accepted run, clamped to `1 … stem_words`. |
| `max_sites` | int | 200 | Site cap; hard ceiling 1000. |

### 2.5 Response

```json
{ "ok": true,
  "stem": "claudeMcpEnabled",
  "stem_words": ["claude", "mcp", "enabled"],
  "min_run": 2,
  "files": [
    { "path": "src/config.cpp",
      "sites": [
        { "line": 602, "name": "claude.mcp_enabled", "role": "json_key",
          "run": ["claude", "mcp", "enabled"], "run_len": 3,
          "text": "    return m_data.value(\"claude.mcp_enabled\").toBool(true);" } ] } ],
  "files_count": 12,
  "sites_count": 41,
  "weak_matches_available": 7,
  "truncated": false,
  "etag": "…" }
```

`weak_matches_available` is the count of candidates rejected **only** by
`min_run`. It exists so the tight default is not silently lossy: a caller who
wants the wide net (the reporter's `audioLod` / `AudioLodApplySink` case,
which shares one word with `lodEnabled`) sees that it exists and re-runs with
`min_run: 1`. It is 0 when `min_run` is already 1.

### 2.6 Files

| File | Change |
|---|---|
| `src/cochangefamily.h` / `.cpp` | **New.** The pure seam: `splitWords`, `longestRun`, `scanPattern`, `classifyRole`. Own TU under `ants_core_lib` SOURCES, beside `src/findsources.cpp`. No Qt Widgets, no `RemoteControl`, no `MainWindow`. |
| `src/remotecontrol_workspace.cpp` | `RemoteControl::cmdCoChangeFamily()` appended; calls the seam + `rcRunRg()`. |
| `src/remotecontrol.h` | Handler declaration. |
| `src/mainwindow.cpp` | `registerToolProvider("co_change_family", CallerCwdContract::Required, …)`. |
| `src/claudeintegration.cpp` | `tools/list` schema; `selection_hint`; prefix-tag bucket; `callerCwdContractFor`; `isEtagSupportedTool`; `isFieldProjectionTool`. |
| `CMakeLists.txt` | `src/cochangefamily.cpp` into `ants_core_lib`; the feature test into `test_claude`. |

## 3. Invariants

- **INV-1** — `splitWords()` splits on `_`, `-` and `.`, and on every
  lower→upper boundary; lowercases each part; drops empty parts. The five
  forms in § 2.2 produce exactly the sequences shown there. *Test:*
  `tests/features/co_change_family/` case `SplitWordsFormsAgree`.
- **INV-2** — the scan pattern is the case-insensitive alternation of the
  stem's adjacent word pairs joined by an optional single `_`, `.` or `-`;
  a one-word stem yields the bare word. *Test:* the pattern block below, run
  by `spec_conformance`, plus case `ScanPatternShape`.

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

- **INV-3** — a candidate becomes a site only when its longest contiguous run
  of stem words is `>= min_run`. `min_run` defaults to `min(2, stem_words)`
  and is clamped to `1 … stem_words`; a value outside that range is clamped,
  never refused. *Test:* case `MinRunGatesAndClamps`.
- **INV-4** — a run whose every word is a stopword is dropped whatever its
  length. The list is frozen here: `get`, `set`, `is`, `has`, `on`, `off`,
  `enabled`, `disabled`, `value`, `data`, `flag`, `count`, `size`, `index`,
  `name`, `type`, `mode`, `m`, `p`. *Test:* case `StopwordOnlyRunsDropped`.
- **INV-5** — every emitted `role` is one of the six values in § 2.3, assigned
  in that precedence order. No seventh value is ever emitted. *Test:* case
  `RoleVocabularyIsClosed`, asserting against the enumerated set.
- **INV-6** — `files[]` is ordered by each file's maximum `run_len`
  descending, ties broken by `path` ascending; `sites[]` within a file is
  ordered by `line` ascending. *Test:* case `OrderingIsDeterministic`.
- **INV-7** — when the site count exceeds `max_sites` the response carries
  `truncated: true`. No site is dropped without that flag being set. *Test:*
  case `TruncationIsFlagged`.
- **INV-8** — `weak_matches_available` counts candidates rejected by INV-3
  alone; a candidate rejected by INV-4 is never counted there, and the field
  is 0 when `min_run == 1`. *Test:* case `WeakCountExcludesStopwordDrops`.
- **INV-9** — an empty `caller_cwd` refuses `code:"caller_cwd_required"`; a
  `stem` that yields zero words after `splitWords` refuses `code:"bad_args"`;
  a non-zero `rg` exit that is not "no matches" refuses `code:"rg_failed"`.
  *Test:* case `RefusalCodes`.
- **INV-10** — `src/cochangefamily.cpp` references no symbol from
  `RemoteControl`, `MainWindow` or `ClaudeIntegration`, so it links into
  `test_core` alone. *Test:* case `SeamTuHasNoChromeSymbols` (source-grep over
  the TU), plus the `test_core` link itself.
- **INV-11** — the verb appears in `tools/list` with
  `inputSchema.type == "object"` and `additionalProperties == false`, is
  registered with `CallerCwdContract::Required` at both the registration site
  and `callerCwdContractFor`, and is listed in `isEtagSupportedTool` and
  `isFieldProjectionTool`. *Test:* case `RegistrationAndSchema`.

## 4. RAM / build cost

No new build target and no new external library. `src/cochangefamily.cpp`
joins the existing `ants_core_lib`; the handler joins an existing RC TU, so
the RC TU count and its ordinal markers are unchanged.

Memory is bounded by the response cap, not by the tree: `rg` streams, and the
handler holds at most `max_sites` site records (hard ceiling 1000) plus one
line of matched text each, clipped to 512 bytes — the same clip
`workspace_search` applies. Worst case ≈ 1000 × ~600 B ≈ 600 KB, transient,
freed with the response. No cache, no process-lifetime state, so nothing to
evict.

## 5. Out of scope

- **Semantic roles ("apply sink", "editor widget").** Permanent exclusion, not
  deferred — § 2.3 gives the reason. No follow-up id.
- **Suggesting the new field's names.** The verb reports where the *exemplar*
  is touched; writing the mirror is the agent's job. A generator would have to
  know the target's type and defaults, which is a different verb.
- **Cross-repo families.** Permanent exclusion: the scan is rooted at one
  `caller_cwd`, as every project-scoped verb is.
- **Frequency-ranked weak matches.** Deferred — § 7's open question; no id
  filed yet, because the `min_run: 1` re-run may make it unnecessary and
  filing work nobody intends to do is what `specs.md` § 4 forbids.

## 6. Tests

Feature test: `tests/features/co_change_family/`, compiled into the
**`test_claude`** bundle (beside `tests/features/mcp_find_sources/`; it is not
a standalone target). Label `features;fast`. Covers INV-1 … INV-11, one case
per invariant as named in § 3.

Two project conventions apply and are not optional here:

- **Verify each case fails against pre-fix source first** — for a new verb
  that means stubbing the seam to return empty and watching the assertions
  (not the compile) fail.
- **Build `test_claude` specifically and check `ctest -N -R co_change` moved.**
  Building the wrong target succeeds silently and runs the old binary, so a
  green suite would read as success with the new test never compiled.

The `spec_conformance` verb runs INV-2's pattern block against its table
directly out of this document; that case needs no C++ counterpart beyond
`ScanPatternShape` asserting the generator produces that pattern for this
stem.

## 7. Open questions

- **Is `min(2, stem_words)` the right default?** It is tight: it returns the
  reporter's `setLodEnabled` and `"lod_enabled"` and drops their `audioLod`.
  `weak_matches_available` makes the wide net one argument away, which is the
  cheapest way to find out from real use. If callers always re-run with
  `min_run: 1`, the default is wrong and frequency ranking (§ 5) is the fix.
- **Should the scan honour `.ants/project.json` roots, or the whole repo?**
  This spec says the whole repo (minus `.gitignore`, via `rg`), because a
  config key's `docs/` and `CLAUDE.md` mentions are co-change sites and the
  declared source roots exclude them. That is a deliberate divergence from
  `find_sources`, which walks the declared roots only.

## 8. Cross-doc impact

| Doc | Change |
|---|---|
| `docs/standards/mcp-behavioural-notes.md` | Per-verb note: the matching rule, the role vocabulary, `weak_matches_available`. |
| `CHANGELOG.md` | Entry under Added on the shipping release. |
| `ROADMAP.md` | ANTS-3368 flipped on ship, with the resolution line. |
| `Ants_Terminal_Ants_MCP_Feedback.md` | The Vestige finding's `**Proposed ID:**` slot closes when it ships. |
| `mcp-error-codes.md` | **No change** — every refusal reuses an existing code. |

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
