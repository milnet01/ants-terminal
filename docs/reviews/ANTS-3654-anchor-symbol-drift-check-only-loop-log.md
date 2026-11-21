# ANTS-3654 — anchor-symbol drift check and the `only:"stale"` filter — review loop log

Record for [`../specs/ANTS-3654-anchor-symbol-drift-check-only.md`](../specs/ANTS-3654-anchor-symbol-drift-check-only.md) § Cold-eyes loop log.

## Cold-eyes loop log

**Loop 1 (2026-07-27)** — CRITICAL 1 · HIGH 2 · MEDIUM 8 · LOW 6 · INFO 2
(17 verified, 0 unverified). Three lanes cold-briefed — contract/testability,
doc-vs-code accuracy, cross-doc/structure. Two returned clean; one escalated to
a full depth read. Every finding was verified against live source before it was
treated as real, and every actionable severity fixed:

- **CRITICAL** — § 6 widened the target read without saying the *response* must
  not widen with it. `src/doccitations.cpp:666-672` appends every line the
  reader returns to `text`, while `range_truncated` is derived independently at
  `:674` — so a naive implementation regresses the parent's response contract on
  every anchored citation and the truncation flag stays correct, hiding it. § 2
  now pins `text` / `range_truncated` / `text_clipped` / `end_clamped` to
  ANTS-3636's semantics, and INV-13's `:1-10` row asserts the 3-line `text`
  beside the verdict.
- **HIGH** — "the full resolved range" overlooked the per-line byte clip:
  `TargetReader` hands back lines already through
  `clipToBytes(…, maxTextBytes, …)` (`:372`, `:401`), so a needle past byte 2000
  of a long line is invisible → a false stale, the direction § 2 swears off.
  Now states the search sees lines as the reader hands them over, and carries
  the clip as the third false-positive shape rather than pretending it away.
- **HIGH** — the anchor regex admits a lone colon (`a:b`) but the needle was
  defined only for `::`, so two implementers would emit different JSON. Needle
  is now "the text after the last `::`", with the no-`::` and single-colon cases
  spelled out and fixtured.
- **MEDIUM ×8** — § 2 never said whether `anchor_symbol` carries the span or the
  derived needle (now: the span, verbatim). § 1 called `only:"stale"` a new
  request value when it already ships (`src/doccitations.h:28`, filtered at
  `src/doccitations.cpp:888-897`) — reworded to "widens an existing", with the
  changed line named in
  § 5. § 1's "never restates a number" pledge was broken 145 lines later by a
  parenthetical `(= 40)` — dropped. The `maxAnchorGap` boundary was pinned on
  one side only, passing under both `<` and `<=` — paired row added. INV-13's
  fixture had no non-`ok` row, leaving half its own "both fields absent" claim
  untested — added. The EOF-clamp and CRLF rules had no fixture at all — added.
  § 4 named neither the test label nor the must-fail-first requirement
  `specs.md` § 3.6 mandates — both added.
- **LOW ×6** — span content matched verbatim, no CommonMark one-space strip
  (`src/markdownscan.h:109-111`), stated and fixtured. Gap formula written out,
  its unit named as a UTF-16 column index rather than "characters".
  `anchor_missing` glossed — it counts a missing *needle*, not a missing anchor
  — and its `0` marked as today's value. INV-14 now asserts `len(unparsed[])`
  separately from `counts.unparsed`, since the count alone would pass an
  implementation that filtered the array. § 4 names `test_core`
  (`CMakeLists.txt:1984`).
- **INFO ×2, not actioned** — the unquantified false-positive rate is
  deliberate and labelled as such. Three benign edges stay unstated: a doc with
  zero citations under `only:"stale"`, two citations sharing one anchor span,
  and an anchor and its citation inside the same span.

**Loop 2 (2026-07-27)** — CRITICAL 0 · HIGH 0 · MEDIUM 3 · LOW 1 · INFO 0
(2 verified, 2 unverified). All three lanes re-briefed cold, with no account of
loop 1's findings or fixes — a fix that holds is one nobody raises again.
**Converged:** every verified finding was polish.

- **MEDIUM, polish** — § 1's "is a field of `Options`" reads as "already
  compiles", sending an implementer looking for a member that is not there.
  Reworded to "declared in ANTS-3636 § 2.7's listing" and marked not-yet-shipped
  at first mention rather than only in § 5, 200 lines later.
- **LOW, polish** — § 5 called the parent's § 2.5 a "one-line pointer"; it is
  six lines. Now "a short pointer".
- **Dismissed (2), and both matter as evidence.** One lane placed
  `filterControlChars` at `src/remotecontrol.h:127`; `:127` is the comment line
  above it and `:128` is the declaration — which is what this spec cites and
  what `doc_citations` resolves it to. The other asked § 2 to state that the
  anchor's closing line equals the citation span's opening line; § 2 states
  exactly that, and names why the measurement needs it. Neither survived a
  file open, which is why agent claims are verified before they are fixed.
- **ANTS-3642** owns the residual: the un-anchored remainder and a
  caller-supplied `expect` map.
