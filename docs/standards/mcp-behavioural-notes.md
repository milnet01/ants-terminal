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

Applied by `mcp::appendReadHints` after the etag/`fields=` steps to a
successful read response — independent of which verb produced it, so they
appear in no single tool schema. **The two nudges do not share a size
gate**, which is the part callers get wrong:

- **`next_call_hint`** — when the response carries a fresh `etag` and the
  caller didn't send `etag_match`, nudges reusing it
  (`pass etag_match="<etag>" next call to skip an unchanged re-read`).
  **Fires at any body size** (ANTS-2180): a 304 on the *next* call saves
  the full body however small *this* slice was, and the highest-churn
  re-read targets — `read_region` / `file_outline` symbol slices across an
  edit loop — are usually under 4 KiB.
- **`leaner_call_hint`** — names the cheaper mode on that verb
  (`roadmap_query`→`headline_only`/`section_index`/`status:active`;
  `workspace_search`→ a narrower `lane=`/`glob=`, or
  `headline_only=`/`count_only=`/`files_only=` for a leaner row shape;
  `file_outline`→`filter`; else the generic `fields=`). **Gated on a body
  of ≥ 4 KiB** (`kLeanerThresholdBytes`) — a lean-mode hint only pays on a
  worthwhile body. The `roadmap_query` and `file_outline` branches
  additionally fire only when the caller isn't already on that lean path;
  the `workspace_search` branch has no such check.

Two traps in `leanerModeHintFor`'s branch list, both live:

- **The `workspace_search` branch no longer names `max_match_bytes`**, and
  a maintainer re-deriving the branch list from an older copy of this doc
  will put it back. ANTS-3548 made that clip default-ON (512 B), so
  nudging it buys nothing; the remaining levers are the narrower search
  and the leaner row shape above.
- **The `file_outline` branch names a `filter` argument its published
  schema does not expose** (`leanerModeHintFor` emits `pass
  filter=<substr>`; the verb's inputSchema has no such property). A caller
  cannot act on it. Tracked as ANTS-3839 — the defect is code-side, not
  here.

Both hints are presentation-only (added after the etag is computed, so
they never perturb the hash) and are skipped on 304s, refusals
(`ok:false`) and `fields=`-narrowed calls. **Each is also emitted at most
once per process per tool** — `claimHint`'s test-and-set latch (ANTS-3550;
the key is `claude.mcp_hint_latch`, catalogued with its default in
[`mcp-config-keys.md`](mcp-config-keys.md)). So a hint that appears once
and never again is working as designed, not a bug; a test or retry path
written against "every qualifying response" will be wrong on the second
call.

## Tabular (columnar) array encoding (ANTS-2090)

Opt-in per-call `encoding:"tabular"` on the list-shaped read verbs —
`roadmap_query`, `workspace_search`, `file_outline`, `find_sources`,
`find_caller`, `codebase_index`, `docs_index`, `changelog_query`. The
dispatcher runs
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
JSON).

**But tabular + offload costs you row paging, and that is not obvious from
either feature on its own.** `dominantArrayKey` does not recurse into
nested objects, so a `{__cols__,__rows__}` member is never selected as the
dominant array — a tabularised body that spills therefore carries no
`head_rows` preview and no `head_rows_key`, and `read_spill` **row mode
refuses `not_array`** on it. Byte-page a tabularised spill
(`offset`/`max_bytes`); reach for row mode only on an untabularised one.

Spec + cold-eyes log: `docs/specs/ANTS-2090.md`.

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

**Offload envelope** (`offloaded:true`): the base fields are `handle` (the
64-char sha256), `head` (a leading byte-prefix of the body),
`head_truncated`, `bytes` (the full body size), and a `hint` — all emitted
unconditionally. When the body's dominant field is a row-shaped array,
ANTS-3538 adds a typed **`head_rows` preview** — `head_rows` (the first few
whole elements), `head_rows_key` (the array's field name),
`head_rows_truncated`, `row_count` (the array's total length) — so the
caller sees real rows, not just a clipped byte-string, and the `hint`
additionally advertises row paging (it does so whenever a dominant array
exists, even when the rows are too large to fit the preview budget). A body
with no row-shaped array carries only the base fields and a
byte-paging-only `hint`.

**`read_spill` — byte mode (default).** `offset`/`max_bytes` return
`{ok, content, offset, bytes, total_bytes, truncated}`; page forward by
re-calling with `offset` ← the returned `offset + bytes` until
`truncated` is false. **`bytes` here is *this page's* length**, not the
offload envelope's `bytes` (which is the whole body) — the whole-body
figure in this envelope is `total_bytes`. Sizing a page loop off the wrong
one of the two fetches everything in one go. Byte mode never parses the body, so it works on any
spill regardless of size or shape.

**`read_spill` — row mode (ANTS-3545).** A numeric `row_offset`/`row_count`
routes to the parsed row-pager and returns
`{ok, mode:"rows", key, rows, row_offset, total_rows, truncated}` — clean
element boundaries over the dominant array (same key as `head_rows_key`).
**Either key alone routes to row mode** — `row_offset` alone, `row_count`
alone (which starts at row 0), or both; omitting *both* stays in byte mode.
A `row_count` of `0`, or omitted with `row_offset` present, serves the
default page (`kSpillRowsDefault`, 100 rows). A **negative** `row_offset`
or `row_count` never reaches the pager — it is refused `bad_args` first, so
"≤ 0" is two different answers and not one. `row_offset` past the end
yields an empty non-truncated page.
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
  to page: a scalar-only body, a **bare root array**, or a fully-tabular
  `{__cols__,__rows__}` body. The `hint` says byte-page it instead. Row
  mode only.

`raw:true` suppresses offload entirely (see its bullet under § Read /
incremental verbs), so an Edit-from-output caller gets true bytes rather
than a head+pointer.
Specs: `docs/specs/ANTS-2094.md` (offload + byte paging). ANTS-3538
(`head_rows` preview) and ANTS-3545 (row mode) shipped without a spec of
their own — those bare ids are ROADMAP entries, not missing links.

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
Claude Code **v2.1.121+** honours (as of 2026-08) to keep those tools
always-loaded
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
- **`co_change_family`** (ANTS-3368) — every edit site of ONE settings
  field, grouped by file. Matches on the longest run of the stem's words
  **contiguous in both** the stem and the candidate, not on the name — so
  `setMcpEnabled` belongs to the `claudeMcpEnabled` family while
  `mcpTraceEnabled` does not. That is what reaches the affixed derived
  names (`setX`, `m_X`, `XChanged`) and the JSON string key, none of which
  `find_caller` can see: its anchor is `\b<sym>\s*\(`, so it matches call
  sites only and `_` being a word character excludes `m_…` outright.
  **`min_run` widens the SCAN, not just the filter** — at the default the
  rg pattern alternates adjacent word *pairs* and can never produce a
  one-word match, so `min_run:1` is what reaches a site sharing a single
  word (`audioLod` from `lodEnabled`), at the cost of near-full-text
  scanning on a common word. `role` is **lexical, never semantic** —
  `json_key|member|mutator|signal|type|reference` and nothing else; an
  "apply sink" is a mutator on a class that happens not to be the settings
  store, which needs type resolution this scanner does not have, so read
  the *path* for that. One row per `(path, line)`: a line matching several
  stems is emitted once, owned by the longest run. When `max_sites` binds,
  the sites kept are the highest `run_len` — never the first N in scan
  order — and `truncated` is set; `truncated` also covers an exhausted rg
  budget, while `rg_failed` is reserved for a scanner that did not run.
  **Scans the whole repo** (minus `.gitignore`), unlike `find_sources`,
  because a config key's `docs/` and `CLAUDE.md` mentions are co-change
  sites. Spec `docs/specs/ANTS-3368-co-change-family.md`.
- **`roadmap_query` `mode:"report"` (ANTS-4501)** — the roadmap's totals,
  lifecycle split and throughput per day / week / month / year, over the
  STORE and not the markdown. Read-only: it writes no row, no file, no
  migration and allocates no id, so it can never freshen the rows it
  summarises. **Every bucketed figure ships a `coverage` block over the same
  population, and reading it is not optional** — the period figures are
  computed from the `created` / `shipped` columns, which are stamped only
  from 2026-08-20 and are NULL on every row filed before that, so on a
  corpus that has not been backfilled a "closed this month" is a count over
  a few percent of the items and would otherwise read as a total.
  `roadmap_log op:"backfill_dates"` is what fills the rest. `open` is an
  ENUMERATION — planned + in-progress + considered — never
  `status != 'shipped'`: the schema admits a fifth value `dropped`, so the
  two disagree on any project that has one. Every median carries the
  `sample` it was computed from, which is smaller than the population it
  looks like. `scope:"all"` sums every registered project — the one view no
  single ROADMAP.md can give.
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
- **`downshifted` (ANTS-3543 `roadmap_query` + `workspace_search`;
  ANTS-3576 `changelog_query`)** —
  on the **auto-truncate path**, if a large list would drop
  its tail the server first projects the WHOLE list to its lean shape
  (`roadmap_query` → `{id, status, headline_oneline, section_slug}`;
  `workspace_search` → `{file, line, headline}` + `also_at`;
  `changelog_query` entries → `{version, category, ids, text_oneline}`) and
  re-measures, so a scanning caller keeps every row's identity instead of
  a silent cut-off. Emits truthy-only `downshifted:true`; the drop signals
  (`truncated`/`next_offset`/`results_dropped`) are **cleared** when the
  lean list is complete and only reappear if even the lean list overflows.
  `workspace_search` also sets `headline_only:true` — so it can appear on a
  response the caller did **not** request (meaning widens from "you asked
  for lean rows" to "rows were leaned to fit"). Pure helpers:
  `PaginationEngine::pageBullets`'s `RowProjector` arg and
  `RemoteControl::downshiftMatches` (both socket-free unit-tested).
  **The opt-out is per verb and the argument names are not shared** — one
  sentence in this verb's vocabulary would be wrong for the other two:
  `roadmap_query` turns the projector off under `mode:"headline_only"` or
  `include_body`, and an explicit `limit` selects fat paging;
  `changelog_query` likewise under `headline_only` or `include_body`;
  `workspace_search` only under `headline_only:true` — it has no `limit`,
  `include_body` or `mode` argument at all, so its lean-row flag is the
  whole opt-out. Spec `docs/specs/ANTS-3543.md`.
- **`read_log`** — filters a log file (the Ants debug log or a
  `caller_cwd` path) to matching lines via the pure `ReadLog::filter`
  helper; streaming drop-oldest byte cap + `since_cursor` incremental
  tailing (ANTS-1855).
- **`read_region`** — returns an exact line range or a named symbol's
  body (resolved via the flat `file_outline` scanner — class /
  `Class::method` forms) from a project file via the pure
  `ReadRegion::extract` helper; ETag-304 free re-read + head-anchored
  incremental byte cap; caller_cwd-Required (ANTS-2021). Its
  `call_sequence:true` facet has its own bullet further down this section
  (ANTS-2157) — this bullet is not the whole `read_region` entry.
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
  existing dir; an all-dropped key falls back). **An empty summary says WHY
  (ANTS-4419):** alongside `empty:true`, an `empty_reason` of
  `project_not_registered` (indexable source exists but not under the walked
  roots, and no `.ants/project.json` declares where it lives) /
  `declared_roots_hold_no_source` / `no_indexable_source`, plus an
  `empty_hint` and an `empty_detail` carrying `ProjectSettings::detect()`'s
  measured per-directory counts. On the first two reasons the index is
  INAPPLICABLE rather than authoritative — ANTS-2148's bare boolean was read
  as "no code here" and answered with a grep fallback. A non-empty summary
  gains none of these fields. The same file's
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
- **`workspace_search match_wrapped:true` (ANTS-4547)** — matches a
  quotation that is HARD-WRAPPED in the file. A run of whitespace in
  `pattern` matches a run of whitespace *and markdown blockquote markers*
  in the text; `line` reports where the matched span STARTS and each
  row's `text` is folded to one line. Off by default, and LITERAL only —
  with `regex:true` it refuses `bad_args`, because re-flowing a caller's
  regex changes what their own pattern means. Why it exists: prose here
  wraps at ~70 columns, so a line-oriented search misses text that is
  present and exact, and a review gate that DISMISSES a finding whose
  quote it cannot locate then ships the real defect. Normalisation is
  TWO-SIDED — paste the quotation with its own newlines, indentation and
  `>` markers and it still matches unmarked text; normalising only the
  file was the bug that sat in the hand-rolled `sed | tr | grep` version
  of this for months. The rule itself is `src/wrapmatch.{h,cpp}`, shared
  with `roadmap_log op:"amend_body"` (ANTS-4550) so the two verbs cannot
  answer one quotation differently.

## Write / edit verbs

**A verb family is documented in one section, so a few read verbs live in
this one** — `feedback_query` shares a bullet with `feedback_log`,
`spec_query`'s list mode sits beside `spec_log` because half its contract
is what `spec_log` refuses, and `project_query` follows `project_settings`
as the other half of the project surface. Look a verb up by name, not by
which heading you expect it under.

- **`apply_edits`** — applies N `{path, old, new}` edits across M project
  files in one atomic-per-file call (pure `ApplyEdits::applyToContent` +
  `QSaveFile` + `fsyncParentDir`); per-edit `skipped[]` (not_found /
  ambiguous / too_large / commit_failed); fail-closed `bad_path` on root
  escape; caller_cwd-Required (ANTS-2022). **Near miss on `not_found`
  (ANTS-4418):** when the file's text differs from `old` only in spacing, the
  skip row also carries `candidates:[{line,text}]`, `near_miss_line` and a
  `hint` — the same field names `read_region` section-mode and `roadmap_log`'s
  bullet locators already use, so one caller path handles all three. Bare
  `not_found` is equally consistent with "the text is gone", "wrong file" and
  "you are one space out", and this is the verb where the third is commonest.
  Reported only on a UNIQUE whitespace-class miss for a SINGLE-LINE `old`: two
  candidates cannot say which to retry, and a multi-line alignment would be a
  confident guess. Retry with the reported `text` verbatim, or with the
  `start_line`/`end_line` form for that line.
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
  10 MiB + 100k-instruction hook (inherited), plus a wall-clock and an
  output cap — `claude.mcp_project_query_timeout_ms` and
  `claude.mcp_project_query_result_cap_bytes`, whose defaults and clamps
  are owned by [`mcp-config-keys.md`](mcp-config-keys.md) and deliberately
  not restated here. Threading: each call runs on a **fresh ephemeral
  worker thread** with a bounded join (timeout + 250 ms grace); a snippet
  stuck in an uninterruptible C call is **detached** (returns
  `query_timeout`, worker held as a zombie until process exit) so the GUI
  is never frozen beyond the budget. Marshalling (§2.4): nil→null, bool,
  number (integer subtype preserved; non-finite → `query_error`), string
  (invalid UTF-8 → `query_error`), array-like table → array, string-keyed
  table → object (both recursive, ≤ 32 levels — the bound also catches a
  circular table); function/userdata/thread/non-string-key → `query_error`.
  Feature-gated by `claude.mcp_project_query_enabled` (off →
  `query_disabled`, checked before arg validation; the default is in
  [`mcp-config-keys.md`](mcp-config-keys.md)); under the master
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
  `PASS-N-M` id or `headline`; a missing required arg is `bad_args`. The
  other three ops each answer differently, and only one of them refuses
  `format_mismatch` (ANTS-3837): `op:"create_section"` refuses
  `format_mismatch` (`rcPassHeadingsWriteRefusal`); `op:"amend_body"`
  refuses `unsupported_format` (ANTS-3406 — the distinction between the two
  codes is [`mcp-error-codes.md`](mcp-error-codes.md)'s to state, and is
  not restated here); and `op:"bundle_row"` **writes normally** — it appends to a
  Markdown table under a named section and never parses bullets, so the
  roadmap's bullet format never reaches it (ANTS-1691).
- **A `note` is prose (ANTS-4549)** — under `op:"annotate"`, `op:"flip"`
  and `op:"flip_batch"` a note may name a trailer key only with the label
  first on its line, the shape the render writes a declaration in.
  Mid-line it refuses `body_shadowed`. The note is appended to the body
  and § 2.6 re-derives every trailer column from that new body, so a bare
  `Kind:` in a sentence was read as a declaration and its following words
  written to the column — caught on AI_Prompts/AIPR-0033 only because the
  store's CHECK constraint refused an invalid kind, which is luck rather
  than a guard: an accepted kind would have written silently, and the
  other four columns have no CHECK at all. Position rather than
  `op:"append"`'s value comparison, because a note supplies no column to
  compare against and blanket-refusing would close § 2.6's own remediation
  route — a `Layman:` note is the only way to fill that column on a
  migrated item, and the render gates on it.
- **`roadmap_log op:"amend_body"` matches across a hard wrap (ANTS-4550)** —
  `old_text` is exact apart from its whitespace runs, which span a
  line break, so a phrase straddling the ~70-col wrap is amended in ONE
  call rather than by N single-line calls whose joint result is checked by
  nothing (the hazard ANTS-4097 could only echo). The wrapped pass runs
  ONLY after the exact pass finds nothing, so no previously-succeeding
  call changed; `body_match_ambiguous` is enforced on whichever pass
  matched. A wrapped match re-flows the lines it spanned into one and the
  envelope carries `wrapped_match:true`. Both the markdown and the store
  path go through `WrapMatch::patchOnce`, differing only in indent policy:
  markdown keeps ANTS-3752's continuation indent for a multi-line
  `new_text`, the store does not, because it holds the body as an
  unindented residual the render indents on the way out.
- **`roadmap_log op:"backfill_dates"` (ANTS-4501)** — a ONE-OFF that walks
  the project's git history and fills the `created` / `shipped` columns for
  the rows predating forward stamping. Not a `roadmap_query` mode, and
  deliberately: it writes, and that verb's report is pinned read-only. Per
  project (`caller_cwd`, like `roadmap_migrate`), because it needs that
  project's repository while the store is machine-global. Dates come from
  each commit's AUTHOR date — the committer date moves under every rebase,
  so a rewritten history would otherwise change every figure downstream.
  **It never overwrites a non-NULL date**, so it is re-runnable and a hand
  correction survives; it never invents one for an id no commit showed (those
  are counted in `undated_count`); it skips the `shipped` COLUMN for any id
  whose current status is not shipped, so a reopened item is not silently
  re-closed from the ✅ still sitting in its history; and it does not touch
  `last_modified` at all. `dry_run` reports the counts the real run would
  write. Measured on Ants Terminal 2026-08-20: 1525 revisions in 3.9 s,
  2179 of 2179 items dated, 0 undated. Refusals: `not_a_git_repo`,
  `project_not_registered`, `git_failed`.
- **`roadmap_log` on a store-migrated project (ANTS-3809)** — all eight ops
  (`append`, `append_batch`, `flip`, `flip_batch`, `annotate`,
  `amend_body`, `create_section`, `bundle_row`) **mutate the store and
  re-render** instead of splicing markdown; `RoadmapRender::render()`
  writes every byte of the roadmap files. "Store-migrated" is the pair
  ANTS-3793's `RoadmapSource::migratedProject()` tests — a store row *and*
  an `ants-v1` emoji-format roadmap
  ([`roadmap-data-model.md`](roadmap-data-model.md),
  [`roadmap-format.md`](roadmap-format.md) § 3.5.1). A project with a store
  row but a GFM roadmap keeps the GFM splice path, one with a pass-headings
  roadmap keeps that format's own path — the bullet above, which now
  accounts for all eight ops — and a project with no store row is
  unaffected. Differences a
  caller sees:
  - **The render owns the WHOLE file, and every write now says what that
    cost.** A hand-edit the store does not model — the preamble above the
    first heading is the case with no verb at all — is reverted by the next
    op, and used to be reverted in silence under an `ok:true` envelope
    (ANTS-4465). `items_rendered` reads as reassurance and is not: it counts
    items, not content, so a store that has fallen behind the file passes it
    exactly as a fresh one does (ANTS-4462). So `commitAndRender()` renders
    the store as it stood **before** the mutation and diffs that against the
    file, reporting `discarded_external_edits` (bool) plus
    `discarded_edit_lines` on the true arm — `would_*` on a dry run, per
    ANTS-4463's tense rule. Both are **absent** when nothing measured the
    file, which is not the same as clean. Three things the shape rules out:
    diffing the *post*-mutation render instead (it differs from the file by
    the change the call was made to write, so every healthy write reports);
    comparing mtimes (the sequence writes the store and *then* the file, so
    the file is always newer and every project reads stale); and refusing on
    drift (one hand-edit anywhere would brick every op, which is the
    `render_gate_unmet` shape both items were filed against). Expect one true
    report per project on the **first** write after a migration: the file
    still carries the author's bytes wherever the store keeps a canonical
    form — a table separator, say (ANTS-3832) — and that publish really does
    overwrite them.
  - **Envelope**: `line`, `lines`, `bytes` and `bytes_written` are all
    dropped, and `files_written` / `items_rendered` come from the render.
    On the markdown path `line` **would be** `firstLine + 1`, and ANTS-3793
    INV-2 declares `firstLine` / `lastLine` to be 0 on the store path — so
    every bullet would report line 1, which is why the field is dropped
    instead of sent meaningless.
  - **`dry_run` commits nothing on either path, but the store path can
    refuse instead of previewing.** The preview is produced by the same
    validating render, and the gate is checked *before* the dry-run return,
    so on a project with an unmet gate a `dry_run` call refuses
    `render_gate_unmet` and returns no would-be bullet — where the markdown
    path has no equivalent gate and previews whenever the op resolves at
    all. Otherwise the would-be result comes back and neither the store nor
    the file is touched.
  - **`line_range` has three outcomes, not one.** It exists only inside
    `flip_batch`'s `locators[]`, and it is last in locator precedence. A
    range that is the **effective** locator (no `id` / `anchor` /
    `headline` beside it) is refused `locator_unsupported`
    (ANTS-3809 § 2.4) — the store fills `firstLine`/`lastLine` with 0, so a
    range from line 1 would match *every* bullet. One accompanied by an
    `anchor` still refuses `bad_op_combo`, the `ants-v1` format refusal
    that runs ahead of the store dispatch. One accompanied by an `id` or
    `headline` is simply unused — those win on precedence. Both refusals
    are **per locator** and land in `skipped[]`, so the batch's other
    locators still apply.
  - **Trailer columns follow the body — for the columns the request did not
    itself supply.** `flip` / `annotate` / `amend_body` re-derive `kind` /
    `layman` / `source` / `lanes` / `evidence` from the body they just
    wrote (ANTS-3809 § 2.6 / INV-4); a column the request *did* supply
    keeps the supplied value and is not re-derived. `append` /
    `append_batch` instead refuse `body_shadowed` when a supplied column
    and the body in the same request disagree on that key's value
    (ANTS-3809 § 2.5). `append` refuses the call; `append_batch` refuses
    **per bullet**, dropping that one into `skipped[]` — evaluated before
    ids are assigned, so a dropped bullet leaves no gap in the batch's
    contiguous id run.
  - **Id allocation** reads the store's `id_high_water` row, still floored
    to the committed corpus ([`roadmap-format.md`](roadmap-format.md)
    § 3.5.1); `.roadmap-counter`
    is neither read nor written. `id_strategy:"stable_prefix"` consults
    neither carrier. This is the cutover's **interim** rule — ANTS-3794
    replaces the corpus floor with the published export.
  - **`bundle_row`** is a read-modify-write of the section's first
    `kind='table'` element's canonical-JSON payload (ANTS-3756 INV-24),
    creating exactly one when the section has none.
  - **New refusal codes** — `render_gate_unmet` (a public open item with no
    `Layman:` line blocks the render — the gate is per *project*, so the
    caller's own bullet may be blameless), `render_failed` and
    `store_failed`, plus `locator_unsupported` and `body_shadowed` above,
    and `write_failed` reused for a committed store whose publishing
    render did not land. `render_gate_unmet`, `render_failed` and
    `store_failed` write nothing, rolling the transaction back wherever
    one had opened (ANTS-3809 INV-1);
    `write_failed` leaves the store committed and the file stale-behind,
    recoverable by re-running the render — any later successful op on that
    project does it, since every op re-renders the whole project.
    `render_gate_unmet`'s envelope carries `gate_failures[]` naming the
    offending ids, so a blameless caller is not left guessing which items
    block it. **ANTS-4141 adds `render_would_drop`** at the same seam and
    with the same write-nothing, roll-back semantics: the write path renders
    the store's whole document, which assumes the store is a superset of the
    file, and where that is false a bullet the store never imported is
    absent from the render's output and the publish deletes it. The dry
    render's id set is compared against the ids the files it would rewrite
    hold today, and the ids are named in `error` rather than an array —
    capped at 25 with a `+N more` tail, because the remedy is one import and
    not a per-id edit. Full entries in
    [`mcp-error-codes.md`](mcp-error-codes.md). Spec
    `docs/specs/ANTS-3809-roadmap-write-half.md`.

## `model_switch_stats`

(ANTS-1735, extended by ANTS-1889, sharpened by ANTS-1891.) Required
`caller_cwd`; ETag + `fields` opt-in. Aggregates the model-switch ledger
into avoided/regret ratios and pending-record counts (the trust signal
that gates the default-ON flip of
[`docs/specs/ANTS-1735.md`](../specs/ANTS-1735.md) § 8 OQ-3; never writes
ledger/config).

This verb gets its own H2 rather than a bullet under § Read verbs because
its envelope has accreted six revisions' worth of contract; the shape is
deliberate, not an inconsistency.

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

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-06 | 2 (same doc, independent, cold) | 0 / 3 / 6 / 8 / 1 | First gate on this document, triggered by ANTS-3837's edit. Both lanes independently led on the same three defects, all in the dispatch-wide nudges section and all pre-existing: the >4 KiB gate was stated for **both** nudges when only `leaner_call_hint` carries it (ANTS-2180), `workspace_search`'s tip still named `max_match_bytes` after ANTS-3548 retired it, and the once-per-process `claimHint` latch (ANTS-3550) was absent from an otherwise-exhaustive skip list. Blast radius: `unsupported_format` and `claude.mcp_hint_latch` were each named here but missing from the standard that owns them, so a row was added to `mcp-error-codes.md` and to `mcp-config-keys.md`. Dimension tally: dim 2×6, dim 12×4, dim 5×3, dim 4×3, dim 7×2, dim 6×2, dim 11×2, dim 1×1, dim 8×1. Dismissed on evidence: the `mode:"bundles"` envelope really does carry `total_bundle_count`/`bundles_omitted` (`remotecontrol.cpp:4545-4546`), so this doc was the correct side; and the read verbs filed under § Write are deliberately co-located with their write siblings — the organising rule was stated rather than the bullets moved. Surfaced as code-side: ANTS-3839 (`file_outline`'s `filter` tip names an argument the schema has no property for). |
| 2 | 2026-08-06 | 2 (same doc, independent, cold) | 0 / 4 / 6 / 8 / 1 | **None of loop 1's three defects resurfaced** — the cold re-read is what proves those fixes held. Both lanes again led on the same defect: `read_spill`'s row-mode paragraph said a `row_count` "≤ 0" serves the default page while the refusal list said a negative row arg is `bad_args`, and both are in the document — a caller cannot write the call correctly from it. Negative is refused before the pager (`remotecontrol.cpp`), `0` and omitted serve the default (`mcpspill.cpp`), so the two answers were collapsed into one clause. Second shared find: `encoding:"tabular"` was documented as composing freely with `offload`, but `dominantArrayKey` never selects a tabular member, so a tabularised spill loses `head_rows` **and** `read_spill` row mode refuses `not_array` — a caller combining two documented token-savers hit that with nothing to explain it. Third: `changelog_query` was missing from the closed list of tabular-capable verbs while the same document treats it as one 150 lines later. Dimension tally: dim 5×5, dim 1×4, dim 4×4, dim 7×3, dim 2×2, dim 12×1, dim 6×1, dim 11×1, dim 8×1, dim 14×1. **Seven of nineteen were collateral from loop 1's own edits** — two restated a default the catalogue owns (the exact policy this doc states elsewhere), one referred to "the table" in a document with no table, and one over-claimed an organising rule the `read_region` split contradicts; all fixed. Dismissed on evidence: the row-paging `hint` is gated on the parse cap after all, so it cannot advertise a mode that would refuse (lane misread "preview budget" as the 1 MiB cap). **Stopped at loop 2, not converged** — see the deferred tail on ANTS-3837's bullet. Lane B independently observed what the stop rests on: by shape this is a **reference/table doc**, which `/cold-eyes`' own scope table routes to `/doc-lint`, not to a judgement gate. |
