# Feature spec: ANTS-5219 — `get_scrollback` caps its lines and says when it cut

The contract is `docs/specs/ANTS-5219-scrollback-line-cap.md`. This test locks
its invariants; the numbering below is that spec's.

## Invariants under test

- **INV-1** — `RemoteControl::capScrollbackRequest` clamps a request to
  `[1, kGetTextMaxLines]`, returns the fallback unclamped for a value <= 0,
  and reports `max(0, min(requested, available) - lines)` as `linesCapped`.
  A table of requested, available and fallback values.
- **INV-2** — the plain reply's `<capped at L of R requested lines>` marker is
  guarded by `linesCapped > 0`. Source scrape of the `get_scrollback` provider.
- **INV-3** — every `recentOutput` result in the provider passes through
  `trimScrollbackForGetText` with `kGetTextDefaultBytesCap`. Source scrape.
- **INV-4** — the `since_cursor` envelope sets `truncated`, `lines_dropped` and
  `bytes_dropped`, and the cap marker appears only in the plain reply. Source
  scrape.
- **INV-5** — `RemoteControl::cmdGetText` reads its line cap from
  `kGetTextMaxLines`, with no literal of its own. Source scrape.

## Red proof

Against the unfixed tree, INV-1 fails on a test-first stub of
`capScrollbackRequest` that applies no cap, and INV-2 to INV-5 fail on the
scrapes.
