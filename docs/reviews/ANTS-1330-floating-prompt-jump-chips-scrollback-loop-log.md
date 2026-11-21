# ANTS-1330 — floating prompt-jump chips in scrollback — review loop log

Record for [`../specs/ANTS-1330-floating-prompt-jump-chips-scrollback.md`](../specs/ANTS-1330-floating-prompt-jump-chips-scrollback.md) § 5. Cold-eyes loop log.

## Cold-eyes loop log

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
