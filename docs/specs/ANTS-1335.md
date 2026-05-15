# ANTS-1335 — UTF-8 C1 control-byte strip in `filterControlChars`

**Status:** spec draft → review → implement
**Kind:** security
**Source:** indie-review-2026-05-14 (lane-2 M2)
**Pairs with:** ANTS-1294 (MCP output sanitisation — same defense-in-depth
philosophy applied to the **inbound** payload side) and the existing
ANTS-1052 / ANTS-1132 trust model on the rc UDS socket.

---

## 1. Summary

`RemoteControl::filterControlChars` (`remotecontrol.h:95-113`) strips C0
control bytes (`0x00..0x1F` minus HT/LF/CR + DEL `0x7F`) from any
inbound `send-text`, `launch`, or `new-tab` payload before it reaches
the PTY. The header comment (`remotecontrol.h:79-84`) explicitly defers
C1 stripping (U+0080..U+009F) to "the AI-dialog layer" on the
grounds that bytes `0x80..0xBF` cannot be byte-distinguished from UTF-8
continuation bytes.

That deferral is wrong for the rc/MCP path: bytes coming from
`cmdSendText`/`cmdLaunch`/`cmdNewTab` never traverse the AI-dialog
layer — they go straight to `target->sendToPty()` at
`remotecontrol.cpp:336/527/578`. Stripping C0 but not C1 means a
same-UID rc-socket peer can deliver the UTF-8 encoding of U+009B (CSI
introducer, encoded as `0xC2 0x9B`), U+009D (OSC), U+009E (PM), or
U+009F (APC) to a parser that honours 8-bit C1 controls — bypassing
the C0 filter the rc layer was hardened with in 0.7.52
(ANTS-0.7.52-HIGH `command`-filter parity).

This spec extends `filterControlChars` to detect the exact 2-byte
UTF-8 encoding of U+0080..U+009F (`0xC2` followed by `0x80..0x9F`) and
strip the pair atomically, preserving every other byte. Regular
multi-byte UTF-8 is untouched.

## 2. Problem statement (threat model)

Trust model on the rc UDS socket (per ANTS-1132):

- 0700 perms + `lstat(S_ISSOCK)` + `setSocketOptions(UserAccessOption)`
  on the server side.
- `SO_PEERCRED` UID match on every `newConnection`
  (`remotecontrol.cpp:138-158`).
- C0 strip via `filterControlChars` on `send-text` (always),
  `launch.command` and `new-tab.command` (default-on, `raw:true`
  opt-out) — added in ANTS-0.7.52 to prevent ESC / OSC 52 / cursor-
  reprogramming injection.

The trust boundary is "same UID, but the bytes are user-input shaped
data." A misbehaving Claude Code session, a Lua plugin via
`ants.rc.send()`, or a hand-crafted `nc -U` client running as the
same user can deliver arbitrary bytes through this seam. The C0 filter
catches `0x1B` (ESC) — closing the 7-bit CSI/OSC/DCS introducer
attack — but `vtparser` also honours the 8-bit C1 forms:

| Codepoint | Name | UTF-8 bytes | Behaviour in `vtparser` |
|---|---|---|---|
| U+009B | CSI (8-bit) | `0xC2 0x9B` | enters CSI state, identical to ESC `[` |
| U+009D | OSC (8-bit) | `0xC2 0x9D` | enters OSC state, identical to ESC `]` |
| U+009E | PM  (8-bit) | `0xC2 0x9E` | privacy message |
| U+009F | APC (8-bit) | `0xC2 0x9F` | application program command |
| U+0090 | DCS (8-bit) | `0xC2 0x90` | device control string |
| U+0085 | NEL (8-bit) | `0xC2 0x85` | next line (cursor + CR + LF) |
| U+0088 | HTS (8-bit) | `0xC2 0x88` | sets horizontal tab |

The 8-bit forms reach the same dispatchers as their 7-bit `ESC`-led
twins. So a payload like `b"\xC2\x9D52;c;<base64>\x07"` is a valid
OSC 52 clipboard-write that bypasses the C0 filter (no ESC byte to
strip).

### 2.1 Concrete realisations

- **OSC 52 clipboard overwrite** via `cmdSendText`:
  `{"cmd":"send-text","text":"52;c;cGF5bG9hZA=="}`.
  The UTF-8 of that string is `0xC2 0x9D ...` — passes the C0 filter
  unchanged.
- **Cursor reprogramming / palette change** via `cmdLaunch.command`.
- **APC image injection** via `cmdNewTab.command` (the same vector
  the C0 filter was designed to block, just at the 8-bit form).
- **DCS / Sixel injection** in any of the three.

All three handlers route through `filterControlChars` by default
(except when the caller passes `raw:true`, which is the documented
escape hatch).

### 2.2 Why `vtparser` honours 8-bit C1

`vtparser.cpp` implements the Paul Williams xterm state machine
verbatim, which treats `0x80..0x9F` as direct entries into the
matching states (CSI/OSC/DCS/APC/SOS). This is *correct* terminal
behaviour for trusted PTY output (8-bit C1 has been part of every
xterm-class emulator since 1996). The problem is only that the rc
seam delivers untrusted bytes into a parser that assumes trusted
input.

## 3. Surface — current state

Source survey (2026-05-15):

| Caller | Site | Filter present | C0 stripped | C1 stripped |
|---|---|---|---|---|
| `cmdSendText` | `remotecontrol.cpp:335` | yes (default-on) | ✅ | ❌ |
| `cmdLaunch` (`command`) | `remotecontrol.cpp:504` | yes (default-on) | ✅ | ❌ |
| `cmdNewTab` (`command`) | `remotecontrol.cpp:559` | yes (default-on) | ✅ | ❌ |
| `cmdLaunch` (`cwd`) | `remotecontrol.cpp:513-526` | inline `cwdHasControl` | ✅ | ❌ (but cwd doesn't reach the PTY) |
| `cmdNewTab` (`cwd`) | `remotecontrol.cpp:564-577` | inline `cwdHasControl` | ✅ | ❌ (same) |
| AI-dialog → terminal | `aidialog.cpp` | QChar-level strip | ✅ | ✅ (the comment's claim) |

Three byte-stream paths to harden. The `cwd` paths don't matter for
this spec — they're path strings consumed by `newTabForRemote`, not
PTY writes. They get ANTS-1347's `validatePath` treatment separately.

## 4. Design

**One filter, one change, three call-sites pick it up for free.**

### 4.1 Algorithm

UTF-8 encoding of U+0080..U+009F is exactly the 2-byte sequence
`0xC2 0x80` through `0xC2 0x9F`. No other UTF-8 sequence — 1-byte
ASCII (`0x00..0x7F`), 2-byte non-`0xC2` (`0xC3..0xDF`), 3-byte
(`0xE0..0xEF`), 4-byte (`0xF0..0xF4`), or any continuation byte
(`0x80..0xBF`) following a non-`0xC2` leader — can encode U+0080..
U+009F. The strip rule is therefore unambiguously:

```
for each pair (b0, b1) in stream:
    if b0 == 0xC2 and 0x80 <= b1 <= 0x9F:
        skip both bytes; record 2 stripped
    else:
        emit b0; rescan starting at b1
```

A bare `0xC2` at end-of-stream is preserved (not a C1 sequence;
truncated UTF-8 is the caller's problem). A bare `0x80..0x9F`
continuation byte WITHOUT a preceding `0xC2` is also preserved —
it's already malformed UTF-8 and the existing filter doesn't
synthesise meaning from invalid input.

### 4.2 Patch

```cpp
// remotecontrol.h — replace the body of filterControlChars
static inline QByteArray filterControlChars(const QByteArray &in,
                                            int *out_stripped = nullptr) {
    QByteArray out;
    out.reserve(in.size());
    int removed = 0;
    const int n = in.size();
    for (int i = 0; i < n; ++i) {
        const unsigned char b = static_cast<unsigned char>(in[i]);

        // C1 8-bit form: 0xC2 followed by 0x80..0x9F is the UTF-8
        // encoding of U+0080..U+009F. The C1 introducers (CSI/OSC/
        // DCS/APC/PM/SOS) at 8 bits are honoured by every xterm-
        // class parser including ours — strip them at the rc-seam
        // boundary so untrusted JSON bytes can't drive the parser
        // into a CSI/OSC state without an ESC. See ANTS-1335 spec.
        if (b == 0xC2 && i + 1 < n) {
            const unsigned char b1 =
                static_cast<unsigned char>(in[i + 1]);
            if (b1 >= 0x80 && b1 <= 0x9F) {
                removed += 2;
                ++i;  // skip both bytes
                continue;
            }
        }

        const bool isAllowedWhitespace =
            (b == 0x09 || b == 0x0A || b == 0x0D);
        const bool isC0Bad = (b < 0x20) && !isAllowedWhitespace;
        const bool isDel = (b == 0x7F);
        if (isC0Bad || isDel) {
            ++removed;
            continue;
        }
        out.append(static_cast<char>(b));
    }
    if (out_stripped) *out_stripped = removed;
    return out;
}
```

### 4.3 Comment fix

The leading paragraph of `filterControlChars` (`remotecontrol.h:75-84`)
currently says C1 stripping is the AI-dialog's job. Replace with text
that names the rc/MCP seam responsibility:

```
// Strips C0 control bytes (0x00..0x1F minus HT/LF/CR), DEL (0x7F),
// and the UTF-8 encoding of C1 control codepoints U+0080..U+009F
// (encoded as 0xC2 0x80 .. 0xC2 0x9F) from an rc-socket payload
// before it reaches the PTY. Both classes — 7-bit ESC-led and
// 8-bit C1-led — would otherwise drive vtparser into CSI/OSC/DCS/
// APC states (cursor reprogramming, OSC 52 clipboard overwrites,
// bracketed-paste toggle, Sixel/APC image injection) from
// untrusted same-UID input.
//
// Preserves HT/LF/CR (regular PTY keystrokes), all printable ASCII,
// all multi-byte UTF-8 not in the C1 range, and any malformed
// pre-existing input — we don't synthesise meaning from invalid
// bytes.
```

### 4.4 Why not also strip in `aidialog.cpp`

`aidialog.cpp` already strips C1 at the QChar level (the existing
comment's claim is correct *for that layer*). No change there.
This spec only fixes the rc-byte-stream path.

## 5. Invariants

- **INV-1 — One filter, hardened in place.** `filterControlChars`
  in `remotecontrol.h` is the only byte-level pre-PTY filter on the
  rc surface. No new helper is introduced; the existing call-sites
  (`cmdSendText`, `cmdLaunch.command`, `cmdNewTab.command`) pick
  up the C1 strip without changes.

- **INV-2 — `0xC2 [0x80..0x9F]` strips atomically.** When the
  filter encounters byte `0xC2` followed by a byte in `0x80..0x9F`
  it removes both bytes and increments `out_stripped` by 2.

- **INV-3 — Bare `0xC2` at EOI is preserved.** A trailing `0xC2`
  with no follow-on byte is emitted as-is (malformed UTF-8 in,
  malformed UTF-8 out; not the filter's job to synthesise).

- **INV-4 — Bare `0x80..0x9F` without `0xC2` leader is preserved.**
  Continuation bytes without a valid leader are already malformed
  UTF-8; the filter doesn't reinterpret them.

- **INV-5 — Other 2-byte UTF-8 untouched.** `0xC3 [0x80..0xBF]`
  (U+00C0..U+00FF) and every other valid 2-byte sequence pass
  through unchanged. Regression test PV-7 below pins this.

- **INV-6 — 3- and 4-byte UTF-8 untouched.** Any `0xE0..0xEF` or
  `0xF0..0xF4` leader passes through verbatim along with all of
  its continuation bytes. Regression test PV-8 pins this.

- **INV-7 — `out_stripped` reflects bytes removed, not codepoints.**
  A single C1 strip increments by 2 (matches caller's
  `response.stripped = N` byte count). Documented in the comment.

- **INV-8 — `raw:true` still bypasses.** The opt-out path
  (`rawBypass` in `cmdSendText`/`cmdLaunch`/`cmdNewTab`) still
  delivers the unfiltered payload. This is the documented test-
  harness escape hatch and remains unchanged.

- **INV-9 — No allocation past `in.size()`.** `out.reserve(in.size())`
  bounds the output buffer at the input size (filter is
  length-monotonically-decreasing). No heap growth past that.

- **INV-10 — Zero state.** Filter is a pure function over its input.
  No statics, no caches.

## 6. Failure modes & rejection table

`filterControlChars` doesn't reject — it filters silently and reports
the count. The caller can react to `stripped > 0` if it wants to (the
current rc responses already include `"stripped": N` when the filter
removed any C0; this spec doesn't change that contract — `stripped`
now counts C1 bytes too).

Future hardening could escalate (e.g., reject the whole request if
`stripped > 0` for `launch`). Out of scope here.

## 7. Test plan

**`tests/features/rc_c1_byte_filter/`** (new).

### Engine tests — call `filterControlChars` directly

- **PV-1 empty input** → `out == ""`, `stripped == 0`.
- **PV-2 pure ASCII** (`"hello world\n"`) → unchanged,
  `stripped == 0`.
- **PV-3 C0 strip alone** (`"a\x01b\x1Bc"`) → `"abc"`,
  `stripped == 2`. (Regression for ANTS-0.7.52 baseline.)
- **PV-4 CSI 8-bit strip** (`"a\xC2\x9Bb"`) → `"ab"`,
  `stripped == 2`.
- **PV-5 OSC 8-bit strip** (`"\xC2\x9D52;c;ZGF0YQ==\x07"`) →
  `"52;c;ZGF0YQ=="`, `stripped == 3`. The OSC introducer is C1
  (2 bytes); BEL `0x07` is C0 (1 byte) and also stripped by the
  existing C0 filter (it's not in the HT/LF/CR allow-list).
  Caller-side: the resulting payload no longer activates OSC
  parsing — neither the introducer nor any plausible terminator
  reaches the parser.
- **PV-6 APC 8-bit strip** (`"x\xC2\x9F!_payload\xC2\x9C"`) →
  `"x!_payload"`, `stripped == 4` (both the APC introducer and
  ST terminator are C1).
- **PV-7 0xC3 NOT stripped** (`"\xC3\x80\xC3\xBF"` → U+00C0
  U+00FF) → unchanged, `stripped == 0`.
- **PV-8 3-byte UTF-8 untouched**
  (`"日本語"` — `0xE6 0x97 0xA5 0xE6 0x9C 0xAC 0xE8 0xAA 0x9E`)
  → unchanged, `stripped == 0`. Critically, the `0x9C` byte inside
  日 is a continuation byte, not C1 ST.
- **PV-9 4-byte UTF-8 untouched** (`"🦀"` — `0xF0 0x9F 0xA6 0x80`)
  → unchanged, `stripped == 0`.
- **PV-10 bare 0xC2 at EOI** (`"a\xC2"`) → `"a\xC2"`,
  `stripped == 0`.
- **PV-11 bare 0xC2 followed by ASCII** (`"a\xC2!"`) →
  `"a\xC2!"`, `stripped == 0` (`!` is not in `0x80..0x9F`).
- **PV-12 multiple C1 in a row** (`"\xC2\x9B\xC2\x9D\xC2\x9E"`) →
  empty, `stripped == 6`.
- **PV-13 mixed C0 + C1 + UTF-8**
  (`"a\x01\xC2\x9B日\xC2\x9F"`) → `"a日"`, `stripped == 5`.
- **PV-14 HT/LF/CR preserved** (`"a\tb\nc\rd"`) → unchanged,
  `stripped == 0`.
- **PV-15 DEL stripped** (`"a\x7Fb"`) → `"ab"`, `stripped == 1`.
- **PV-16 reserve bound** — a 1 MiB random input produces an output
  no larger than 1 MiB (no allocation growth past `reserve`).

### Wiring tests — source-grep

- **WI-1** `remotecontrol.h` contains exactly one `b == 0xC2 &&`
  guarded block in `filterControlChars`.
- **WI-2** `remotecontrol.h` no longer contains the phrase
  "Stripping C1 is the AI-dialog layer's job" (replaced by the
  rc-seam comment).
- **WI-3** Each of `cmdSendText`, `cmdLaunch`, `cmdNewTab`
  retains its existing `filterControlChars` call (regression
  against accidental removal).

### Integration test — end-to-end through `vtparser`

- **INT-1** `cmdSendText` with a payload whose UTF-8 bytes are
  `0xC2 0x9B 0x33 0x31 0x6D 0x20 0x72 0x65 0x64 0x20 0xC2 0x9B 0x30 0x6D`
  (two 8-bit CSI introducers framing the text `31m red 0m` — the
  attacker's path-of-least-resistance way to set red foreground and
  then reset, all without an `ESC` byte). With the patch, the rc
  filter strips both `0xC2 0x9B` pairs (`stripped == 4`) and the PTY
  receives only the printable bytes (`"31m red 0m"`); the grid
  renders default colour throughout, no SGR side effect.
  Pre-patch regression control: both `0xC2 0x9B` pairs survive,
  `vtparser` decodes them to U+009B, routes via `processChar(0x9B)`
  → `transition(CsiEntry)` (`vtparser.cpp:274`), and the grid
  renders red text until the second introducer's reset. Asserts the
  rc seam now matches the 7-bit ESC `[` no-side-effect contract for
  the 8-bit twin.

### Pre-fix regression check

Engine tests PV-4 through PV-13 fail to assert against pre-patch
`filterControlChars` (the bytes pass through unchanged). Integration
test INT-1 produces a visible SGR side effect. All loud.

## 8. Migration / back-compat

- **Wire format:** unchanged. Same request schema, same response
  envelope, same `stripped` counter (now counts C1 bytes too,
  documented in INV-7).
- **`raw:true` opt-out:** unchanged. Test harnesses or plugins that
  legitimately need byte-passthrough (e.g., for testing the C1
  paths themselves) still get raw bytes.
- **AI-dialog path:** unchanged. The QChar-level strip there is
  redundant with this new byte-level strip in the rc/MCP path, but
  the two layers are independent (AI-dialog operates on string
  input that's never seen the rc filter).
- **Comment update only:** the header doc claim "stripping C1 is the
  AI-dialog layer's job" is replaced by accurate text. No public
  API change.

## 9. Performance & RAM budget

- **Per-call cost:** one additional comparison (`b == 0xC2`) per
  byte, then on hit a peek + range-check. Branch-predictor friendly
  (rare hit on real input). On a 1 MiB benchmark of pure ASCII the
  additional overhead is ≈ 1.0× the existing C0 scan — a single
  extra compare per byte.
- **Memory:** `out.reserve(in.size())` already bounds heap usage at
  input size. No new allocations.
- **Idle:** zero. No timers, no caches, no watchers.
- **rc-path latency:** sub-µs per typical command (32-byte
  send-text). No measurable wire-time impact.

Total new RAM/CPU budget: negligible. No long-lived structures.

## 10. Out of scope

- **C1 in PTY output stream (PTY → vtparser direction).** The PTY
  is trusted output (we wrote the shell, the shell can emit any C1
  it wants). The rc filter is one-directional: input from peers.
- **Reject-on-stripped escalation.** A future ticket might escalate
  `stripped > 0` to a request-level reject for the `launch` and
  `new-tab` verbs (where a benign caller has no reason to include
  control bytes). Out of scope here — the current "strip and
  report" contract is preserved.
- **`cwd` field hardening.** `cmdLaunch.cwd` and `cmdNewTab.cwd`
  already have an inline `cwdHasControl` check that rejects on
  the C0/backslash class. C1 in `cwd` is left for ANTS-1347 to
  fold in alongside `PathValidation::validatePath` (which already
  NFC-normalises and rejects backslash; adding `0x80..0x9F`
  rejects is a one-line addition there).
- **Other C1 forms in non-UTF-8 input.** If the rc socket ever
  accepts non-UTF-8 byte streams (Latin-1, GBK, etc.), C1 could
  appear as raw `0x80..0x9F` single bytes. Today the JSON wire
  format is UTF-8 only (Qt's `QJsonDocument` enforces it), so
  bare-C1 cannot reach `filterControlChars`. If a future protocol
  adds binary frames, this filter needs a binary-mode revisit.
- **vtparser-side hardening.** Making vtparser ignore 8-bit C1
  entirely would also close the vector but break legitimate PTY
  output. Wrong layer for the fix.

## 11. Cold-eyes review log

**Loop 0 (initial draft, 2026-05-15):** Drafted above.

**Loop 1 — VERIFY against current source (2026-05-15):**

- ✅ `filterControlChars` is at `remotecontrol.h:95-113` (inline static).
  Leading comment block at `:75-94` claims C1 stripping is the AI-dialog
  layer's job — text matches § 4.3 exactly.
- ✅ Call-sites: `cmdSendText` at `remotecontrol.cpp:335` (`stripped`
  emitted at `:339-341`), `cmdLaunch.command` at `:504`
  (`stripped` at `:530`), `cmdNewTab.command` at `:559`
  (`stripped` at `:581`). Three byte-stream paths, all default-on,
  all picking up the C1 strip for free once the helper is patched.
- ✅ `vtparser` 8-bit C1 routing confirmed:
  - **UTF-8 path** (`0xC2 0x9B`): byte `0xC2` enters the 2-byte
    sequence state at `vtparser.cpp:165-168`
    (`(byte & 0xE0) == 0xC0`); continuation byte `0x9B`
    accumulates at `:134` to codepoint `0x9B`; `:139` overlong-check
    passes (`0x9B >= 0x80`); `flushCodepoint(0x9B)` at `:144` →
    `processChar(0x9B)` → CSI dispatch at `:274`
    (`case 0x9B: transition(CsiEntry); break;`).
  - **Bare-byte path** (`0x9B` alone, without `0xC2` leader): blocked
    upstream by `QJsonDocument::fromJson`'s UTF-8 strictness — invalid
    UTF-8 returns parse error, no command runs. Only the UTF-8-valid
    form `0xC2 0x9B` can reach `filterControlChars`. The bare-byte
    branch in `vtparser.cpp:155-164` exists for the PTY-output side
    (legacy 8-bit-C1-aware producers), which is correctly out of
    scope for the rc-input filter.
- ✅ INT-1 originally claimed the payload `"31m red 0m"` was a CSI
  sequence; it isn't — it's printable ASCII. Fixed to use JSON
  `"31m red 0m"` which the decoder emits as
  `0xC2 0x9B ...` UTF-8 bytes.
- ✅ ANTS-0.7.52 reference: indie-review-2026-04-27 introduced the
  `command`-filter parity (`launch` + `new-tab` route through
  `filterControlChars` by default). The lead comments at
  `remotecontrol.cpp:480-486` and `:548-549` confirm this. The new
  C1 strip composes with that existing default-on filter — no new
  opt-out, no new wire field.
- ✅ INV-1 through INV-10 reviewed for consistency. Every INV maps
  to at least one PV-* or WI-* test case:
  - INV-1 ↔ WI-1 (single guarded block) + WI-3 (existing call-sites
    retained)
  - INV-2 ↔ PV-4, PV-5, PV-6, PV-12, PV-13 (positive-strip cases)
  - INV-3 ↔ PV-10
  - INV-4 ↔ (not separately testable through the JSON wire — covered
    by the design discussion in Loop 1 above; QJsonDocument rejects
    bare 0x80-0x9F upstream)
  - INV-5 ↔ PV-7
  - INV-6 ↔ PV-8, PV-9
  - INV-7 ↔ PV-4, PV-5, PV-12, PV-13 (assert `stripped == bytes`)
  - INV-8 ↔ design-level; rawBypass branch unchanged, smoke-tested
    by existing `remote_control_opt_in` feature test
  - INV-9 ↔ PV-16
  - INV-10 ↔ no statics in patch (verified by grep — § 4.2 patch
    contains no `static` keyword)
- ✅ Cross-doc: no contradiction with ANTS-1294 (which sanitises
  *outbound* MCP responses) or ANTS-1295 (which anchors path-typed
  arguments). The three specs together cover the three trust-boundary
  dimensions on the rc/MCP surface: bytes-out, paths-in, bytes-in.
- ✅ RAM budget: § 9 unchanged after verification — one extra compare
  per byte on the hot path, no new heap, no statics.

Status: **CLEAN.** Ready for the Loop 2 implementation pass.

---

*End of spec.*

---

*End of spec.*
