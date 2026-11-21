# ANTS-1335 — UTF-8 C1 control-byte strip in `filterControlChars` — review loop log

Record for [`../specs/ANTS-1335-utf-8-c1-control-byte.md`](../specs/ANTS-1335-utf-8-c1-control-byte.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

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
