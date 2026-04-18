# Threaded parse equivalence

**Status:** locked in 0.7.0.

## Invariant

For any byte sequence fed to `VtParser`, the sequence of `VtAction`s
emitted — concatenated in order — must be identical whether the parser
ran on the GUI thread (legacy path) or on the VtStream worker thread
(0.7.0 path).

In other words: moving parse work off-thread is allowed to change
*when* actions arrive, but must never change *what* actions arrive or
*in what order*.

This test pins the contract at the `VtParser` level. It does not spin up
an actual QThread — the equivalence is proven at the algorithmic layer
(one parser fed byte-by-byte; a second parser fed the same bytes in
worker-batch-sized chunks; emitted actions compared token-by-token).
Running the actual thread transport is covered separately by the Qt
queued-connection machinery, which is provider-tested — this spec pins
the application-level invariant those mechanics must preserve.

## Corpus

Fixture inputs must cover, at minimum, one of each:

1. Plain ASCII, no escape sequences.
2. Mixed ANSI: SGR, cursor-position, scroll region, tabs, wraps.
3. DCS payload (Sixel).
4. APC payload (Kitty graphics).
5. OSC long-string (OSC 52 clipboard set with a non-trivial base64 blob).
6. UTF-8 multi-byte across a buffer boundary. The boundary must fall
   inside a codepoint's continuation bytes to exercise the UTF-8
   decoder's resumption state.
7. Synchronous-output DEC mode 2026 begin/end pair.
8. OSC 133 A/B/C/D shell-integration markers.
9. CSI with many (≥32) numeric parameters.
10. Input split into 1-byte chunks.
11. Input split into arbitrary chunk sizes (a pseudo-random pattern
    derived from a fixed seed for reproducibility).

## Pass criteria

For every fixture:

- The number of emitted `VtAction`s is identical between the reference
  single-shot parse and every chunked parse.
- For each index `i`, `actions[i]` comparison covers `type`, `codepoint`,
  `controlChar`, `finalChar`, `params`, `colonSep`, `intermediate`, and
  `oscString`. All must match byte-for-byte.

On any mismatch the test prints the fixture name, the diverging action
index, and a diagnostic dump of both action tokens so the failure is
self-diagnosing.
