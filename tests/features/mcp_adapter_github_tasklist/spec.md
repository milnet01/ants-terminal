# mcp_adapter_github_tasklist — feature contract

Tier 1 (read-side adapter) + Tier 2 (write-side `op:"flip"`) tests
for ANTS-1428. See `docs/specs/ANTS-1428.md` for the full design
+ rationale; this is the test-side mirror.

## What this test guards

### Tier 1 — `RoadmapDialog::parseBullets` adapter branch

- **INV-1 / Read-side detection.** No `<!-- ants-roadmap-format:
  1 -->` marker AND ≥ 1 `^- \[[ x]\]` line in first 100 non-
  empty lines → adapter engages. Marker present → native parser
  regardless of content.
- **INV-2 / Bold-ID preservation.** A GFM bullet that begins
  with a `**Bold-ID.**` token reports that token as
  `BulletRecord.id` with `synthetic == false`. Multi-prefix
  projects (`Sh4`, `Ed1`, `VEST-0042`) all work.
- **INV-3 / Synthetic-ID stability.** A GFM bullet with no
  bold-ID token reports a content-hash-derived ID
  (`synthetic == true`). The ID is stable across line
  reorders; FNV-1a 64-bit gives effectively zero collisions
  at document scale.
- **INV-4 / Status mapping.** `- [x]` → `status:"✅"`,
  `- [ ]` → `status:"📋"`, inline emoji prefix wins
  (including the contradictory case `- [x] 📋 ...` → `📋`).
- **INV-5 / Section completion inheritance.** A `## Heading
  (COMPLETE)` or `### Heading - done` section causes its
  enclosed planned-state bullets to inherit ✅ (unless an
  inline emoji overrides).

### Tier 2 — `RemoteControl::cmdRoadmapLog op:"flip"`

- **INV-6 / Anchor injection on first flip.** When the located
  bullet has neither a bold-ID nor an existing anchor, the
  write injects `^prefix-NNNN` on the last line of the
  bullet's headline content. The counter advances exactly
  once.
- **INV-7 / No re-injection on subsequent flip.** A second flip
  located by the just-injected anchor does NOT inject a second
  anchor and does NOT advance the counter.
- **INV-8 / Locator failure envelope.** Locator with zero
  matches refuses with `code:"bullet_not_found"`; > 1 match
  refuses with `code:"bullet_ambiguous"`. Both envelopes carry
  `suggestions[]` (≤ 3 nearest-neighbour bullets) and the file
  + counter are left unchanged.
- **INV-9 / Counter consume-on-write semantics.** A flip that
  located by a bold-ID or existing anchor does NOT advance the
  counter — only anchor-injecting writes do. INV-8 contract.
- **INV-10 / `prefix_hint` regex + default.** Valid hint is
  used verbatim; absent hint defaults to leaf-dir uppercase
  first 4 chars; invalid hint refuses with `bad_op_combo`.
- **INV-11 / Fenced-code refusal.** Located bullet inside a
  ``` ... ``` fenced block refuses with
  `code:"anchor_unsafe_context"`; no write, no counter bump.
- **INV-12 / `id_hint` under op:"flip" is bad_op_combo.**
- **INV-13 / Headline + (id|anchor) is bad_op_combo.** Headline
  locator is not permitted alongside a canonical-handle locator.

## Bundle

`test_claude` — joins the sibling `parseBullets` tests
(`roadmap_parser_blank_line_continuation`,
`mcp_roadmap_unrecognised_format`).
