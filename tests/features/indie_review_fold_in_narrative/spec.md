# indie_review_fold_in narrative_mode (ANTS-1644)

Source-grep feature test for the narrative-mode port from ANTS-1635
(test_audit_fold_in) into the indie-review sibling.

## Surface

- `src/remotecontrol.cpp` `cmdIndieReviewFoldIn` — narrative-mode
  short-circuit lives after the `RcGate::checkCallerCwd` gate and
  before the `actionable.isEmpty()` validation.
- `src/claudeintegration.cpp` indie_review_fold_in descriptor —
  `narrative_mode` (bool) + `narrative_md` (string) schema props;
  `actionable` dropped from `required` (only `caller_cwd` remains).

## Invariants (mirror docs/specs/ANTS-1644.md § 3)

- **INV-2** Handler shorts to narrative mode when
  `narrative_mode==true`. The branch reads `narrative_md`, calls
  `RoadmapFoldIn::insertBlock` (atomic write inherited unchanged),
  and does NOT call `RoadmapFoldIn::allocateIds`. Heading literal is
  `### 🔍 Indie-review fold-in (`. Branch is positioned AFTER
  `RcGate::checkCallerCwd` (INV-5: gate intact) and BEFORE the
  `actionable[].isEmpty()` validation.
- **INV-3** Empty `narrative_md` refuses with code
  `narrative_md_required`.
- **INV-4** `actionable[]`-missing refusal error contains
  `narrative_mode` so callers find the escape hatch from the refusal
  alone.
- **INV-9** Descriptor declares `narrative_mode` + `narrative_md`
  schema props; `actionable` is NOT in `schema["required"]`;
  `caller_cwd` IS.

## Rationale

Pure source-grep — no engine link needed. Runs under `test_claude`
which already exposes `SRC_REMOTECONTROL_CPP_PATH` and
`SRC_CLAUDE_INTEGRATION_CPP_PATH`. Mirrors the
`cold_eyes_fold_in_freeform` pattern (ANTS-1510).
