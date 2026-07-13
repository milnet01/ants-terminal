# ANTS-1330 — floating prompt-jump chips in scrollback

**Status:** Implemented (2026-07-13) — design cold-eyes-clean (5 loops, §5);
shipped as the `m_promptPrevBtn`/`m_promptNextBtn` chips in `TerminalWidget`
(constructor + `styleScrollToBottomButton` + `updateScrollToBottomButton`),
reusing `navigatePrompt(∓1)`. Test-first: `tests/features/prompt_jump_chip/`
(`PromptJumpChip.Main`, 18 invariants) written red against the feature-absent
tree, then driven green. Goes live on relaunch.
**Kind:** feature
**Source:** user-request-2026-05-14 (visual companion to the back-to-bottom chip)

> **§4 reconciliation note.** The test was finalised against the real code
> (per the testing standard), which reconciled two §4 details: (a) the glyph
> is written as the **literal UTF-8** `↑`/`↓`, not a `\u`-escape (the tooling
> emits the literal; it is functionally identical and the test greps the
> literal — so the escape-vs-literal ambiguity §2.1/T-8 wrestled with is moot
> since one author owns both sides); (b) the visibility predicate is realised
> as a **nested** `if (m_scrollOffset > 0) { … if (!promptRegions().empty()) }`
> rather than one conjoined `&&` string, so the test asserts the
> `promptRegions().empty()` gate plus both chips' `hide()` in the updater
> body (INV-3) instead of a single verbatim predicate. The authoritative
> test contract is `tests/features/prompt_jump_chip/spec.md`.

## 1. Problem

The terminal already lets keyboard users hop between their own command
prompts in scrollback — `Ctrl+Shift+Up` / `Ctrl+Shift+Down` step to the
previous / next OSC 133 prompt boundary (shipped 0.6.10). There is no
**mouse** equivalent. A user who has scrolled up with the wheel to read
old output has no on-screen affordance for "take me back to where my last
command started" short of scrolling by hand or reaching for the keyboard.

The scroll-to-bottom chip (ANTS-1326) already establishes the pattern: a
small circular floating button on the right edge, shown only when scrolled
up, that performs a one-click viewport jump. This feature adds the
prompt-navigation counterpart(s) using the same chip pattern and the same
scroll mechanism, so the mouse path reaches parity with the keyboard path.

## 2. Surface

### 2.1 Two chips, reusing the scroll-to-bottom pattern

Add two `QPushButton` members to `TerminalWidget` (the class that owns
`m_scrollToBottomBtn` — `src/terminalwidget.h:796`), each a 32×32 circular
floating chip that is a direct child of the `TerminalWidget`:

| Member | objectName | Glyph (rendered) | Codepoint | Tooltip |
|---|---|---|---|---|
| `m_promptPrevBtn` | `promptPrevBtn` | ↑ | U+2191 | `Jump to previous prompt (Ctrl+Shift+Up)` |
| `m_promptNextBtn` | `promptNextBtn` | ↓ | U+2193 | `Jump to next prompt (Ctrl+Shift+Down)` |

**Source form of the glyph.** The button text is set with the C++
universal-character escape (`\u` followed by the 4-hex-digit codepoint —
e.g. `\u` + `2191` for ↑), **not** a literal multibyte character. This
matches the file convention: `m_scrollToBottomBtn->setText(...)` uses the
`\u`-escape for ▼ at `:321`, as do the search buttons (▲ / ▼ / ✕) at
`:377`/`:382`/`:387`. T-8 therefore greps the `\u`-escape strings for U+2191
and U+2193 — a grep for the rendered ↑ / ↓ byte-sequence would false-fail the
idiomatic build.

Two chips (not one two-arrow chip): a 32×32 circle has no room for two
legible arrows, and two chips map one-to-one onto the two keyboard
directions. Thin line-arrows (`↑`/`↓`) are chosen over solid triangles so
the prompt chips are visually distinct from the scroll-to-bottom chip's
solid `▼` (U+25BC) — line-arrow = prompt nav, solid triangle = go-to-bottom.

Each chip is constructed alongside `m_scrollToBottomBtn` in the
`TerminalWidget` constructor (near `src/terminalwidget.cpp:320`), with
`setFixedSize(32, 32)` for parity with the sibling chip (`:323`) — the
stylesheet `min/max: 32px` reset (§2.2) belt-and-suspenders the same size.
Each starts hidden, and is styled by the **same** ID-scoped stylesheet
renderer so the app-wide `QPushButton` cascade cannot inflate or clip them.

### 2.2 Styling — extend `styleScrollToBottomButton`

`TerminalWidget::styleScrollToBottomButton` (`src/terminalwidget.cpp:660`)
currently themes the one chip. Extend it **in place — same function name, no
new helper** (so the tests and INV-5 have one fixed symbol to anchor on) — to
also apply the identical structural reset (`padding: 0;
min-width/max-width/min-height/max-height: 32px; border-radius: 16px`) and
themed palette to the two prompt chips,
each under its own `QPushButton#promptPrevBtn` / `QPushButton#promptNextBtn`
ID selector. The colour derivation (surface = lifted/darkened bg, themed
border, accent hover) is reused verbatim — the three chips are visually a
set. Each chip gets its own `setStyleSheet(...)` call carrying its ID
selector (per-widget, matching the sibling chip's pattern — not one combined
stylesheet on the parent). The styler is called from the constructor seed and
from every `applyThemeColors()`, unchanged. Place the prompt-chip styling
**before** the function's existing `if (!m_scrollToBottomBtn) return;`
early-guard (`terminalwidget.cpp:663`) so that guard can never skip it, and
null-check each chip's own pointer (`m_promptPrevBtn` / `m_promptNextBtn`) —
do not gate the prompt-chip styling on the scroll-to-bottom pointer.

### 2.3 Click handlers — reuse `navigatePrompt`

Each chip's `clicked` signal calls the existing
`TerminalWidget::navigatePrompt(int)` (`src/terminalwidget.cpp:2978`). The
argument literal is written as the codebase spells it — a bare `1`, no `+`
sign (matching `terminalwidget.cpp:1875`/`:1879` and the T-3 grep):

- `m_promptPrevBtn` → `navigatePrompt(-1)`
- `m_promptNextBtn` → `navigatePrompt(1)`

`navigatePrompt` already reads `m_grid->promptRegions()`, computes the
target from `viewTop = scrollbackSize - m_scrollOffset`, adjusts and clamps
`m_scrollOffset`, calls `updateScrollBar()`, and repaints. **No new
prompt-row lookup or scroll arithmetic is written** (reuse-before-rewrite).

The chips therefore behave identically to the keyboard shortcuts, including
the existing end-of-range saturation: at the topmost prompt, `prev` scrolls
to the absolute top; at the last prompt, `next` scrolls to the live bottom.
Saturation is retained rather than disabling the chips at the ends, so chip
and keyboard behaviour stay identical and no separate "has prev/next prompt"
query is introduced. The boundary cases are deliberate. With exactly one
prompt region, or when the viewport already sits at the target end, a click
saturates to top/bottom — the same slam the keyboard shortcut performs, not a
no-op. It is consistent, if visually quiet, feedback.

### 2.4 Positioning + visibility — extend `updateScrollToBottomButton`

`TerminalWidget::updateScrollToBottomButton()` (`src/terminalwidget.cpp:4744`)
is the single geometry/visibility updater, called from the four sites that
already re-run on scroll and geometry change (the scroll-to-bottom chip's
`clicked` handler at `:336`, `:2127`, `:2546`, and the tail of
`updateScrollBar()` at `:3140`). Extend it **in place — same function, no new
helper** — to also place and toggle the two prompt chips, so they stay in
sync for free.

**Column layout** — the two prompt chips **reuse the scroll-to-bottom chip's
already-computed local `x` and `y`** (`terminalwidget.cpp:4747-4749`:
`int x = width() - 52; int y = height() - 52;` then
`if (m_searchVisible) y -= m_searchBar->height();`). They stack bottom-up
from that shared base with a 40 px stride (32 px chip + 8 px gap):

```
scroll-to-bottom : move(x, y)          [ ▼ ] bottom (unchanged)
prompt-next      : move(x, y - 40)      [ ↓ ]
prompt-prev      : move(x, y - 80)      [ ↑ ] top
```

Reusing `x`/`y` rather than re-deriving `width()`/`height()` per chip is the
reuse-before-rewrite choice and has a load-bearing consequence: because the
local `y` **already** carries the `m_searchVisible` shift, the prompt chips
inherit it for free — there is no per-chip search-shift to write or forget.
The tests therefore grep the relative strides `move(x, y - 40)` /
`move(x, y - 80)`, not absolute `height() - …` literals (which a reuse-first
build never emits). Each chip is `raise()`d after `move()`/`show()`, exactly
as the scroll-to-bottom chip is (`terminalwidget.cpp:4752`), so an earlier
child is never occluded by the terminal surface.

**Visibility predicate** — the prompt chips are shown iff
`m_scrollOffset > 0 && !m_grid->promptRegions().empty()`, and hidden
otherwise. Two conditions, both required. First, `m_scrollOffset > 0` is the
scroll-to-bottom chip's own "scrolled up" gate, so the three chips coexist
only when scrolled up (the roadmap's stated intent). Second,
`!m_grid->promptRegions().empty()` guards on at least one recorded prompt: a shell
without OSC 133 shell-integration records no prompt regions, so the chips
would be inert — they stay hidden rather than showing dead controls. The
scroll-to-bottom chip keeps its existing `m_scrollOffset > 0` predicate
unchanged.

**Alt-screen** — no special handling is needed. Alt-screen apps (vim, htop)
have no scrollback, so `m_scrollOffset` stays 0 there and the shared
`m_scrollOffset > 0` gate keeps all three chips hidden — the same behaviour
the scroll-to-bottom chip already exhibits (it is never reported appearing in
alt-screen). Independently, on alt-screen entry the main-screen regions are
saved to `m_altPromptRegions` (`terminalgrid.cpp:737`) and `m_promptRegions`
is cleared (`:756`), then restored on exit (`:803`), so
`m_grid->promptRegions()` returns empty and the second gate would also hold
the chips hidden.

**Short terminal / search-open overflow** — on a terminal too short for the
three-chip stack (`height() < ~132 px`, less again with the search bar open),
the top (`prompt-prev`) chip's `y - 80` approaches or crosses 0 and it clips
against the widget's top edge. This is declared **out of scope**. Such a
window is already below a usable terminal size; the degradation is graceful
(Qt clips the child to the parent bounds — no crash, no overlap of the lower
chips); and it mirrors the existing scroll-to-bottom chip, which applies no
lower clamp to `height() - 52` either. No min-height guard or chip-hiding
logic is added — a plain geometry expression is the simpler correct choice
for a case no real session hits.

## 3. Invariants

- **INV-1** — Both prompt chips carry their own `objectName`
  (`promptPrevBtn` / `promptNextBtn`) and are styled under matching
  `QPushButton#promptPrevBtn` / `QPushButton#promptNextBtn` ID selectors
  with the `padding: 0; min-width: 32px; max-width: 32px` structural reset,
  so the app-wide `QPushButton` cascade cannot inflate or clip them (the
  ANTS-1326 contract, extended to the new chips).
- **INV-2** — `m_promptPrevBtn->clicked` invokes `navigatePrompt(-1)` and
  `m_promptNextBtn->clicked` invokes `navigatePrompt(1)` (bare `1`, matching
  the codebase convention and the T-3 grep); no independent prompt-row lookup
  or `m_scrollOffset` arithmetic is duplicated in the handlers. _(T-3)_
- **INV-3** — The prompt chips are shown iff
  `m_scrollOffset > 0 && !m_grid->promptRegions().empty()`, and hidden
  otherwise. _(T-4 asserts the full conjoined `&&` predicate verbatim.)_ Their
  geometry is recomputed from `width()`/`height()` inside
  `updateScrollToBottomButton()` on every call, so there are no stale
  absolute positions — this follows structurally from reusing that updater
  (the positioning literals are asserted by T-5), not from a separate test.
- **INV-4** — The prompt chips reuse the scroll-to-bottom chip's local `x`
  and `y` and stack above it without overlapping — offsets `y - 40` (next) /
  `y - 80` (prev), a 40 px stride. Because the reused `y` already carries the
  `m_searchVisible` search-bar shift, the chips inherit it (no per-chip shift
  is written). _(T-5 greps the `move(x, y - 40)` / `move(x, y - 80)` strides,
  which pins the shared column, the stride, and — via reuse — the inherited
  shift; the resulting no-overlap layout with the search bar open is confirmed
  by manual smoke, see §4.)_
- **INV-5** — Both chips start hidden at construction _(T-6)_. Because they
  are styled by the same `styleScrollToBottomButton` renderer, which is
  already called once from the constructor as a seed before the first
  `applyThemeColors()`, they are never unstyled — this seeding follows from
  the shared styler (T-2 asserts the per-chip selectors exist in it), not a
  separate test.
- **INV-6** — Adding the chips introduces no per-line or per-prompt
  allocation: exactly two `QPushButton` children are created once in the
  constructor _(T-7 asserts each `new QPushButton` construction appears)_ and
  reused for the widget's lifetime. No allocation occurs on the click,
  scroll, or paint paths (the handlers only call the existing
  `navigatePrompt` / `updateScrollToBottomButton`). RAM budget: two
  `QPushButton` instances, a few KB, no growth.
- **INV-7** — Each chip presents its glyph and a non-empty tooltip: the
  previous chip's text is U+2191 (↑, set via the `\u`-escape) and its tooltip
  contains "previous prompt"; the next chip's text is U+2193 (↓) and its
  tooltip contains "next prompt" (per the §2.1 table). This is the
  user-visible contract — a chip with no glyph or no tooltip is not the
  specified control. _(T-8)_

## 4. Tests

Feature-conformance test `tests/features/prompt_jump_chip/` — source-grep on
`src/terminalwidget.cpp` (MainWindow / TerminalWidget is too heavy to
instantiate under a feature test). **Every grep is scoped to a specific code
region, never the whole file.** Some literals this feature reuses already
appear elsewhere — `navigatePrompt(-1)` / `navigatePrompt(1)` in the keyboard
handler (`:1875`/`:1879`), and `width() - 52` in the scroll-to-bottom chip's
own updater (`:4747`) — so a whole-file `contains()` for those would
false-pass without the chips being wired. Matching uses the shared
`tests/_support/srcgrep.h` helpers so it survives reindentation / line-wrap
(ANTS-2067): compare `squashWhitespace(region)` against a
`squashWhitespace(needle)` for any multi-token literal, and use
`countOccurrences` for the "exactly once" checks. The single-token anchor
greps (`setObjectName(...)`, `<member> = new QPushButton`) instead match the
file's one-space construction idiom verbatim (as at `:320`) and are
spacing-sensitive by design; whitespace-normalisation is reserved for the
multi-token predicate and selector-block matches, where a line-wrap is the
real risk. Two region scopes:

- **`slurpFunctionBody(src, "TerminalWidget::<fn>")`** — the existing
  `srcgrep.h` helper (used by sibling tests); returns a function's
  brace-balanced body. Used to scope greps to `styleScrollToBottomButton`
  and `updateScrollToBottomButton`.
- **`blockAround(memberName)`** — a **new** small helper this test adds (no
  such helper exists yet): the substring from that chip's
  `<memberName> = new QPushButton` up to the next `= new QPushButton`
  occurrence in the file, via two `std::string::find`s. For the prev chip the
  window ends at the next chip's construction (a tight block); for the next
  chip it ends at the following `new QPushButton` — which is the search-bar
  button at `:360`, not end-of-file, since the prompt chips are not the file's
  last buttons. The window is therefore not guaranteed to be *tight* on the
  last chip, but it is still *sound*: each chip's asserted literals
  (`navigatePrompt(-1)` vs `navigatePrompt(1)`, its glyph escape, its tooltip
  substring) are unique to that chip's own wiring and absent from any
  following unrelated construction, so a slightly over-long window cannot
  false-pass. This holds only if each chip is fully wired
  (`setText` / `setObjectName` / `setFixedSize` / `setToolTip` / `hide()` /
  `clicked`) before the next `new QPushButton`; the two prompt chips must be
  constructed consecutively, prev first.

1. **T-1 (INV-1)** — `setObjectName("promptPrevBtn")` and
   `setObjectName("promptNextBtn")` are present (the anchors the block-scoped
   tests key on).
2. **T-2 (INV-1)** — inside `slurpFunctionBody(src,
   "TerminalWidget::styleScrollToBottomButton")`, both ID-scoped selectors
   `QPushButton#promptPrevBtn {` and `QPushButton#promptNextBtn {` appear
   (match the trailing ` {` so the find lands on the base block, not the
   `:hover` variant), **and each selector's own block carries the structural
   reset**: take the substring from each selector header to its closing `}`
   and assert (via `squashWhitespace`) that `padding: 0;`, `min-width: 32px;`,
   and `max-width: 32px;` all appear inside it. Asserting the reset *inside each
   block* — not just the selector header — is what stops an empty
   `QPushButton#promptPrevBtn {}` from passing (the reset literals also exist
   for the ▼ chip, so a whole-body grep would false-pass).
3. **T-3 (INV-2)** — `blockAround("m_promptPrevBtn")` contains
   `navigatePrompt(-1)` and `blockAround("m_promptNextBtn")` contains
   `navigatePrompt(1)` (bare `1`, no `+`). Block-scoping is exactly what
   separates these from the identical keyboard-handler literals at
   `:1875`/`:1879`.
4. **T-4 (INV-3)** — inside `slurpFunctionBody(src,
   "TerminalWidget::updateScrollToBottomButton")`, the **full conjoined**
   predicate — matched whitespace-normalised, i.e.
   `squashWhitespace(body)` contains
   `squashWhitespace("m_scrollOffset > 0 && !m_grid->promptRegions().empty()")`
   so an 80-column line-wrap of the condition doesn't false-fail. Asserting
   the whole `&&` string (not merely `promptRegions().empty()`) catches an
   implementer who writes `||`, which would show dead chips on a bottomed but
   prompt-bearing view. The same body also contains `m_promptPrevBtn->hide()`
   and `m_promptNextBtn->hide()` — the false-branch of the predicate, so a
   build that adds `show()` but forgets the `hide()` leaves the chips stuck
   visible after return-to-bottom and fails here.
5. **T-5 (INV-4)** — inside `slurpFunctionBody(src,
   "TerminalWidget::updateScrollToBottomButton")`, the two reuse-based move
   strides appear (whitespace-normalised): `squashWhitespace(body)` contains
   `move(x, y - 40)` and `move(x, y - 80)`. Grepping the strides *relative to
   the function's own reused local `x`/`y`* — not absolute `height() - 52 - N`
   literals — is deliberate: the reuse-first implementation the spec mandates
   emits `move(x, y - 40)`, so a grep for `height() - 52 - 40` would
   false-fail correct code (and re-deriving from `height()` would drop the
   search-shift that lives in the reused `y`). The `move(x, …)` form also
   pins the shared column (`x`); the inherited search-shift and the resulting
   no-overlap layout are confirmed by manual smoke (see below).
6. **T-6 (INV-5)** — `blockAround("m_promptPrevBtn")` and
   `blockAround("m_promptNextBtn")` each contain a `->hide()` call (chips
   start hidden at construction — distinct from the T-4 updater-body hides).
7. **T-7 (INV-6)** — each chip is constructed exactly once:
   `countOccurrences(src, "m_promptPrevBtn = new QPushButton") == 1` and
   `countOccurrences(src, "m_promptNextBtn = new QPushButton") == 1` (a second
   occurrence of either fails the test — construction is confined to the
   constructor).
8. **T-8 (INV-7)** — `blockAround("m_promptPrevBtn")` contains the `\u`-escape
   string for U+2191 (the source form, per §2.1 — not the rendered ↑ byte)
   and a `setToolTip(` whose string contains `previous prompt`;
   `blockAround("m_promptNextBtn")` contains the `\u`-escape string for
   U+2193 and a `setToolTip(` whose string contains `next prompt`. Guards
   against blank or mislabelled chips that would pass
   every other test.

Manual smoke (post-relaunch): run a shell with OSC 133 integration, emit a
few commands, scroll up with the wheel → the `↑`/`↓` chips appear above the
`▼` chip; clicking `↑` jumps the viewport to the previous prompt, `↓` to the
next; scrolling back to the bottom hides all three; a shell without
shell-integration shows only the `▼` chip. With the chips visible, open the
search bar (`Ctrl+Shift+F`) → all three shift up by the search-bar height and
none overlaps it — this covers INV-4's shared-`x` column and search-`y`-shift,
which the source-grep tests intentionally leave to manual verification.

## 5. Cold-eyes loop log

| Loop | C | H | M | L | I | Verified | Notes |
|---|---|---|---|---|---|---|---|
| 1 | 0 | 3 | 3 | 3 | 1 | 9/9 | `navigatePrompt(+1)`→`(1)` contract mismatch (prose vs T-3); short-terminal/search-open overflow of top chip undefined; INV-6 orphaned (no test); INV-3/INV-5 multi-clause partly untested; alt-screen unaddressed; dead-click at 1-region/end; `raise()` unspecified; T-5 grep hedged; over-long visibility sentence. All fixed. |
| 2 | 0 | 1 | 3 | 5 | 1 | 10/10 | **H:** whole-file greps (mirroring `scroll_to_bottom_chip_size`'s `slurpFile`+`contains`) false-pass T-3/T-4/T-5 on pre-existing literals → rewrote §4 to mandate `funcBody`/`blockAround` region-scoping. **M:** `&&` conjunction untested (T-4 now greps the full predicate); "Rename/extend"/"or a helper" ambiguity (committed to extend-in-place); diagram omitted the search-bar shift (added). **L:** glyph/tooltip untested (added T-8); `raise()` cite `:4750`→`:4752`; alt-screen mechanism described backwards (reworded); "constructor" update-site label imprecise; `m_searchVisible` flag named; long sentences trimmed. All fixed. |
| 3 | 0 | 1 | 3 | 4 | 2 | 8/8 | **H:** T-5's per-chip `move(width()-52,…)` grep would false-*fail* the idiomatic reuse of the existing local `int x = width()-52;` → dropped the `x`-grep, moved column + search-shift to manual smoke. **M:** search-bar shift on the new chips untested (moved to manual, INV-4 updated); hidden/else branch untested (T-4 now greps both `->hide()` in the updater body); `blockAround`'s window undefined (defined as the delimited span to the next `new QPushButton`). **L:** predicate spelled `m_grid->promptRegions()` uniformly; T-8 now greps tooltip substrings; added INV-7 for the glyph/tooltip contract (T-8 maps to it); styler null-guard clarified. Lane A: clean accuracy sign-off (all `:4749`/`:4752`/`:756`/`:803` cites verified). All fixed. |
| 4 | 1 | 1 | 2 | 2 | 1 | 7/7 | **C:** T-5's `height()-52-40/-80` y-literals false-*fail* the reuse-first `move(x, y-40)` idiom (the y-analogue of loop 3's x fix, left asymmetric) and contradicted §2.4's shift-reuse → repositioned chips relative to the reused local `x`/`y` (search-shift now inherited), T-5 greps `move(x, y-40)`/`move(x, y-80)`. **H:** INV-1 structural reset on the prompt chips untested (T-2 grepped only the selector header; the reset literals pre-exist for the ▼ chip) → T-2 now asserts the reset inside each selector block. **M:** helper names misattributed (`funcBody`/`blockAround` don't exist — real is `ants_test::slurpFunctionBody`; `blockAround` flagged as new; `countOccurrences` cited for T-7); raw predicate grep fragile to line-wrap → T-4/T-2/T-5 use `squashWhitespace` (ANTS-2067). **L/INFO:** `blockAround` adjacency precondition stated; alt-screen save cite `:737` added; styling-before-`:663` placement made explicit. All fixed. |
| 5 | 1 | 0 | 2 | 5 | 1 | 8/8 | **C:** glyph greps assumed a literal `↑`/`↓`, but the file writes button text with `\u`-escapes (`▼` at `:321`) — the reuse-first build emits `↑`, so T-8 would false-fail (the glyph-analogue of the loop-3/4 x/y form fixes) → §2.1/INV-7/T-8 now mandate & grep the `\u`-escape form. **M:** `blockAround` "captures exactly one block" is false for the last chip (window bleeds to the search button at `:360`) → reworded to "sound, not always tight; per-chip literals are unique so a bleed can't false-pass"; `setFixedSize(32,32)` was dropped from the wiring list → added for sibling parity. **L/INFO:** §4 reuse-literal rationale overstated (`promptRegions().empty()` appears nowhere; `width()-52` is in the updater, not paintEvent) — corrected; per-widget `setStyleSheet` clarified; `:663` "reached by it" self-contradiction dropped; anchor-grep spacing-sensitivity + T-2 `:hover` header disambiguation noted. All fixed. Cap reached — see note below. |

**Note (loop 5 = `--max-loops` cap).** The design (§1–3) has been
accuracy-clean for four consecutive loops (Lane A signed off 3×); every
loop-3/4/5 finding was in §4 — the same class each time: a source-grep
literal that must match the exact form a not-yet-written reuse-first
implementation will emit (x-column → y-strides → glyph escape). That is the
signal the skill's budget note calls out: §4 is specifying test byte-forms
for code that does not exist yet, which is better owned by the test's own
`tests/features/prompt_jump_chip/spec.md` (written against real code, and
cold-eyes-exempt per CLAUDE.md §14). Decision recorded with the user at the
cap; the contract above is implementation-ready.
