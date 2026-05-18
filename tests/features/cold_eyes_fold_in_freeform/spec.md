# cold_eyes_fold_in freeform-mode (ANTS-1510)

`cold_eyes_fold_in`'s default flow allocates `[ANTS-NNNN]` IDs from
`.roadmap-counter` and renders bullets that match the shareable
`docs/standards/roadmap-format.md § 3.5.1` scheme. Projects whose
ROADMAP follows a different convention (RetroDB's "Pass N.M"
headings being the surfacing example, but the pattern generalises to
any non-`[PROJ-NNNN]` ID scheme) historically had the tool refuse with
no recoverable path other than "write the block by hand".

This spec covers the freeform mode added by ANTS-1510 that lets such
projects use the tool. The same overload exists in
`ColdEyesEngine::templateColdEyesFoldInBlockFreeform`; the
`indie_review_fold_in`, `test_audit_fold_in`, and
`debt_sweep_apply_fix` tools track the same enhancement in their own
follow-up roadmap entries.

## Surface

- `src/coldeyesengine.h` — declares
  `templateColdEyesFoldInBlockFreeform(actionable, dateIso)` alongside
  the existing ID-prefixed `templateColdEyesFoldInBlock`.
- `src/coldeyesengine.cpp` — implementation; emits `### 📝 Cold-eyes
  <DATE>` heading + one `- **Cold-eyes finding:** <file:line> — cited
  across [<lanes>]` bullet per finding + a single trailing `Source:
  cold-eyes-<DATE>` provenance line.
- `src/remotecontrol.cpp` `cmdColdEyesFoldIn` — accepts
  `id_allocation: "auto" | "skip"` (default `auto`). On `"skip"`,
  `RoadmapFoldIn::allocateIds` is not called, the freeform template
  is used, and the response carries `id_allocation:"skip"` plus an
  empty `allocated_ids:[]`.
- `src/claudeintegration.cpp` cold_eyes_fold_in descriptor — describes
  the required project shape for `auto`, names the
  `release_block_heading` requirement when no auto-detect target
  exists, and adds an `id_allocation` enum property with both modes.

## Invariants

- **INV-1** `src/coldeyesengine.h` declares `templateColdEyesFoldInBlockFreeform`
  in `namespace ColdEyesEngine` with the two-arg signature
  (`actionable`, `dateIso`).
- **INV-2** `src/coldeyesengine.cpp` defines the freeform template — its body
  emits `### 📝 Cold-eyes <DATE>` and `- **Cold-eyes finding:**`
  bullets WITHOUT a `[ANTS-` prefix.
- **INV-3** `src/remotecontrol.cpp` `cmdColdEyesFoldIn` recognises the
  `id_allocation` parameter, accepts `"auto"` and `"skip"`, and
  refuses any other value with `code:"bad_args"`.
- **INV-4** When `id_allocation:"skip"` is passed, the handler skips
  `RoadmapFoldIn::allocateIds` (no `.roadmap-counter` touch) and calls
  `templateColdEyesFoldInBlockFreeform`.
- **INV-5** The response envelope carries `id_allocation:<mode>` echo
  for both modes.
- **INV-6** `src/claudeintegration.cpp` cold_eyes_fold_in descriptor
  declares the `id_allocation` schema property with `enum: ["auto",
  "skip"]` and `default: "auto"`, and the description text mentions
  both the `.roadmap-counter` requirement and the freeform-mode
  escape hatch.

## Rationale

Source-grep test — verifies the wiring is present, not the
end-to-end behaviour (the RetroDB-style end-to-end happens out of
process). Caller can compose `id_allocation:"skip"` +
`release_block_heading:"<their heading>"` to splice without any
project-format assumptions.
