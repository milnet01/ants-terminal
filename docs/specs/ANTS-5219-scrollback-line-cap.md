# ANTS-5219 — Cap `get_scrollback`'s lines and say when its reply is cut

**Status:** accepted (2026-09-14), review-contract loop 1 (one lane, user preference), three findings fixed.
**Kind:** review-fix.
**Source:** ROADMAP.md ANTS-5219 (split from ANTS-5080; code-quality-review-2026-09-11 perf pass, lane mainwindow-b; user decision 2026-09-14).
**Pairs with:** ANTS-1500 (`since_cursor` mode), ANTS-1348 (`get_text`'s byte cap).

## 1. Problem

1. The `get_scrollback` provider in `MainWindow::setupClaudeMcpProviders`
   passes `args.value("lines").toInt(50)` straight to
   `TerminalWidget::recentOutput`, on the GUI thread. Nothing caps it.
   `recentOutput` stops only at the scrollback's size, which
   `Config::scrollbackLines` bounds by its configured maximum.
2. With `since_cursor`, the provider returns
   `recentOutput(added + screenRows)`, and `added` can reach the ring's
   capacity, `TerminalGrid::maxScrollback`.
3. Neither reply can say it was cut. The plain reply is raw text. The
   envelope carries `cursor`, `cursor_stale` and `stale_reason` only.

The tool description in `ClaudeIntegration` promises "the last N lines".

## 2. Surface

### 2.1 Decision and precedent

User decision (2026-09-14): a line cap, and an explicit truncation marker in
the reply. `RemoteControl::cmdGetText` already solves this shape for
`get_text`, so `get_scrollback` reuses it rather than inventing a second one.

### 2.2 Shared pieces — `src/remotecontrol.h`

```cpp
// ANTS-5219 — the most trailing lines get_text and get_scrollback return.
static constexpr int kGetTextMaxLines = 10000;

struct ScrollbackRequest {
    int lines = 0;         // lines to read
    int linesCapped = 0;   // lines the caller asked for and exist but were cut
};
// Clamps a requested line count to [1, kGetTextMaxLines]. A value <= 0 returns
// `fallback` unclamped. `available` is the lines the terminal holds.
static ScrollbackRequest capScrollbackRequest(int requested, int available,
                                              int fallback);
```

- `linesCapped` is `max(0, min(requested, available) - lines)`.
- `RemoteControl::cmdGetText` uses `kGetTextMaxLines` in place of its literal.
  Its replies do not change.

### 2.3 Plain reply

- `capScrollbackRequest(lines, available, 0)`, where `available` is the
  terminal's scrollback size plus its screen rows. A requested count <= 0
  therefore still reads no lines and returns an empty reply, as it does today.
- The text from `recentOutput(request.lines)` goes through
  `RemoteControl::trimScrollbackForGetText(raw, kGetTextDefaultBytesCap)`.
- When `request.linesCapped > 0`, the reply starts with the line
  `<capped at L of R requested lines>`, where L is `request.lines` and R the
  requested count. The byte trim's own marker, `<truncated N bytes / M lines>`,
  follows when that trim fired.
- A reply that was not cut carries no marker, so today's callers see the same
  text.

### 2.4 `since_cursor` envelope

- `content` goes through the same request cap and byte trim, with
  `added + screenRows` as the requested count.
- `content` never starts with the cap line: its R would be a count the caller
  never sent. The cap is reported only by `truncated` and `lines_dropped`.
  `content` carries the byte trim's own marker when that trim fired, as
  `get_text`'s `text` does.
- New field `truncated`, always present. When it is true, `lines_dropped` and
  `bytes_dropped` are present too, as in `get_text`'s reply. `lines_dropped`
  counts lines cut by the line cap and by the byte trim together;
  `bytes_dropped` counts the byte trim only.
- `cursor` still advances to the current position. A cut reply's lines are
  not replayed by the next call; `truncated` is what says so.
- The stale fallback (`cursor_stale:true`) uses the same pipeline.

## 3. Invariants

- **INV-1** — `capScrollbackRequest` clamps to `[1, kGetTextMaxLines]`, returns
  the fallback unclamped for a value <= 0, and reports
  `max(0, min(requested, available) - lines)` as `linesCapped`. Broken by a
  clamp that lets a request through, or a count that reports a cut on a
  terminal holding fewer lines than the cap. *Test:*
  `tests/features/mcp_get_scrollback_cap`, a table of requested, available
  and expected values.
- **INV-2** — the plain reply starts with the cap marker exactly when
  `linesCapped > 0`. Broken by an unconditional marker (which changes every
  reply) or a missing one. *Test:* `tests/features/mcp_get_scrollback_cap`,
  source scrape of the `get_scrollback` provider: the marker literal is
  present and guarded by `linesCapped`.
- **INV-3** — both replies pass through `trimScrollbackForGetText` with
  `kGetTextDefaultBytesCap`. Broken by a path that calls `recentOutput`
  and returns its text unguarded. *Test:*
  `tests/features/mcp_get_scrollback_cap`, source scrape: no
  `recentOutput(` result in the provider is returned without the trim.
- **INV-4** — the `since_cursor` envelope always carries `truncated`, and
  carries `lines_dropped` and `bytes_dropped` whenever it is true; its
  `content` never starts with the cap line. Broken by a field set only on one
  branch, or by the cap marker prepended to `content`. *Test:*
  `tests/features/mcp_get_scrollback_cap`, source scrape of the envelope's
  field writes.
- **INV-5** — `cmdGetText` reads its line cap from `kGetTextMaxLines`, with
  no literal cap of its own. Broken by two caps that can drift. *Test:*
  `tests/features/mcp_get_scrollback_cap`, source scrape of
  `RemoteControl::cmdGetText`.

## 4. RAM / build cost

No new build target: the test joins the `test_claude` bundle. No new state.
The cap bounds the string the GUI thread builds per call.

## 5. Out of scope

- Moving `recentOutput` off the GUI thread — excluded. The cap bounds the
  work instead, which is what the user decided.
- Capping `TerminalWidget::recentOutput` itself — excluded. Its other callers
  (the AI dialog's context, the status-bar model-switch scans, `cmdGetText`)
  must not change.
- Replaying lines a cut `since_cursor` reply dropped — excluded. The caller
  is told, and can read the scrollback export.

## 6. Tests

Feature test: `tests/features/mcp_get_scrollback_cap/`, in the `test_claude`
bundle. Covers INV-1, INV-2, INV-3, INV-4 and INV-5. Verify each fails against
pre-fix source first. `tests/features/mcp_extra_tools` scrapes a byte window
around this provider, so new code here can push `get_text` out of it; run that
suite after the change.

## 7. Cross-doc impact

- `get_scrollback`'s tool description in `ClaudeIntegration` — say the reply
  holds at most `kGetTextMaxLines` lines and says when it is cut.
- `docs/standards/mcp-behavioural-notes.md` — the `get_scrollback` entry.
- `CHANGELOG.md` — a `### Fixed` entry.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-14 | 1 (user preference: keep token use down; one reviewer, one pass) | 1 | 1 | 1 | 0 | Verified 3, fixed 3, dismissed 1. [Q2] lines <= 0 read 50 lines while the spec promised unchanged text; today it returns an empty reply, so get_scrollback passes a fallback of 0, unclamped. [Q3] section 2.4 left open whether since_cursor content carries the cap line; it does not, the cap is reported by truncated and lines_dropped. [Q1, from the lane's open question] section 5 told callers to raise lines, which the since_cursor path never reads; removed. Dismissed: where capScrollbackRequest is defined changes nothing built, and a link failure would surface at build. |
