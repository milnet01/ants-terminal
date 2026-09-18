# Feature: the roadmap dialog's rebuild reads only what moved, and says when it cannot read

## Invariants

**INV-1 — an oversized live roadmap is reported, not cut.**
`RoadmapDialog::loadMarkdown(path, false, &err)` on a live file larger than
the 64 MiB assembled cap returns empty text and sets `err` to a sentence
naming the path.

**INV-2 — an unreadable live roadmap is reported, not blank.** The same call
on a path that does not open returns empty text and sets `err` naming the
path. The dialog shows "Could not read this roadmap." with that sentence.

**INV-3 — the source is read once per change.** Two rebuilds with no input
file changed read the roadmap source once. Changing the live file makes the
next rebuild read it again. Observed through `sourceReadsForTest()`.

**INV-4 — an empty shipped-date answer is kept.** A sibling `CHANGELOG.md`
with no dated release is parsed once across rebuilds, not once per rebuild.
Observed through `shippedDateParsesForTest()`.

**INV-5 — the recent-commit `git log` does not hold the GUI thread.** Right
after the dialog is constructed in a repository, the run is in flight and no
subjects are in hand. After the event loop runs, the fixture commit's subject
is.

**INV-6 — the current-work tint follows the ToolUse colour.**
`src/roadmapdialog.cpp` carries no `rgba(229,194,74` literal, and the card
stylesheet still renders `.rm-cur{background:rgba(229,194,74,0.08);}`.

## Rationale

The ANTS-5088 performance pass found these costs in the roadmap dialog.
`rebuild()` runs per debounced keystroke and per filter toggle, so each one
was paid while typing.

## Test surface

`test_roadmap_dialog_rebuild_cost.cpp`, in the `test_dialogs` bundle. INV-1
uses a sparse file, so nothing large is written. INV-5 builds a one-commit
repository and skips when git is absent.

## Regression history

- **ANTS-5088:** the defects above. Locked by this spec.
