# Feature: terminal output cannot flood trigger actions or command_finished

## Problem

Program output drives two kinds of event out of `TerminalWidget`, and
neither had a rate limit.

- Every OSC 133 D marker calls the grid's command-finished callback, which
  emits `commandFinished`. `MainWindow::connectTerminal` turns each one into
  a `command_finished` plugin event posted to every plugin worker.
- Every line a trigger rule matches emits `triggerFired` or
  `triggerRunScript`. `MainWindow::onTriggerFired` turns a `command` rule
  into a detached `/bin/sh`, a `notify` rule into a D-Bus notification, and
  a `run_script` rule into a `palette_action` plugin event.

So `cat`-ing a file full of markers, or a matching log line in a tight loop,
starts one shell, one notification or one plugin call per sequence, with no
bound.

## Surface

`TerminalWidget` keeps two per-terminal budgets, each a one-second window:

- one for `commandFinished`;
- one shared by every trigger dispatch — `triggerFired` and
  `triggerRunScript`, from instant and per-line rules alike.

When a budget is spent for the current second, further events of that kind
are dropped silently until the window advances. The two budgets are
independent. Grid-mutation triggers (`highlight_line`, `highlight_text`,
`make_hyperlink`) emit no signal and are not limited.

## Invariants

- **INV-1** — A per-line trigger flood is capped. Feeding several hundred
  completed lines that match a `notify` rule emits `triggerFired` at least
  once and at most 100 times.
- **INV-2** — A `run_script` flood is capped the same way:
  `triggerRunScript` fires at least once and at most 100 times.
- **INV-3** — Trigger kinds share one budget. Alternating lines matching a
  `notify` rule and a `run_script` rule deliver at most 100 signals in
  total.
- **INV-4** — A command-finished flood is capped. Feeding several hundred
  OSC 133 D markers emits `commandFinished` at least once and at most 100
  times.
- **INV-5** — The budget refills. After a flood spends it, waiting past the
  one-second window lets a matching line emit `triggerFired` again.
- **INV-6** — Ordinary use is untouched. Three matching lines emit
  `triggerFired` three times.

## Test surface

`test_trigger_event_rate_limit.cpp` constructs a `TerminalWidget` and feeds
bytes through a `VtParser` into `grid()->processAction`, as
`tests/features/trigger_line_dispatch` does. Instant rules run in the private
`checkTriggers`, which the test cannot reach; they take the same budget
through the same helper as per-line rules, and that is checked by reading
the code, not by this test.

## Out of scope

- The exact budget. The invariants bound it from above by a generous number,
  so a retune is not a regression.
- What `MainWindow` does with each signal.

## Regression history

- **ANTS-5079:** `command_finished` and `palette_action` plugin events had no
  rate limit, and terminal output drives both. The same path also started
  `command` and `notify` trigger actions without limit. Fixed by the budgets
  above.
