# ANTS-2139 — `docs_index` MCP tool (project-agnostic documentation map, lazy + disk-cached) — review loop log

Record for [`../specs/ANTS-2139-docs-index-mcp-tool-project.md`](../specs/ANTS-2139-docs-index-mcp-tool-project.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-15) — `/cold-eyes`, 3 parallel fresh cold reviewers.**
  0 CRITICAL. Verified + fixed: (HIGH ×2, two reviewers independently) the
  `offer(kind, name, line)` citation in § 2.2 was wrong — the sink is
  `offer(kind, name, signature)` (`fileoutline.cpp:263-265`) with the line
  *text* as the third arg and the line *number* from a separate `totalLines`
  counter; corrected (the "no level slot, no links" conclusion held). (HIGH)
  INV-4's expected `links` output mixed raw `../x/y.md` with a `<dir>/sib.md`
  placeholder and was not derivable from the resolution rule → rewrote with a
  concrete doc-at-`docs/specs/d.md` and a fully-resolved sorted array. (HIGH)
  `kMaxDocBytes` was a dangling constant with no enforcing step or covering
  invariant (the exact defect ANTS-1637 loop 1 scrubbed for `kMaxQuerySymbols`)
  → added a § 2.2 enforcement step + **INV-19**. (HIGH) `kMaxHeadingsPerDoc` /
  `kMaxLinksPerDoc` silent caps had no test surface → folded into **INV-19**.
  (HIGH) `linked_from` was an unbounded reverse-edge list (no cap) → added
  `kMaxLinkedFrom` + `linked_from_truncated` + **INV-18**. (HIGH) the `dirs[]`
  rollup had no bucketing rule (so INV-16's test was unwriteable) → defined
  `dir` = immediate parent dir, root docs → `"."`. (HIGH) the INV→test-type
  mapping was ambiguous → § 6 now lists pure-driver vs source-scrape per INV.
  (MEDIUM) no assembled summary JSON (keys lived only in a § 2.5 table cell) →
  added one to § 2.7 mirroring ANTS-1637; defined `heading_count`. (MEDIUM)
  `topic=` "substring hit" was ambiguous → pinned binary per-token (weight at
  most once). (MEDIUM) softened the over-stated "mirrors `find_sources`'
  `FileHit` shape" (it carries `role`). (MEDIUM) `validatePath` call → added the
  explicit `/*allowOutsideRoot=*/false` and grounded the symlink-escape defence
  in `pathvalidation.cpp:90-92` (verified, not softened). (MEDIUM) `docs/plans/`
  example (deprecated dir) → `docs/journal/`. (MEDIUM) INV-2 now states
  staleness is mtime-only (no map-source signal, unlike `codebase_index`).
  (MEDIUM) § 4 RAM estimate noted as headings-only + a small string/link
  constant. (LOW) pinned that `found` rides the body (not meta); noted the
  `id=` entry omits the redundant `id`; added the `session_orient` eager-refresh
  follow-up to § 5. No new MCP refusal code minted. All three reviewers found
  the wiring-site citations (`claudeintegration.cpp:3222/8882/9051`,
  `mainwindow.cpp:4893-4894`, `mcpprojection.cpp:38`,
  `remotecontrol.cpp:8320-8357`) exact against current source.

- **Loop 2 (2026-06-15) — `/cold-eyes`, 3 parallel fresh cold reviewers.**
  0 CRITICAL. Every loop-1 fix held on the cold read — reviewers independently
  confirmed INV-4 is now derivable from the rule, `kMaxDocBytes` is no longer
  dangling (enforcing step + INV-19), the `offer(kind, name, signature)`
  citation is correct, and `linked_from` is capped — none recurred. Verified +
  fixed: (HIGH) `topic_truncated` was shown in **both** the § 2.5 `topic=` body
  and the § 2.7 meta block → pinned all `*_truncated` flags to the meta block
  (mirroring `codebase_index`'s `symbols_truncated`), dropped them from the body
  cells, added a caveat. (HIGH) `serve()`'s comment didn't state it forwards the
  refresh count + cache path into `query()` → added the line. (HIGH, one
  reviewer) the explicit `/*allowOutsideRoot=*/false` over-claimed the *flag* as
  the trust-boundary defence, while the live sibling `cmdCodebaseIndex` omits it
  (4-arg call, `remotecontrol.cpp:8340-8342`) → reworded § 2.7 to the 4-arg form
  and named the **canonical-form anchor** (`pathvalidation.cpp:71` + `:95-101`)
  as the actual defence. (MEDIUM) the `pathvalidation.cpp:90-92` cite pointed at
  the *comment*, not the check → repointed to `:71` (canonicalise) + `:95-101`
  (anchored-under reject) in § 2.7 and INV-7. (MEDIUM) INV-15 didn't pin the
  INV-12/INV-15 interaction → added "independent guards, whichever binds first".
  (MEDIUM) no invariant tied `heading_count` to `headings[].length` → folded
  into INV-16. (MEDIUM) `topic=` scoring over post-cap headings was implicit →
  stated. (LOW) added `(INV-1..19)` to the § 3 TOC entry for house-style parity.
  One cross-doc drift surfaced but **left out of scope** (sibling
  `ANTS-1637-codebase-index-mcp-tool-pre.md` § 2.3 step 1 still cites the stale `resolveCallerCwdRoot`; the
  live handler uses `expandGlobalConfigSentinel` → `resolveRootCanonical` —
  ANTS-2139 is the accurate one) — flagged for a separate ANTS-1637 doc-drift
  fix, not folded here.

- **Loop 3 (2026-06-15) — `/cold-eyes`, 3 parallel fresh cold reviewers.**
  0 CRITICAL; one reviewer returned 0 HIGH ("implementation-ready … clean
  enough to accept"). All loop-1/2 fixes held (the meta-block truncation flags,
  the canonical-anchor cite, INV-4's derivable example, the `serve` plumbing all
  read clean — none recurred). Verified + fixed: (HIGH, two reviewers) § 2.2
  said "≤ `kMaxLineBytes` bytes/line **read**, mirroring `FileOutline`" — but
  `FileOutline` reads the full line then **skips** (`continue`) any line over
  the cap (`fileoutline.cpp:256`), a guard not a partial read → reworded to
  "skip over-long lines, not truncate-and-match" and added the long-line case to
  INV-3. (HIGH) `kMaxLineBytes` was the one remaining dangling constant (absent
  from `Options`, no covering invariant) → dispositioned as a fixed internal
  regex-skip guard (not Options-overridable) covered by INV-3's new long-line
  test. (HIGH) `links[]` could contain a target with no `DocEntry` (a link to a
  not-walked `packaging/x.md`) but the spec never said so → § 2.2 + INV-4 now
  state a kept target need not be indexed, and `linked_from` connects only
  both-indexed pairs. (MEDIUM) the assembled summary example dropped
  `linked_from_truncated` while the meta example listed it → added it for
  envelope consistency; (MEDIUM) the summary body cell re-listed `docs_truncated`
  (loop-2 residue) → scrubbed to ride the meta block like the other rows.
  (MEDIUM) all summary counts/`dirs[]` pinned to the post-truncation in-memory
  `docs[]` (INV-16 + § 2.5) so they hold under `docs_truncated`. (MEDIUM) INV-10
  now asserts `generated_at_ms` is byte-identical across the two warm calls (the
  load-bearing fact behind etag stability). (MEDIUM) INV-16's test pins
  `heading_count == maxHeadingsPerDoc` at the cap. (MEDIUM) the `id=` entry-shape
  divergence (omits `id`) called out once in the § 2.5 caveats so it is not
  "helpfully" re-added. (LOW) tightened the `pathvalidation.cpp:95-101` cite to
  `:71` + `:96-100` (95 is the existence guard; the `anchoredUnder` reject is
  96-100); (LOW) added the ROADMAP lane-widening note to § 7 for landing parity
  with ANTS-1637. No new MCP refusal code minted.

- **Loop 4 (2026-06-15) — `/cold-eyes`, 3 parallel fresh cold reviewers
  (confirming pass).** 0 CRITICAL, **0 HIGH** — one reviewer returned zero
  actionable findings ("17/17 code citations exact … implementation-ready on the
  accuracy lane"); all loop-1–3 fixes held on the cold read (the `kMaxLineBytes`
  skip-not-read correction, the canonical-anchor `:71`+`:96-100` cite, INV-4's
  derivable example, the meta-block truncation flags — none recurred). Folded the
  remaining minor-polish residue: (MEDIUM) INV-15 didn't say *how* the byte
  ceiling is checked → pinned it to a running serialised-size estimate (not an
  O(n²) re-serialise), making the cut point reproducible. (MEDIUM) § 4 gave only
  the aggregate RAM figure → added the per-doc serialised-entry bound
  (`kMaxHeadingsPerDoc`×~80 B + `kMaxLinksPerDoc`×path ≈ <120 KB) so no single
  doc can alone breach `kMaxCacheBytes`. (MEDIUM) `kMaxTopicHits` enforcement
  lived only in a table cell → added the truncate-and-set-`topic_truncated`
  clause to the § 2.5 `topic=` caveat. (LOW) dropped the process parenthetical
  ("split late …") from INV-18's body. Citation precision re-confirmed by all
  three reviewers (`fileoutline.cpp:84/256/263-265/323`,
  `pathvalidation.cpp:71`+`:96-100`, the six wiring sites). Design unchanged
  since loop 1 — converged → **accepted**.
