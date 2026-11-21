# ANTS-1348 — Server-side byte cap on `cmdGetText` (`mcp__ants__get_text`) — review loop log

Record for [`../specs/ANTS-1348-server-side-byte-cap-cmdgettext.md`](../specs/ANTS-1348-server-side-byte-cap-cmdgettext.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

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
