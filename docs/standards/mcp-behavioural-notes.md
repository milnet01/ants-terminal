<!-- ants-mcp-behavioural-notes: 1 -->
# Ants Terminal MCP per-verb behavioural notes

Behavioural reference for individual MCP verbs — *not* authoring rules
(those live in [`mcp-tools.md`](mcp-tools.md)). Relocated out of the
always-loaded `CLAUDE.md` preamble by ANTS-2088 so the per-session token
cost is only paid when a verb's exact behaviour is actually in question.
Read this on demand; the verb catalogue + one-line *when to use* per verb
is available live via `tool_info {catalog:true}`.

Keep this file in sync with the code as you would any spec.

## Dispatch-wide read-response nudges (ANTS-2081 / ANTS-2086)

Applied by `mcp::appendReadHints` after the etag/`fields=` steps, to any
**large** (>4 KiB) successful read response — independent of which verb
produced it, so they appear in no single tool schema:

- **`next_call_hint`** — when the response carries a fresh `etag` and the
  caller didn't send `etag_match`, nudges reusing it
  (`pass etag_match="<etag>" next call to skip an unchanged re-read`).
- **`leaner_call_hint`** — names the cheaper mode on that verb
  (`roadmap_query`→`headline_only`/`section_index`/`status:active`,
  `workspace_search`→`max_match_bytes`, `file_outline`→`filter`, else the
  generic `fields=`), only when the caller isn't already on the lean path.

Both are presentation-only (added after the etag is computed, so they
never perturb the hash) and are skipped on 304s, refusals (`ok:false`),
`fields=`-narrowed calls, and bodies under the threshold.

## Tabular (columnar) array encoding (ANTS-2090)

Opt-in per-call `encoding:"tabular"` on the list-shaped read verbs —
`roadmap_query`, `workspace_search`, `file_outline`, `find_sources`,
`find_caller`, `codebase_index`, `docs_index`. The dispatcher runs
`mcp::tabularize` after `appendReadHints` and before `offloadBody`: each
**top-level** array-of-objects is repacked into a columnar form that drops
the per-row key repetition dominating list replies (30–60% smaller on big
lists):

```json
"bullets": { "__cols__": ["id","status"],
             "__rows__": [["ANTS-2090","📋"], ["ANTS-2093","📋"]] }
```

**Decode:** for any object value carrying a `__cols__` key, zip `__cols__`
with each `__rows__` entry to rebuild the array; a `null` cell means that
key was absent on that element (an explicit-null and a missing key decode
identically — the documented collapse).

Self-guarding per array — it no-ops on a non-object body, an `ok:false`
refusal, a 304, an array with <2 elements or any non-object element, and
any array whose columnar form would not be strictly smaller (so it can
**never** cost bytes). `__cols__` is lexicographically sorted, output is
deterministic, and nested object/array cell values are carried verbatim
(no recursion in v1). Per-call only — it changes a field's *shape*, so the
caller must know how to decode it; there is no session default (unlike
`compact`/`offload`). Composes with `fields=` (tabularizes whatever
survived projection) and `offload` (a smaller body may now fit under the
spill threshold; if it still spills, the spill file holds valid tabular
JSON). Spec + cold-eyes log: `docs/specs/ANTS-2090.md`.

## Result offload + `read_spill` (ANTS-2094 / ANTS-3538 / ANTS-3545)

When a read reply exceeds the spill threshold, the dispatcher's last step
(`mcp::offloadBody`, after `appendReadHints` and `tabularize`) parks the
full body on disk and returns a small pointer instead — so a giant reply
costs a few hundred tokens up front, and the caller re-reads only the slice
it needs. The body is written **content-addressed** to
`~/.cache/ants-terminal/mcp-spill/<sha256>.json` (swept at 24 h). The write
is **fail-open**: if the private dir, `open`, write, or `commit` fails —
or the built envelope isn't smaller than the body — the verb returns the
**full inline body**, never an error (a partial temp file is cancelled, and
a content-addressed spill written just before a late fail-open is a benign
duplicate the 24 h sweep reclaims).

**Offload envelope** (`offloaded:true`): `handle` (the 64-char sha256),
`head` (a leading byte-prefix of the body), `bytes`/`row_count`, and a
`hint` naming both re-read modes. When the body's dominant field is a
row-shaped array, ANTS-3538 adds a typed **`head_rows` preview** —
`head_rows` (the first few whole elements), `head_rows_key` (the array's
field name), `head_rows_truncated`, `head_truncated` — so the caller sees
real rows, not just a clipped byte-string.

**`read_spill` — byte mode (default).** `offset`/`max_bytes` return
`{ok, content, offset, bytes, total_bytes, truncated}`; page forward by
re-calling with `offset` ← the returned `offset + bytes` until
`truncated` is false. Byte mode never parses the body, so it works on any
spill regardless of size or shape.

**`read_spill` — row mode (ANTS-3545).** A numeric `row_offset`/`row_count`
routes to the parsed row-pager and returns
`{ok, mode:"rows", key, rows, row_offset, total_rows, truncated}` — clean
element boundaries over the dominant array (same key as `head_rows_key`).
`row_count ≤ 0` (or omitted) serves the default page (`kSpillRowsDefault`,
100 rows); `row_offset` past the end yields an empty non-truncated page.
Row mode **wins over byte mode** when a call carries both key sets (the
byte `offset`/`max_bytes` are ignored).

**Refusal codes** (all on the `read_spill` verb):
- `bad_args` — `handle` isn't a 64-char lowercase sha256, or a byte/row
  arg is negative.
- `not_found` — handle never spilled or was evicted; re-issue the original
  call to regenerate it.
- `too_large` — body > 1 MiB (`kStructuredParseMaxBytes`), stat-gated
  **before** load so an over-cap body is never parsed into RAM; the `hint`
  says byte-page it via `offset`/`max_bytes` instead. Row mode only.
- `not_array` — body isn't a JSON object with a dominant row-shaped array
  to page; the `hint` says byte-page it instead. Row mode only.

`raw:true` (see the read-verbs note below) suppresses offload entirely, so
an Edit-from-output caller gets true bytes rather than a head+pointer.
Specs: `docs/specs/ANTS-2094.md` (offload + byte paging), ANTS-3538
(`head_rows` preview), ANTS-3545 (row mode).

## Trimmed descriptions + `tool_info` `detail` (ANTS-2079)

The seven largest tool descriptions (`roadmap_query`,
`model_switch_stats`, `roadmap_log`, `test_audit_partition`,
`changelog_log`, `workspace_search`, `verify_changes`) ship only a short
essentials `description` on the wire — load-bearing contract surface (op
set, refusal codes, `caller_cwd`, required args) plus a runtime-appended
`Full per-op detail via tool_info {name:"…"}` pointer. The encyclopedic
per-op prose lives in a `detail` field that is **stripped from
`tools/list`** and returned only by `tool_info {name:"<tool>"}` (alongside
`description`, `inputSchema`, `selection_hint`). So when a session needs a
trimmed verb's full per-op/per-arg reference, make one `tool_info` call —
the same call ANTS-1399 already prescribes before invoking a verb. Tools
that did not author a `detail` simply omit the key from `tool_info`.

## Tool-search deferral exemption (ANTS-2158)

Claude Code defers MCP tool *schemas* (tool-search): a deferred verb
needs a `ToolSearch` round-trip before its first call, while built-in
Bash grep / Read / Edit are always loaded — a friction gradient that
nudges long sessions back to raw grep (Vestige Obs #18). The
`tools/list` builder marks a curated high-frequency set with
`"anthropic/alwaysLoad": true` in each tool's `_meta` object, which
Claude Code **v2.1.121+** honours to keep those tools always-loaded
(older clients ignore the field — graceful). The set
(`kEagerVerbs` in `claudeintegration.cpp`) is deliberately small — each
always-loaded tool costs context — and covers the verbs that most
directly replace always-loaded built-ins: `workspace_search`,
`find_definition`, `file_outline`, `read_region` (grep / Read
substitutes) and `roadmap_log` / `changelog_log` (ROADMAP/CHANGELOG
edit substitutes). There is **no** server-wide "eager-load the whole
server" lever; the only other knob is the client-side per-server
`"alwaysLoad": true` in `.mcp.json`, which a heavy user can set
themselves. Deferral itself is a Claude Code architectural choice, not
server-controllable beyond this per-tool hint.

## Read / incremental verbs

- **`get_scrollback`** — since-cursor incremental mode (ANTS-1500).
- **`roadmap_query`** — recognises ants-v1 / github-task-list /
  pass-headings formats (ANTS-1530). `mode:"bundles"` (ANTS-1922)
  groups the active subset (📋/🚧, id-bearing) into thematic
  work-bundles by a denoised headline-token edge (union-find /
  transitive closure) — the one-call "next bundle of related to-dos?"
  view. **ANTS-2155 reworked the edge** (the original Jaccard ≥ 0.50
  divided by the union, so long vocabulary-varied headlines never
  cleared the bar and the real roadmap produced all-singletons):
  clustering tokens are now DENOISED first — stop-words, length ≤ 2,
  pure numbers (`6162`), and file/path/qualified identifiers
  (`auditdialog.cpp`) are dropped, plus corpus-ubiquitous tokens
  (document frequency > max(8, 40 %) — a TF-style filter). The edge is
  then length-insensitive: ≥ 2 shared denoised tokens AND (overlap
  coefficient |∩|/min ≥ 0.5 OR ≥ 3 shared tokens OR a shared lane). The
  envelope also carries `total_bundle_count` + `bundles_omitted`
  alongside the emitted `bundle_count` so a truncated response no longer
  reads as if clustering happened when it didn't. Active-only: a passed `status` is ignored;
  refuses `bad_mode_combo` with `section`/`id`/`ids`. Each bundle:
  `{bundle_label, lanes, size, items[]}`; each item may carry
  `possibly_resolved_by`/`possibly_resolved_score` (a ✅ sibling at
  Jaccard ≥0.60 may already cover it) and `gate_note`/`blocked` (a
  body gate/blocker marker — `blocked by`, `until …lands/ships`, …;
  bare `blocks` is excluded). Byte-stable envelope (size-desc,
  id-asc); whole-bundle soft-cap truncation at
  `PaginationEngine::kSoftCapBytes` (no `next_offset` in v1). The
  builder is the pure static `RemoteControl::buildRoadmapBundlesEnvelope`
  (warm-cache, no re-parse). Spec `docs/specs/ANTS-1922.md`.
- **`read_log`** — filters a log file (the Ants debug log or a
  `caller_cwd` path) to matching lines via the pure `ReadLog::filter`
  helper; streaming drop-oldest byte cap + `since_cursor` incremental
  tailing (ANTS-1855).
- **`read_region`** — returns an exact line range or a named symbol's
  body (resolved via the flat `file_outline` scanner — class /
  `Class::method` forms) from a project file via the pure
  `ReadRegion::extract` helper; ETag-304 free re-read + head-anchored
  incremental byte cap; caller_cwd-Required (ANTS-2021).
- **`raw:true` (ANTS-2218)** — opt-in verbatim framing, honoured by
  `read_region` / `read_regions` / `workspace_search`
  (`mcp::isRawEligible`). The default frame neutralises any literal
  `</ants_mcp_data>` close-tag and `<!--`/`-->` comment markers in the
  returned bytes (lossy, to stop hostile content forging the response
  frame — ANTS-1294/1670/1996). That corrupts an `Edit`/`apply_edits`
  built from a file that *itself* contains those tokens (this MCP source,
  a spec, HTML/markdown with comments). `raw:true` instead emits the bytes
  byte-for-byte inside an **unforgeable nonce frame**
  (`<ants_mcp_data_raw__<nonce> …>…</ants_mcp_data_raw__<nonce>>`, the
  nonce a content-hash verified absent from the payload), and suppresses
  the ANTS-2094 offload so the agent gets true bytes, not a head+pointer.
  Set it when you will Edit from the output; omit otherwise. The default
  scrub is deliberately NOT made reversible — any escaping a good agent
  could invert, a hostile normalising tokeniser could invert too.
- **`codebase_index`** — serves a pre-computed project structural map
  (symbols-per-file + lane→files) so a session stops re-deriving shape
  with grep / `file_outline` / CLAUDE.md reads. One verb, selectors
  `symbol` / `lane` / `file_path` / none→summary (≥2 → `bad_args`; a
  miss is `ok:true,found:false`, never a code). Lazy disk cache at
  `~/.cache/ants-terminal/codebase-index/<cwdHash>.json` (cold-built on
  absent / unparseable / version|root-mismatch, mtime-incremental
  refresh); reuses `FileOutline` + `SubsystemMap`; ETag-304 + `fields`;
  caller_cwd-Required (ANTS-1637). **Layout override (ANTS-2160):** a
  repo-committed `<root>/.ants/project.json` may declare
  `source_roots`/`test_roots` (arrays of dirs); when present the walk
  uses them instead of the default `src/`+`tests/` (each must be an
  existing dir; an all-dropped key falls back). The same file's
  `docs_dir` / `roadmap` / `changelog` / `specs_dir` keys redirect
  `docs_index` / `roadmap_query`+`roadmap_log` / `changelog_log` /
  `spec_query`+`spec_log`+`current_state` / `project_layout`
  respectively. Pure loader `ProjectSettings::load`
  (`src/projectsettings.cpp`); paths validated under root via
  `PathValidation::isInsideProject`; absent file / key ⟹ today's
  heuristic (zero regression). Spec `docs/specs/ANTS-2160.md`.
- **`docs_index`** — the documentation-map sibling of `codebase_index`:
  serves a pre-computed, **project-agnostic** map of every doc so a
  session finds the right doc in one call instead of grep / `Read` across
  an unfamiliar layout. Walks `<root>/*.md` (non-recursive) +
  `<root>/docs/**/*.md` (recursive); per doc captures the heading outline,
  first-H1 title, best-effort `**Status:**`, and the outbound relative-`.md`
  link graph. Selectors `topic` (title×3 / path×2 / heading×1 scored) /
  `doc_path` (one doc's outline + `linked_from` reverse edges) / `id`
  (filename stem) / none→summary (≥2 → `bad_args`; a miss is
  `ok:true,found:false`). Indexes headings/title/path, **not** full body
  prose. Lazy disk cache at `~/.cache/ants-terminal/docs-index/<cwdHash>.json`
  (cold-built on absent / unparseable / version|root-mismatch,
  mtime-incremental refresh); ETag-304 + `fields`; caller_cwd-Required.
  Complements `spec_query` (the deep per-spec reader) — `docs_index` is the
  shallow cross-doc discovery layer over all doc types (ANTS-2139).

- **`similar_code`** — `include_bodies:true` (ANTS-2156) makes each
  (top-N, score-ranked) match additionally carry the FULL enclosing
  definition — `symbol`, `body` lines, `body_start_line`/`body_end_line`,
  `body_truncated` — extracted via `ReadRegion::extract`, so a session
  copying an in-repo idiom gets the complete exemplar in ONE call instead
  of N follow-up Reads. A match whose signature doesn't resolve to an
  outline symbol carries `body_unavailable:true` (file:line still given).
  Pair with a small `max_results` (default 3). Ranking is the existing
  structural-similarity score; richer canonical-ness signals
  (call-count / recency) are a deferred follow-on.
- **`read_region`** — `call_sequence:true` (ANTS-2157) is the
  integration-brief view: alongside the symbol's body it returns
  `call_sequence` (the body's call-expressions in source order — a
  pipeline's STAGES, each `{line, callee}`, with the line as the
  insertion point; the signature line is skipped, scanning stops at the
  symbol's end) and `accessors` (the distinct `m_` members + `get`/`is`/
  `has` getters referenced — the helpers a new stage usually needs).
  Answers "where do I hook into this pipeline, in order, and what does a
  new step touch?" in one call. Best paired with a `symbol` naming the
  driver function. Opt-in (back-compat when absent); a heuristic line
  scan over the region read_region already slices. v1 is one-function
  scoped (cross-method pipeline mapping is a follow-on).

## Write / edit verbs

- **`apply_edits`** — applies N `{path, old, new}` edits across M project
  files in one atomic-per-file call (pure `ApplyEdits::applyToContent` +
  `QSaveFile` + `fsyncParentDir`); per-edit `skipped[]` (not_found /
  ambiguous / too_large / commit_failed); fail-closed `bad_path` on root
  escape; caller_cwd-Required (ANTS-2022).
- **`project_settings`** — detect a non-standard layout + create/update
  `<root>/.ants/project.json` (the ANTS-2160 reader's source). Ops:
  `detect` (read-only — `{present, suggestion:{source_roots?, reason,
  default_source_count, total_source_count, would_use_roots?, excluded?}}`;
  its bounded shallow walk reuses `CodebaseIndex::isIndexableSuffix` so
  counts agree with what the index admits, and discounts vendored /
  third-party trees — `*-deps`, `*-prefix`, `third_party`, `external`,
  `deps`, `vendor`, … — so a bundled-dependency dir is never ranked as a
  `source_root` nor counted in `total_source_count`, ANTS-3357. On a "miss"
  (default `src/`+`tests/` walk indexes < half the source) it now suggests
  ALL first-party source subdirs sorted count-desc/name-asc — not a
  dominant-cover subset — so a spread src-less layout or a low-count
  entry-point dir like `app/` is covered, not dropped; `reason` is ALWAYS
  non-empty (nullopt `source_roots`, not an empty reason, is the
  "no suggestion" signal); `would_use_roots` echoes the roots already in
  effect (declared on `present:true`, else the default roots that hold
  source) and `excluded` lists the skipped noise/vendored dirs — ANTS-3369),
  `init` (write the detected/explicit keys; refuses
  `settings_exists` on an existing file — no clobber; `written:false`
  when nothing to suggest), `set` (create-or-update; raw-JSON merge
  preserving unknown keys; a `null` value clears a key; refuses
  `bad_args` when no key is supplied, `unrecognised_format` on a
  malformed existing file). Validation is **STRICT at write-time**
  (`bad_path` on an escaping / non-existent / wrong-type declared path) —
  deliberately unlike the ANTS-2160 reader's lenient read-time drop. The
  file is written **world-readable 0644** by design (NOT
  `setOwnerOnlyPerms`). Pure helpers `ProjectSettings::detect` /
  `applyWrite`; handler `cmdProjectSettings`. `session_orient` surfaces a
  `project_settings_suggestion` block when no settings file exists AND
  `codebase_index.file_count` is below a low-water mark (= 5), so a
  standard project never pays for the detector walk (ANTS-2161).
- **`project_query`** — run an agent-supplied **read-only** Lua snippet
  server-side and return only its marshalled result (the code-execution
  token-saver; ANTS-2093). Args `caller_cwd` + `code` (the snippet, which
  must `return` a value). Host surface is the `project.*` table only:
  `read(relpath)` / `list(subdir?)` / `root()` — plus the
  `string`/`table`/`math`/`utf8` stdlib. **None** of the plugin `ants.*`
  write callbacks are installed; `os`/`io`/`load`/`require`/`debug` stay
  unloaded. Confinement: every `project.*` path runs through
  `PathValidation::validatePath` against the canonical `caller_cwd`
  (canonical anchor — a `..`/absolute/symlink escape raises). Caps: VM
  10 MiB + 100k-instruction hook (inherited), wall-clock
  `claude.mcp_project_query_timeout_ms` (default 1500, clamp [100,5000]),
  output `claude.mcp_project_query_result_cap_bytes` (default 64 KiB,
  clamp [1 KiB,1 MiB]). Threading: each call runs on a **fresh ephemeral
  worker thread** with a bounded join (timeout + 250 ms grace); a snippet
  stuck in an uninterruptible C call is **detached** (returns
  `query_timeout`, worker held as a zombie until process exit) so the GUI
  is never frozen beyond the budget. Marshalling (§2.4): nil→null, bool,
  number (integer subtype preserved; non-finite → `query_error`), string
  (invalid UTF-8 → `query_error`), array-like table → array, string-keyed
  table → object (both recursive, ≤ 32 levels — the bound also catches a
  circular table); function/userdata/thread/non-string-key → `query_error`.
  Feature-gated by `claude.mcp_project_query_enabled` (**default ON**;
  off → `query_disabled`, checked before arg validation); under the master
  `claude.mcp_enabled` gate (off → `mcp_disabled`, takes precedence).
  A large result spills via the ANTS-2094 offload path (in
  `isOffloadEligible`). Refusal codes: `query_error` / `query_timeout` /
  `query_oom` / `result_too_large` / `query_disabled`
  (mcp-error-codes.md § 6). Compiled out under `-DANTS_LUA_PLUGINS=OFF`
  (verb unregistered; absent from the catalogue). Pure entry point
  `LuaEngine::projectQueryVerb`; the provider lambda lives in
  `mainwindow.cpp` (the core-lib `RemoteControl` can't see `LuaEngine`).
- **`feedback_query` / `feedback_log`** — read/write the
  `*_Ants_MCP_Feedback.md` files via the pure `FeedbackFile` module
  (delta parse + block render; ANTS-1961/1962); suffix-guarded on
  `_Ants_MCP_Feedback.md`; append-only at EOF.
- **`spec_log`** — writes a spec's Status line / cold-eyes loop log /
  `INV-N` via the pure `SpecLog` module (`op:"set_status"` /
  `"append_loop"` / `"append_inv"`), never renumbering; reuses
  `spec_query`'s id routing (ANTS-1963). Shared id routing accepts any
  `<PREFIX>-NNNN` id (e.g. `DOOM-0009`, not just `ANTS-NNNN`) and resolves
  the file via `resolveSpecRelForId` — exact `<id>.md`, then a `<id>-*.md`
  glob for topic-suffixed specs (`DOOM-0009-path-tracer.md`), ANTS-3356.
- **`spec_query` list mode (ANTS-3360)** — called with neither `id` nor
  `path`, enumerates the specs dir → `{mode:"list", specs_dir,
  specs:[{id, title, status, path, size_bytes, mtime_ms}], count,
  truncated}` (bounded at 500, filename order) — the spec-side analogue of
  `roadmap_query mode:section_index` for spec discovery. `spec_log` with
  neither id nor path still refuses `bad_id` (it has no list semantics).
- **`roadmap_log` on pass-headings (`#### Pass N.M`) roadmaps** —
  append/append_batch/flip/flip_batch/annotate now WRITE the heading
  format via the pure `PassHeadingWrite` module (ANTS-2126), routed
  before the GFM counter/field guards. `op:"append"` needs a `pass` arg
  (`"43.5"`/`"43.5.B"`); status required, kind/source ignored,
  `.roadmap-counter` untouched. Flip/annotate locate by the synthesised
  `PASS-N-M` id or `headline`; a missing required arg is `bad_args`. Only
  `op:"create_section"` still refuses `format_mismatch`.

## `model_switch_stats`

(ANTS-1735, extended by ANTS-1889, sharpened by ANTS-1891.) Required
`caller_cwd`; ETag + `fields` opt-in. Aggregates the model-switch ledger
into avoided/regret ratios and pending-record counts (the trust signal
that gates §8 OQ-3 default-ON flip; never writes ledger/config).

- Envelope surfaces the live switcher config
  (`auto_model_switch_enabled`, `floor_tier`, `min_dwell_sec`) + `scope`
  echo so callers can tell "feature OFF" from "ON, no candidates yet"
  from "ON with measured outcomes"; accepts optional `scope:"project"`
  (default) or `"global"` to aggregate across all projects (ANTS-1889).
- ANTS-1891 — `regret_count` includes under-route harm;
  `measured_downgrades` excludes inconclusive 0-turn records (counted in
  new `inconclusive_count` instead); `clean_end_count` +
  `weighted_avoided` credit clean session-ends (no override / correction
  / under-route within ~10 min of session end) as ½ Opus turn avoided
  each, so end-of-task downgrades — the dominant ledger shape — are no
  longer invisible; headline withholds the ratio until a configurable
  floor of measured downgrades is reached and reads "calibrating (N/F
  measured)" below the floor (ANTS-1909 renamed the pre-floor phrase from
  "insufficient data"). Envelope readers should check
  `measured_downgrades > 0` before treating `regret_rate` as meaningful.
- ANTS-1894 (INV-12) — envelope additionally carries a slim
  `near_misses:{total_24h, dominant_blocker}` block (independent and
  meaningful from the first record); pass `mode:"near_misses"` for the
  full blocker breakdown.
- ANTS-1909 — the headline also carries the `dwell=Ns` parenthetical
  and, when the 24 h near-miss block is non-empty, appends "N near-misses
  in 24 h blocked by <dominant_blocker>" on both the no-switches and
  calibrating branches so the trust signal reads as "evaluating but
  blocked" rather than "feature did nothing".
- ANTS-1944 — `g.current` is actuator-anchored (repeat-suppression
  only): a pure `reconcileCurrentTier` helper in `modelautoswitch`
  overrides the stale transcript read with the actuator's last-injected
  tier, but ONLY when the clamped recommendation equals that tier
  (suppressing a re-fire, never reverting a user's manual `/model`).
  Provenance fields on `Result` (`currentModelFromCommand`,
  `currentModelTsMs`) let the helper distinguish a fresh command read
  (always wins) from an assistant-turn read that may be stale.
- ANTS-1941 — the trust signal counts only current-epoch records:
  `StatsConfig::minEpoch` is set to `kSwitcherEpoch` at both dispatch
  sites so pre-fix contamination is filtered out by the epoch boundary,
  not the age window (the age window can't evict recent pre-fix records).
  Envelope adds `min_epoch` + `excluded_pre_epoch_count` when
  epoch-filtered; callers reading `regret_rate` from the live MCP verb
  now measure only the current switcher behaviour. `statsForProject` /
  `statsEnvelope` and any `minEpoch=0` caller remain unaffected (all-time
  forensic view).
