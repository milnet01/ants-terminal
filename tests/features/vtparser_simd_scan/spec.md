# Feature: VT-parser SIMD fast-path preserves action-stream equivalence

## Contract

`VtParser::feed()` emits an action stream that depends only on the bytes
it consumes, never on how those bytes are chunked across calls or on
whether the internal implementation used a SIMD scan to skip ahead
through safe ASCII runs. For any input byte sequence `S`, the following
three feeds MUST produce byte-identical action streams:

1. `feed(S, |S|)` — single call; the SIMD fast path exercises longest
   runs.
2. `feed(S[0..1], 1); feed(S[1..2], 1); …; feed(S[|S|-1..|S|], 1);` —
   one byte at a time; SIMD path is effectively disabled because every
   `feed()` starts with ≤1 byte of safe ASCII available.
3. A pseudo-random chunking of `S` into 1..17-byte feeds — exercises
   the boundary between SIMD chunks and the scalar fallback.

"Byte-identical action stream" means: the sequence of `VtAction`s
delivered to the callback has the same length and each corresponding
pair of actions has equal `type`, `codepoint`, `controlChar`,
`finalChar`, `params`, `colonSep`, `intermediate`, and `oscString`.

## Rationale

The 0.7.0 performance item "SIMD VT-parser scan" fast-paths the Ground
state by scanning for the next non-printable-ASCII byte 16 at a time
via SSE2 / NEON. Every byte in the safe range `[0x20..0x7E]` is emitted
as a `Print` action directly, bypassing the full state machine. This
is the dominant parser speedup on TUI-repaint workloads (see ROADMAP
§0.7 Performance, citing Ghostty's benchmark).

The risk is that the SIMD scan returns an incorrect boundary — either
past the first interesting byte (state machine sees the wrong state)
or short of the last safe byte (no correctness issue but defeats the
optimization). Either failure mode silently breaks parser behavior in
ways unit tests on individual escape sequences might miss, because the
break only triggers on specific byte-boundary alignments between safe
runs and escape bytes.

## Invariant

For the sixteen-case corpus defined in `test_vtparser_simd.cpp` (covering
plain ASCII, runs with embedded escapes, UTF-8 multibyte, C0 controls,
DEL, CSI sequences with parameters, OSC sequences with BEL and ST
terminators, and boundary-aligned interesting bytes at every offset
0..31), all three feed strategies produce byte-identical action streams.

## Scope

### In scope
- Action-stream equivalence across chunking.
- Coverage of every byte class: printable ASCII, C0 controls, DEL,
  UTF-8 leaders/continuations, ESC-prefixed sequences (CSI, OSC, DCS).
- Boundary alignment: the first interesting byte must be detected at
  every offset modulo 16 (SSE2 lane width).

### Out of scope
- Throughput benchmarks. Correctness here; performance is validated by
  hand against a representative workload and documented in
  `CHANGELOG.md`.
- Architectures without SSE2 or NEON. The scalar fallback is the same
  loop the parser had before the SIMD change; equivalence holds
  trivially.
- Malformed UTF-8 recovery paths beyond what the pre-SIMD parser
  already handled — this change does not alter UTF-8 decoding.

## Regression history

- **0.7.0-pre:** SIMD fast path introduced. This test exists to catch
  any future attempt to widen the fast path (e.g. to batch `Print`
  into a single action, or to skip past high-bit bytes assuming they're
  UTF-8 continuations) that silently diverges from the scalar parser.
