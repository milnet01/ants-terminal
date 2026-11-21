# ANTS-1078 — Screen-reader accessibility for `TerminalWidget` (H9 core slice) — review loop log

Record for [`../specs/ANTS-1078-screen-reader-accessibility.md`](../specs/ANTS-1078-screen-reader-accessibility.md) § Cold-eyes loop log.

## Cold-eyes loop log

**Loop 1 (2026-06-29)** — two independent cold reviewers. Verified
findings fixed: **1 CRITICAL** — §2.5 mischaracterised `m_claudeDetectTimer`
as a trailing-edge debounce ("fires only after output settles");
verified at `terminalwidget.cpp:2430-2443` it is a *leading-edge ~300 ms
throttle* that fires during continuous output, so §2.5 + INV-7 were
rewritten around the real cadence (≤~3.3 events/s ceiling).
**2 HIGH** — `accessibleText() const` called the non-const-only `grid()`
(`terminalwidget.h:102`); rewritten to read `m_grid` directly like
`selectedText()` (H1). Wide/double-width-cell round-trip undefined;
scoped in § 2.6 + INV-6 (H2). **3 MEDIUM** — unnamed test bundle / label
/ grep-lane mechanism (fixed: `test_chrome` bundle, GoogleTest model,
source-reading `TEST()`s); `'\n'`-offset rect undefined (§ 2.6);
empty/all-blank viewport string undefined (§ 2.6 + INV-2);
trailing-space caret offset undefined (§ 2.6 + INV-4); `selection()`
out-param contract unstated (§ 2.3). **LOW** — deferred-work now cites a
real id (ANTS-3363); Qt-version note for the `Terminal` role;
caret-event-vs-`text-changed` substitution noted; ROADMAP cite made
drift-proof. Dismissed (unverified): reviewer-A INFO that the ROADMAP
gate citation was off by 2 lines — it was correct.

**Loop 2 (2026-06-29)** — two fresh cold reviewers; no loop-1 finding
resurfaced (fixes held). Verified findings fixed: **1 CRITICAL** —
`accessibleCaretOffset()` didn't use the existing
`effectiveCursorRow()/effectiveCursorCol()` frozen-vs-live policy
(`terminalwidget.h:514-515`), so the caret would desync from the frozen
viewport text when scrolled back; now pinned to those accessors in
§ 2.2 + INV-4 (with a scrolled-back assertion). **HIGH** — the
scrollback/screen seam in `accessibleText()` left unstated (now: always
`rows` lines, `cellAtGlobal` the single source across the seam);
`SRC_TERMINALACCESSIBLE_PATH` framed inconsistently as new-vs-present
(now marked added-by-this-slice). **MEDIUM** — ctest label was
`features` but the standard + the `test_chrome` bundle use `features;fast`
(`CMakeLists.txt:797`); the no-flood rate ceiling had no test surface
(added INV-9, one-event-per-invocation via a `QAccessible` spy, and
demoted the ~3.3/s figure to rationale); `firstVisibleGlobalLine()`
extract-vs-duplicate ambiguity (now: extracts the `:726` inline
mapping); blank cell/row predicate undefined (now defined in § 2.6 as
codepoint `∈ {0, ' '}`, no combining); `attributes()` could return
`*endOffset` past `characterCount()` on a blank buffer (now clamped).
**LOW** — RAM figure corrected to ~20 KB (UTF-16); INV-7 grep reworded
to token-order (not literal first-statement); § 2.5 reworded to name the
`checkForClaudePermissionPrompt()` timeout slot; honest scoping of the
caret-event limitation for output streaming above a fixed prompt
(manual Orca recipe added to § 6, deferred close to ANTS-3363).

**Loop 3 (2026-06-29)** — two fresh cold reviewers; no loop-1/2 finding
resurfaced. Verified findings fixed: **HIGH** — the caret screen-row→
viewport-offset arithmetic was unstated (now § 2.2:
`caretViewportRow = effectiveCursorRow() + m_scrollOffset`); the
off-viewport caret offset was undefined (now `characterCount()` with its
trigger named); INV-7's grep token was *vacuous* — it matched
`accessibleText(`, which the slot never calls (now matches
`accessibleCaretOffset(` / `QAccessible::updateAccessibility(`); § 4
mis-stated the link surface — `QAccessibleWidget` is QtWidgets (not
QtGui), reached transitively via the vt lib's `Qt6::OpenGLWidgets`
(`CMakeLists.txt:360`), no explicit `Qt6::Widgets` (now corrected).
**MEDIUM** — the `scrollback_frozen_view` precedent is in `test_vt`, not
`test_chrome` (clarified: mechanism reused via `test_chrome`'s own
`SRC_TERMINALWIDGET_PATH`); blank-cell trim is combining-aware and
deliberately differs from `selectedText()`'s naive space-chop (stated);
`accessibleOffsetAt` must not reuse `pixelToCell`'s clamp, which never
returns "no cell" (stated); the flood ceiling rests on the throttle's
own tests, not this slice (noted in § 6). **LOW** — INV-8 now asserts no
string/buffer member (not just `QString`); two off-by-one citations
tightened (`a11y_chrome_names/spec.md:125-127`; `main.cpp:164` is the
ctor line); composition-loop cite `:3600-3618`; non-BMP caret maps to
the cell's first UTF-16 unit.

**Loop 4 (2026-06-29)** — two fresh cold reviewers; no prior-loop finding
resurfaced. Verified findings fixed: **HIGH** — §2.3 sketched
`mapToGlobal(accessibleRectForOffset(o))`, but `QWidget::mapToGlobal`
takes a `QPoint` (the `QRect` overload is Qt-6.7+); now
`QRect(mapToGlobal(r.topLeft()), r.size())`, with the widget→screen
mapping noted as manual-only (no offscreen geometry). §4 mis-filed
`QTextBoundaryFinder` as QtGui — it is **QtCore** (corrected; conclusion
unchanged, still transitively linked). **MEDIUM** — `textAtOffset` &
friends now enumerate *every* `QAccessible::TextBoundaryType`
(`CharBoundary`→Grapheme, Word, Sentence, Line+Paragraph→Line,
`NoBoundary`→whole) rather than an incomplete list missing
`CharBoundary`/`NoBoundary`; INV-6 now pins its buffer to known
single-column ASCII and adds the three `accessibleOffsetAt → -1`
no-cell assertions; §4 now justifies `test_chrome` over the
README-implied `test_vt`. **LOW/INFO** — `text(s, e)` honours the
`e == -1`-means-end Qt convention; `interface_cast` delegates non-Text
types to the base. Also surfaced (code-side, not a spec defect, now
roadmapped): a stale comment at `terminalwidget.cpp:4792` still calls
`m_claudeDetectTimer` a "single-shot trailing-edge" timer — contradicts
the authoritative ANTS-1862 leading-edge-throttle comment at `:2430-2443`
this spec relies on.

**Loop 5 (2026-06-29)** — two fresh cold reviewers; both independently
flagged the **same single substantive item** (the loop-3-introduced
contradiction), and nothing structural/architectural surfaced. Verified
findings fixed: **HIGH** — §2.2 said `accessibleText()` "always yields
exactly `rows` lines" while §2.6/INV-2 drop trailing all-blank lines;
reworded so the walk (all rows) and the trailing-blank trim are
sequential, not both "always." INV-3's `text(s,e) == mid(s,e-s)`
equality is only unconditional in range; now scoped to
`0 <= s <= e <= characterCount()` with the `e == -1`/clamp normalisation
called out separately. **MEDIUM/precision** — INV-9 now also asserts the
emitted event is a `QAccessibleTextCursorEvent` carrying
`accessibleCaretOffset()` (not just an event count); INV-7's source scan
is bounded to the `notifyAccessibilityChanged` function body so it can't
match the `:4792` stale region; INV-8 widened to any string/buffer
member; the `test_vt` compile-def cite corrected `:928`→`:932` (`:928` is
the comment line); §2.5's "`:174` timeout slot" corrected (`:174` is the
`connect`; slot body at `:4589`), and the stale `:4792` comment now cited
as tracked by ANTS-3364. **Dismissed (unverified):** a reviewer also
named `terminalwidget.cpp:179` as a third stale timer comment — that line
is `m_syncTimer.setInterval(500)`, unrelated; ANTS-3364 widened to the
genuine second site `terminalwidget.h:123` only.

**Convergence / sign-off (2026-06-29).** Five loops, ten independent cold
reviews, zero CRITICAL after loop 2 and zero structural/architectural
findings after loop 2. The tail (loops 3–5) found only
internal-consistency, invariant-precision, Qt-API/module accuracy, and
citation items — all fixed. Per the project's polish-convergence rule
(a loop whose only verified findings are polish/precision, nothing
structural/mechanical/architectural, is the sign-off gate), the spec is
**accepted**; ready to implement.
