# ANTS-2079 — Trim load-bearing MCP tool-description blobs; serve per-op detail via `tool_info`

**Status:** shipped (2026-06-16); cold-eyes loops 1–3 folded, clean;
implemented + all 2127 tests green (see § 9).
**Kind:** optimize.
**Source:** ROADMAP.md ANTS-2079 (cross-session DOOM Ants 2026-06-11, re-confirmed 2026-06-15). Design direction pre-recorded in `docs/journal/ANTS-2079-2085-mcp-token-design-notes.md` § "ANTS-2079".
**Pairs with:** ANTS-1399 (`tool_info` descriptor verb — the on-demand channel this spec fills), ANTS-1985 (`tool_info {catalog:true}`).
**Layman:** Shorten the long help text the assistant loads for every MCP tool at startup, and keep the full version one quick lookup away — so each session uses fewer tokens.

## 1. Problem

Every MCP `tools/list` response carries the full `description` string of
every registered tool. A Claude Code session pays that payload once per
session at handshake (and again per-tool whenever a deferred schema is
loaded via `ToolSearch`). Several descriptions are multi-KB of
*encyclopedic per-op prose* — far more than a session needs to decide
"should I call this verb, and with what args".

**Metric.** Sizes below are the **wire-content byte length of the
`description` string** — the decoded text actually shipped in
`tools/list`, NOT the raw source-file span (which is larger: indentation,
quotes, and adjacent-literal concatenation boilerplate). Wire-content is
the quantity a session pays for in tokens, so it is the quantity this
spec trims and measures. Reproduce with: for each tool's
`…["description"] = QStringLiteral(...)` literal in
`src/claudeintegration.cpp`, decode the concatenated string segments and
count UTF-8 bytes. Reproduce per tool with (from the repo root):

```bash
# wire-content bytes of <tool>'s description literal
python3 - "$tool" <<'PY'
import re,sys; name=sys.argv[1]; src=open('src/claudeintegration.cpp').read().split('\n')
i=next(k for k,l in enumerate(src) if f'"{name}"' in l and '["name"]' in l)
j=next(k for k in range(i,i+60) if '"description"' in src[k]); n=0
for k in range(j,j+200):
    for s in re.findall(r'"((?:\\.|[^"\\])*)"',src[k]):
        n+=len(s.encode().decode('unicode_escape','ignore').encode())
    if src[k].rstrip().endswith(');'): break
print(name,n,'bytes')
PY
```

The earlier figures in
`docs/journal/ANTS-2079-2085-mcp-token-design-notes.md` (`roadmap_log`
"~2.1 KB", `roadmap_query` "~1.5 KB") are **stale** — written 2026-06-11
before subsequent growth — and are superseded by the measurement below.

Wire-content `description` bytes, measured 2026-06-16
(anchor = the tool's `["name"]` registration line in
`src/claudeintegration.cpp`):

1. `roadmap_query` — ~4561 B (name @ ~1878). DOOM names the roadmap verbs
   "the single biggest remaining per-session token sink for the write verbs".
2. `model_switch_stats` — ~4089 B (name @ ~2312).
3. `roadmap_log` — ~3336 B (name @ ~6811).
4. `test_audit_partition` — ~2579 B (name @ ~5718).
5. `changelog_log` — ~2149 B (name @ ~7419).
6. `workspace_search` — ~1955 B (name @ ~2699).
7. `verify_changes` — ~1872 B (name @ ~5453).

These seven are the tools whose wire `description` is ≥ ~1800 B; the
next-largest, `cold_eyes_brief` (~1609 B), sits below a clear ~260 B gap
(§ 2.5).

`tool_info {name:"…"}` (ANTS-1399) already exists as the per-tool
drill-in channel, but today it returns the **same** `description` string
that ships in `tools/list` — it reads the `m_lastToolsList` snapshot and
echoes `description` verbatim (`src/claudeintegration.cpp:8626`). So
there is no "short inline / long on-demand" split to trim into: deleting
prose from the inline description would simply lose it.

Consequence: the biggest per-session token sink on the MCP surface has no
mechanism to shed weight without losing documentation other sessions rely
on (documented refusal codes, op sets, required args).

## 2. Surface

The design adds a second, optional per-tool documentation string —
`detail` — authored beside the (now-trimmed) `description`, **stripped
from the `tools/list` wire payload**, and returned only by
`tool_info {name}`. No information is deleted; the encyclopedic prose
moves from a string every session pays for to one fetched on demand.

### 2.1 The `detail` field (authoring + storage)

At each long tool's registration site, the existing single description is
split into two co-located literals:

```cpp
// before
t["description"] = QStringLiteral("[mcp-roadmap] Append a bullet … <5.5 KB of per-op prose>");

// after
t["description"] = QStringLiteral(
    "[mcp-roadmap] Append/flip/annotate ROADMAP.md bullets. "
    "ops: append|append_batch|flip|flip_batch|annotate|create_section. "
    "Refusal codes: bullet_not_found, format_mismatch, bad_args, "
    "caller_cwd_required. caller_cwd Required.");   // ~248 B wire (illustrative)
t["detail"] = QStringLiteral(
    "<the per-op encyclopedic prose, verbatim from the old description>");
```

**Feasibility (densest tool).** The `// ~248 B` annotation is the measured
wire-content of the short form above for `roadmap_log` — the in-scope tool
with the most to keep inline (6 ops + 4 refusal codes). It plus the ~55 B
pointer = ~303 B, comfortably under the ≤ 700 B authoring target / ≤ 800 B
test ceiling (INV-5). So the ceiling is feasible for every in-scope tool,
not just the smaller ones.

`detail` is a sibling key on the same tool JSON object, authored
immediately after `description` so the two read as one unit (the
six-month test) and so source-scrape window tests still see the moved
prose in the same byte neighbourhood (§ 6, blast radius). Precondition:
every in-scope tool MUST author a **non-empty** `description` AND a
**non-empty** `detail`; the wire-strip step (§ 2.2) and INV-2 assume both.

### 2.2 Wire strip + runtime pointer (`tools/list`)

The split happens in source; the *separation* happens at build time in
the `tools/list` handler, immediately **after** the descriptor snapshot
(`m_lastToolsList = tools;`, `src/claudeintegration.cpp:8154`) and
**before** the lite/full shape branch (~8164):

```cpp
m_lastToolsList = tools;            // snapshot keeps `detail` (line 8154, unchanged)

// ANTS-2079 — strip per-op detail from the wire payload; the snapshot
// above retains it so tool_info can serve it on demand. QJsonArray is
// copy-on-write: mutating `tools` here detaches it, leaving
// m_lastToolsList intact.
for (int i = 0; i < tools.size(); ++i) {
    QJsonObject t = tools.at(i).toObject();
    if (t.contains(QStringLiteral("detail"))) {
        t.remove(QStringLiteral("detail"));
        t[QStringLiteral("description")] =
            t.value(QStringLiteral("description")).toString()
            + QStringLiteral(" Full per-op detail via tool_info {name:\"%1\"}.")
                  .arg(t.value(QStringLiteral("name")).toString());
        tools.replace(i, t);
    }
}
```

- The snapshot (`m_lastToolsList`) keeps the clean short `description` +
  the `detail` sibling — both for `tool_info` to serve.
- The wire array (`tools`) loses `detail` and gains a one-line pointer
  appended to `description` so a session reading `tools/list` knows where
  the rest lives. The pointer is runtime-appended (not in source), so no
  source-scrape test sees it. Tool names are `[a-z_]+` by convention (no
  characters needing JSON escaping), so the `.arg(name)` interpolation
  into the `{name:"%1"}` pointer is safe as written.
- Order is load-bearing: the snapshot at 8154 must precede the strip, and
  the strip must precede both the `wantLite` decision (~8164) / its
  `if (wantLite)` branch (~8174) and the full-shape
  `result["tools"] = tools;` (~8218), so neither wire shape carries
  `detail`.
- The lite shape is unaffected: it emits `{name, summary, kind}` built
  from scratch (~8200-ff, never copying `detail`), and its `summaryFor`
  prefers `selection_hint` over the `description` first-sentence fallback.
  All seven in-scope tools set a non-empty `selection_hint` (verified
  2026-06-16), so a lite summary never falls back to the pointer-bearing
  wire `description` — the runtime pointer cannot leak into a lite summary.

### 2.3 `tool_info` serves `detail`

The `tool_info {name}` match-found `else` branch
(`src/claudeintegration.cpp:8623`, the block that sets `name` /
`description` @ 8626 / `inputSchema` / `selection_hint` @ 8631) gains one
passthrough, placed right after the `selection_hint` assignment. Unlike
`selection_hint` — which is set **unconditionally** because every tool
has one (the @ 8631 line carries no guard, per ANTS-1453) — `detail` is set
**conditionally**, because only the in-scope tools author it:

```cpp
env["selection_hint"] = match.value(QStringLiteral("selection_hint"));
// ANTS-2079 — surface the per-op detail trimmed from the wire payload.
// Conditional (NOT mirroring selection_hint's unconditional set): present
// only for tools that authored one; absent key for the rest, so a reader
// treating it as "" sees no change. An unconditional set would emit
// detail:null for the other ~66 tools (73 registered − 7) and break INV-2.
if (match.contains(QStringLiteral("detail")))
    env["detail"] = match.value(QStringLiteral("detail"));
```

`tool_info` thus returns `{name, description (short), inputSchema,
selection_hint, detail?}`. A session that wants the full documentation
makes exactly one `tool_info` call — the same call ANTS-1399 already
prescribes for "load a verb's schema before calling it".

### 2.4 Trim rubric (what stays short vs moves to detail)

Applied to every tool in scope (§ 2.5). The **short `description`**
keeps only the load-bearing contract surface:

- the `[<kind>]` category prefix (parsed by `categoryOf` / `kindForName`;
  INV-6);
- one sentence of *what + when* (the selection cue);
- the **op set** / **selector set** (the names a caller must choose from);
- the **refusal codes** other sessions branch on (the documented
  `code` taxonomy — `docs/standards/mcp-error-codes.md`);
- the `caller_cwd` contract word (Required/Optional) and any
  required arg names.

The **`detail`** absorbs everything else, verbatim where practical:

- per-op / per-arg encyclopedic prose;
- `ANTS-NNNN` provenance citations;
- advisory notes (SIZE NOTE, transport-drop notes, tuning guidance);
- response-envelope field-by-field listings.

Invariant: short + detail together are a **superset** of the original
description's load-bearing content — no documented code, op, or required
arg is dropped from *both* (INV-3). Pure prose may be tightened.

### 2.5 Scope (first application)

Apply the split to every tool whose current wire-content `description`
is **≥ ~1800 B** — the seven tools enumerated in § 1 (`roadmap_query`,
`model_switch_stats`, `roadmap_log`, `test_audit_partition`,
`changelog_log`, `workspace_search`, `verify_changes`; ~4561 → ~1872 B).
The bar sits in the clear ~260 B gap below the eighth-largest,
`cold_eyes_brief` (~1609 B), so the in-scope set is unambiguous. Target
short form ≤ 700 B wire-content each (the test ceiling is ≤ 800 B with
the runtime pointer, INV-5).

**Name-collision note.** The only MCP tool that exposes an *input arg*
named `detail` is `test_results` (`props["detail"]` at
`src/claudeintegration.cpp:4585`, out of scope) — an earlier draft of
this note wrongly attributed it to `test_audit_partition`, corrected
during implementation (2026-06-16). The new tool-descriptor `detail`
field lives on the tool JSON object, not in `inputSchema.properties`, so
the two never alias at runtime. Caution for two source-scrape tests: (a)
`mcp_dispatch_forward_completeness` scans a tool block for the literal
`props["` token — do NOT write `props["detail"]` in a descriptor comment
or it is mis-counted as an unread schema prop; (b) INV-3's anchor list
for any tool must not conflate an arg-doc token with the descriptor
field.

Tools just below the bar (`cold_eyes_brief` ~1609 B,
`test_audit_fold_in`, `roadmap_branch_drift`, `session_orient`) are left
for a follow-up pass (§ 5, ROADMAP id allocated on ship) — the mechanism
is identical, so extending
coverage later is a description edit, not a contract change.

## 3. Invariants

- **INV-1** — A tool that authors a `detail` literal does NOT carry a
  `detail` key in the **full-shape** `tools/list` wire payload. (The lite
  shape is structurally `{name, summary, kind}`-only — built from scratch
  at `src/claudeintegration.cpp:8200`-ff — so it can never carry `detail`
  and is not at risk; only the full shape needs the strip.) *Test:*
  feature test drives full-shape `tools/list`, asserts no in-scope tool
  object contains `detail`.
- **INV-2** — `tool_info {name:"<in-scope tool>"}` returns a non-empty
  `detail` field; `tool_info {name:"<tool with no detail>"}` (e.g.
  `get_cwd`) omits the `detail` key entirely. *Test:* feature test drives
  `tool_info` for one in-scope and one out-of-scope tool.
- **INV-3** — For each in-scope tool, every documented refusal `code` and
  every `op`/selector name it documented pre-trim is still present in the
  post-trim `description` **or** `detail` (nothing silently dropped).
  *Test:* source-scrape — the test fixture carries a per-tool **anchor
  token list** (the codes/ops captured from the pre-trim literal at
  authoring time; the pre-trim literal itself is not recoverable at test
  time), and asserts every anchor token appears in the union of that
  tool's `description` + `detail` source literals. The anchor list lives
  at `tests/features/mcp_tool_detail_field/anchors/<tool>.txt` (one token
  per line), captured **mechanically** from the pre-trim source
  (`git show HEAD:src/claudeintegration.cpp`, before the trimming commit)
  so the capture is reproducible rather than hand-transcribed — an
  incomplete hand-copy would re-introduce the very "silent drop" INV-3
  guards against.
- **INV-4** — Each in-scope tool's wire `description` carries the
  runtime-appended pointer `tool_info {name:"<name>"}`. *Test:* feature
  test asserts the substring in the `tools/list` description for an
  in-scope tool, and asserts it is ABSENT for an out-of-scope tool.
- **INV-5** — Each in-scope tool's `tools/list` wire `description` (short
  essentials + runtime pointer) is ≤ 800 B — far under its pre-trim size
  (§ 1, ~1872–4561 B), so the saving is realised. *Test:* feature test
  drives `tools/list` and asserts each in-scope tool's `description`
  length ≤ 800 B. (Budget: authoring target ≤ 700 B + the runtime pointer
  — a ~44 B fixed template ` Full per-op detail via tool_info {name:"%1"}.`
  plus the tool name, so ≤ ~62 B for the longest name `model_switch_stats`
  (18 chars) — = ≤ ~762 B, under the 800 B ceiling.)
- **INV-6** — Every short `description` still begins with its `[<kind>]`
  prefix, so `categoryOf` (catalog mode) and `kindForName` continue to
  bucket it (not `other`). *Test:* feature test asserts each in-scope
  tool's `tools/list` description `startsWith("[")` and a known kind tag
  (the per-tool runtime guard), and `mcp_tool_prefix_tags` INV-3 (which
  asserts every registered tool is bucketed by `kindForName`) stays green.
  NOTE: `mcp_tool_info_verb` INV-12 only source-scrapes that the
  `categoryOf` handler exists — it does NOT check any tool's runtime
  prefix, so it is not the guard for this invariant.
- **INV-7** — `m_lastToolsList` (the snapshot `tool_info` reads) retains
  the clean short `description` WITHOUT the runtime pointer and WITH the
  `detail` sibling; the wire strip mutates only the `tools` array.
  *Test:* feature test asserts `tool_info`'s returned `description` does
  NOT contain the `tool_info {name:` pointer text AND that `tool_info`'s
  `detail` is non-empty for an in-scope tool — so a reversed
  snapshot/strip order (snapshot taken *after* the strip, losing `detail`)
  fails fast here, not only transitively at INV-2.
- **INV-8** — No regression in the existing source-scrape window tests
  that assert documented phrases for in-scope tools
  (`mcp_verify_changes_timeout_headroom`,
  `mcp_global_config_sentinel`, `roadmap_query_filter_section_headers`,
  `mcp_workspace_search`, `roadmap_query_duplicate_ids`): the moved
  phrases remain within each test's byte window, or the window is
  re-sized in the same commit. *Test:* the feature tests in those five
  directories pass.

## 4. RAM / build cost

The `detail` literals are compile-time `QStringLiteral`s in the binary's
read-only data. Building a `QJsonObject` copies each string into the
object's own storage, so the snapshot (`m_lastToolsList`) holds its own
copy of every `detail`. That copy is the one new runtime cost. The wire-strip
loop is O(tools) over the already-built array, run once per `tools/list`
— ~7 `QString` concatenations (one `.arg()` per in-scope tool) per call,
and `tools/list` fires only at handshake + per deferred-schema load, not
per tool call, so the cost is negligible (no hot path). No new build
target, no external library.

## 5. Out of scope

- Trimming the ~2.0–2.5 KB second tier (`cold_eyes_brief`,
  `test_audit_fold_in`, `roadmap_branch_drift`, `session_orient`) and any
  tool that later crosses the 2500 B bar — follow-up, tracked by a new
  ROADMAP id once this mechanism ships.
- A session-wide `verbosity` preference that would also gate description
  length — that is ANTS-2085 (separate item, `session_memory`-backed).
- De-duplicating per-arg prose that already lives in `inputSchema`
  property descriptions — a finer cleanup, not required for the saving.

## 6. Tests

Feature test: `tests/features/mcp_tool_detail_field/`. Covers INV-1
through INV-7; INV-8 carries no new assertion — it is satisfied by the
pre-existing source-scrape tests (the blast-radius list below) staying
green. Label `features;fast`; added to the `test_claude`
`ants_add_gui_bundle` SOURCES list (CMakeLists.txt ~1007 — the bundle that
owns the other `tool_info` / tools-list feature tests). Verify the new
test FAILS against pre-change source (no `detail` field, no wire strip)
before the implementation is in place.

**Blast radius (pre-existing tests to keep green — INV-8).** The
source-scrape window tests match *raw source text*, not the runtime
payload; because `detail` is authored at the same registration site as
`description`, the moved prose stays in the same byte neighbourhood, so
these mostly pass unchanged. Where a trim shifts a required substring or
an `inputSchema` property out of a test's fixed window, the window is
re-sized in the same commit:

- `mcp_verify_changes_timeout_headroom` — `verify_changes` 4000/5000 B
  windows assert `tool-side`/`transport` proximity + phase-timing fields;
  keep those phrases adjacent in `detail`.
- `mcp_global_config_sentinel` — `workspace_search` 8000 B window asserts
  `~global` + `~/.claude/`; `file_outline` 4000 B window (file_outline is
  out of scope, untouched).
- `roadmap_query_filter_section_headers` (13000 B) /
  `roadmap_query_duplicate_ids` (9000 B) — `roadmap_query` schema-prop
  windows; trimming the description *reduces* the offset to the props, so
  the props stay in-window (verify, don't assume).
- `mcp_workspace_search` (16000 B) — asserts `required`/`pattern`; same
  reasoning.
- `mcp_tool_prefix_tags` INV-3 / `mcp_tool_info_verb` INV-12 — the
  `[<kind>]` prefix + `kindForName` body must stay intact (INV-6).

## 7. Cross-doc impact

- `docs/standards/mcp-tools.md` — document the `description`/`detail`
  split as an authoring rule (short essentials inline, encyclopedic prose
  in `detail`, retain `[<kind>]` prefix + refusal codes inline).
- `docs/standards/mcp-behavioural-notes.md` — note that `tool_info`
  returns a `detail` field for trimmed verbs.
- `CHANGELOG.md` — Changed/Optimized entry (token-saving).
- `docs/journal/ANTS-2079-2085-mcp-token-design-notes.md` — the § 2079
  "deferred" note is superseded by this spec; leave as historical record.
- `CLAUDE.md` — no change (the MCP-authoring pointer already routes to
  `mcp-tools.md`).

## 8. Cold-eyes loop log

Rows live in [`../reviews/ANTS-2079-trim-load-bearing-mcp-tool-loop-log.md`](../reviews/ANTS-2079-trim-load-bearing-mcp-tool-loop-log.md).

## 9. Implementation notes (2026-06-16, shipped)

Corrections discovered while implementing, recorded for the audit trail:

- **INV-5 budget understated two runtime additions.** The reconstructed
  *wire* `description` is `[<kind>] ` prefix (≤ ~12 B, added by the
  tools/list prefix loop at `claudeintegration.cpp` ~8081) **+** short
  literal **+** the ~221 B "Etag tip:" memo (appended by that same loop
  for etag-supported tools — among the seven, only `roadmap_query` and
  `model_switch_stats`) **+** the per-op pointer. The ≤ 800 B ceiling
  still HOLDS: measured wire sizes are 707 / 641 / 601 / 584 / 536 / 656 /
  586 B (roadmap_query, model_switch_stats, workspace_search,
  verify_changes, test_audit_partition, roadmap_log, changelog_log). The
  two etag tools were trimmed to ~400 B essentials to absorb the memo. The
  feature test reconstructs all four components and asserts ≤ 800 B.
- **Test harness is source-scrape, not live-dispatcher.** This bundle has
  no seam to drive `tools/list` / `tool_info` at runtime (same as
  `mcp_tool_info_verb`), so INV-1/2/4/6/7 are validated through the source
  constructs that produce the runtime behaviour, and INV-5 via the
  reconstruction above. INV-3 reads the anchor fixtures and asserts each
  token survives in the tool's `description`+`detail` source slice.
- **`detail` authored as the full verbatim original; short is new prose.**
  Each split keeps the original literal in place as the `detail` value
  (so the INV-8 source-scrape windows stay in the same byte
  neighbourhood) and prepends a fresh short `description`. Net: `detail`
  is a strict superset of the pre-trim description (INV-3 holds by
  construction); ~3 KB of rodata added across the seven tools (the short
  forms), not removed — negligible.
- **INV-8 blast radius, observed.** Two pre-existing tests needed a
  one-line touch: `roadmap_query_filter_section_headers` INV-4 (window
  13→14 KiB, the short+comment pushed `include_section_headers` down ~570
  B) and a descriptor comment reworded to avoid the literal `props["`
  token that `mcp_dispatch_forward_completeness` scrapes. All 2127 tests
  green.
