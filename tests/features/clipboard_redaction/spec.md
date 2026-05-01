# Clipboard-write redaction funnel (ANTS-1014)

7th-audit memory flagged the lack of a central funnel for
clipboard writes. 15 raw `QApplication::clipboard()->setText(...)`
sites lived in tree, two of which (OSC 52 callback, Lua plugin
`ants.clipboard.write` glue) come from untrusted sources.

Companion to `docs/specs/ANTS-1014.md`. The full design / threat
model lives there; this file is the test contract.

## Invariants

Source-grep + behavioural drive of the pure `sanitize` helper.

- **INV-1** Header `src/clipboardguard.h` declares the
  `clipboardguard` namespace, a `Source` enum class with the three
  documented values (`Trusted`, `UntrustedPty`, `UntrustedPlugin`),
  and a `sanitize(QString text, Source source) -> QString` free
  function. Asserted by source-grep.
- **INV-2** `sanitize(text, Trusted)` strips embedded `QChar(0)`
  (NUL) characters. Behavioural drive: feed `"a\0b\0c"`, expect
  `"abc"`.
- **INV-3** `sanitize(text, UntrustedPty)` truncates input
  exceeding 1 MiB. Behavioural drive: feed 2 MiB of `'A'`, expect
  exactly 1 MiB out.
- **INV-4** `sanitize(text, UntrustedPlugin)` applies the same
  1 MiB cap as `UntrustedPty`. Behavioural drive.
- **INV-5** `sanitize(text, Trusted)` does NOT truncate large
  inputs — a 2 MiB feed comes through unchanged. The user
  pasting a large file dump from the context menu is a legitimate
  flow. Behavioural drive.
- **INV-6** No raw `QApplication::clipboard()->setText(` call
  survives in `src/terminalwidget.cpp`. Asserted by negative
  source-grep.
- **INV-7** No raw `QApplication::clipboard()->setText(` call
  survives in `src/mainwindow.cpp`. Asserted by negative
  source-grep.
- **INV-8** Each migrated call site declares an appropriate
  `Source`: the OSC 52 callback uses `Source::UntrustedPty`;
  the Lua plugin glue uses `Source::UntrustedPlugin`. Asserted
  by source-grep that both `clipboardguard::Source::UntrustedPty`
  and `clipboardguard::Source::UntrustedPlugin` appear in the
  codebase (each at a single call site, which is enough).

## CMake wiring

Test target wired in `CMakeLists.txt` via `add_executable` +
`add_test` with `LABELS "features;fast"`,
`target_link_libraries Qt6::Core` (the test only needs `QString`,
not Qt6::Gui — sanitize is pure), `target_compile_definitions`
for `TERMINALWIDGET_CPP` and `MAINWINDOW_CPP` (so the test can
slurp them for INV-6 / 7 / 8).

## How to verify pre-fix code fails

```bash
# Replace <impl-sha> with the commit that lands ANTS-1014.
git checkout <impl-sha>~1 -- src/terminalwidget.cpp src/mainwindow.cpp
cmake --build build --target test_clipboard_redaction
ctest --test-dir build -R clipboard_redaction
# Expect every INV to fail: helper namespace doesn't exist,
# raw setText sites are still in tree.
```
