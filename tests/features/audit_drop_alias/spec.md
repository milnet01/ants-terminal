# audit_drop_alias — `// audit: drop` alias for the existing
# `// ants-audit: disable` token (ANTS-1111)

Source-grep verification that the `audit:\s*drop` alternative was
added to the inline-suppress regex at `auditdialog.cpp:2055`.
Behaviour identical to the long form — same parser code runs.

## INVs

- INV-9 (positive): the regex source string in auditdialog.cpp
  contains `audit:\s*drop(?:-next-line|-file)?` as an alternative
  inside the `reBare` regex.
- The pre-existing `ants-audit:\s*disable` alternative is still
  present (no replacement, both coexist).
