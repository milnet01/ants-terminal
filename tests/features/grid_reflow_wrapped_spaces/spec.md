# Feature: re-wrapping keeps the spaces inside a wrapped line

## Invariants

**INV-1 — a space at the end of a soft-wrapped row survives a resize.** On a
10-column grid, `abcdefghi jklm` fills row 0 with `abcdefghi ` and wraps `jklm`
onto row 1. After `resize` to 20 columns, row 0 reads `abcdefghi jklm`: the
space stays between `i` and `j`.

**INV-2 — trailing blanks at the end of a logical line are still dropped.** On
a 10-column grid, `ab` then CR LF then `cd`. After `resize` to 5 columns,
row 1 starts with `c`: the blank tail of row 0 does not wrap onto a new row.

## Rationale

`TerminalGrid::resize` joins soft-wrapped rows into logical lines
(`joinLogical`) and re-wraps them. It trimmed trailing spaces from every row
before joining, so a space that happened to fall in a wrapped row's last
column was lost and the words either side were glued together. Trimming now
applies only to the row that ends a logical line.

## Test surface

`test_grid_reflow_wrapped_spaces.cpp` feeds byte strings through `VtParser`
into a `TerminalGrid`, calls `resize`, and reads cells with `cellAt`. No GUI.

## Regression history

- **ANTS-5076:** `joinLogical` trimmed trailing spaces from wrapped rows, so
  re-wrapping joined words. Locked by this spec.
