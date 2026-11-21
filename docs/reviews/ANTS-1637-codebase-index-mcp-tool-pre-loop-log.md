# ANTS-1637 — `codebase_index` MCP tool (pre-computed project structural map, lazy + disk-cached) — review loop log

Record for [`../specs/ANTS-1637-codebase-index-mcp-tool-pre.md`](../specs/ANTS-1637-codebase-index-mcp-tool-pre.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-05) — `/cold-eyes`, 3 parallel fresh cold reviewers.**
  0 CRITICAL. Verified + fixed: (HIGH) § 2.2/header/INV-3 claimed lane
  membership came from a `SubsystemMap` `laneToFiles` — but `SubsystemMap::Lane`
  is `{name, summary}` only (`subsystemmap.h:20-23`) and the live
  `subsystem op=files` derives files via `resolveLaneFiles`, a `src/<lane>*`
  prefix glob (`remotecontrol.cpp:7626`, ANTS-1251-INV-4) → rewrote § 2.2 to
  compute membership (module-map lane-name prefix ∪ the unlisted-family
  regex), reusing the live glob rule so `codebase_index` and `subsystem`
  agree. (HIGH ×2, raised by two reviewers independently) the `truncated`
  envelope field was overloaded by the build ceiling (INV-11) and the
  `lane=` query cap → split into `files_truncated` / `symbols_truncated`
  across § 2.1/2.4/2.6/INV-11. (HIGH ×2) `kMaxQuerySymbols` had a dangling
  `// INV-...` comment and no covering invariant → added **INV-14** + filled
  the comment. (HIGH) corrupt/unparseable cache was an undefined hot-path
  edge → added a fourth cold-build trigger (`fromJson` parse failure) and
  extended INV-13's test. (MEDIUM) missing `src/`/`tests/` + zero-file root
  unspecified → § 2.3/INV-1 now skip an absent subtree silently and return
  an `ok:true` empty summary. (MEDIUM) `generated_at_ms` warm-query
  semantics pinned (preserved on a no-write query). (MEDIUM) concurrent
  clobber noted as safe-by-construction in INV-12. (MEDIUM) § 2.4
  selector→shape prose → a table; (MEDIUM) § 4 RAM/perf hand-wave ("tens of
  ms", "low single-digit MB") → a derived cache-size ceiling + a cost model
  with no fake wall-clock figure; (MEDIUM) `staleFiles` "cheap/expensive"
  adjectives → an explicit cost relation. (LOW) softened the
  `find_definition` soft-miss analogy (no `found` field upstream — `found`
  is `codebase_index`'s own); pinned the build's `maxSymbols=kMaxSymbolsCap`;
  resolved the three § 8 open questions (exact `symbol=`; deterministic
  `src/`-then-`tests/` ceiling order; lane-regex only fires for non-module-map
  files). No new MCP refusal code minted.

- **Loop 2 (2026-06-05) — `/cold-eyes`, 3 parallel fresh cold reviewers.**
  0 CRITICAL; every loop-1 fix held (the truncation split, lane-membership
  correction, and soft-miss model all read clean on the cold pass — none
  recurred). Verified + fixed: (HIGH, two reviewers independently) § 2.2
  step 1 claimed `codebase_index` and `subsystem` "agree on a file's lane",
  but `resolveLaneFiles` globs `src/` **only** (`remotecontrol.cpp:7626-7653`,
  confirmed) → scoped the agreement to `src/` files and stated `tests/` lane
  membership is `codebase_index`-only (no `subsystem` counterpart, so they
  cannot disagree). (HIGH) `query()` took an undefined `QueryParams` →
  defined `struct QueryParams { QString symbol; QString lane; QString filePath; }`
  and the none/one/≥2 selector rule. (HIGH) `kMaxSymbolsCap` was referenced
  but not sourced as `FileOutline`'s clamp → annotated in the § 2.2 constant
  block (inherited, not redeclared). (HIGH/MEDIUM) pinning the RAM number
  exposed that `kMaxIndexFiles`×`kMaxSymbolsCap`×bytes reaches **gigabytes**
  worst-case, contradicting "single-digit MB" → added `kMaxCacheBytes`=16 MiB
  as the binding heap/cache ceiling (build/refresh stop at it, INV-11), and
  showed the 621-file ≈1–2 MB arithmetic (~120 B/symbol). (HIGH) INV-12's
  concurrency clause had no falsifying `*Test:*` and duplicated § 2.5 →
  removed it from the invariant (kept as a § 2.5 design note). (MEDIUM) the
  summary example's `roles`/`languages` summed to 621/214 not `file_count`
  287 → corrected to sum to 287 + stated the sum rule. (MEDIUM) the family
  regex's `(cpp|h)` didn't cover the `{.h,.hpp,.hh}` header set → widened to
  `(?:cpp|cc|h|hpp|hh)` and pinned the indexed-suffix set
  `{.cpp,.cc,.h,.hpp,.hh,.py}` (so `language` ∈ {cpp,py} in v1; md/json
  reserved). (MEDIUM) INV-11's test used 3 `src/` files, never exercising
  the cross-subtree order → fixture now spans 2 `src/` + 1 `tests/`.
  (MEDIUM) a `file_path` resolving to a directory → `found:false` (added).
  (MEDIUM) § 2.6 ETag/fields/caller_cwd boilerplate → led with a
  `mcp-tools.md` umbrella reference, keeping only the tool-specific wiring.
  (LOW) `findsources.h` citation `:5-15` → `:1-15`; § 2.3 cold-build triggers
  → a bullet list.

- **Loop 3 (2026-06-05) — `/cold-eyes`, 3 parallel fresh cold reviewers.**
  0 CRITICAL; all loop-1/2 fixes held on the cold read. Verified + fixed:
  (HIGH) the `Symbol.kind` example was `"function"`, but `FileOutline` emits
  `"func"`/`"class"`/`"qt"`/`"heading"` (`fileoutline.cpp:271-313`) — would
  have broken the INV-5 test assertion → corrected to the real set (only
  `func`/`class`/`qt` reachable in v1, md not walked). (HIGH) the spec told
  the implementer to reference `FileOutline`'s `kMaxSymbolsCap`/`kMaxLineBytes`
  by name, but both are **file-private** (anonymous namespace,
  `fileoutline.cpp:13-16`) — not header-exported → reworded to "compute
  clamps internally; pass literal 1000". (HIGH) the dispatcher hashes the
  whole envelope incl. volatile `refreshed_files`/`generated_at_ms`, so the
  § 1 "free re-read" needed pinning → INV-9 now states the etag is stable
  across consecutive warm queries and the one non-content miss (first warm
  query after a write). (HIGH) the `lane=` symbol cap dropped "file then
  outline order" without a deterministic *file* order → pinned sorted
  `laneToFiles` (Index lists now documented sorted) + INV-14 asserts *which*
  symbol survives. (HIGH/MEDIUM) pinning RAM exposed the file-count cap
  alone allows GB worst-case → split the byte ceiling into its own **INV-15**
  (`kMaxCacheBytes`, the binding bound) distinct from INV-11's file-count
  cap. (MEDIUM) per-file 1000-symbol silent truncation on `file_path=`/`lane=`
  documented; (MEDIUM) `file_count` = indexed count, `""` lane excluded from
  `lane_count`/`lanes[]`; (MEDIUM) concurrent cold-build duplicated-CPU note
  (§ 2.5); (MEDIUM) mtime-granularity gap added to § 5 out-of-scope; (MEDIUM)
  § 2.6 mechanism prose trimmed to wiring deltas; (MEDIUM) `~tens of
  symbols/file` → `~30`. (LOW) INV-13 belt-and-suspenders phrasing;
  `cmdFindDefinition` pinned `:9939`; `resolveLaneFiles` cite `:7653`→`:7652`;
  dropped the bare "Karpathy § 2" reference for plain English; INV-10 notes
  `fields` narrows whichever body is present.

- **Loop 4 (2026-06-05) — `/cold-eyes`, 3 parallel fresh cold reviewers.**
  0 CRITICAL; one reviewer found 0 HIGH ("18/18 code citations exact"), the
  other two converged on the same structural HIGH. All loop-1–3 fixes held.
  Verified + fixed: (HIGH, all three) INV-15 was wedged between INV-11 and
  INV-12 (split late in loop 3) → relocated to the list end (id kept 15, with
  a one-line note) so the list reads 11→12→13→14→15. (HIGH) the
  `fileoutline.cpp` constant cite disagreed with itself (`:13-16` in § 2.2 vs
  `:15-16` in § 2.4) → unified on `:15-16` (constants) noting the namespace
  opens at `:13`. (HIGH) INV-9's test didn't exercise the N→0 one-time-miss
  claim → extended to a four-call recipe (cold build → first warm re-emit →
  second warm 304 → touch → bust). (MEDIUM) `refresh` out-param comment
  `*refreshed` → `*refreshedOut` (matches the signature + INV-2). (MEDIUM)
  flagged the `found:false` vs `read_region`'s `symbol_not_found` split as a
  *chosen* cross-tool divergence (index-absence ≠ region-extract failure).
  (MEDIUM) pinned empty-string selector (`{symbol:""}`) = absent → summary.
  (MEDIUM) § 4 "I/O-bound on file count" → "CPU on line count" (regex per
  admitted line — `FileOutline` is CPU-bound). (MEDIUM, dedup) cut the
  triplicated `generated_at_ms` prose paragraph and the duplicated ETag
  stability narrative (now pinned once in INV-9; § 2.6 keeps only wiring +
  an INV-9 pointer); trimmed § 2.4's truncation re-explanation to an INV-14
  cross-ref. (completeness, raised as an open question) added a § 2.5 rule
  that `kIndexVersion` gates `FileOutline`'s output format too, so an Ants
  upgrade that changes outline output invalidates stale caches (INV-13).
  (LOW) added a Contents TOC; split a 55-word § 4 sentence.

- **Loop 5 (2026-06-05) — `/cold-eyes`, 3 parallel fresh cold reviewers
  (confirming pass).** 0 CRITICAL. The accuracy/cross-reference lane returned
  **zero** findings ("18/18 code citations exact … implementation-ready");
  all loops-1–4 fixes held. Folded the remaining test-surface + clarity
  residue: (HIGH) INV-15's byte ceiling didn't pin the deterministic file
  order INV-11 does (the two ceilings interleave in one walk) → added the
  same `src/`-sorted-then-`tests/`-sorted order + a *which-survives* assert.
  (HIGH) INV-2 tested only the `changed` path → extended to assert a removed
  file leaves both `files[]` and every `laneToFiles[]` list and an added file
  gets its computed lane (`refreshedOut==2`). (MEDIUM) promoted the
  roles/languages-sum-to-`file_count` rule from prose to **INV-16** (it broke
  once as an example in loop 2). (MEDIUM) noted `symbols_truncated` is always
  `false` for non-`lane=` bodies; (MEDIUM) `file_path=` in-root symlink
  canonicalising outside root → `bad_path` (§ 2.4 + INV-7); (LOW) INV-3 test
  pins `refreshedOut==0` on a map-source-only change; trimmed § 2.4's
  response-symbol-cap bullet to an INV-14 cross-ref; simplified the § 2.4
  heading so its TOC anchor is renderer-portable. Design unchanged since
  loop 3 — converged → **accepted**.
