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

## Read / incremental verbs

- **`get_scrollback`** — since-cursor incremental mode (ANTS-1500).
- **`roadmap_query`** — recognises ants-v1 / github-task-list /
  pass-headings formats (ANTS-1530). `mode:"bundles"` (ANTS-1922)
  groups the active subset (📋/🚧, id-bearing) into thematic
  work-bundles by headline-token Jaccard (≥0.50, ≥2 shared tokens,
  union-find / transitive closure) — the one-call "next bundle of
  related to-dos?" view. Active-only: a passed `status` is ignored;
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

## Write / edit verbs

- **`apply_edits`** — applies N `{path, old, new}` edits across M project
  files in one atomic-per-file call (pure `ApplyEdits::applyToContent` +
  `QSaveFile` + `fsyncParentDir`); per-edit `skipped[]` (not_found /
  ambiguous / too_large / commit_failed); fail-closed `bad_path` on root
  escape; caller_cwd-Required (ANTS-2022).
- **`feedback_query` / `feedback_log`** — read/write the
  `*_Ants_MCP_Feedback.md` files via the pure `FeedbackFile` module
  (delta parse + block render; ANTS-1961/1962); suffix-guarded on
  `_Ants_MCP_Feedback.md`; append-only at EOF.
- **`spec_log`** — writes a spec's Status line / cold-eyes loop log /
  `INV-N` via the pure `SpecLog` module (`op:"set_status"` /
  `"append_loop"` / `"append_inv"`), never renumbering; reuses
  `spec_query`'s id routing (ANTS-1963).
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
