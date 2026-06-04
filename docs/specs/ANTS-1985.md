# ANTS-1985 — `tool_info {catalog:true}`: one-call full-verb catalog

**Status:** implemented on main (2026-06-04), ships next release; cold-eyes loops 1–6 folded.
**Kind:** feature.
**Lanes:** `claudeintegration` (`tool_info` inline handler +
descriptor schema), `mcporientation` (prelude "Full catalog:" line),
`mcp_tool_info_verb` feature test.
**Source:** ROADMAP.md ANTS-1985 (user-request-2026-06-04, § v0.7.94
ship-day findings).

**Layman:** A Claude session sees a list of ~70 Ants helper names at
startup but no description of each — finding out what they do costs one
lookup per helper. This adds a single command that returns every helper
with a one-line "what it's for", so a session can see the whole toolkit
in one cheap call.

## 1. Problem

A fresh CC session cannot see its full Ants-MCP toolkit in one cheap
move. Two existing surfaces each fall short:

1. The always-on SessionStart cheat-sheet
   (`src/mcporientation.cpp:56-67`) lists only **12** verbs and is at
   the INV-10 1200-byte cap (the ROADMAP entry reports ~24 B headroom
   at spec time) — it cannot grow to list more.
2. The session-start deferred-tools list gives all ~70 tool **names**,
   but their descriptions / `selection_hint`s load only per-tool via
   `ToolSearch` / `tool_info(name)`. So "what is my whole toolkit, and
   when do I reach for each?" costs ~70 individual probes.

There is no single call that returns every verb with its one-line
"when to use" cue.

## 2. Surface

Extend the existing `tool_info` meta-verb with an optional boolean
`catalog` arg. When `catalog:true`, the handler ignores `name` and
returns every registered verb grouped by category, each with its
`selection_hint` one-liner — built from the `m_lastToolsList`
snapshot that `tools/list` already populates (no new state).

### 2.1 Envelope

```json
{
  "ok": true,
  "catalog": {
    "<category>": [
      { "name": "roadmap_query", "selection_hint": "Use to look up …" }
    ]
  },
  "tool_count": 70,
  "category_count": 22
}
```

`tool_count` / `category_count` are the integer totals (the values
above are illustrative). The live counts follow whatever `tools/list`
registers and whatever distinct categories appear in the snapshot —
the ~22 labels `kindForName` (`src/claudeintegration.cpp:7245-7345`)
emits, counting `other` only when some descriptor actually resolves to
it. `category_count` therefore counts `other` exactly when it is a
present key, never as a fixed `+1`.

### 2.2 Category source

Each descriptor's `description` already carries a `[<kind>] ` prefix
injected by the `tools/list` prefix loop
(`src/claudeintegration.cpp:7372-7374`, ANTS-1518 / `kindForName`).
That loop runs **before** `m_lastToolsList = tools`
(`src/claudeintegration.cpp:7445`), so the snapshot descriptions carry
the bracket. The catalog parses that leading `[...]` token as the
category. A descriptor with no bracket prefix — or a malformed one (a
`[` with no closing `]`, or an empty `[]`) — falls back to category
`"other"`, keeping the parse total. This reuses the existing
category derivation rather than re-deriving categories from `name`.

### 2.3 Ordering

Categories are sorted ascending by key; verbs are sorted ascending by
`name` within each category — stable output across calls (no
`m_lastToolsList` insertion-order leakage). `selection_hint` is passed
through verbatim from the descriptor (every tool sets one — ANTS-1453
HINT-1); a descriptor missing the field (should not happen) emits an
empty string, not a fallback to `description`.

### 2.4 Handler dispatch order (load-bearing)

The catalog branch is evaluated **before** the existing
`reqName.isEmpty()` → `missing_name` guard
(`src/claudeintegration.cpp:7808`). Catalog mode supplies no `name`, so
were the `isEmpty()` guard reached first, every catalog call would
wrongly refuse with `missing_name`. Pseudocode for the new handler
head:

```cpp
if (argsObj.value("catalog").toBool()) {
    if (m_lastToolsList.isEmpty())  // → code:"tools_not_ready"
    else                            // → build grouped catalog
} else if (reqName.isEmpty())       // → code:"missing_name"  (unchanged)
else if (m_lastToolsList.isEmpty()) // → code:"tools_not_ready" (unchanged)
else                                // → single-tool slice (unchanged)
```

### 2.5 Schema change

`name` is removed from the `tool_info` descriptor's `required` array —
the imperative construction site at
`src/claudeintegration.cpp:6959-6961` (`QJsonArray req;
req.append("name"); schema["required"] = req;`) — since catalog mode
needs no name; the handler enforces the name-vs-catalog choice at
runtime. A boolean `catalog` property is declared on `props`
(alongside `name`) and `additionalProperties` stays `false`.

### 2.6 Prelude repoint

The orientation prelude's "Full catalog:" line
(`src/mcporientation.cpp:69-71`) is repointed at
`tool_info {catalog:true}`. The current block reads, verbatim —

```
Full catalog: load with ToolSearch query "mcp__ants" (or per-name
with "select:mcp__ants__<name>"). Tool descriptors carry
selection_hint lines describing when to reach for each.
```

— and is **179 B** (three lines including their `\n` terminators, as
they sit in the heredoc). The replacement copy must be **≤ 179 B** so
the rendered heredoc cannot exceed its current size (the binding
constraint, given only ~24 B headroom under the 1200-byte cap). The
reference replacement —

```
Full catalog (every verb + one-line "when to use"): call tool_info
with {"catalog":true}. ToolSearch query "mcp__ants" loads schemas.
```

— is **134 B** (same two-lines-with-terminators basis), *widening*
headroom by ~45 B rather than consuming it. The rendered-heredoc 1200-byte cap is enforced by
`mcp_orientation_install` INV-10 (the runtime byte-cap test,
`test_mcp_orientation_install.cpp:333`), not by this byte arithmetic.

### 2.7 Why extend `tool_info` rather than add a `catalog` verb

- A new top-level verb would itself grow the ~70-verb surface the
  feature exists to make legible, and would need its own
  `selection_hint` registration (perturbing the count-based
  `mcp_orientation_install` INV-6 coverage check).
- `tool_info` is already the descriptor-introspection meta-verb;
  catalog is the all-verbs sibling of its one-verb slice. One name
  means the prelude points at a single tool.
- The legacy empty-`name` → `missing_name` contract
  (ANTS-1399 INV-5) is preserved untouched: catalog mode is gated on an
  explicit `catalog:true`, never on empty `name`.

## 3. Invariants

Numbered **INV-9..INV-14** — these contracts land in the existing
`tests/features/mcp_tool_info_verb/` home, whose `spec.md` already owns
`INV-1..INV-8` for ANTS-1399. Per `specs.md` §5.5 the sequence is
continued, not restarted, so each id is unambiguous in that test home.
References below to **ANTS-1399 INV-N** mean the original tool_info
contracts, never these.

- **INV-9** — The `tool_info` descriptor in the `tools/list` block
  declares a boolean `catalog` property, keeps
  `additionalProperties:false`, and does **not** list `name` in
  `required`. *Test:* `mcp_tool_info_verb` source-scrape — assert
  `"catalog"` property literal in the descriptor block and `name`
  absent from that schema's `required`.
- **INV-10** — When `arguments.catalog == true`, the handler returns the
  grouped-catalog envelope (`ok:true`, `catalog` object, `tool_count`,
  `category_count`) built from `m_lastToolsList`, and does **not** take
  the `missing_name` / `unknown_tool` branches. *Test:*
  `mcp_tool_info_verb` source-scrape — assert the catalog branch plus
  the `tool_count` / `category_count` envelope-key literals in the
  handler region.
- **INV-11** — Catalog mode with an empty `m_lastToolsList` emits
  `code:"tools_not_ready"`. *Test:* `mcp_tool_info_verb` source-scrape
  — assert the `tools_not_ready` literal is reachable from the catalog
  branch.
- **INV-12** — Categories are derived from the descriptor's leading
  `[...]` description prefix; a missing or malformed prefix (no `[`, no
  closing `]`, or empty `[]`) falls back to `"other"`. Categories and
  the verbs within each are emitted in sorted order. *Test:*
  `mcp_tool_info_verb` source-scrape — assert the prefix-parse + sort
  calls in the catalog branch.
- **INV-13** — `catalog` absent/false leaves every legacy `tool_info`
  branch (`missing_name`, `unknown_tool` + `available[]`,
  `tools_not_ready`, single-tool slice) behaviourally unchanged. *Test:*
  ANTS-1399 INV-4/INV-5/INV-6 (existing) continue to pass; the
  `missing_name` literal remains present.
- **INV-14** — The orientation prelude template's "Full catalog:" text
  names `tool_info` + `catalog`, and the rendered heredoc stays ≤ 1200
  bytes. *Test:* `mcp_orientation_install` INV-10 enforces the ≤ 1200 B
  cap; that test's INV-12 separately checks byte-equality of the
  installed file against the rendered template (a verbatim-install
  check, not a byte-cap guard).

## 4. RAM / build cost

Zero new persistent state. The catalog object is built transiently from
`m_lastToolsList` (already resident, ~70 descriptors) per call and
discarded after serialisation. Wire size ~8–11 KB on-demand (≤280-char
hints × ~70 verbs), paid only when a caller asks — versus the ~70
separate `tool_info(name)` probes it replaces. No new build target, no
new external library.

## 5. Out of scope

- ETag / `fields=` projection / idempotent-read caching for catalog
  mode — `tool_info` is inline-dispatched process-global meta; the
  response only changes on rebuild. Add later if a caller polls it.
- Restructuring `tools/list` into a data table (ANTS-1049 / ANTS-1444
  lane). Category derivation stays the runtime `[...]`-prefix parse.
- Per-category prose summaries or token-cost rollups in the envelope —
  ship name + hint first; iterate if callers ask.

## 6. Tests

Feature test: `tests/features/mcp_tool_info_verb/` (extend the existing
suite; add INV-9..INV-14 to that directory's `spec.md`). Covers
INV-9..INV-14 via source-scrape. Label `features;fast`. Verify the new
assertions fail against pre-fix source first (no `catalog` property /
branch present), then pass once the handler + schema land.
`mcp_orientation_install` INV-10 (byte cap) is the live guard for
INV-14.

## 7. Cross-doc impact

- `CHANGELOG.md` — new entry under the next dated section.
- `ROADMAP.md` — ANTS-1985 flipped 📋 → ✅, migrated to CHANGELOG.
- No CLAUDE.md module-map change (no new file); no PLUGINS.md change
  (no Lua surface change).

## Cold-eyes loop log

Numbering note: loops 1–4 predate the Loop-5 renumber, so where they
say "INV-4" / "INV-6" they mean this doc's own pre-renumber ids — now
**INV-12** / **INV-14** respectively. `mcp_orientation_install INV-10` /
`INV-12` always mean that orientation test's own ids.

- **Loop 1 (2026-06-04).** Findings folded: handler must evaluate the
  catalog branch *before* the `reqName.isEmpty()` → `missing_name`
  guard (added § 2.4 + INV pseudocode); softened the unmeasured byte-cap
  claims and attributed the "~24 B headroom" figure to the ROADMAP
  source (§ 2.6, INV-6); corrected the illustrative envelope counts
  (`category_count` ~22 + `other`, not 21 — § 2.1); brought the spec
  into `specs.md` shape (numbered sections, `Surface` heading, per-INV
  `*Test:*` clauses, dated `spec draft` Status, this log). Dismissed:
  `**Lanes:**` header line flagged as non-standard — kept as established
  corpus convention, harmless to `spec_query`.
- **Loop 2 (2026-06-04).** Accuracy nits folded: corrected the
  `kindForName` line range to `7245-7345` (§ 2.1); named the exact
  `required`-array construction site `6959-6961` as the schema edit
  point (§ 2.5); time-stamped the "~24 B headroom" figure as a
  spec-time reading (§ 1). No CRITICAL/HIGH; all load-bearing claims
  (handler dispatch order, category derivation, snapshot timing, byte
  cap) verified sound against live source.
- **Loop 3 (2026-06-04).** Folded: pinned the prelude byte budget with
  measured figures (current block 179 B, reference replacement 134 B,
  headroom widens 45 B — § 2.6); restated this doc's prelude invariant
  so `mcp_orientation_install INV-12` is described as a verbatim-install
  check, not a byte-cap guard (only that test's INV-10 enforces the
  cap); made the category parse total by specifying the
  malformed-bracket (`[` unclosed / empty `[]`) fallback to `"other"`
  (§ 2.2, INV-4). All cited line numbers / symbols re-verified exact.
- **Loop 4 (2026-06-04).** Folded: reconciled the `category_count`
  example with the prose — `other` is counted only when present, never
  a fixed `+1` (§ 2.1); softened "single source of truth" since the
  catalog reads both the category prefix and `selection_hint` (§ 2.2);
  clarified the 134 B figure is a raw-string measurement with the
  rendered cap enforced by INV-10, not the byte arithmetic (§ 2.6). No
  CRITICAL/HIGH across loops 2–4; all measured figures (179 B / 134 B)
  and line citations confirmed exact by the reviewer.
- **Loop 5 (2026-06-04).** Folded one HIGH: the design-doc invariants
  collided in number with ANTS-1399's `INV-1..8` in the shared
  `mcp_tool_info_verb` test home — renumbered to **INV-9..INV-14** per
  `specs.md` §5.5 and disambiguated every prose `INV-N` reference as
  ANTS-1399 vs ANTS-1985 (§ 2.7, INV-13, § 6). Clarified the 179/134 B
  figures as the heredoc representation (lines incl. `\n`). No
  CRITICAL; all citations re-confirmed exact.
- **Loop 6 (2026-06-04).** Editorial only: disambiguated bare `INV-N`
  ids in this log (numbering note + qualified the loop-3 entry);
  quoted the current "Full catalog:" block verbatim in § 2.6 so it can
  be diffed against HEAD; rephrased INV-14's byte-cap-vs-byte-equality
  double negative. No CRITICAL/HIGH; the design contract has been
  stable and verified clean since loop 1.
