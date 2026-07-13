# audit_drop_alias — `// audit: drop[=rule]` alias for the existing
# `// ants-audit: disable` token (ANTS-1111)

Runtime verification that `AuditDialog::commentSuppresses()` honours the
`audit: drop` alias with the same semantics as the long
`ants-audit: disable` form. Upgraded from source-grep to a real call of
the compiled static method (ANTS-3355) — the grep guard could not catch a
behavioural regression in the parser (moved alternative, broken terminator
class, rule-list mismatch); the runtime call does.

`commentSuppresses` is a static method that uses only Qt Core
(QString / QRegularExpression), so the test needs no QApplication or
dialog instance.

## INVs (ANTS-1111 §4)

- INV-9 (bare): `commentSuppresses("audit: drop", <any>)` → true. The
  `audit:drop` (no space) and `audit: drop-next-line` variants also
  suppress.
- INV-9 (ruled): `commentSuppresses("audit: drop=foo-bar", "foo-bar")` →
  true; `commentSuppresses("audit: drop=foo-bar", "baz")` → false.
- INV-9 (glob): a `*` in the rule list matches by prefix —
  `"audit: drop=goog*"` suppresses `google-creds` but not `aws-creds`.
- Over-match guard (§2.6): a comment that merely mentions audit/drop but
  is not the token (`"audit: see ANTS-1111"`, `"dropping the audit
  table"`, `""`) must NOT suppress.
- Coexistence: the pre-existing `ants-audit: disable[=rule]` form still
  suppresses with identical rule-list semantics.
