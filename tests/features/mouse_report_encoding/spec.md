# Mouse reports: release, drag, extra buttons and the wheel

ANTS-6654. While an app has mouse reporting on (DECSET 1000, 1002 or 1003),
`TerminalWidget` reports clicks, drags and the wheel. Three pure helpers
decide the bytes, so the test calls them without a PTY:

- `encodeMouseReport` builds one report, SGR (1006) or the normal `CSI M`
  form;
- `mouseReportButton` gives a mouse event's base button code, or -1 for a
  button with none;
- `takeWheelNotches` turns wheel deltas into whole notches of 120 units.

## Invariants

- **INV-1 — the normal form reports a release.** A release encodes as
  button 3 plus the modifier bits: `CSI M` then `3+32`, column+32, row+32.
  Ants does not offer X10 mode 9, the only mode without releases.
- **INV-2 — a drag carries the motion bit in both forms.** A left-button
  drag (button 0, neither press nor release) encodes `0+32+32` in the
  normal form and `32` in SGR. Bare motion (button 35) is unchanged.
- **INV-3 — a press is unchanged.** A left press is `CSI M` `32` in the
  normal form and `CSI < 0;col;row M` in SGR; an SGR release ends in `m`.
- **INV-4 — extra buttons send nothing.** Back and Forward pressed or
  released give -1; a left press with Back held gives 0; motion with no
  button held gives 35.
- **INV-5 — the wheel reports per notch.** Four deltas of 30 give 0, 0, 0
  and then 1 notch; a delta of 240 gives 2; -120 gives -1; the remainder
  carries to the next event.

## Regression history

- **ANTS-6654:** the normal form dropped every release and left drags
  without the motion bit; Back and Forward were reported as a left click;
  each small touchpad delta sent a whole wheel report.
