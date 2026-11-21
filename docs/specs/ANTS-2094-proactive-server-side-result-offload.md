# ANTS-2094 — Proactive server-side result offload (observation masking)

**Status:** accepted (2026-06-16) — cold-eyes loops 1–5 clean; OQ-1 resolved (default OFF for v1). Amended 2026-07-16 (ANTS-3538 — additive structured preview for array bodies, § 2.3.1 / INV-13; 6-loop cold-eyes amendment review converged to polish — INV-2/9/12 preservation affirmed sound by both reviewers). Amended 2026-07-16 (ANTS-3552 — INV-1/INV-6 test-notes updated for the extracted `mcp::shouldOffload` predicate + read_spill edge coverage; 1-loop 2-lane cold-eyes affirmed the amended sections clean, surfaced pre-existing drift elsewhere → ANTS-3554). Amended 2026-07-16 (ANTS-3554 — corrected the INV-5/7/9/11/14 + § 6 test-notes to match the tests and backfilled the offload/read_spill coverage they claimed: fault-injection fail-open, row-mode `too_large`, empty-array / root-array `not_array`, and the refusal-`hint` source-scrape; cold-eyes below). Amended 2026-08-26 (ANTS-4692 — § 2.3.2 / INV-15: the body's own top-level fields survive an offload, with an `isProtectedCompactKey` floor and a `preserved_omitted` signal so a dropped field is never mistaken for one never requested).
**Kind:** enhancement.
**Source:** ROADMAP.md ANTS-2094 (in-session-2026-06-11, observation-masking /
context-offload research).
**Pairs with:** ANTS-2091 (`compact`), ANTS-2081/2086 (`appendReadHints`) —
the other dispatch-site token-reduction transforms this slots beside.

## 1. Problem

Some MCP read verbs return multi-KB bodies that the agent only skims:
a `get_scrollback` dump, a wide `workspace_search`, a `codebase_index`
map, a long `read_log` tail. Today the full body always crosses the wire
and lands in the model's context, even when the agent needed only the
head or a summary line. The dispatch site already measures the cost —
`claudeintegration.cpp:8884-8886` computes `outBytes`/`rawBytes` for every
call — but does nothing to cap a large body proactively.

The Claude Code client has an emergency spill for results over ~25 K, but
it is client-side, reactive, and returns an opaque truncation — not a
usable head plus a re-read handle. There is no *server-controlled*,
*proactive* offload that hands back a short preview the agent can act on
without re-reading.

Consequences:

1. Large read results inflate context tokens even when unread in full —
   directly against the project's token-reduction focus.
2. The agent has no cheap "peek, then fetch the rest only if needed"
   path for a big body — it is all-or-nothing per call.

**Layman:** When a tool's answer is huge, save it to a file and hand back
a short preview plus a ticket number — Claude reads the rest only if it
actually needs it.

## 2. Surface

### 2.1 Trigger — opt-in, at the dispatch choke point

A single insertion point in the `tools/call` dispatch in
`src/claudeintegration.cpp`, **after** the existing `fields=` / `compact`
/ `appendReadHints` transforms (`appendReadHints` ends at line 8854) and
**immediately before** the wrap block (the `if (toolHandled)` at line
8856; the `wrapMcpData(toolName, responseText)` call itself is line
8872) — so offload measures the *final*, already-trimmed body.
Pseudocode:

```cpp
const qint64 bodyBytes = responseText.toUtf8().size();
if (toolHandled && !etagUnchanged && !rawRequested &&    // raw:true (ANTS-2218) returns verbatim
    mcp::isOffloadEligible(toolName) &&
    mcp::offloadRequested(argsObj) &&                 // per-call arg OR session default
    bodyBytes >= mcp::offloadThresholdFor(toolName, argsObj) &&  // ANTS-5364
    bodyBytes >  mcp::offloadHeadBytes()) {           // never spill what fits in the head
    responseText = mcp::offloadBody(toolName, responseText);   // spill + head envelope
}
```

The second guard (`bodyBytes > offloadHeadBytes()`) is a cheap first-line
filter against spilling a body that already fits inside the returned head
— which would hand back an envelope (head ≈ whole body + wrap + fields)
*larger* than the original. It is **mandatory, not cosmetic**: although
at the *default* config (threshold 16384 ≫ head 2048) it never trips, the
clamp ranges (§ 2.6) overlap — a threshold floor of 4096 sits below the
head ceiling of 16384 — so without the guard a misconfiguration would
routinely attempt sub-head spills. It is load-bearing for **INV-2**: an
offloaded body always strictly exceeds the head, so `head_truncated` is
always `true` when `offloaded:true`. It is **not**, on its own, sufficient
for INV-9's net-saving guarantee — a body only marginally over the head
still yields an oversized envelope — so `mcp::offloadBody` additionally
fails open on any finished envelope that would not shrink the body
(ANTS-3540; § 2.3.1 / INV-9).

`mcp::offloadRequested(argsObj)` mirrors the ANTS-2085 `compact`
resolution: the per-call `offload` bool wins when present (true OR
false), and so does the text `"true"` or `"false"`, in any case,
which a client sends because no schema declares `offload` (ANTS-6827).
Otherwise it falls back to `mcp::offloadDefault()` (driven by the
`claude.mcp_offload_large_results` config key — see § 2.6).

Eligible verbs (`mcp::isOffloadEligible`, new predicate in
`src/mcpprojection.cpp` beside `projectFields`): the large-body
read verbs only — `get_scrollback`, `get_text`, `read_log`,
`read_region`, `read_regions` (ANTS-2219), `workspace_search`,
`codebase_index`, `docs_index`, `find_sources`, `debt_sweep_scan`
(ANTS-3345), `roadmap_query`, `project_query` (ANTS-2093). This set
deliberately **differs** from
the projection set, which ANTS-4524 has since made universal (it adds `get_scrollback` / `get_text` /
`workspace_search` / `find_sources`, which take no `fields=` but still
emit large bodies) — the two predicates are independent, not expected to
match. Write verbs, control-plane verbs (`get_session_info`,
`token_usage`, `tool_info`), refusal envelopes (`!toolHandled`), and 304
short-circuits (`etagUnchanged`) are never offloaded.

### 2.2 Spill file — content-addressed, under the cache root

New helper module `src/mcpspill.{h,cpp}` joining `ants_core_lib`
(pure Qt6::Core, no widgets). Spill dir, following the
`src/codebaseindex.cpp:441-442` idiom (call `writableLocation`, not the
bare enum):

```
QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
    + "/ants-terminal/mcp-spill/<sha256(body)>.json"
```

- **Content-addressed** key = `sha256` of the raw (pre-wrap)
  `responseText` UTF-8 bytes. An identical body reuses the same file
  (write is idempotent). Because the key is the *content*, not a project
  path, this cache cannot **shadow** across a project relocation
  (mcp-caches.md invariant) — two projects with byte-identical output
  legitimately share one file; differing output differs in hash.
- The file holds the **full** raw body verbatim (the un-offloaded
  `responseText`).
- Written via `QSaveFile`; `secureio.h` `setOwnerOnlyPerms(const
  QString&)` is applied to the **final path after `commit()`** (mode
  0600) — not to the `QSaveFile` device, whose temp-file perms do not
  reliably survive the atomic rename. The `mcp-spill/` dir is created
  mode 0700. Spilled bodies can contain scrollback / file content, so
  they carry the same owner-only contract as the config file
  (coding.md § 5.2).
- The cache is **global, not project-scoped** — one shared
  content-addressed store across every project, under `~/.cache` (so
  byte-identical output from two projects dedups to one file). This is
  not a cross-project content leak: the handle *is* the `sha256` of the
  body, so a caller can only name a handle it could compute, which means
  it already possessed the content. `read_spill` therefore takes
  `caller_cwd` as **Optional** (§ 2.4), not Required — it resolves by
  global handle, not by project root.

**Fail-open on write failure.** If the spill write fails for any reason
(cache dir unwritable, disk full, `QSaveFile::commit()` returns false),
offload is **abandoned** and the original full `responseText` is returned
unchanged. Offload is an optimisation, never a failure mode: a caller
must never get an error or a dangling pointer because the cache could not
be written.

### 2.3 Returned head + pointer envelope

`mcp::offloadBody` replaces the large `responseText` with a small JSON
envelope:

```json
{
  "ok": true,
  "offloaded": true,
  "handle": "<sha256(body)>",
  "bytes": 41037,
  "threshold_bytes": 16384,
  "head": "<first mcp::offloadHeadBytes() UTF-8 bytes of the raw body, truncated on a UTF-8 char boundary>",
  "head_truncated": true,
  "hint": "Large result spilled. Re-read the full body via read_spill {handle:\"<sha256>\"} (optional offset/max_bytes to page)."
}
```

- `bytes` is the raw (pre-wrap) body's UTF-8 byte length — equal to the
  spilled file size, since the file holds the body verbatim.
- `threshold_bytes` is the threshold this call crossed (INV-1's
  `offloadThresholdFor`). It tells a caller why the body spilled.
- `head` is an explicit *prefix preview*, never presented as the
  complete result. Because offload only fires when `bytes >
  offloadHeadBytes()` (§ 2.1), an offloaded `head` is always a strict
  prefix and `head_truncated` is always `true` — the field stays in the
  schema for explicitness and forward-compat. The prefix is cut on a
  **UTF-8 character boundary** (never mid-sequence) so `head` is always
  valid UTF-8. The agent keys off `offloaded:true` to know a re-read is
  needed for completeness. A worked `head` is the literal prefix of the
  spilled *inner* body (offload precedes `wrapMcpData`, so no wrap tag is
  in it), e.g. for an offloaded `workspace_search`:
  `"head": "{\"matches\":[{\"file\":\"src/claudeintegration.cpp\",\"line\":8872,\"text\":\"…"` —
  a truncated, still-valid-UTF-8 fragment, not parseable JSON on its own.
- No `line_count`: eligible bodies are compact JSON envelopes
  (`wrapMcpData` / `responseText` emit single-line compact JSON, so a
  line count is ~always 1 and misleading). The re-read pages by **byte
  offset** instead (§ 2.4) — the ROADMAP bullet's `line_count` field is
  deliberately dropped for this reason.
- The envelope is small (≤ ~`offloadHeadBytes` + ~330 B fixed overhead for
  a non-array body; up to ~`2×offloadHeadBytes` + ~400 B when the § 2.3.1
  structured preview is present — the extra ~70 B is the four structured
  field names, and its `head_rows` content is capped at `offloadHeadBytes()`
  — and always strictly `< bytes` for the structured path, INV-9) so it
  passes through `wrapMcpData` and the trace
  byte-accounting normally; it is itself never re-offloaded — offload is a
  **single non-recursive pass** at the § 2.1
  insertion point, so its own output is never re-fed through the guard
  (this holds regardless of the envelope's size, which at clamp extremes
  can exceed `threshold`).

### 2.3.1 Structured preview for array-shaped bodies (ANTS-3538)

The byte-prefix `head` (§ 2.3) is cut at a fixed byte length on a UTF-8
boundary, so for a body whose payload is a JSON array of rows it almost
always lands mid-row: the fragment is valid UTF-8 but **not parseable
JSON**, so a caller that needed only the first few rows or the total count
still pays a full `read_spill` round-trip. To let those callers answer
from the envelope, `offloadBody` **additionally** emits a structured
preview when the raw body is a JSON object carrying a dominant array
field. This is **purely additive** — the pre-3538 fields (`head`,
`head_truncated`, `ok`, `offloaded`, `handle`, `bytes`, `hint`) are emitted
unchanged (INV-2 holds) and the four structured fields are appended beside
them; a caller that ignores the new fields reads the pre-3538 fields
exactly as before. (When the preview *is* present the whole envelope is
larger than pre-3538 — each pre-3538 field's key/value bytes are unchanged,
though their *positions* in the serialized string may shift as the new keys
interleave under `QJsonObject` key-sorting; the whole envelope is
byte-identical to pre-3538 only in the omission cases below.)

Detection and shape:

- The structured preview is attempted only when `bytes <=
  kStructuredParseMaxBytes` (`mcpspill.h`, = 1 MiB). A larger body skips
  the parse and takes the byte-prefix head only — bounding the transient
  `QJsonDocument::fromJson` footprint (§ 4) on the offload path; such a
  body is large enough that the caller pages it via `read_spill`
  regardless.
- Parse the raw body as JSON. If it does not parse, is not a JSON
  **object**, or carries no non-empty array-valued member, emit no
  structured preview — the byte-prefix head is the only preview (the
  **non-array / unparseable fallback**).
- The **dominant array** is the **root** envelope object's own direct
  member (detection scans only the root object's members — **no recursion**
  into nested objects) whose value is a **non-empty** JSON array with the
  most elements. Ties break to the **first key in `QJsonObject` iteration
  order**: Qt stores members sorted by `QString` (UTF-16 code-unit) order —
  which equals lexicographic order for the ASCII keys these verbs emit —
  and source-document / insertion order is discarded at parse, so iterating
  yields a deterministic sorted-first choice; both tied arrays are equally
  valid previews, so the tie-break needs no stronger rule. This selects
  (illustratively — the longest member array always wins) `matches` for
  `workspace_search`, `bullets` for `roadmap_query`, `files` for
  `find_sources`, etc., **without hard-coding per-verb key names** — the
  transform stays verb-agnostic like the rest of `offloadBody`. This
  detection is shared with `read_spill` row-paging (§ 2.4.1, ANTS-3545)
  through one `dominantArrayKey` helper, so the previewed key
  (`head_rows_key`) and the key row-paging pages are always the same array.
- Because detection scans only the root's direct members, the columnar
  `__rows__` of a **tabular-encoded** (ANTS-2090) array — an array-of-objects
  packed *in place* into a `{__cols__, __rows__}` object *before* offload at
  `claudeintegration.cpp` — is **never** structured-previewed: `__rows__` is
  nested one level inside that object, not a root member, so the
  no-recursion scan cannot select it. This is **intentional**: the
  positional `__rows__` are uninterpretable without their sibling `__cols__`.
  It does **not** follow that a tabular body always takes the byte-head
  fallback — ANTS-2090 packs only arrays-**of-objects**, so a body may still
  carry a *sibling* root-level array that tabular left untouched (e.g.
  `find_sources` packs `files` but leaves its string array `unmatched_terms`
  a plain root array), and detection will legitimately structured-preview
  that sibling (INV-9 still holds by construction). Only a body whose
  *every* root array member was packed exposes no root array and falls to
  the byte-head fallback.
- Emit these four fields beside the existing envelope:
  - `head_rows_key` — the dominant array's field name (string).
  - `row_count` — the dominant array's **full** element count (not the
    previewed count).
  - `head_rows` — a JSON array of the first K **complete** elements of the
    dominant array, each emitted **whole** (never byte-cut, so `head_rows`
    is always parseable JSON). The elements are re-serialized through
    `QJsonDocument`, so each is *semantically* equal to the spilled body's
    element (deep-equal — INV-13) but not necessarily byte-identical
    (whitespace / key order / number formatting may normalize).
  - `head_rows_truncated` — `true` iff `row_count > head_rows.size()`
    (the remaining rows live only in the spill file).

Row budget — a **small** preview, INV-9 airtight by construction:

- The preview is deliberately kept small (a few rows, not the whole body —
  else it would not be a *preview* and offload would save nothing): the
  `head_rows` **content** is capped at
  `headRowsBudget = min(offloadHeadBytes(), bytes − S0 − 1)`, where `S0` is
  the measured compact UTF-8 size of the envelope built with all pre-3538
  fields + the § 2.3.2 preserved members + `head_rows_key` + `row_count` +
  `head_rows_truncated` + an **empty** `head_rows`. The `offloadHeadBytes()` term caps the preview
  size (so offload stays a real saving, not a `bytes − 1` near-copy); the
  `bytes − S0 − 1` term is what makes INV-9 exact.
- Measure `S0` with `head_rows_truncated` rendered in its **wider** form
  (`false`, 5 B) even though the field usually ends up `true` (4 B) — so
  the finished field can only *shrink* the envelope below `S0`, never grow
  it past the budget. (Were `S0` measured with the shorter `true`, a run
  where every row fits would flip the field to `false`, add 1 B, and
  consume the `−1` strictness margin — the envelope could then reach
  exactly `bytes`.)
- Append a complete element only while the cumulative serialized size of the
  head_rows content (elements + their separators) stays within
  `headRowsBudget` **and** the appended count does not exceed `kHeadRowsMax`
  (`mcpspill.h`, = 25). `S0` already counts the **escaped** `head` and every
  other envelope byte at its widest, so the finished envelope size is
  `S0 + content ≤ S0 + (bytes − S0 − 1) = bytes − 1 < bytes` — INV-9 holds
  **by construction** for the structured path, with no slack constant to
  tune and robust to `head` escaping, config-clamp extremes, and future
  `hint` / field-name growth. (`kHeadRowsMax` caps cardinality
  independently, so a body of thousands of tiny rows previews at most 25 and
  the append loop is bounded.) The `head` byte-prefix and `head_rows`
  overlap in content, but since `S0` measures the *whole* envelope the
  duplication is counted, never assumed away.
- The omission fallback (next bullet) reduces to the pre-3538 envelope and
  inherits *its* guarantee. That base envelope's former
  head-guard-vs-fixed-overhead exposure (the § 2.1 guard
  `bytes > offloadHeadBytes()` omits the ~330 B envelope overhead, so a
  head≈threshold config *could* offload a just-over-head body to a net loss)
  is **closed by ANTS-3540**: `offloadBody` measures the finished envelope and
  fails open — returns the body unchanged, per INV-11 — whenever it would not
  be strictly smaller than the body. So INV-9 now holds for *every* offloaded
  envelope at *every* config: the 3538 structured path never added an exposure,
  and the base path no longer has one either.
- If not even the **first** complete element fits within `headRowsBudget`
  (a body only marginally over the head guard, or a single very large row),
  the four `head_rows` fields above are omitted. The envelope is **not** the
  non-array fallback there, and this is the case the shape preview was added
  for: ANTS-4397 emits one summary row per actual row precisely when the row
  BODIES do not all fit, and ANTS-3545's row-paging `hint` has already
  replaced the base one (it is gated on a dominant array existing, not on the
  preview fitting). The `head_rows` preview is a **best-effort** saving, not a
  guarantee for every array body.
- **The shape-preview family (ANTS-4397 / ANTS-4474 / ANTS-4519).** When the
  row bodies do not all fit, the envelope carries `rows_preview_key`,
  `rows_preview` (one `{index, bytes, head}` per row), `rows_preview_truncated`
  and `rows_preview_hint`; `rows_preview_head_chars` rides only the arm where
  the per-row head was narrowed to cover every row. Where not even narrowed
  heads cover them all, ANTS-4519 emits **no** array and instead carries
  `rows_preview_omitted`, `row_count` and `rows_preview_hint` — a bare list of
  row LENGTHS says no more than `row_count` and `bytes` already do.

### 2.3.2 Preserved top-level fields (ANTS-4692)

Offload replaces the body with a freshly-built envelope, so every root
member of the body other than the previewed array is discarded. Several
verbs' own schemas instruct callers to branch on fields that live there.

The reported case is `roadmap_query` with `check_sync`. `file_in_sync` and
`sync_checked` say whether a hand edit would be silently discarded by the
next write; `source` says whether the project is store-backed. Both are
fields a session is told to read *before* writing, and both vanish when the
same call happens to spill. The direction of the loss is what makes it
dangerous: **absent is indistinguishable from never-requested**, so a caller
treating a missing field as its zero value reads "out of sync", or skips the
check it was told to make.

`read_spill` recovers them in principle — INV-6's page from offset 0 carries
the whole body — but the caller has no signal that anything was dropped, and
pays a round-trip over a large body to read a few bytes.

Rule — copy the body's root members onto the envelope:

- **Skipped:** the dominant array — it is the payload being spilled, and
  `head_rows_key` / `rows_preview_key` already name it — and every key the
  offload owns: `ok`, `offloaded`, `handle`, `bytes`, `threshold_bytes`, `head`,
  `head_truncated`, `hint`, `preserved_omitted`, the four `head_rows` fields
  and the shape-preview family, all enumerated in §§ 2.3 and 2.3.1. A body
  member of the same name is dropped, not merged: those fields describe the
  offload, not the result.
- **`ok` is the one exception to that skip.** The envelope sets it as a
  placeholder, so a body carrying its own `ok` wins. An offloaded body whose
  `ok` is false must not be reported as a success.
- **Budget:** a member is copied when its compact serialization fits
  `kPreservedFieldMaxBytes` (`mcpspill.h`, 512 B — wide enough for a path, an
  `etag` or a `source`, narrow enough that no one member can spend the whole
  total), and while the running preserved total fits `offloadHeadBytes()`.
- **Order, because that running total makes the set order-dependent:**
  protected keys first, so the floor below cannot be starved by an ordinary
  member, then the rest in `QJsonObject` key order — the same sorted iteration
  § 2.3.1 already relies on for its dominant-array tie-break.
- **Floor:** a key `mcp::isProtectedCompactKey` protects is copied regardless
  of the *total* budget. That is the same predicate compact mode uses
  (ANTS-2091 / ANTS-4677), so the two paths cannot disagree about which
  fields invert a result's meaning when dropped, and its `*_checked` suffix
  rule is what carries `sync_checked`. The per-member cap still applies: a
  protected key large enough to matter to the budget is not a flag. The
  predicate is file-local to `mcpprojection.cpp` today and must be published
  on `mcpprojection.h` beside `isOffloadEligible` — one definition, two
  callers, so compact mode and offload cannot drift apart.
- **Signal:** a member rejected by the per-member cap or the running total is
  named in `preserved_omitted`, an array of key-name strings. That is the
  whole of its content — a skipped key never appears there, nor does the
  dominant array. An absent field is then never ambiguous between dropped and
  never-requested —
  the reasoning `spec_lint`'s `skipped[]` and `doc_integrity`'s
  `*_suppressed` counters already apply.

Ordering is load-bearing: preservation runs **before** the § 2.3.1 row-budget
probe, so `S0` counts the preserved fields and the row budget shrinks to
accommodate them. INV-9 therefore holds by construction, with the ANTS-3540
finished-envelope fail-open as the backstop.

The transform stays **verb-agnostic**, like the rest of `offloadBody`: no
per-verb key list. A key list needs every verb's author to remember it, which
is the failure ANTS-4677 named when it replaced one with a suffix rule.

**Not preserved**, deliberately: a body that does not parse, is not a JSON
object, or exceeds `kStructuredParseMaxBytes`. That is § 2.3.1's gate and the
parse is shared, so preservation adds no parse and no RAM (§ 4). A body over
that cap keeps the byte head alone, as it does today.

### 2.4 Re-read verb — `read_spill`

The ROADMAP bullet says "re-read via `read_region`", but `read_region`
resolves its path through `PathValidation::validatePath`
(`src/pathvalidation.h:50`) with `allowOutsideRoot=false` and exposes no
opt-out arg — it serves **only** files under the project root. (The
`allow_outside_project` arg belongs to a different tool, the test-audit
synthesis verb, `src/mainwindow.cpp:4719`; it is not a `read_region`
knob.) The spill file lives under `~/.cache`, outside every project root
— so `read_region` cannot serve it without weakening its path boundary.
This spec resolves that by adding a dedicated, spill-scoped verb instead
of widening `read_region`.

New verb **`read_spill`** (handler `RemoteControl::cmdReadSpill` in
`src/remotecontrol.cpp`, declared in `src/remotecontrol.h`, dispatched
via `rcDelegate(&RemoteControl::cmdReadSpill)` in `src/mainwindow.cpp`,
schema + caller_cwd contract in `src/claudeintegration.cpp` — the same
wiring path as `cmdReadRegion`, `src/remotecontrol_workspace.cpp` /
`src/mainwindow.cpp:4886-4888` (registration through dispatch)):

- Args: `handle` (required, must match `^[0-9a-f]{64}$` — a bare
  lowercase sha256, no path separators, no traversal), optional `offset`
  (0-based byte offset into the body, default 0) and `max_bytes` (default
  512 KiB, 4 MiB ceiling — same clamp as `read_region`). A negative
  `offset` or `max_bytes` refuses `bad_args` (a caller error, not
  silently clamped). Byte paging, not line paging — the body is
  single-line compact JSON (§ 2.3).
- `caller_cwd` is **Optional** (not Required): the spill store is global
  and content-addressed (§ 2.2), so the verb routes by `handle`, not by
  project root — it is not a project-scoped read under the ANTS-1404
  contract.
- Resolves **only** to `<spill-dir>/<handle>.json`; any handle failing
  the regex refuses `bad_args`; a well-formed but absent handle refuses
  `not_found` (mcp-error-codes.md taxonomy — `not_found` over the
  cache-specific `not_cached` because a missing handle is a
  caller-named-resource miss, not an unpopulated cache). It never accepts
  an arbitrary path, so it adds no new filesystem reach beyond the
  spill dir.
- Returns `{content, offset, bytes, total_bytes, truncated}` — the
  requested byte slice as a UTF-8 string (cut on a char boundary),
  `bytes` = slice length, `total_bytes` = full body length, `truncated`
  set when `offset + bytes < total_bytes`.
- Not itself offload-eligible (re-reading a slice must return real
  content).

#### 2.4.1 Structured row-paging (ANTS-3545)

Byte paging (§ 2.4) reaches every byte of a spilled body, but for an
**array-shaped** body it lands mid-JSON: a slice `[offset, offset+max_bytes)`
almost never begins and ends on element boundaries, so the caller cannot
parse the window into rows without also fetching the surrounding bytes. The
§ 2.3.1 structured preview already returns the **first** K rows for free
(`head_rows`), but rows K…2K then force a full re-read. Row-paging closes
that gap: it returns an arbitrary **row** window of the same dominant array,
parsed, with no byte arithmetic.

**Routing (`cmdReadSpill`).** The `cmdReadSpill` handler
(`src/remotecontrol.cpp`) enters **row mode** when the request carries a
**numeric** `row_offset` or `row_count` — tested with `isDouble()`, mirroring
byte mode's `offV.isDouble()` arg reads (`remotecontrol_workspace.cpp`), so a
present-but-non-numeric row arg (`null`, `"5"`) does **not** trigger row mode
and cannot smuggle past the row-arg validation. Either numeric key present ⇒
row mode; **neither** ⇒ the byte path of § 2.4, byte-identical to pre-3545 —
so the byte-mode envelope never grows a `mode` field. Row mode routes to
`mcp::readSpillRows` and emits the row envelope below; byte mode is untouched.
When a request mixes row keys with byte-mode `offset`/`max_bytes`, **row mode
wins** and the byte args are ignored (they page a different unit; there is no
coherent both) — so `cmdReadSpill`'s existing negative-`offset`/`max_bytes`
`bad_args` gate (`remotecontrol_workspace.cpp`) must move **inside** the byte
branch, or a mixed request with a stray negative byte arg would wrongly refuse
instead of running row mode. Only the `^[0-9a-f]{64}$` handle-regex `bad_args`
gate stays a shared pre-gate (it applies to any handle, both modes);
`readSpillRows`, like `readSpill`, does **no** regex check — it receives an
already-validated handle. The **negative-row-arg** `bad_args` gate lives in
`readSpillRows` itself (not `cmdReadSpill`), which is what makes it directly
unit-testable in the core-lib bundle (byte mode can only gate in the handler
because `readSpill`'s `maxBytes<=0→default` can't tell negative from zero;
`readSpillRows` takes the raw `qint64`s and *can*). The row-vs-byte routing
decision and precedence themselves live in `cmdReadSpill` and are only
**source-scrapeable** from the bundle (it links `ants_core_lib`, not
`RemoteControl`), not behaviorally assertable — see INV-14. Row mode is
reachable only if the tool schema declares the row args:
`cmdReadSpill`'s `read_spill` `inputSchema` in `src/claudeintegration.cpp` is
`additionalProperties:false`, so `row_offset` / `row_count` **must** be added
to its `properties` (both `integer`, `minimum:0`) and the tool `description`
extended, or a strict MCP validator drops the keys and the feature is
unreachable (§ 7). Row-mode args:

- `row_offset` — 0-based index of the first row (default 0).
- `row_count` — number of rows to return (default `kSpillRowsDefault`,
  `mcpspill.h`, **= 100**; `0` or omitted ⇒ the default). The window is the
  **half-open** range `[row_offset, row_offset + row_count)`, so passing
  `row_offset` = the prior page's `row_offset + returned rows.size()` (or the
  preview's `head_rows.size()`) continues exactly where the last page ended —
  no overlap, no gap.
- A **negative** `row_offset` or `row_count` refuses `bad_args` (caller
  error, not silently clamped). `cmdReadSpill` parses each row arg as `qint64`
  with omitted ⇒ `0`, then calls `readSpillRows`, which resolves the args in
  this order: **negative ⇒ `bad_args` first**, then `row_count == 0` ⇒
  `kSpillRowsDefault` (so `0`/omitted maps to the default, but a negative is
  refused before the default can mask it). Because `readSpillRows` owns this
  check, the schema's `minimum:0` (which a compliant client validator honours,
  so it never sends a negative) does not make the gate untestable — the
  bundle's direct `readSpillRows(handle, -1, …)` call exercises it.

Row-mode behaviour — `mcp::readSpillRows(const QString &handle, qint64
rowOffset, qint64 rowCount)` (`src/mcpspill.cpp`) returns a `SpillRows { bool
ok; QString code; QString key; QJsonArray rows; qint64 rowOffset; qint64
totalRows; bool truncated; }` result struct (declared in `mcpspill.h`,
parallel to `SpillSlice`; `code` carries the refusal token — `bad_args` /
`not_found` / `too_large` / `not_array` — when `!ok`). `cmdReadSpill` maps the
struct onto the envelope below and, as in byte mode, **owns the human `error`
+ `hint` strings** for each refusal `code` (the struct carries only the token,
not prose — mirroring how `cmdReadSpill` builds `read_spill`'s `not_found`
message from `SpillSlice::code`). A refusal returns `{ok:false, code, error,
hint}` — the `hint` (present on `too_large` / `not_array`, redirecting to byte
paging) is a row-mode addition; byte-mode `read_spill` refusals carry no
`hint`:

- **Inherits INV-5.** Row mode *is* `read_spill`, so it preserves the base
  handle contract first. The `^[0-9a-f]{64}$` handle-regex `bad_args` gate is
  in `cmdReadSpill` (`remotecontrol_workspace.cpp`), ahead of both modes;
  `readSpillRows` itself, like `readSpill`, does no regex check — a
  well-formed but **absent / evicted** handle surfaces as `not_found` (a
  recoverable signal — § 2.5) from the file open, before any parse. These are
  not row-specific refusals; they gate every `read_spill` call.
- **Parse cap — stat before read.** A body larger than
  `kStructuredParseMaxBytes` (`mcpspill.h`, = 1 MiB) refuses `too_large` — row
  mode must `QJsonDocument::fromJson` the whole body, the same RAM bound the
  § 2.3.1 preview honours (§ 4). Crucially, check the file size **before**
  reading it: `QFileInfo(path).size() > kStructuredParseMaxBytes` (or
  `QFile::size()` after open, before `readAll()`) → `too_large`. Do **not**
  mirror `readSpill`'s unconditional `readAll()` (`mcpspill.cpp:233`) — a spill
  body can be many MB (offload threshold clamps up to 1 MiB, but
  `get_scrollback` bodies are larger), so reading first would load the whole
  body into RAM only to reject it, defeating the RAM bound on the earlyoom
  host. The refusal `hint` points the caller at byte paging
  (`offset`/`max_bytes`), which has no parse cost.
- **Same dominant array as the preview.** Row mode selects the dominant
  array with the **identical** detection `offloadBody` uses for
  `head_rows_key` (§ 2.3.1) — the root object's own direct **non-empty**
  array member with the most elements, ties to the lexicographically-first
  key. The two share one helper (`dominantArrayKey`, `src/mcpspill.cpp`), so
  the key a caller reads from `head_rows_key` is guaranteed to be the key
  row-paging pages: preview and pager can never disagree.
- **Not row-shaped.** A body that does not parse, is **not a JSON object**
  (including a bare root-level JSON *array* — row mode requires an
  object-wrapped array, matching § 2.3.1's `doc.isObject()` gate; no Ants verb
  emits a root-array envelope, all emit `{ok, …}`), or carries no non-empty
  root array member refuses `not_array` (no rows to page — byte-page it; the
  `hint` says so). This is the § 2.3.1 non-array test surfaced as a refusal
  rather than a silent preview omission. By the shared `dominantArrayKey`
  helper this also fixes the tabular (ANTS-2090) behaviour: a **fully**-tabular
  body (every array-of-objects packed into a nested `{__cols__,__rows__}`)
  exposes no root array → `not_array`, while a tabular body with a leftover
  sibling scalar array (e.g. `find_sources`'s `unmatched_terms`) pages **that**
  sibling — the same array its `head_rows_key` preview names.
- **Slice.** Guard the past-end case **first** (`row_offset >= total_rows` →
  **empty** `rows`, `truncated:false` — past-end, mirroring byte mode's
  offset-past-end), then take `effective_count = min(row_count, total_rows −
  row_offset)` rows starting at `row_offset`. Compute the window length by
  **subtraction** (`total_rows − row_offset`, guaranteed ≥ 0 after the guard),
  never as `row_offset + row_count` — an addition of two caller-controlled
  values could overflow `qint64` and select a negative bound; byte mode dodges
  the same trap with `qMin(cap, total − offset)` (`mcpspill.cpp:247`). Read
  the row args as `qint64` the way byte mode reads `offset`/`max_bytes`. Each
  returned element is emitted **whole** (re-serialized through `QJsonDocument`,
  so deep-equal to the spilled element, never byte-cut). No output-byte
  ceiling is applied: the parse cap already bounds the **whole** body at 1 MiB
  (< `kReadCeilingBytes` = 4 MiB), so any row window — a subset of that body —
  is under the byte ceiling by construction, and a page is never cut short
  *below* `effective_count` for size. `row_count` and `total_rows` alone bound
  the window; a large `row_count` returns up to the remaining rows (≤ the
  ≤ 1 MiB body), which the caller sized deliberately. Note the **default**
  page (`row_count` omitted ⇒ `kSpillRowsDefault = 100`) is bounded by *row
  count*, not bytes, so for a large-row body it can exceed byte mode's 512 KiB
  default page (up to the ≤ 1 MiB body) — an intentional trade (row mode is
  row-count-predictable, byte mode is byte-predictable), still hard-bounded by
  the parse cap.
- **Envelope.** `{ok:true, mode:"rows", key, rows, row_offset, total_rows,
  truncated}`: `key` = the dominant array's field name (identical value to the
  offload preview's `head_rows_key` — a shorter name for the row envelope, not
  a different array), `rows` = the parsed window, `row_offset` = the request's
  offset echoed back, `total_rows` = the dominant array's full length,
  `truncated` = `row_offset + rows.size() < total_rows` (more rows remain).
  The **returned** row count is `rows.size()` — not echoed as a separate field
  (it would collide with the input `row_count` arg name and be redundant with
  the array), exactly as byte mode reports its returned length via `bytes`
  rather than echoing the requested `max_bytes`. To page, the caller advances
  `row_offset` by the **returned** `rows.size()` (parallel to byte mode's
  `offset + bytes`); the only short page is the final one (`row_offset +
  row_count > total_rows`), which carries `truncated:false`. The byte-mode
  envelope (§ 2.4) is unchanged and carries no `mode` field, so an existing
  byte-paging caller sees no difference.
- Row mode is **not** itself offload-eligible (read_spill is excluded from
  `isOffloadEligible`, INV-10) and adds no filesystem reach — it resolves the
  same `<spill-dir>/<handle>.json` by the same validated handle.

### 2.5 Eviction — bounded disk, named policy

Spill files are derived and disposable. Cap (constants in `mcpspill.h`):

- **`kSpillMaxFiles = 64`** and **`kSpillMaxBytes = 64 MiB`** — whichever
  binds first. A body over `kSpillMaxBytes` is not spilled (ANTS-5104,
  decided 2026-09-14): `offloadBody` returns a `result_too_large` refusal
  with a hint to narrow the call, writes nothing and evicts nothing, so
  INV-7 holds. On each write, after adding the new file, evict
  oldest-by-mtime until both caps hold — but **never evict the handle
  written this call**, keyed on **handle identity** (not write-time): an
  idempotent re-spill (INV-8) of an already-present body keeps the file's
  *old* mtime, so an mtime-only carve-out would wrongly let it be evicted
  as "oldest." The carve-out therefore names the current handle, and a
  re-spill **touches** the file's mtime so LRU treats a re-accessed body
  as freshly used. (This also covers a sub-second mtime tie with a
  sibling.)
- **Session-start sweep**: `mcp::spillSweep()` (in `src/mcpspill.cpp`)
  drops any spill file with mtime older than **24 h**. Called once from
  the MCP-server bring-up in `src/mainwindow.cpp` (beside
  `mcp::setOffloadConfig`, with the other one-shot cache setup), so it runs
  at session start without a timer. Stays a candidate for the documented `~/.cache` GC sweep in
  mcp-caches.md.

RAM cost is ~0: the body streams to disk via `QSaveFile`; only the head
prefix (`offloadHeadBytes`, default 2 KiB) is held to build the
envelope. Disk is bounded by the caps above.

**Evicted-handle race.** A handle the agent holds can be evicted before
it re-reads (cap eviction or the 24 h sweep), so `read_spill` returns
`not_found`. This is an **expected, agent-recoverable** outcome, not an
error to design against: eviction is oldest-mtime-first, so the handle
just returned is the *newest* and survives until `kSpillMaxFiles` newer
spills accrue in the same session; if it is nonetheless gone, the agent
re-issues the original tool call (the body is reproducible). No
pin-most-recent-N mechanism is added — oldest-first eviction already
protects the freshest handle, and a recoverable `not_found` is cheaper
than the bookkeeping.

**Concurrency.** MCP `tools/call` dispatch is single-threaded (one call
at a time on the Qt event loop — the same assumption `mcp_trace` relies
on, ANTS-1360 INV-10), so a spill write and an eviction pass never
interleave; serialised dispatch is what makes the write+evict sequence
safe. Content-addressing is a separate, weaker property: it only
guarantees a re-spill of the same body is a harmless overwrite of an
identical file — not write-race safety, which the single thread
provides.

### 2.6 Config keys

- `claude.mcp_offload_large_results` (bool, default **true** since the
  2026-06-25 fast-follow; **false** in the v1 ship — see § Open questions
  OQ-1) — session default for `mcp::offloadDefault()`. Exposed in
  Settings → General ("Offload huge Ants MCP replies to a preview +
  pointer"); persisted + republished live on Apply.
- `claude.mcp_offload_threshold_bytes` (int, default **16384**, clamp
  `[4096, 1048576]`) — `mcp::offloadThresholdBytes()`. Config-file-only.
- `claude.mcp_offload_head_bytes` (int, default **2048**, clamp
  `[256, 16384]`) — `mcp::offloadHeadBytes()`. Config-file-only.

## 3. Invariants

- **INV-1** — Offload fires only for an `isOffloadEligible` verb whose
  final body is `>=` the call's threshold **and** `> offloadHeadBytes()`
  (the head guard, § 2.1) **and** offload is requested (per-call
  `offload:true` or session default) **and** the call is not `raw:true`
  (ANTS-2218 — a raw request returns verbatim bytes, never a spill
  envelope). A sub-threshold, fits-in-head, ineligible, raw-requested,
  control-plane, refusal, or 304 response is never offloaded.
  The call's threshold is `mcp::offloadThresholdFor(toolName, args)`
  (ANTS-5364): `offloadThresholdBytes()`, except that `roadmap_query`'s
  explicit `max_body_bytes` raises it to that value plus 4 KiB, at most
  64 KiB, and `read_regions` (ANTS-5291) and `read_region` (ANTS-4954) use
  64 KiB on every call. Neither lowers the configured threshold.
  `Inv1ExplicitBodyCapRaisesThreshold` and `Inv1ReadRegionsUsesTheCeiling` pin
  those rules.
  *Test:* `Inv1EligibilityAndRequestResolution` — drives the eligibility
  set (`isOffloadEligible`) and per-call-vs-default request resolution
  (`offloadRequested`). The `>=`-threshold / `>`-head boundary is the
  extracted `mcp::shouldOffload(bodyBytes)` predicate (ANTS-3552), gated at
  the dispatch site (`claudeintegration.cpp`): `offloadBody` applies no
  *threshold* guard — it spills whenever the head+pointer envelope would
  shrink the body (else INV-9 fail-open), independent of the threshold — so
  the dispatch must gate the call by size. `Inv1ShouldOffloadBoundary`
  asserts the boundary behaviourally (fires at `==threshold`; the `>head`
  guard binds when the head clamp meets/exceeds the threshold);
  `Inv1OffloadBodyHasNoInternalThresholdGuard` pins that a below-threshold
  body handed to `offloadBody` still spills; `Inv1DispatchUsesShouldOffload`
  source-scrapes the dispatch gate.
- **INV-2** — When offload fires, the returned body is the head+pointer
  envelope with `offloaded:true`, a 64-hex `handle`, `bytes` equal to the
  raw body's UTF-8 length, and a `head` that is a strict
  UTF-8-char-boundary prefix of the raw body with `head_truncated:true`
  (always, since the head guard means an offloaded body always exceeds
  the head — § 2.1). The envelope also carries a non-empty `hint` string
  naming `read_spill` (the recovery contract — it is how the agent learns
  the re-read verb). It also carries `threshold_bytes`, the call's
  threshold from INV-1, so `bytes` can be read against it (ANTS-4954).
  *Test:* compare `head` against the first N bytes of
  the spilled file; assert `bytes == file size`, `head_truncated`, and
  that `hint` contains `read_spill`. `Inv2EnvelopeNamesItsThreshold`
  asserts `threshold_bytes`.
- **INV-3** — The spill file at `<spill-dir>/<handle>.json` contains the
  full raw pre-wrap body byte-for-byte; `sha256(file) == handle`.
  *Test:* hash the file, compare to the envelope handle.
- **INV-4** — Spill file is mode 0600 and the `mcp-spill/` dir is 0700.
  *Test:* `QFileInfo::permissions()` mask assertion (mirrors
  `local_persistence_hardening`).
- **INV-5** — `read_spill` resolves **only** under the spill dir: a
  `handle` not matching `^[0-9a-f]{64}$` (incl. any `/`, `..`, abs path)
  refuses `bad_args`; a well-formed but absent handle (never spilled, or
  evicted) refuses `not_found` — a recoverable signal, not an error
  (§ 2.5). It reads no path outside the spill dir. *Test:* the
  `^[0-9a-f]{64}$` → `bad_args` regex gate lives in `cmdReadSpill`, **not**
  the free `readSpill`/`readSpillRows` (which resolve `spillPath(handle)`
  and return `not_found` for anything absent), so it is asserted by
  source-scrape in `Inv10ReadSpillWiring` (the bundle links `ants_core_lib`,
  not `RemoteControl`, so the regex refusal can only be checked by literal —
  a traversal / uppercase-hex / wrong-length handle never reaches a
  behavioural `bad_args` at the free-function layer); `Inv5And6ReadSpillPaging`
  round-trips a real handle and asserts a well-formed-but-absent handle
  yields `not_found` (the evicted-handle case is byte-identical — the file
  is simply gone).
- **INV-6** — `read_spill` pages by **byte offset**: returns
  `{content, offset, bytes, total_bytes, truncated}` where `content` is
  the `[offset, offset+max_bytes)` slice cut on a UTF-8 char boundary,
  honouring the 512 KiB default / 4 MiB ceiling clamp; `truncated` is set
  iff `offset + bytes < total_bytes`, and `bytes == len(content)`. To
  page, the caller advances by the **returned** `offset + bytes` (not the
  requested `offset + max_bytes`) — the char-boundary cut can shorten a
  slice, so naive `max_bytes` paging would drop the straddling bytes.
  The cut is taken against the body, not against a window exactly
  `max_bytes` long, and a `max_bytes` smaller than the character at
  `offset` returns that one whole character so paging always advances.
  Each page reads only its own bytes from the spill file. *(Amended
  2026-09-13, ANTS-5104, recording what was built.)*
  Edge cases: `offset >= total_bytes` → empty `content`,
  `truncated:false`; `max_bytes <= 0` (0 or omitted) falls back to the
  512 KiB default page (`kReadDefaultBytes`), and any positive value is
  clamped to the 4 MiB ceiling (`kReadCeilingBytes`) — so a `max_bytes:0`
  request at an in-range offset returns up to a 512 KiB page, and at an
  at/after-end offset returns empty. `cmdReadSpill` refuses a negative
  `offset` or `max_bytes` with `bad_args` before `readSpill` runs.
  (A zero-byte body never reaches here — offload requires
  `bytes > offloadHeadBytes() >= 256`, so no zero-byte spill file exists.)
  *Test:* `Inv5And6ReadSpillPaging` — full read, paging by returned
  `offset+bytes`, the offset-past-end empty case, and unknown-handle
  `not_found`; `Inv6ReadSpillMaxBytesDefaultsToPage` pins the `max_bytes<=0`
  → 512 KiB default-page fallback (both `0` and a negative value, at the free
  function), and `Inv6ReadSpillNegativeArgGateAtDispatch` source-scrapes
  `cmdReadSpill`'s negative-`offset`/`max_bytes` → `bad_args` gate
  (ANTS-3552).
- **INV-7** — Eviction holds both caps: after writes, the spill dir has
  `<= kSpillMaxFiles` files and `<= kSpillMaxBytes` total, evicting
  oldest mtime first, never evicting the handle written this call (keyed
  on handle identity, § 2.5); the session-start sweep (`mcp::spillSweep()`)
  drops files older than 24 h. *Test:* `Inv7EvictionAndSweep` writes past
  the file cap and asserts the just-written handle survives with the count
  `<= kSpillMaxFiles`, then backdates a spill's mtime, calls
  `mcp::spillSweep()`, and asserts removal. (An idempotent re-spill (INV-8)
  rewrites the file and so refreshes its mtime, so a re-spilled handle is
  never the oldest — the handle-identity carve-out and mtime ordering cannot
  conflict in practice, so that case is not separately exercised.)
- **INV-8** — Content-addressing: spilling a byte-identical body twice
  yields one file and a stable handle (idempotent write); a differing
  body yields a different handle. No relocation shadow (mcp-caches.md).
  *Test:* spill same body twice → one file, equal handles.
- **INV-9** — Byte-accounting honesty: the offload (inserted at the
  8854–8856 gap) runs before `outBytes = wrapped.toUtf8().size()`
  (`claudeintegration.cpp:8884`), so the `out_bytes` / `resp_bytes`
  `recordDispatch` records (and `mcp_trace` / `token_usage` surface) is
  the small *offloaded* envelope's wire size, not the pre-offload body.
  (The recorded raw-bytes arg is `buf.size()` — the inbound *request*
  buffer, `:8895` — independent of offload either way; the wire saving
  shows up in `out_bytes`, not raw-bytes.) *Test:* source-scrape that the
  `mcp::offloadBody` call precedes the `recordDispatch(` call in the
  dispatch body (scrape by symbol, not literal line numbers — every line
  number in this spec is illustrative as of 2026-06-16 and drifts on
  edits higher in the function, so the test asserts ordering by symbol);
  manual recipe — an offloaded
  call's recorded `out_bytes` is strictly less than the envelope `bytes`
  (the § 2.1 head guard plus the ANTS-3540 finished-envelope fail-open in
  `mcp::offloadBody` guarantee the strict `<` at every config — an envelope
  that would not shrink the body is never emitted; the body is returned
  unchanged instead). Note the ANTS-3540 fail-open still has a disk
  side-effect: `offloadBody` writes the spill file and runs `evict()`
  **before** the finished-envelope size check, so a body that fails open on
  that check leaves its content-addressed spill file on disk (distinct from
  INV-11's write-failure fail-open, which writes nothing). That file is
  benign — content-addressed (idempotent, re-used verbatim by any later real
  offload of the same body) and reclaimed by the 24 h sweep — a redundant
  write, not a correctness issue.
- **INV-10** — `read_spill` is wired like every other read verb: handler
  `RemoteControl::cmdReadSpill` (`src/remotecontrol.cpp`) dispatched via
  `rcDelegate(&RemoteControl::cmdReadSpill)` in mainwindow, schema in
  `claudeintegration`, `registerToolProvider("read_spill"` in mainwindow
  — but with an **Optional** (not Required) `caller_cwd` contract, since
  the spill store is global/content-addressed (§ 2.4) — and is NOT in
  `isOffloadEligible`. *Test:* `WiringContract` source-scrape (mirrors
  `mcp_read_region`); assert `read_spill` is absent from the ANTS-1404
  Required-tier table.
- **INV-11** — Spill-write failure is fail-open: if the spill write
  fails (dir unwritable, disk full, `QSaveFile::commit()` false), the
  call returns the original full `responseText` unchanged — never an
  error, never an `offloaded:true` envelope pointing at a missing file
  (§ 2.2). `QSaveFile` is commit-or-nothing, so a failed write leaves no
  partial / orphaned temp file in the spill dir. *Test:*
  `Inv11FailOpenFaultInjection` points the spill dir under a regular-file
  parent (so `ensurePrivateDir`'s `mkpath` fails regardless of uid), then
  asserts `offloadBody` returns the body verbatim (`offloaded` absent/false)
  with no spill file left behind; `Inv11FailOpenWiring` source-scrapes all
  four `return body;` fail-open exits (`ensurePrivateDir` / open /
  short-write / `commit`). A fifth exit: a spill whose permissions cannot
  be set owner-only is removed and the body returned. *(Amended 2026-09-13,
  ANTS-5104, recording what was built.)*
- **INV-12** — Config clamps hold and the head is char-safe:
  `offloadThresholdBytes()` clamps to `[4096, 1048576]`,
  `offloadHeadBytes()` to `[256, 16384]`; the `head` prefix never splits
  a UTF-8 multi-byte sequence. *Test:* out-of-range config values clamp;
  a body whose byte `offloadHeadBytes()` lands mid-sequence yields a
  shorter, valid-UTF-8 `head`.
- **INV-13** (ANTS-3538) — For an offloaded body `<= kStructuredParseMaxBytes`
  that parses as a JSON object with a non-empty array member, the envelope
  **additionally** carries `head_rows_key` (the dominant array's field name
  — the root object's own direct **non-empty** array member with the most
  elements, ties to the lexicographically-first key per `QJsonObject`
  iteration order), `row_count` (its full element count), `head_rows` (the
  first K **complete** elements, K bounded by the § 2.3.1 row budget so the
  envelope stays strictly smaller than the raw body — INV-9 preserved), and
  `head_rows_truncated` (`row_count > head_rows.size()`). Each `head_rows`
  element is emitted whole (never byte-cut), so `head_rows` is always
  parseable JSON. The byte-prefix `head` / `head_truncated` (INV-2) are
  unchanged and always present — the structured preview is purely additive.
  A body that does not parse, is not a JSON object, carries no non-empty
  array member, is tabular-encoded (no root-level array member — § 2.3.1),
  or exceeds `kStructuredParseMaxBytes` emits **no** structured preview
  (byte-prefix head only); a body whose first complete element does not fit
  within `headRowsBudget` (§ 2.3.1 — only marginally over the head guard, or
  a single very large row) omits the four `head_rows` fields but still carries
  the § 2.3.1 shape preview and the row-paging `hint`. *Test:*
  offload a `workspace_search`-shaped body with N > K rows; assert
  `head_rows_key=="matches"`, `row_count==N`, `head_rows` parses as an
  array of complete rows with `size() <= kHeadRowsMax`,
  `head_rows_truncated==true`, and every element of `head_rows` deep-equals
  the corresponding element of the spilled body's array; assert the
  offloaded envelope's byte size stays `< bytes` (INV-9 preserved). Add a
  **budget-driven** truncation case — a few large-but-fitting rows so
  `headRowsBudget` (not `kHeadRowsMax`) forces the cut — and assert
  `head_rows_truncated==true` with `head_rows.size() < row_count`. Then
  cover the omission edges that carry the INV-9-preserving fallback, each
  asserting the four `head_rows` fields absent while `head` / `head_truncated`
  remain: a scalar-only (no-array) body; a body whose only array member is
  empty (`{"matches":[]}`); an unparseable / non-JSON-object-root body; a
  body `> kStructuredParseMaxBytes` (parse skipped); and a single-oversized-row
  body whose first element exceeds `headRowsBudget`
  (`Inv13SingleOversizedRowOmitted`). (A body marginally over a head≈threshold
  config no longer reaches the structured path at all — the whole offload fails
  open per ANTS-3540, covered by `Inv9BaseEnvelopeNeverExceedsBody`.) For the
  tabular (ANTS-2090)
  interaction, assert a body whose **only** root array member was packed
  (all arrays-of-objects) emits the byte-prefix head only, while a tabular
  body with a leftover sibling scalar array (e.g. `find_sources`'s
  `unmatched_terms`) structured-previews that sibling. Finally, lock the
  tie-break: a body with two equal-length root arrays (keys e.g. `"aaa"` /
  `"zzz"`) asserts `head_rows_key=="aaa"` (the lexicographically-first key).
- **INV-14** (ANTS-3545) — `read_spill` **row-paging**: a request carrying a
  `row_offset` or `row_count` key returns `{mode:"rows", key, rows,
  row_offset, total_rows, truncated}` where `key` is the **same** dominant
  array `head_rows_key` names (both go through the one `dominantArrayKey`
  helper — § 2.4.1 — so preview and pager select the identical array), `rows`
  is the parsed **half-open** `[row_offset, row_offset + row_count)` window
  (each element whole and deep-equal to the spilled element, never byte-cut),
  the returned count is `rows.size()` (not echoed separately — the caller
  pages by `row_offset + rows.size()`, mirroring byte mode's `offset +
  bytes`), `total_rows` == the dominant array's full length, and `truncated`
  iff `row_offset + rows.size() < total_rows`. No output-byte ceiling binds:
  the 1 MiB parse cap bounds the whole body below the 4 MiB `kReadCeilingBytes`,
  so a window (a subset) is always under it and a page is bounded only by
  `row_count` / `total_rows`. Refusals — in `cmdReadSpill`-then-`readSpillRows`
  order: (1) a handle failing `^[0-9a-f]{64}$` → `bad_args` (a `cmdReadSpill`
  pre-gate, ahead of both modes); (2) a **negative** `row_offset` /
  `row_count` → `bad_args` (in `readSpillRows` itself; only `0`/omitted →
  default, never a negative); (3) an absent/evicted handle → `not_found`
  (inherited from INV-5); (4) a body `> kStructuredParseMaxBytes` → `too_large`
  (statted before read); (5) a body that does not parse / is not a JSON object
  (incl. a bare root array) / carries no non-empty root array → `not_array`.
  `row_offset >= total_rows` → empty `rows`, `truncated:false`. When a request
  mixes row keys with byte `offset`/`max_bytes`, row mode wins and the byte
  args are ignored. A request with **neither** row key is the byte path
  (INV-6), byte-identical to pre-3545 — so `mode` is absent from every
  byte-mode envelope. *Test:* `Inv14ReadSpillRowPaging` calls
  `mcp::readSpillRows` directly (like INV-5/6 call `mcp::readSpill`).
  Must-fail-first here is by **link failure** — `readSpillRows` doesn't exist
  pre-feature, so the bundle won't link (distinct from INV-5/6/13, which call
  existing functions and red by *assertion*; same reproduce-first intent,
  different RED mechanism). It offloads an N-row body with N **not** a multiple
  of K, pages `[0,K)`, `[K,2K)`, then a **final partial** page (`< K` rows,
  `truncated:false`), asserting the windows are contiguous and each deep-equals
  the spilled rows at absolute indices; asserts the paged `key` equals the
  offloaded envelope's `head_rows_key`; `row_offset >= total_rows` → empty +
  `truncated:false`; a **negative** `row_offset`/`row_count` → `code=="bad_args"`
  (a direct-call assertion, since the gate is in `readSpillRows`); a
  scalar-only body → `not_array`. `Inv14RowModeRefusalEdges` adds the two
  further `not_array` shapes (an empty-array member and a bare root array)
  plus the `> kStructuredParseMaxBytes` → `too_large` gate (asserted on the
  row path, the file left unread — the stat gate). The `too_large` /
  `not_array` refusals each carry a byte-paging redirect `hint`, but that
  `hint` is built in `cmdReadSpill` (the `SpillRows` struct `readSpillRows`
  returns has **no** `hint` field), so it is asserted by source-scrape, not
  on the direct `readSpillRows` call. A companion **source-scrape** (`Inv14RowModeWiring`,
  mirroring INV-10 — the bundle links `ants_core_lib`, not `RemoteControl`, so
  `cmdReadSpill` is not runtime-callable and only its *literals* can be
  asserted, not its behaviour) that
  `cmdReadSpill` routes on numeric `row_offset`/`row_count` to `readSpillRows`,
  that the `read_spill` schema declares the two row props, and that both
  redirect `hint` literals are present — the observable
  `mode:"rows"` literal is absent from pre-feature code, giving the wiring test
  its RED.
- **INV-15** (ANTS-4692) — For an offloaded body that parses as a JSON object
  within `kStructuredParseMaxBytes`, the envelope **additionally** carries
  each root member that is not the dominant array, is not a key the offload
  owns (§ 2.3.2), fits `kPreservedFieldMaxBytes`, and fits the running
  preserved total — plus, regardless of that total, every key
  `mcp::isProtectedCompactKey` protects. Members are taken protected-first,
  then in `QJsonObject` key order. A body member named `ok` overrides the
  envelope's placeholder `ok`. A member rejected by the per-member cap or the
  running total is named in `preserved_omitted`; a skipped key and the
  dominant array are not. So an absent field is never ambiguous between
  dropped and never-requested. The INV-2 byte head and the § 2.3.1 preview are
  unchanged — preservation is purely additive and is measured **before** the
  row budget, so INV-9 holds by construction with the ANTS-3540 fail-open as
  backstop. *Test:* offload a `roadmap_query check_sync`-shaped body carrying
  `source`, `file_in_sync` and a dominant `bullets` array; assert `source` and
  `file_in_sync` survive on the envelope, `bullets` does not, and the envelope
  stays `< bytes`. Assert a `sync_checked:false` body keeps that field when
  the preserved-total budget is already spent by earlier members (the
  protected floor), and that a protected key over `kPreservedFieldMaxBytes` is
  still dropped. Assert an oversized member is absent **and** named in
  `preserved_omitted`. Assert a body carrying `ok:false` yields `ok:false` on
  the envelope. Assert a body member named `handle` does not overwrite the
  spill handle. Assert a body over `kStructuredParseMaxBytes` preserves
  nothing and emits no `preserved_omitted` (no parse, § 2.3.2).

## 4. RAM / build cost

- **RAM:** ~0 steady-state. Per offloaded call: the head prefix
  (`offloadHeadBytes`, ≤ 16 KiB) + transient `QSaveFile` buffering. No
  in-process retention of spilled bodies. ANTS-3538 adds a transient
  `QJsonDocument::fromJson` parse of the body when building the structured
  preview (a parsed value tree — up to ~3–6× the JSON text for a
  many-small-elements body — briefly, freed before return); the body is
  already streamed to disk on the same call, so this is a comparable,
  bounded one-shot cost on the (cold) offload path, not steady-state. The
  parse is **capped**: it is skipped for bodies over `kStructuredParseMaxBytes`
  (`mcpspill.h`, = 1 MiB), so the transient peak is bounded at ~6 MiB
  regardless of how large a spilled body is (the offload threshold clamps
  up to 1 MiB and unbounded `get_scrollback` bodies can be multi-MB — the
  cap keeps the parse off the earlyoom host's back).
- **Row-paging (ANTS-3545)** reuses the *same* transient parse on the
  `read_spill` path — `readSpillRows` parses the spilled body to slice rows —
  under the **identical** `kStructuredParseMaxBytes` (1 MiB) cap: a larger
  body refuses `too_large` **without reading it** — the stat-before-read gate
  (§ 2.4.1) checks `QFileInfo::size()` first, so the `readAll()` only ever
  loads a ≤ 1 MiB body. The transient peak is that ≤ 1 MiB read + the ~6 MiB
  parse tree + the re-serialized row slice (≤ 1 MiB), so ~8 MiB on the (cold)
  re-read path — bounded, one-shot, no steady-state retention (read buffer,
  parsed tree, and slice all freed before return). (Byte-mode `readSpill` has
  no such cap — its `readAll()` buffer equals the *full* body size, which is
  the existing behaviour row mode deliberately does **not** copy.)
- **Disk:** bounded by `kSpillMaxFiles` (64) × per-file size, hard-capped
  at `kSpillMaxBytes` (64 MiB); oldest-mtime eviction + 24 h sweep.
- **Build:** one new source pair `src/mcpspill.{h,cpp}` in
  `ants_core_lib`; one new feature-test bundle source. No new external
  library (Qt6::Core only). ANTS-3545 adds only a `readSpillRows` function +
  a shared `dominantArrayKey` helper to the existing pair (no new source, no
  new library).

## 5. Out of scope

- A Settings-UI toggle for the config keys — config-file-only in v1.
  **Done in the 2026-06-25 fast-follow:** the master bool gained a
  Settings → General checkbox ("Offload huge Ants MCP replies to a preview
  + pointer"); threshold/head stay config-file-only tuning keys.
- Flipping `claude.mcp_offload_large_results` default ON — deferred to a
  fast-follow once `read_spill` is field-proven (§ Open questions).
  **Done in the 2026-06-25 fast-follow** (default true; § 2.6, OQ-1).
- A `migrate-cwd`-style re-key — N/A; content-addressed, not path-keyed.
- Reusing `read_region` for the re-read — explicitly rejected (§ 2.4);
  would weaken its project-root path boundary.
- **Row-paging (ANTS-3545) non-goals:** row-level *field projection* (return
  only named keys per row), cross-page *dedup*, and a hard byte cap on the row
  window beyond the 1 MiB parse bound are all out of scope — row mode returns
  whole rows of the one dominant array, bounded by `row_count` and the parse
  cap. A body `> kStructuredParseMaxBytes` is not row-pageable at all (refuses
  `too_large`); byte paging remains its only re-read path.

## 6. Tests

Feature test: `tests/features/mcp_result_offload/`. One `test_*.cpp` in
the bundle carries a dedicated case per invariant (INV-1..INV-15) — the
spill/offload INVs (1–4, 7–9, 11–13, 15) assert against `mcp::offloadBody` /
`mcpspill` directly; the `read_spill` INVs 5–6 call `mcp::readSpill`
directly, INV-14 (ANTS-3545) calls `mcp::readSpillRows` directly (row
continuity incl. a final partial page + `key`==`head_rows_key` + the
negative-arg `bad_args` / `not_array` / `too_large` / past-end edges) and adds
a `cmdReadSpill` source-scrape (`Inv14RowModeWiring` — row-key routing →
`mode:"rows"` + the two schema props, mirroring INV-10; literals only, since
the bundle can't link `RemoteControl`), and INV-10 source-scrapes the
byte-verb wiring (`cmdReadSpill` handler + schema + registration). INV-9
carries two cases: `Inv9OffloadPrecedesRecordDispatch` (byte-capture
ordering) and `Inv9BaseEnvelopeNeverExceedsBody` (the ANTS-3540 base-envelope
fail-open — an offloaded envelope is always strictly `< bytes`).
INV-13 (ANTS-3538) asserts the structured preview against a
`workspace_search`-shaped body directly through `mcp::offloadBody`,
including the budget-driven truncation case, the tie-break case (two
equal-length root arrays → lexicographically-first key), the omission edges
(scalar-only / no-array, empty array member, unparseable / non-object,
over-`kStructuredParseMaxBytes`, single oversized row,
tabular whose only root array was packed) that carry the INV-9-preserving
all-four-or-none fallback, and the positive tabular-sibling case (a tabular
body with a leftover scalar array — `find_sources`'s `unmatched_terms` —
structured-previews that sibling).
Label `features;fast`. Add the source to an existing feature bundle's
`SOURCES` list (per `tests/features/README.md`; do NOT `add_executable`).
Verify each invariant **fails against pre-feature code** before wiring
the implementation (offload absent → no `offloaded:true` envelope;
`read_spill` unregistered → unknown-tool).

## 7. Cross-doc impact

- **CLAUDE.md** — add `read_spill` to the MCP verb catalogue mention;
  note the offload config keys beside the auto-model keys block.
- **docs/standards/mcp-caches.md** — add the `mcp-spill` row to the cache
  inventory table (content-addressed; **Cold/Orphan**, never Shadow;
  GC-sweep candidate) in the same commit (§ checklist item 3).
- **docs/standards/mcp-tools.md** / **mcp-behavioural-notes.md** —
  document the `offload` arg + `read_spill` verb contract; note the
  ANTS-3538 structured preview (`head_rows` / `row_count` /
  `head_rows_key` / `head_rows_truncated`) on the offload envelope and the
  ANTS-3545 `read_spill` row-mode (`row_offset` / `row_count` →
  `{mode:"rows", key, rows, row_offset, total_rows, truncated}`); and state
  the ANTS-4692 preserved fields — that the body's own top-level members
  survive an offload, and that a member named in `preserved_omitted` was
  dropped rather than never requested.
- **docs/standards/mcp-error-codes.md** — add `not_array` (newly introduced)
  and `too_large` (already used at `remotecontrol.cpp` read_region/apply_edits
  oversized-resource sites but never tabulated — the taxonomy tabulates only
  `result_too_large`, not bare `too_large`) as **category 2 (resource-state)**
  codes, in the same commit as the refusal sites (the standard's "extend the
  taxonomy in the same commit as the new refusal" rule). ANTS-3545's row mode
  is the closing use site for both.
- **src/claudeintegration.cpp** (code, same commit) — the `read_spill`
  `inputSchema` is `additionalProperties:false`, so `row_offset` / `row_count`
  (`integer`, `minimum:0`) **must** be added to its `properties` and the tool
  `description` extended, else the row args are rejected and the mode is
  unreachable (§ 2.4.1 Routing). Not a doc, but load-bearing and easy to miss.
- **src/mcpspill.cpp `offloadBody` `hint`** (code, same commit) — the
  per-response `hint` (`mcpspill.cpp:141`) currently advertises only byte
  paging ("optional offset/max_bytes to page"). Extend it to also mention row
  paging (`row_offset`/`row_count`) gated on **`domCount > 0`** — the dominant
  array count computed at the top of the ≤ 1 MiB structured-preview block,
  **before** the `head_rows` budget check. Gating on `domCount`, **not** on
  `head_rows_key` being emitted, is deliberate: a body whose first row exceeds
  `headRowsBudget` omits the whole preview (no `head_rows_key`) yet still has a
  pageable dominant array — and that large-row body is exactly where byte
  paging lands mid-row and row paging helps most. This gate also correctly
  stays byte-only for a `> 1 MiB` body (row mode would refuse `too_large`, and
  `domCount` is never computed there) and a non-array body (`domCount == 0`).
  That envelope is where the agent decides how to continue, so the ergonomic
  path must be advertised there, not only in the tool schema it may not re-read
  (discoverability gates the feature's value).
- **CHANGELOG.md** — enhancement entry on ship.
- **tool_info catalog** — `read_spill` gains a one-line *when to use*
  (byte paging **or** `row_offset`/`row_count` row paging). The tool
  `description` / `selection_hint` must concretely tell the agent that row mode
  returns a **different** envelope shape (`{mode:"rows", key, rows, …}`, no
  `content`), not merely that the args exist — so it knows `mode:"rows"` is a
  distinct contract.

## 8. Open questions

- **OQ-1 (default) — RESOLVED at sign-off (2026-06-16):** v1 ships
  `claude.mcp_offload_large_results` default **OFF** (explicit
  `offload:true` opt-in). Offload changes the response contract — an agent
  that ignores `offloaded:true` and treats `head` as complete would
  silently act on truncated data — so shipping it ON across every session
  before `read_spill` round-tripping is field-proven is the one place the
  project's "token-savers default ON" rule yields to safety. Flipping the
  default ON is a tracked fast-follow (§ 5) once the re-read path has real
  miles. No remaining open questions.
  **Fast-follow shipped (2026-06-25):** default flipped to **ON**
  (config.cpp `claudeMcpOffloadLargeResults()` → `toBool(true)`), with the
  contract-change risk held in check by the fail-open guard (INV-11 — any
  spill-write failure returns the body unchanged) and the conformance suite
  proving the `read_spill` round-trip (INV-5/6/10). Live field-proof of the
  round-trip through a running instance happens at the next relaunch.

## Cold-eyes loop log

Rows live in [`../reviews/ANTS-2094-proactive-server-side-result-offload-loop-log.md`](../reviews/ANTS-2094-proactive-server-side-result-offload-loop-log.md).
