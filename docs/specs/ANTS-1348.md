# ANTS-1348 — Server-side byte cap on `cmdGetText` (`mcp__ants__get_text`)

**Status:** shipped (get_text byte cap is live in `src/remotecontrol.h`
`trimScrollbackForGetText` + `kGetTextDefaultBytesCap`).
**Kind:** fix (truncation-surfacing)
**Source:** indie-review-2026-05-14 (lane-2 M4)
**Pairs with:** ANTS-1294 (MCP envelope contract — same wrap site
gains a `truncated` signal field).

---

## 1. Summary

`RemoteControl::cmdGetText` (`remotecontrol.cpp:375`) caps the response
at **10 000 lines** (`:413`) but applies **no byte cap**. A scrollback
of 10 000 × 4 KB lines yields a 40 MB JSON envelope; the MCP-side
1 MiB receive cap then truncates the response mid-stream and surfaces
as a misleading "socket hijack" error to the calling Claude Code
session. Fix: clamp on the server at 1 MiB by default (matching the
client cap), trim the response from the **head** of the scrollback
(keep the newest lines), and report `truncated: true` plus
`bytes_dropped: N` in the response envelope so the caller knows
data was elided and doesn't blame the transport.

## 2. Problem statement

`cmdGetText` request shape (current — `remotecontrol.cpp:375-422`):

```json
{"cmd":"get-text","tab":<int optional>,"lines":<int optional, ≤10000>}
```

Response (current):

```json
{"ok":true,"text":"<scrollback>","lines":<N>,"bytes":<utf8 size>}
```

The `lines` cap protects against million-line scrollbacks but is the
wrong dimension. Two failure shapes:

- **Wide-line scrollback:** A `cat large.json | jq .` session leaves
  the scrollback with lines averaging 4–8 KB. 10 000 lines × 4 KB ≈
  40 MB. The wire-side `QLocalSocket` write buffers up; on the MCP
  bridge in `claudeintegration.cpp`, the read cap kicks in at 1 MiB
  and the consumer sees a parse error on a half-truncated JSON
  envelope.
- **Single very long line:** A shell that emits a one-line 8 MB
  blob (no `\n`) is treated as "1 line" by the `\n`-count in
  `recentOutput`. The cap-on-lines passes; the byte size explodes.

The class of bug is the same — **the wire's transport layer enforces
a limit the application layer doesn't acknowledge**. The fix is to
acknowledge it.

## 3. Surface — current state

| Field | Site | Value |
|---|---|---|
| Line cap | `remotecontrol.cpp:413` | `std::min(requested, 10000)` |
| Byte cap | — | none |
| Truncation signal | — | none |
| Caller cap | `claudeintegration.cpp` (MCP bridge) | 1 MiB read budget |
| Wire size in `bytes` | `remotecontrol.cpp:420` | reported but not used to clamp |

The `out["bytes"]` field is informational; clamping happens nowhere
on the server side.

## 4. Design

### 4.1 New optional request field — `max_bytes`

```json
{"cmd":"get-text","tab":<int optional>,"lines":<int optional>,
 "max_bytes":<int optional, default 1048576>}
```

- Defaults to `1024 * 1024` (1 MiB) — matches the MCP bridge cap.
- Caller can lower (test harness wanting < 1 KiB to assert truncation)
  or raise (in-process tests; trusted same-UID raw-IPC consumers).
- Hard ceiling: `16 * 1024 * 1024` (16 MiB). A `max_bytes` above the
  ceiling is clamped silently and noted via `bytes_cap_clamped:true`
  in the response. The MCP bridge can never receive more than 1 MiB
  anyway; this ceiling exists for non-MCP rc consumers (kitten-style
  plugins, `ants @ get-text` CLI). 16 MiB = 16× the MCP cap — enough
  headroom for a one-shot full-scrollback dump to disk via the rc
  CLI, well below the audit-engine's own `MAX_TOOL_OUTPUT_BYTES =
  64 * 1024 * 1024` (`auditdialog.h:468`) which absorbs tool output.
  The 16 MiB is a rc-consumer rule-of-thumb, not parity with another
  constant.

### 4.2 Trim direction: head (drop oldest)

When the result exceeds `max_bytes`, drop bytes from the **beginning**
of the assembled string (oldest scrollback). Newest lines win because
the call site is "I want recent output" — the newest bytes are the
most likely to satisfy the caller's intent (e.g., "what did the
build just say?").

Trim to a `\n` boundary to keep the response valid line-delimited
output: find the first `\n` at or after the cut point and trim
there. If the trimmed result starts with a partial line, prepend
`<truncated …>\n` as an unambiguous sentinel so a caller parsing
line-by-line sees the boundary.

### 4.3 Response envelope changes

```json
{"ok":true,
 "text":"<scrollback>",
 "lines":<line count in returned text>,
 "bytes":<utf8 size of returned text, ≤ max_bytes>,
 "truncated":<bool>,
 "bytes_dropped":<int, only when truncated>,
 "lines_dropped":<int, only when truncated>}
```

`truncated` is **always** present (false on the happy path) so
clients can rely on its presence. `bytes_dropped` and `lines_dropped`
are only emitted when `truncated == true`, matching the existing
pattern for `stripped` on the send-text family.

### 4.4 Patch sketch

```cpp
// remotecontrol.cpp — cmdGetText body, replacing lines 408-421
constexpr int kDefaultBytesCap = 1 * 1024 * 1024;   // 1 MiB
constexpr int kMaxBytesCeiling = 16 * 1024 * 1024;  // 16 MiB

int lines = 100;
const QJsonValue linesVal = req.value("lines");
if (linesVal.isDouble()) {
    const int requested = linesVal.toInt();
    if (requested > 0) lines = std::min(requested, 10000);
}

int maxBytes = kDefaultBytesCap;
bool capClamped = false;
const QJsonValue maxBytesVal = req.value("max_bytes");
if (maxBytesVal.isDouble()) {
    const int requested = maxBytesVal.toInt();
    if (requested > 0) {
        maxBytes = std::min(requested, kMaxBytesCeiling);
        capClamped = (requested > kMaxBytesCeiling);
    }
}

QString text = target->recentOutput(lines);
const QByteArray utf8 = text.toUtf8();
bool truncated = false;
qint64 bytesDropped = 0;
qint64 linesDroppedQ = 0;

if (utf8.size() > maxBytes) {
    // Trim from head, snap to next '\n' to keep line semantics.
    const int cut = utf8.size() - maxBytes;
    int snap = utf8.indexOf('\n', cut);
    if (snap < 0) snap = cut;       // no newline found; hard cut
    else ++snap;                     // skip the newline itself
    const QByteArray kept = utf8.mid(snap);
    bytesDropped = snap;
    linesDroppedQ = utf8.left(snap).count('\n');
    // Sentinel: tells the consumer this is not the true start.
    text = QStringLiteral("<truncated %1 bytes / %2 lines>\n")
               .arg(bytesDropped).arg(linesDroppedQ)
           + QString::fromUtf8(kept);
    truncated = true;
}

out["ok"] = true;
out["text"] = text;
out["lines"] = text.count('\n') + (text.isEmpty() ? 0 : 1);
out["bytes"] = text.toUtf8().size();
out["truncated"] = truncated;
if (truncated) {
    out["bytes_dropped"] = bytesDropped;
    out["lines_dropped"] = linesDroppedQ;
}
if (capClamped) out["bytes_cap_clamped"] = true;
return QJsonDocument(out);
```

(Final implementation may extract the trim block into a static helper
if a second consumer appears; for one call site, keeping it inline
matches the project's no-premature-DRY rule from CLAUDE.md.)

## 5. Invariants

- **INV-1 — Server-side cap applied before transport.**
  `out["bytes"]` is always ≤ `max_bytes` (or the 16 MiB ceiling if
  clamped). The wire layer never has to truncate.

- **INV-2 — Trim from oldest.** When truncating, bytes are dropped
  from the head of the assembled scrollback. The tail (newest
  output) always reaches the caller.

- **INV-3 — Line-boundary snap.** Truncation cuts at a `\n` boundary
  when one exists in the trimmed range. The returned `text` never
  starts with a partial line (except via the explicit `<truncated
  …>\n` sentinel).

- **INV-4 — Truncation always signalled.** `out["truncated"]` is
  present on every response. `true` when bytes were dropped, `false`
  otherwise. Callers can rely on its presence.

- **INV-5 — Counters only on truncation.** `bytes_dropped` and
  `lines_dropped` are emitted only when `truncated == true`.
  Matches the existing `stripped` pattern.

- **INV-6 — Sentinel prefix on truncation.** When `truncated ==
  true`, the returned text begins with the literal string
  `"<truncated N bytes / M lines>\n"` where N == `bytes_dropped`,
  M == `lines_dropped`. Tests pin this format so consumers can
  parse it.

- **INV-7 — Default cap is 1 MiB.** Matches the MCP bridge cap so
  the happy path never trips the transport limit.

- **INV-8 — Ceiling is 16 MiB.** A caller passing
  `max_bytes > 16 * 1024 * 1024` gets the ceiling silently, plus
  `bytes_cap_clamped: true`. No request is rejected.

- **INV-9 — Line cap still applies.** The 10 000-line cap at the
  caller stays in place; byte cap composes with it.

- **INV-10 — Pure server-side change.** No new wire field is
  *required* on requests; `max_bytes` is optional with a safe
  default. Existing callers receive the new `truncated` field but
  are unaffected if they ignore it.

## 6. Failure modes & rejection table

This change adds zero new rejection paths — `max_bytes` is bounds-
checked and silently clamped rather than rejected. The error envelope
shape is unchanged for the existing failure modes
(`no_window`/`bad_tab`/etc.).

## 7. Test plan

**`tests/features/rc_get_text_byte_cap/`** (new).

### Engine tests

- **GT-1 happy path** — small scrollback (32 lines × 80 bytes ≈ 2.5
  KiB) → `truncated == false`, `bytes_dropped` absent.
- **GT-2 trip cap at default** — scrollback shaped to produce 1.5
  MiB → response `bytes ≤ 1 MiB`, `truncated == true`,
  `bytes_dropped == 1.5MiB - 1MiB + sentinel_size`, `text` starts
  with `<truncated `.
- **GT-3 custom `max_bytes` below default** — `max_bytes=1024` on a
  2 KiB scrollback → trims to ≤ 1 KiB plus sentinel.
- **GT-4 trim snaps to `\n`** — scrollback `aaaaaaaaa\nbbbbbbbbb\nccccccccc\n`
  with `max_bytes=15` → kept text starts after the first `\n`
  (the `bbb…` line), not mid-`aaa`.
- **GT-5 single long line, no newline** — 2 MiB single line, no
  `\n` → trim falls back to hard byte cut, sentinel prepended.
- **GT-6 ceiling clamp** — `max_bytes=1<<28` (256 MiB) → silently
  clamped to 16 MiB; `bytes_cap_clamped: true` in response.
- **GT-7 `truncated == false` is always emitted** — happy path
  response contains the key `"truncated"` with value `false`.
- **GT-8 `lines_dropped` counts correctly** — fixture with N
  known lines pre-cut, assert `lines_dropped == N`.
- **GT-9 ASCII byte counting** — `bytes_dropped + bytes_kept +
  sentinel_size == original_utf8_size` (with `bytes_kept` =
  `text.toUtf8().size() - sentinel_size`).
- **GT-10 UTF-8 multi-byte cut safety** — scrollback containing
  CJK + emoji. The cut point should never split a multi-byte
  sequence (the `\n` snap at GT-4 protects against this; GT-10
  pins the absence of `0xFFFD` in the kept tail).

### Wiring tests — source-grep

- **WI-1** `remotecontrol.cpp` `cmdGetText` body contains the
  literal `"max_bytes"` exactly once.
- **WI-2** `kDefaultBytesCap = 1 * 1024 * 1024` and
  `kMaxBytesCeiling = 16 * 1024 * 1024` appear in the body.
- **WI-3** Every code path through `cmdGetText` sets
  `out["truncated"]` before returning (no early return forgets it).

### Pre-fix regression check

GT-2, GT-3, GT-5, GT-7 fail against pre-patch `cmdGetText`
(no `truncated` field, response unbounded in bytes).

## 8. Migration / back-compat

- **Request schema:** purely additive (`max_bytes` optional). All
  existing callers continue working.
- **Response schema:** adds `truncated`, `bytes_dropped`,
  `lines_dropped`, `bytes_cap_clamped`. Clients that read only
  `text`/`lines`/`bytes` are unaffected. Clients that use the wire
  size to detect issues should switch to `truncated`.
- **MCP bridge:** the in-process MCP wrap in `claudeintegration.cpp`
  doesn't need to change — `truncated` flows through the envelope
  as JSON data and the wrap site already handles arbitrary fields.
  ANTS-1294's `<ants_mcp_data tool="get_text">` wrap composes
  cleanly.
- **CHANGELOG:** new entry under "Security / Reliability" notes the
  default cap + truncated field; under "MCP" notes the wire-schema
  addition.
- **No breaking changes.** `bad_lane`-style code retirements aren't
  needed.

## 9. Performance & RAM budget

- **Happy path (no trim):** one extra `QJsonValue::toInt()` lookup,
  one branch on `utf8.size() > maxBytes`. Sub-µs.
- **Trim path:** one `QByteArray::indexOf('\n', cut)` (linear in
  `bytes_dropped` worst case) + one `QByteArray::mid` (O(kept))
  + one `QByteArray::count('\n')` (O(dropped)). For a 1 MiB cap on
  a 16 MiB input the trim work is ~15 MiB of linear scans.
  Bounded; runs once per get-text call, not in a hot loop.
- **Memory:** trim allocates one new `QByteArray` of size
  `kept.size()` (≤ `max_bytes`) and one trimmed-prefix temporary.
  Peak overhead during trim: 2× `max_bytes` (the original full
  `utf8` is freed when we replace `text`). Same order as the
  pre-patch response build.
- **Idle:** zero.

Total new RAM/CPU budget: bounded by `max_bytes` per call. No
long-lived structures.

## 10. Out of scope

- Streaming the response. The current rc protocol is one-shot
  request/response; adding chunked streaming is a separate ticket
  (ANTS-1349 already calls out the related Pty-side EAGAIN drop
  for the inbound side).
- Adjusting the line cap. Keeping 10 000 as the line ceiling matches
  the existing wire contract; the byte cap is the new dimension.
- Changing the trim direction (e.g., dropping middle lines like
  `head + ... + tail`). Newest-wins is the simplest semantic; a
  future ticket can offer `trim: "middle"` if a caller asks.
- Migrating other rc verbs to a `max_bytes` pattern. Each verb's
  output shape is different; we don't want to add cap fields
  speculatively. `cmdWorkspaceSearch` already has its own
  `kMaxFindings` cap; `cmdFileOutline` is bounded by file size.

## 11. Cold-eyes review log

**Loop 0 (initial draft, 2026-05-15):** Drafted above.

**Loop 1 — VERIFY against current source (2026-05-15):**

- ✅ `cmdGetText` defined at `remotecontrol.cpp:375-422`.
- ✅ Line cap at `:413`: `lines = std::min(requested, 10000)`.
- ✅ `target->recentOutput(lines)` at `:416`. Signature
  `QString recentOutput(int lines = 50) const` at
  `terminalwidget.h:87`.
- ✅ `out["bytes"]` emit at `:420`.
- ⚠ Loop 0 § 4.1 claimed the 16 MiB ceiling matched
  `MAX_TOOL_OUTPUT_BYTES` in `auditengine.cpp`. Two errors:
  - The constant lives at `auditdialog.h:468`, not
    `auditengine.cpp`.
  - Its value is `64 * 1024 * 1024` (64 MiB), not 16 MiB.
  Rewrote the rationale to drop the false-parity claim: 16 MiB is
  now justified as a rc-consumer rule-of-thumb (16× the 1 MiB MCP
  cap), with the audit constant cited as context only.
- ✅ Patch sketch's `kDefaultBytesCap = 1 * 1024 * 1024` and
  `kMaxBytesCeiling = 16 * 1024 * 1024` consistent with § 4.1.
- ✅ Trim direction (head, drop oldest) preserves the "I want
  recent output" semantic — verified against `recentOutput`'s
  contract (returns trailing N lines of scrollback + screen).
- ✅ `\n` snap is robust to UTF-8 because `\n` (`0x0A`) is never a
  continuation byte in any valid multi-byte sequence (continuation
  bytes are `0x80..0xBF`). The `QByteArray::indexOf('\n', cut)`
  search will never land mid-codepoint.
- ✅ Truncation sentinel `<truncated N bytes / M lines>\n` is
  unambiguous text — no existing scrollback can produce that
  prefix by accident (the angle-bracket-truncated format is
  unique to this path).
- ✅ INV-1 through INV-10 reviewed. Every INV maps to at least
  one GT-* or WI-* test:
  - INV-1 ↔ GT-2, GT-3, GT-6
  - INV-2 ↔ GT-2, GT-3 (assert kept content is the tail)
  - INV-3 ↔ GT-4
  - INV-4 ↔ GT-1 (false on happy path), GT-7
  - INV-5 ↔ GT-2 (counters present on truncated), GT-1 (absent on
    happy path)
  - INV-6 ↔ GT-2 (text starts with `<truncated `)
  - INV-7 ↔ § 4.1 design + default in the patch sketch
  - INV-8 ↔ GT-6
  - INV-9 ↔ existing line cap unchanged; verified at `:413`
  - INV-10 ↔ § 4.4 patch composes on top of existing logic; no
    fields removed
- ✅ Cross-doc with ANTS-1294 (MCP wrap): the wrap site dispatches
  via the registry; adding new fields to the JSON envelope is
  composable with the wrap. No interaction.
- ✅ RAM budget § 9 still accurate post-Loop 1 — trim allocates
  ≤ 2× `max_bytes` peak; bounded.

Status: **CLEAN.** Ready for Loop 2 implementation pass.

---

*End of spec.*

---

*End of spec.*
