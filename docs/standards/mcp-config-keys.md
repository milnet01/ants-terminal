# Ants-MCP config keys (ANTS-3429)

Config-file / Settings keys for the Ants-MCP integration. Moved out of the
project `CLAUDE.md` preamble (ANTS-3429, following the ANTS-1292 module-map and
ANTS-2088 behavioural-notes relocations) so the always-loaded session preamble
stays lean — read on demand. Keys live in
`~/.config/ants-terminal/config.json` unless noted "Settings" (also exposed in
the Settings dialog).

## Master gate (ANTS-1901)

`claude.mcp_enabled` (bool, default true), Settings → General → "Enable Ants
MCP integration". Sits hierarchically above every per-feature key below. When
false, the MCP socket isn't bound at launch, the orientation hook is removed,
the auto-switcher stands down, and every verb refuses with `mcp_disabled`
(turning it off is honoured immediately via the dispatcher guard; turning it
back on takes effect on the next launch).

## Feedback corpus root (ANTS-4471)

`claude.mcp_feedback_root` (string, default empty). The directory holding the
shared `*_Ants_MCP_Feedback.md` corpus.

**It does two different things, and the second is the one that matters most.**
On a MISS it is searched alongside the derived directory, so `candidates` can
name the real file. And on a `feedback_log` write whose path would be DERIVED,
a key naming a directory that holds a corpus REDIRECTS the derivation there and
succeeds — it does not fall through to a refusal. A user who declared the
corpus has already answered the question a refusal would ask. The full
resolution order is `mcp-feedback-files.md` § File location & name, which owns
it; this entry must not restate the order.

**And by `session_orient`'s `feedback_pending` block (ANTS-4896).** That block
scanned the parent of the project root alone, so a corpus this key had already
relocated reported `files_scanned:0` — indistinguishable from an empty inbox.
It now scans both roots, dedupes by canonical path, reports the declared corpus
as `shared_root`, and carries `searched` so an empty scan says where it looked.

**Empty means "the parent of `caller_cwd`"** — the rule the scan already used,
and the right answer for every project whose checkout sits beside the corpus.
Both directories are searched when the key is set, never one instead of the
other, so configuring it cannot break the common case. Results are deduped by
absolute path.

The key exists for the one shape the derived rule cannot reach: a project on a
different filesystem branch from the corpus. Reported against `~/.claude`,
whose derived path is `/home/ants/.claude_Ants_MCP_Feedback.md` — so the
searched directory was `/home/ants`, which holds no feedback file — while the
real file is `claude_config_Ants_MCP_Feedback.md` under a scripts tree. Two
mismatches at once: the corpus is not the caller's parent, and the leaf
`.claude` is not the file's `claude_config`. Nothing in the path connects them,
so the root has to be told rather than derived.

A config key rather than the roadmap store's `project` table (which does hold
every registered root): this is a **hint on a miss**, and coupling the feedback
verbs to the roadmap store to produce one buys a dependency for a convenience.
Hardcoding any absolute path would be wrong on every other machine.

When the search finds nothing, the `not_found` envelope now carries `searched`
(the directories actually looked in) and a `hint` naming this key — so the
answer is actionable rather than terminal.

## Autonomous model switcher (ANTS-1735 §2.7)

Single Settings toggle "Let Ants pick the Claude model for me" + config-only
tuning keys:

- `claude.auto_model_switch` (bool, default false) — master gate.
- `claude.auto_model_min_dwell_sec` (int, default 90, clamp [30, 1800]).
- `claude.auto_model_floor` (`"haiku"`|`"sonnet"`, default `"haiku"`).
- `claude.auto_model_nudge_shown` (bool, default false) — first-run opt-in
  nudge latch (§8 OQ-3).
- `claude.auto_model_toast_enabled` (bool, default true) — ANTS-1893
  switch-event surfacing: status-bar toast on each live auto-switch.
- `claude.auto_model_chip_pulse_enabled` (bool, default true) — ANTS-1893:
  per-tab model chip pulses for ~0.6 s on each switch.
- `claude.auto_model_undo_enabled` (bool, default true) — ANTS-1893: 10 s
  "Undo: back to <Tier>" button in the status bar after each switch; click
  seeds the ANTS-1890 cool-down so the same pick won't immediately re-fire.
- `claude.auto_model_confirm_user_switch` (bool, default true) — ANTS-1951:
  auto-confirm CC's "Switch model?" dialog for user-typed `/model` commands too
  (the auto-switch/chip/undo paths already confirm via
  `performModelSwitchHandshake`). Runs on the 2 s tick independently of the
  master gate; stands down while an Ants-initiated handshake owns the dialog;
  sends only ENTER (no continuation prompt).
- `claude.auto_model_debug` (bool, default false) — ANTS-1976: toggleable
  per-tick switcher trace. When true, force-enables the DebugLog `autoswitch`
  category so every gate evaluation + switch decision is logged to `debug.log`
  (state, composerEmpty, composerStaleMs, toolUseMs, current/target tier, act,
  tier, blockedBy[], reason, epoch). Env equivalent: `ANTS_DEBUG=autoswitch`.
  Off = single bit-test, no cost.

## Result offload (ANTS-2094, observation masking)

When a large read result is spilled to a content-addressed cache file and a
`{offloaded:true, handle, head, …}` envelope returned instead; re-read the full
body via the `read_spill` verb.

**ANTS-3538 — structured preview for array bodies.** When the spilled body is a
JSON object with a dominant array member (`workspace_search` → `matches`,
`roadmap_query` → `bullets`, …), the envelope *additionally* carries
`head_rows` (the first K complete rows, parseable JSON — not a byte-cut),
`row_count` (the array's full length), `head_rows_key` (which array), and
`head_rows_truncated`. So a caller needing only the first rows or the count
answers from the envelope without a `read_spill` round-trip. The preview is
capped at `mcp_offload_head_bytes` and stays strictly smaller than the raw body
(INV-9); it is omitted (byte-prefix `head` only) for non-object / no-array /
tabular-encoded / over-1-MiB bodies, or when nothing fits the budget. The
pre-3538 `head`/`head_truncated` fields are unchanged.

- `claude.mcp_offload_large_results` (bool, default **true** since the
  2026-06-25 fast-follow) — session default for the per-call `offload` arg
  (per-call wins). Default ON per the "token-savers default ON" rule, now that
  `read_spill` round-tripping is field-proven; fails open (untrimmed delivery
  resumes if the scratch cache is unwritable). Exposed in Settings → General
  ("Offload huge Ants MCP replies to a preview + pointer").
- `claude.mcp_offload_threshold_bytes` (int, default 16384, clamp
  `[4096, 1048576]`) — minimum final body size to offload. Config-file-only.
- `claude.mcp_offload_head_bytes` (int, default 2048, clamp `[256, 16384]`) —
  preview head size; offload only fires when the body also exceeds this
  (spilling something that fits in the head saves nothing). Config-file-only.

## Tabular (columnar) array encoding (ANTS-2090)

The opt-in per-call `encoding:"tabular"` arg on the list-shaped read verbs
(roadmap_query, workspace_search, file_outline, find_sources, find_caller,
codebase_index, docs_index) repacks each top-level array-of-objects into a
columnar `{__cols__,__rows__}` form, dropping the per-row key repetition
(30–60% smaller on big lists). Per-call only (no config key / session default —
it changes a field's shape, so the caller must decode it); never costs bytes.
Decode recipe + invariants in
[`mcp-behavioural-notes.md`](mcp-behavioural-notes.md).

## Advisory-hint latch (ANTS-3550)

- `claude.mcp_hint_latch` (bool, default **true**) — the "already-taught"
  latch on the two dispatch-wide read nudges (`next_call_hint` /
  `leaner_call_hint`). While true each nudge is emitted at most once per
  process per tool, then suppressed — token-saving out of the box, and the
  reason a hint a caller saw once never reappears. **Config-file-only, with
  no Settings toggle** (an advanced escape hatch): set it false to have the
  tips emitted on every qualifying response. Published to the
  `mcp::setHintLatchEnabled` module flag at config load and on external
  reload, so an edit takes effect live. Emission rules — including the
  gates the latch sits behind — are in
  [`mcp-behavioural-notes.md`](mcp-behavioural-notes.md).

## `project_query` (ANTS-2093, the code-execution token-saver)

The `project_query` MCP verb runs an agent-supplied **read-only** Lua snippet
server-side over `caller_cwd`'s files and returns only its result (host
surface: `project.read`/`list`/`root` + string/table/math/utf8; sandboxed
read-only, FS-confined, mem/time/output-capped, run on a detachable worker so a
pathological snippet can't freeze the GUI). Reuses the plugin Lua sandbox;
compiled out under `-DANTS_LUA_PLUGINS=OFF`.

- `claude.mcp_project_query_enabled` (bool, default **true**) — feature gate,
  Settings → General ("Let Ants run read-only project queries for Claude").
  Off → `query_disabled` (checked before arg validation); under the master
  `claude.mcp_enabled` gate, which takes precedence (`mcp_disabled`).
- `claude.mcp_project_query_timeout_ms` (int, default 1500, clamp
  `[100, 5000]`) — wall-clock budget; the ceiling bounds the worst-case GUI
  stall (join = budget + 250 ms grace). Config-file-only.
- `claude.mcp_project_query_result_cap_bytes` (int, default 65536, clamp
  `[1024, 1048576]`) — output cap; over-cap → `result_too_large` (aggregate
  harder). Config-file-only.

## Tokens-saved chip + aggregate (ANTS-3572)

Surfaces the `TokenUsageEngine` (ANTS-1284) session savings as a live status-bar
pill plus a persisted month / YTD / all-time total (also on the `token_usage`
verb's `month_saved` / `ytd_saved` / `lifetime_saved` / `monthly[]` fields). See
[`../specs/ANTS-3572.md`](../specs/ANTS-3572.md).

- `claude.tokens_saved_chip_enabled` (bool, default **true**) — Settings →
  General ("Show \"tokens saved\" pill in the status bar"). Gates the widget
  only; the ledger keeps accruing when off (re-enabling shows the true total).
- `claude.tokens_saved_monthly` (object `{"YYYY-MM": number}`) — per-month gross
  saved buckets, pruned to the most-recent 24 keys. Config-file-only; written by
  the fold at session reset / app quit (a **store-only** setter — no per-call
  write).
- `claude.tokens_saved_lifetime` (number) — exact all-time gross saved, never
  pruned. Config-file-only; store-only setter.
- `claude.tokens_saved_since` (string, ISO date) — first-fold date, seeded once;
  reformatted for the tooltip's "since …". Config-file-only; store-only setter.

Values are JSON numbers (exact-integer to 2^53). The three data keys are written
together behind a single `Config::save()` at fold time (never per MCP call).

## Per-project `id_format` (ANTS-3771)

**Not a config key** — it lives in the repo-committed
`<root>/.ants/project.json` (ANTS-2160), not in
`~/.config/ants-terminal/config.json`. Catalogued here because that is where a
session looks for "what can I configure", and because it is the first key in
that file that is not a path.

```json
{ "id_format": { "prefix": "ANTS", "pattern": "^([A-Za-z]{1,4}\\d+)(?:\\.|$)" } }
```

- `id_format.prefix` (string, optional) — the project's canonical ID prefix:
  1-16 characters of `[A-Za-z0-9_-]` with at least one ASCII letter, validated
  by the same `RoadmapFoldIn::isValidIdPrefix()` `roadmap_log`'s `id_prefix`
  argument uses. **Generative half.** It beats the store's `id_prefix` row and
  the markdown sniff in every prefix chain — `roadmap_log op:append` /
  `op:append_batch` and the migration allocator alike — and loses only to an
  explicit `id_prefix` argument. It also refuses a caller-supplied written id
  whose prefix disagrees, with `id_format_mismatch`.
- `id_format.pattern` (string, optional) — PCRE2, at most 512 bytes, matched
  against a **GFM bold lead-in** with its one trailing `.` already dropped.
  **Recognitional half.** Capture group 1 is the id (the whole match when the
  pattern has no group); the text it did not consume becomes the headline. So a
  project writing `- [ ] **AX1. Geometric occlusion**` reports `AX1` instead of
  the whole sentence, and a migration files it `parsed` rather than
  `quarantined`. A **non-match changes nothing** — the bullet keeps the id the
  reader gives it today, and no id is ever emptied.

Both members are optional and independent; a project may declare either alone.
Written with `project_settings op:"set" id_format:{…}`, which refuses
`bad_args` on an invalid member; `ProjectSettings::load()` **drops** an invalid
member instead, so an unreadable declaration never takes the roadmap down with
it. `op:"detect"` cannot suggest it — a grammar is not a path on disk.

Full contract:
[`../specs/ANTS-3771-id-format-declaration.md`](../specs/ANTS-3771-id-format-declaration.md).
