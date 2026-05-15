# Feature: rc cwd anchor on launch / new-tab

Canonical design: `docs/specs/ANTS-1347.md`. Test-side restatement.

## Problem

`cmdLaunch` (`remotecontrol.cpp:466`) and `cmdNewTab`
(`remotecontrol.cpp:534`) accept a `cwd` field. Each runs an inline
`cwdHasControl` lambda rejecting control characters + backslash —
but neither anchors `cwd` against the focused tab's project root.
Same-UID rc/MCP peers can `cwd="/etc"` or `cwd="../../proc/self/fd"`
and the new tab opens outside any project. Every other path-typed
rc verb (file_outline / workspace_search / git_state /
debt_sweep_apply_fix / indie_review_corroborate /
cold_eyes_cross_doc_diff) anchors via
`PathValidation::validatePath` post-ANTS-1295.

Fix: lift the two `cwdHasControl` lambdas to a shared
`cwdHasBadByte` helper that also rejects C1 (path-side counterpart
to ANTS-1335's byte strip), then route `cwd` through
`PathValidation::validatePath` by default. New optional
`allow_outside_root:true` opt-out preserves the legitimate
chdir-anywhere workflow (e.g., a Lua plugin opening a support-
bundle directory).

## External anchors

- ANTS-1295 — central `PathValidation::validatePath`. Eight existing
  call-sites; this spec adds two.
- ANTS-1335 — UTF-8 C1 byte strip in `filterControlChars`. The path
  helper here is the reject-on-bad-byte counterpart for fields where
  silent mutation would mislead the caller.

## Invariants (full list in `docs/specs/ANTS-1347.md`)

- **INV-1** Anchor on by default — every `cmdLaunch`/`cmdNewTab`
  request with non-empty `cwd` and `allow_outside_root != true`
  routes `cwd` through `PathValidation::validatePath`.
- **INV-3** `allow_outside_root:true` skips anchor but not byte
  hygiene.
- **INV-4** `cwdHasBadByte` rejects U+0000..U+001F + backslash + C1
  (U+0080..U+009F).
- **INV-5** Two verbs share one helper. Old `cwdHasControl` lambdas
  removed.
- **INV-6** Anchor reject emits `code:"bad_path"` (uniform with
  ANTS-1295). Byte-hygiene reject keeps `code:"bad_cwd"` for the
  different class.

## What the C++ test pins

CB-1..CB-7 exercise `cwdHasBadByte` directly (pure helper, no Qt
event loop). WI-1..WI-5 grep the source for the wiring. The
end-to-end behavioural assertions (LC-* in the canonical spec) are
not run from `test_core` because calling cmdLaunch/cmdNewTab would
drag remotecontrol.cpp.o into the link line (ANTS-1387 documents the
constraint). Source-grep covers the wiring; runtime tests on the
two verbs can land in `test_chrome` (which already has MainWindow
context) in a future commit if needed.
