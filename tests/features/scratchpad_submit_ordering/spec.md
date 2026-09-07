# scratchpad_submit_ordering — ANTS-4456

Folded out of ANTS-4456 (cold-sweep 2026-08-18, lane render-pty), where
it was carried as an UNVERIFIED HIGH claim. Verified against source
before this contract was written.

## The defect

`TerminalWidget::sendScratchpad` wrote the trailing Enter immediately
after calling `pasteToTerminal`, as a separate statement.

`pasteToTerminal` has two arms. When the payload trips no risk check it
pastes synchronously, and the following Enter is correctly ordered. When
the payload IS risky it builds the confirmation dialog and returns at
once — the paste happens later, in the Paste button's handler.

On that second arm the Enter went to the shell first, on its own,
executing whatever the user had already typed at the prompt. The
scratchpad text then arrived with no Enter and sat unsubmitted. Pressing
Cancel did not help: the Enter had already been sent.

A newline is itself a risk reason and the confirmation defaults on, so
the scratchpad's own purpose — composing a multi-line command — took the
broken arm. Single-line sends took the synchronous arm and worked, which
is why this survived.

## The rule

The Enter belongs to the accepted paste, not to the call that requested
it. A caller cannot sequence it, because the risky arm is asynchronous.
`pasteToTerminal` therefore takes the submit-after choice as a parameter
and sends the Enter itself, on whichever arm actually pastes. Every other
caller keeps the default and is unaffected.

## Invariants checked

- **INV-1.** `sendScratchpad` no longer writes the Enter itself.
- **INV-2.** `sendScratchpad` asks `pasteToTerminal` to submit.
- **INV-3.** The dialog's accept handler sends the Enter, so a confirmed
  paste still submits — and a cancelled one cannot, since the only write
  is on the accept path.
- **INV-4.** The synchronous arm sends it too, so an unrisky scratchpad
  send keeps working.

## Why this is a source-grep test

Driving the real path needs a live PTY and a click on a modeless dialog,
which the unit harness has neither of. The sibling `paste_dialog_custom`
covers the same function the same way, and this contract follows it. The
invariants are about which code path issues the write, which is visible
in the source.
