# sixel_repeat_work_cap — ANTS-4456

Folded out of ANTS-4456 (cold-sweep 2026-08-18, lane vt-core), where
it was carried as an UNVERIFIED HIGH claim. Verified against source
before this contract was written.

## The defect

`TerminalGrid`'s Sixel decoder bounds the repeat introducer `!` per
group only — the count is clamped to `MAX_IMAGE_DIM`. Nothing bounds
the *sum* of repeat counts across a payload.

`$` returns the cursor to column zero without advancing the band. So a
payload that alternates a full-width repeat with `$` keeps every write
inside the image, and the pixel-write total scales with payload length
rather than with image area. The declared image stays small, so the
dimension cap and the image-budget cap both pass.

The result is a hang on untrusted terminal output: displaying such a
file pins the decoder with no cap reached and no error shown.

## The rule

A well-formed Sixel writes each pixel about once. Colour passes rescan
a band, but each pass writes only the pixels belonging to its own
colour, so the total across passes stays near the image area.

The decoder therefore carries a whole-payload pixel-write budget of a
small multiple of the image area. Exhausting it aborts the decode and
reports an inline error, in the same shape as the dimension and image
budget caps.

## Invariants checked

- **INV-1.** A repeat-and-`$` payload whose pixel writes exceed the
  budget produces no inline image.
- **INV-2.** An ordinary small Sixel still renders.
- **INV-3.** A multi-colour Sixel that rescans one band once per
  colour still renders — the budget must not mistake legitimate
  colour passes for the attack.
- **INV-4.** The out-of-bounds variant is refused too. A raster
  header pins the height small and `-` advances the band past it, so
  no pixel write survives the bounds check while the repeat loop
  still spins. The budget is therefore charged on the repeat group,
  not on the surviving write.

## Repro before the fix

INV-1's payload is sized to exceed the budget by a wide margin while
still completing quickly against the pre-fix decoder, so the red run
shows the missing cap rather than the hang. The full-size version of
the same payload is what hangs; it is deliberately not used as a test
input.
