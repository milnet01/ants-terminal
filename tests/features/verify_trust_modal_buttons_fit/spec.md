# The verify.json trust dialog never clips a button label (ANTS-5479)

The "Trust .ants/verify.json?" dialog cut off "Show Details...", "Trust
this SHA" and "Trust this repo" (user screenshot 2026-09-27).
`QMessageBox` caps its own width and squeezed its five buttons to one
width below what their labels need.

## Invariants

- **INV-1** — under the app's themed stylesheet, every visible button in
  the dialog `VerifyTrust::buildPromptBox` fills is at least as wide as
  its own size hint, so no label is cut off.

*Test:* `test_verify_trust_modal_buttons_fit.cpp` builds the real dialog,
shows it off-screen and compares each button's width with its size hint.

INV-1 failed only on Qt 6.4 (CI's ubuntu-24.04). There `QMessageBox`
re-files "Show Details..." in `showEvent`, and the stylesheet's
`QDialogButtonBox QPushButton { min-width }` resets that button's minimum.
Reproduce it in `tools/qt62-guard.sh --job build-test`'s container.
