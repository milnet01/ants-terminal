# token_usage_no_ci_diagnostic — ANTS-1422

See `docs/specs/ANTS-1422.md`.

## Test scope

Source-scrape regression locks the diagnostic envelope added
to `cmdTokenUsage`'s two error branches and the retirement of
the legacy `tuErr` helper.

## Invariants checked

- **INV-1.** no_ci branch emits a `debug` object with
  `m_main_ptr`, `this_rc_ptr`, and `ci_via_getter_null:true`.
- **INV-2.** no_main branch emits the same diagnostic shape
  with `m_main_ptr:"0x0"`.
- **INV-3.** Legacy `tuErr(const QString &, const QString &)`
  helper definition is gone; retirement comment present.
- **INV-4.** Success path doesn't assign a `debug` field.
- **INV-5.** Both error branches set `env["code"]` equal to
  the `env["error"]` they emit.
