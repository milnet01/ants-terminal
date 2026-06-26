# Feature spec: ANTS-2192 — query-response amplification cap

## Problem

Every terminal *query* whose reply Ants writes back to the PTY — DA
(`CSI c`), CPR (`CSI 6n`), DSR (`CSI 5n`), the colour-scheme query
(`CSI ? 996 n`), DECRQSS (`DCS $q`), OSC 10/11/12 colour queries, and the
Kitty keyboard/graphics queries — fired an unconditional
`m_responseCallback` with no rate limit. A hostile or runaway program
streaming a query in a tight loop (the classic `\e[6n` flood) forces one
unbounded PTY write per request: a self-inflicted amplification load.

Response *content* is fixed/derived and never attacker-controlled (that
injection class is already closed); this is purely about *volume*. The
clipboard path (OSC 52) already has a 60 s write quota and OSC 133 a
cool-down; the query-response path had neither.

## Surface

`TerminalGrid::sendQueryResponse(const std::string&)` is the single
chokepoint every query reply now funnels through. It applies a
per-terminal rolling 1-second window (mirroring the OSC 52 quota):
counters reset when the window advances, and once
`QUERY_RESP_MAX_PER_SEC` (256) replies have been written in the current
window the rest are dropped silently.

## Invariants

- **INV-1** — A single query gets its reply. Feeding one `CSI 6n`
  yields exactly one CPR response (the legitimate synchronous
  query/wait path is never throttled).
- **INV-2** — A flood is capped. Feeding thousands of `CSI 6n` queries
  back-to-back (all within one wall-clock second) yields far fewer
  responses than queries — bounded near `QUERY_RESP_MAX_PER_SEC`, not
  one-per-query. This is the red→green discriminator: pre-fix every
  query echoed a response.
- **INV-3** — Content is unchanged. A throttled-but-allowed CPR reply is
  still the correct `CSI row;col R` string (the cap drops whole
  responses; it never mangles a delivered one).
