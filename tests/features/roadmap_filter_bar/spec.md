# roadmap_filter_bar — the collapsed filter chrome

Contract for `tests/features/roadmap_filter_bar/test_roadmap_filter_bar.cpp`.
Owning roadmap item: **ANTS-4412** (user request, 2026-08-15 — *"the top filter
bar is extremely busy"*). Companion to ANTS-3762, which aligned the card rows
on a column grid; this one is the chrome above them.

Before: a tab strip, a full-width search field, a row of five status
checkboxes with the density combo stranded at its right edge, and a second row
of eleven kind checkboxes. Sixteen checkboxes across two rows, ~150 px of
vertical chrome, before one roadmap item is visible.

After: the search row keeps the search field and gains the density combo (a
view preference, not a filter), and one short row carries three controls —
`Status: all ▾`, `Kind: all ▾`, `Reset filters`.

**The checkboxes are the same widgets, re-parented into the buttons' popup
menus.** No signal, objectName, accessibleName or persistence path changed, so
every rule the existing `roadmap_density` and `roadmap_kind_facets` tests lock
still holds and `findChild` still reaches every box. That is what makes this a
chrome change rather than a rewrite, and the first row below is what proves it.

## What each row locks

| Row | Claim |
|---|---|
| `CheckboxesSurviveTheReparent` | All five status boxes and all twelve kind boxes are still reachable by their documented objectNames, still carry their accessibleNames, and still hold their pre-existing default states (status all on, kind all off). |
| `SummariesTrackTheCheckboxes` | The buttons say `Status: all` / `Kind: all` at rest; unchecking a status yields `Status: 4 of 5`; checking two kinds yields `Kind: 2 of 12`. The status set counts DOWN from all-on and the kind set counts UP from empty, so a summary derived from the other's shape would be wrong. |
| `ResetIsEnabledExactlyWhenNarrowed` | The reset button is disabled at rest and enabled by a status uncheck, by a kind check, and — separately — by search text alone. Search counts because it narrows as hard as any checkbox and reset clears it. |
| `ResetRestoresEveryControl` | Clicking reset re-checks every status, unchecks every kind, clears the search box, and leaves the button disabled again. |
| `ControlsAreKeyboardReachable` | The three buttons take strong focus and the two filter buttons carry a menu, so the collapse costs no keyboard access. |

## What is NOT locked

Pixel geometry, and the height actually saved. Both are properties of the
running window rather than of the widget tree, and asserting a layout's
rendered size pins the theme as much as the change. The claim under test is
that the controls behave, not that they measure.
