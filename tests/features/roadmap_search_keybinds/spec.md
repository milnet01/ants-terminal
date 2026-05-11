# roadmap_search_keybinds — feature contract

Locks ANTS-1234: RoadmapDialog `/`-focus + Esc-clear + body-only
auto-expand of cards on substring match. Full design rationale +
re-open conditions live in
[`docs/specs/ANTS-1234.md`](../../../docs/specs/ANTS-1234.md). This
file restates the invariants the test asserts so the test reads as a
contract rather than a regression-only ratchet.

## Invariants under test

These mirror `docs/specs/ANTS-1234.md` § 4. IDs are qualified
(`ANTS-1234-INV-N`) per the ANTS-1236 § 7 convention — bare `INV-N`
is ambiguous once two specs share a test directory tree.

- **ANTS-1234-INV-1.** Pressing `/` on the dialog (without the
  search box focused) sets focus on `m_searchBox` and selects any
  existing text.
- **ANTS-1234-INV-2.** Pressing `/` while `m_searchBox` has focus
  appends `/` to the predicate (the override does NOT fire).
- **ANTS-1234-INV-3.** Pressing Esc while `m_searchBox` has focus
  clears the predicate and removes focus. The dialog stays open.
- **ANTS-1234-INV-4.** Pressing Esc while `m_searchBox` does NOT
  have focus closes the dialog (existing `QDialog::reject` path,
  unchanged).
- **ANTS-1234-INV-5.** When a non-empty search predicate matches a
  card's body but NOT its id / headline / layman, the card renders
  expanded for this render only — `m_expandedItems` is NOT mutated.
- **ANTS-1234-INV-6.** When a search predicate matches the visible
  summary text (id, headline, or layman), the card renders in its
  user-driven expand state — no auto-expand override.
- **ANTS-1234-INV-7.** An `id:NNNN` predicate that matches a card
  auto-expands that card.
- **ANTS-1234-INV-8.** The auto-expand contract is render-only.
  Tests 6 and 8 assert `m_expandedItems` (or the equivalent
  CardRenderOptions field) is unchanged after the render.
- **ANTS-1234-INV-9.** `kRoadmapShortcuts[]` has exactly 10 rows at
  ship including `{"/", "Focus search box"}`. The lockstep is
  regression-locked by the existing ANTS-1236 cheatsheet test, not
  duplicated here.

## Test shape

Standalone C++ executable in the `test_dialogs` bundle (joins the
existing GoogleTest harness). Drives a real `RoadmapDialog`
instance against a `QTemporaryDir` fixture; calls
`renderCardsHtml` directly for the body-only-match cases (no
dialog needed — the renderer is `static`).
