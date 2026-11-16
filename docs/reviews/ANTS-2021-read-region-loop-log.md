# ANTS-2021 — review loop log

The review rows for [`docs/specs/ANTS-2021-read-region-mcp-tool-read.md`](../specs/ANTS-2021-read-region-mcp-tool-read.md).

## Cold-eyes loop log

- **Loop 1 (2026-06-05) — `/cold-eyes`, fresh cold reviewer.** Verified +
  fixed: (CRITICAL) § 2.3 symbol-body range relied on "same-or-shallower
  nesting", but `FileOutline::appendSymbol` (`fileoutline.cpp:115-126`)
  emits only `{line, kind, name, signature}` — no nesting level exists →
  rewrote § 2.3 + INV-2 to a flat next-`symbols[]`-entry range and stated
  the leaf-vs-aggregate consequence plainly. (HIGH) the `etag` was
  described as a handler-computed `(mtime,size)` hash, but it is
  dispatcher-injected via `applyEtagPattern`/`etagFor`
  (`claudeintegration.cpp:8384/8391`, content-hash of the payload;
  `remotecontrol.cpp:6354-6357` confirms the handler must not emit it) →
  corrected § 2.6 + INV-6 + the envelope comment, and reframed the
  "free re-read" win as an **output**-token saving (the file is still
  read; the body is not re-emitted). (MEDIUM) added `symbol_match_count`
  to the § 2.4 envelope (was promised by § 2.3 prose only). (MEDIUM) noted
  the byte-cap drops the **tail** (head-anchored region), the opposite of
  read_log's drop-oldest deque, so a paired implementer doesn't cross the
  wires. (LOW) harmonised the `validatePath` call form with ANTS-2022.
  Open questions (since_cursor dropped; `symbol_not_found` vs soft-miss)
  left in § 7 for the next pass.

- **Loop 2 (2026-06-05) — `/cold-eyes`, fresh cold reviewer.** 0 CRITICAL
  / 0 HIGH; loop-1 fixes all held (none recurred). Verified + fixed:
  (MEDIUM) `total_lines` was in the envelope but unobtainable without a
  full-file scan, contradicting the bounded "stop at `end_line`" read
  (INV-9) and underspecified in symbol mode → dropped `total_lines`
  entirely; a caller needing file length uses `file_outline`. (MEDIUM)
  `end_line` echo under simultaneous EOF-clamp + byte-cap was ambiguous →
  pinned `end_line` = last emitted line and `truncated` as the sole cap
  signal (vs a plain EOF clamp). (impl open-q) pinned the `maxSymbols`
  passed to `FileOutline::compute` (1 M) so symbol mode is never silently
  truncated, with the 1024-byte line-cap caveat noted. INFO surfaced, not
  actioned: `mcp-tools.md:107` ("emit an `etag` field") is stale vs the
  dispatcher-injects-etag code — a standard-doc fix tracked separately, the
  spec follows the code.

- **Loop 3 (2026-06-05) — `/cold-eyes`, fresh cold reviewer.** Caught a
  **CRITICAL regression introduced by the loop-2 fix**: § 2.3 had been
  changed to pass `maxSymbols=1'000'000` "so resolution is never
  truncated", but `FileOutline::compute` **hard-clamps** `maxSymbols` to
  `kMaxSymbolsCap=1000` (`fileoutline.cpp:16,152`) → corrected § 2.3 +
  INV-2 to pass the constant and document the 1000-symbol resolution limit
  honestly (caller falls back to line mode on a ≥1000-symbol file). (MEDIUM)
  unified the 304 envelope to `{ok:true, unchanged:true, etag}` across § 2.6
  + INV-6 (`applyEtagPattern` always sets all three). (MEDIUM) the stale
  `mcp-tools.md` step-7 "emit an `etag` field" line now has a tracked ID —
  **ANTS-2027** — cited in § 8. All loop-1/2 fixes otherwise held.

- **Loop 4 (2026-06-05) — `/cold-eyes`, fresh cold reviewer.** 0 CRITICAL
  / 0 HIGH; the loop-3 `kMaxSymbolsCap` correction held. Verified + fixed:
  (MEDIUM) the byte-cap algorithm was ambiguous — reuse `capJsonArrayToBytes`
  (build-then-trim, which would materialise the whole slice and break the
  heap bound) vs an incremental loop → pinned § 2.5 + INV-9 + § 4 to an
  **incremental stop-early** read (stop at the earlier of `end_line` or the
  `max_bytes` boundary; never `capJsonArrayToBytes`), making INV-9's bounded
  heap claim sound for a large requested range. (MEDIUM) cited the 1024-byte
  cap by its constant (`kMaxLineBytes`, `fileoutline.cpp:15`) and softened
  the "symbol not captured" claim to "declaration line truncated before the
  scanner matches it". INFO (envelope `present iff` example styling) left
  as-is.

- **Loop 5 (2026-06-05) — `/cold-eyes`, fresh cold reviewer (confirming
  pass).** 0 CRITICAL / 0 HIGH; the loop-4 incremental-byte-cap and
  `kMaxLineBytes` citation fixes held. Only residue, all fixed: (citation
  parity) §1 item 2 cited `find_definition`'s dispatch by bare filename
  while its siblings pin lines → pinned `RemoteControl::cmdFindDefinition`,
  `remotecontrol.cpp:9738`; (idiom) the §2 dispatch snippet's
  `CallerCwdContract::Required` → qualified `ClaudeIntegration::` to match
  the in-tree call form. Open question (code-side, not a spec defect):
  confirm `file_outline` still emits `total_lines` so the §2.4 "use
  `file_outline` for length" escape hatch holds — verified true (the
  `file_outline` tool envelope retains `total_lines`). Spec converged →
  **accepted**.
- **Amend (2026-11-16) — an authoring amendment, no reviewer dispatched** — Not a review loop. Amended for ANTS-6502 (origin): the code broke INV-9's heap bound in three places, a whole line read before the cap and whole-file loads in the brace and heading scans. INV-9 now says each line is held only up to `max_bytes` + 1 bytes and the scans hold at most `kScanLineBytes` of one line, plus the headings in section mode. Its test clause named a source-scrape that never existed; it now names `McpReadRegion.Ants6502PeakMemoryStaysBounded`. § 4 says when the brace scan gives up. The inline loop log moved to this file. No invariant added.
