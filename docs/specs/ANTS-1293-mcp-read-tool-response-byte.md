# ANTS-1293 — MCP read-tool response byte cap

**Status:** shipped 2026-05-22 (completes the partial work: `get_text` byte
cap shipped under ANTS-1348; `roadmap_query` offset/limit pagination shipped
under ANTS-1436).
**Kind:** refactor.
**Source:** indie-review-2026-05-13 (token + perf bundle).
**Siblings:** ANTS-1348 (`get_text` byte cap), ANTS-1249 (`file_outline`),
ANTS-1248 (`workspace_search`).

## 1. Problem

`file_outline` on a file full of long signatures, or `workspace_search`
with wide context windows, can produce a multi-K-token response. The count
caps (`max_symbols`, `max_results`) bound the *number* of items but not the
*total size* — a few pathologically long items can still blow the
transport budget. `get_text` already has a byte cap (ANTS-1348) and
`roadmap_query` already paginates; this closes the gap for the two
remaining large structured read tools.

## 2. Surface

- New optional `max_bytes` integer arg on `file_outline` and
  `workspace_search`. The MCP schema declares `minimum:1`, so MCP callers
  reach the handler's `<= 0` branch only via the absent-arg default
  (raw-IPC socket callers can still send `<= 0`, which the helper folds to
  the default); absent → 512 KiB default; `>` 4 MiB ceiling → clamp +
  `bytes_cap_clamped:true`.
- When the serialized response exceeds the cap, trailing array items are
  dropped (tail-trim, so the leading items always survive) and the
  envelope carries `truncated:true` plus a dropped-count field:
  `symbols_dropped` (file_outline) / `results_dropped` (workspace_search).
  `bytes_cap_clamped` is orthogonal to `truncated`: a clamp (requested cap
  above the ceiling) can fire with zero drops, and a trim can fire with no
  clamp.
- Shared kernel: `RemoteControl::capJsonArrayToBytes(env, arrayField,
  droppedField, maxBytes)` — inline in `remotecontrol.h` (testable without
  the MainWindow dep chain, mirroring `trimScrollbackForGetText`).

## 3. Invariants

- **INV-1** — A response already within the cap is returned unchanged by
  the byte cap (no `truncated` flip, no dropped-count field). Note
  `workspace_search` may already carry `truncated:true` from its count cap
  (`max_results`, ANTS-1248) with no `results_dropped`; the byte cap neither
  sets nor clears that pre-existing flag on its fast path.
- **INV-2** — When over the cap, items are dropped from the TAIL; the
  leading items (first symbols / first matches) always survive.
- **INV-3** — On trim, `truncated:true` and `<droppedField>:<n>` are set
  with `n == originalCount - keptCount`.
- **INV-4** — The final serialized envelope (including the metadata fields)
  is `<= max_bytes` whenever `max_bytes` is at least the base-envelope
  floor (headroom for the metadata fields is reserved before trimming).
  When `max_bytes` is below that floor the envelope is minimised to zero
  items but may still exceed the cap — the base scalar fields cannot be
  shrunk (the degenerate case in INV-6).
- **INV-5** — `max_bytes <= 0` uses the 512 KiB default; `max_bytes` above
  the 4 MiB ceiling clamps and sets `bytes_cap_clamped:true`.
- **INV-6** — A cap below the base envelope size keeps zero items without
  crashing. When the array had ≥ 1 item, all are dropped and `truncated` +
  the dropped-count are set (INV-3); when the array was already empty there
  is nothing to drop, so no metadata is added and the (irreducible) base
  envelope is returned as-is — possibly still over the cap.

## 4. Tests

`tests/features/rc_read_tool_byte_cap/test_rc_read_tool_byte_cap.cpp` —
BC-1..BC-5 exercise `capJsonArrayToBytes` directly; WI-1/WI-2 grep the two
handler bodies for the wiring (`capJsonArrayToBytes` + the dropped-count
field name).

## 5. Notes

`max_bytes` composes with the count caps (`max_symbols` / `max_results`):
count cap applies first inside the scanner, byte cap second on the
assembled envelope. It also composes with the ANTS-1720 `fields=`
projection (cap runs in the handler; projection narrows at the dispatch
layer afterwards). The simpler "drop + flag" model (as in `get_text`) is
used rather than `roadmap_query`'s offset/continuation pagination — these
tools have no stable cross-call cursor and the leading items are the
high-value ones.
