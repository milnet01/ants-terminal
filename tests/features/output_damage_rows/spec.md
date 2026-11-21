# Output damage rows

**Status:** locks ANTS-3464. Contract:
[`docs/specs/ANTS-3464-output-damage-rows.md`](../../../docs/specs/ANTS-3464-output-damage-rows.md).

## What we test

An offscreen `TerminalWidget` fed parsed batches through `onVtBatch`, with
no event loop running, so the paint pacer stays armed after the first
batch and later damage stays pending. `takeOutputDamageForTest` reports
both.

1. **INV-1.** A kept canvas, repainted only where the output path issued
   damage, matches a fresh full paint after every batch: typing, cursor
   moves, a spinner, wide and combining text, bracket matching and a
   trigger-rule highlight.
2. **INV-2.** One typed character issues damage no taller than one row's
   band plus the cursor glow.
3. **INV-3.** Each named fallback issues a full repaint.
4. **INV-4.** Damage pending behind the pacer survives a small paint that
   clears the grid's dirty bits.

INV-2 and INV-4 fail against the pre-change output path, which issued a
full repaint for every batch. INV-1 and INV-3 pass before the change and
guard it.
