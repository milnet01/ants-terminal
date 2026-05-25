# CSI parameter-truncation flag (ANTS-1827)

`VtParser` caps a CSI sequence at 32 parameters to bound a DoS-shaped stream.
Pre-fix, params past the cap were dropped silently — unlike OSC/DCS/APC payload
truncation, which surfaces a `VtAction::truncated` flag. A long SGR chain could
therefore apply a state the stream never specified, undetectably.

## Surface

- `VtAction::paramsTruncated` — set on a `CsiDispatch` action when the parser
  dropped one or more params at the 32-param cap.
- `VtParser` resets the in-progress flag on `CsiEntry`, so it never leaks from
  one CSI to the next.
- `TerminalGrid::handleCsi` refuses a `paramsTruncated` CSI outright (mirrors
  `handleOsc`'s truncated-payload refusal).

## Invariants

- **INV-1** A CSI within the 32-param cap is not flagged: `paramsTruncated`
  is false and all params are retained.
- **INV-2** A CSI exceeding 32 params sets `paramsTruncated` and retains exactly
  32 params (the rest are dropped).
- **INV-3** The flag does not leak: a normal CSI immediately following a
  truncated one is not flagged.
