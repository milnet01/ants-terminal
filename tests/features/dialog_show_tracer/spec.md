# Feature: the dialog-show trace line is one honest line

`DialogShowTracer` (`ANTS_TRACE_DIALOGS=1`, or Tools → Debug Mode) logs one
line per top-level window Show. A window title can come from a shell's OSC
title, so it is text the user did not write.

## Invariants

**INV-1 — a title's `%N` is kept as written.** A window titled `a%1b%9c`
appears in its line as `a%1b%9c`; no later value is substituted into it.

**INV-2 — one Show is one line.** A control character in a title (a newline,
a carriage return, ESC) is written as `\xNN`, so the line holds no raw
control character and cannot fake a second log line.

## Test surface

`test_dialog_show_tracer.cpp` builds a `QWidget`, sets its title and calls
`DialogShowTracer::showLine`, the formatter the event filter logs.

## Regression history

- **ANTS-6633:** the title was substituted before the later `.arg` calls,
  which filled its `%N`, and control characters passed through raw.
  Locked by INV-1 and INV-2.
