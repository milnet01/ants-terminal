# roadmap_id_format_declared — a project declares its id format

Contract for ANTS-3771. Full design:
[`docs/specs/ANTS-3771-id-format-declaration.md`](../../../docs/specs/ANTS-3771-id-format-declaration.md).

A project may write its id format down in `.ants/project.json`:

```json
{ "id_format": { "prefix": "ANTS", "pattern": "^([A-Za-z]{1,4}\\d+)(?:\\.|$)" } }
```

`prefix` is generative — it decides what an allocation renders and refuses a
written id that disagrees. `pattern` is recognitional — it decides which text
of a GFM bold lead-in is the item's id. Both members are optional and
independent, and a project that declares neither parses exactly as it does
today.

## Invariants

Numbering follows the design spec's § 3, so a case here and an invariant there
carry one name.

- **INV-1** — no declaration, an empty one, or one whose every member was
  dropped by validation parses **byte-identically** to today.
- **INV-2** — a declared `pattern` changes only the GFM bold-lead-in branch and
  the `rec.id` precedence: on a match the lead-in beats the body-wide `rxId`.
  Every other branch assigns what it assigns today, and on a non-match so does
  that one.
- **INV-3** — on a match `rec.id` and `rec.idToken` are capture group 1 (the
  whole match when the pattern has no group), `rec.boldId` is **empty**, and the
  text the pattern did not consume is the headline. The pattern is applied to
  `extractBoldId()`'s output — the span with its one trailing `.` chopped — not
  to the raw span.
- **INV-4** — on a non-match `rec.id` is exactly what the undeclared path
  assigns. **No bullet loses an id it has today.**
- **INV-5** — `id_inferred` is emitted for a bullet the pattern did **not**
  match and omitted when it did, **including when the capture spans the whole
  bold span** — the case an unchanged `boldId` would get wrong.
- **INV-6** — the store backend is untouched: a declaration carried through
  `bulletsFor()` cannot change a record, because every one is derived from
  `RoadmapRender::bulletText()`, which emits ants-v1.
- **INV-7** — a valid declared `prefix` beats the store's `id_prefix` row and
  the markdown sniff, and loses to an explicit `id_prefix` argument.
- **INV-8** — `roadmap_log op:append` / `op:append_batch` refuse a
  caller-supplied written id that the project's declaration rejects, and write
  nothing. `bad_id_format` when neither the universal grammar nor the declared
  pattern accepts it; `id_format_mismatch` when it is well-formed but its
  prefix is not the declared one.

  **This fires only on a project that DECLARES an id_format**, which is a
  deliberate departure from the design spec's § 2.3 table (row 1 there reads
  "always, declaration or not"). Measured 2026-08-21: `Ts20-SP6` and
  `Demo-SP1` — the documented `stable_id` example and the values
  `mcp_roadmap_log_append_batch` and `roadmap_log_stable_prefix_hint` assert on
  — both FAIL the universal grammar, whose suffix must be `-\d+`. An
  unconditional row makes `id_strategy:"stable_prefix"` unusable and reddens
  two shipped suites.
- **INV-9** — `project_settings op:set` refuses an `id_format` whose `prefix`
  fails `isValidIdPrefix()`, or whose `pattern` is over 512 bytes, does not
  compile, or is not a string — and the file on disk is unchanged.
- **INV-10** — `ProjectSettings::load()` DROPS an invalid member rather than
  failing, leaving the rest of the settings object intact.
- **INV-12** — under a declared `pattern` a migration records a matched bullet
  as `id_origin = 'parsed'`, not `'quarantined'`, and still accepts every token
  the universal grammar accepts.
- **INV-13** — every path that parses a project's roadmap resolves the same id
  for the same bullet.

INV-11 (no second id renderer on the allocation path) has no case here: it is a
source-grep, and a passing test cannot show that nothing was added.
