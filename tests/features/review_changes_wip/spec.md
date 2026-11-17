# Review Changes names the work in progress (ANTS-6806)

## Contract

Review Changes (`diffviewer::show()`, `src/diffviewer.cpp`) shows a
"Work in progress" section above Status. It names the uncommitted work the
way its commit will be named: one `<ID>: <summary>` line per roadmap id the
uncommitted diff adds. The lines come from `reviewwip::lines()`
(`src/reviewwip.h`).

- **INV-1** An id that a CHANGELOG bullet in the diff adds takes that
  bullet's text as its summary: the bold span when there is one, with no
  trailing period.
- **INV-2** An id the diff adds elsewhere, with no CHANGELOG bullet, takes
  its roadmap headline, with no trailing period.
- **INV-3** Only ids the roadmap holds count. A token shaped like an id
  that the roadmap does not hold gives no line.
- **INV-4** Only added lines count. An id in a removed or context line
  gives no line.
- **INV-5** Each id appears once, in the order the diff first adds it.
- **INV-6** In the dialog, the "Work in progress" heading comes before the
  Status heading and carries the lines above. A diff that adds no roadmap
  id shows no such heading.
- **INV-7** `reviewwip::headlines()` reads a roadmap file into id ->
  headline, and returns nothing for a missing file.

## Test

`test_review_changes_wip.cpp`, in the `test_dialogs` bundle. INV-1 to
INV-5 call `reviewwip::lines()` on a written diff. INV-6 and INV-7 build a
real git repository with a `ROADMAP.md` and a `CHANGELOG.md`, edit it, and
read the dialog's text.
