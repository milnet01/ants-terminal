# Feature: wide and combining characters keep their width under LC_ALL=C

ANTS-3792.

## Problem

`TerminalGrid::handlePrint()` measures each glyph with the system
`wcwidth()`, which answers from the process's LC_CTYPE. Under the C
locale glibc reports a CJK ideograph and a combining mark as
unprintable (-1). The grid then drew a CJK character one cell wide
and gave a combining accent a cell of its own. A terminal started
under `LC_ALL=C`, in a minimal container, or from a systemd unit with
a scrubbed environment got that grid.

## Fix

`TerminalGrid::ensureUtf8CType()` leaves a UTF-8 LC_CTYPE alone.
Otherwise it switches LC_CTYPE to the first UTF-8 locale that loads,
trying `C.UTF-8` first; glibc 2.35 and later build that one in.
`main()` calls it after constructing `QApplication`, because that
constructor resets the locale from the environment.

## Invariants

- **INV-1** — Under `LC_ALL=C`, after `ensureUtf8CType()`, printing
  U+4E00 sets `isWideChar` on its cell and advances the cursor two
  columns.
- **INV-2** — Under `LC_ALL=C`, after `ensureUtf8CType()`, U+0301
  printed after `e` joins that cell as a combining mark and does not
  advance the cursor.
- **INV-3** — `src/main.cpp` calls `TerminalGrid::ensureUtf8CType()`
  after `QApplication app(` is constructed.

## Tests

`test_cjk_width_c_locale.cpp`, in the `test_vt` bundle:
`CjkWidthCLocale.WideCharIsTwoCells` (INV-1),
`CjkWidthCLocale.CombiningMarkJoinsPreviousCell` (INV-2),
`CjkWidthCLocaleSource.MainCallsItAfterQApplication` (INV-3).

## Reload

A bug fix; no reload story is owed.
