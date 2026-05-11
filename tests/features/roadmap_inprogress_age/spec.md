# roadmap_inprogress_age — feature contract

Locks ANTS-1237: RoadmapDialog "Updated N days ago" line on 🚧
cards, derived from `git blame --line-porcelain` on the bullet
block (MAX author-time over the bullet line + continuation lines).
Full design rationale + re-open conditions live in
[`docs/specs/ANTS-1237.md`](../../../docs/specs/ANTS-1237.md).
This file restates the invariants the test asserts so the test
reads as a contract rather than a regression-only ratchet.

## Invariants under test

These mirror `docs/specs/ANTS-1237.md` § 4. IDs are qualified
(`ANTS-1237-INV-N`) per the ANTS-1236 § 7 convention.

- **ANTS-1237-INV-1.** When `opts.lastTouchDates.contains(rec.id)`
  AND `rec.status == "🚧"`, the rendered card includes
  `<span class="rm-date">· Updated <X></span>` in the meta row,
  where `<X>` is the output of `humanAge(now − lastTouchDates[
  rec.id])`.
- **ANTS-1237-INV-2.** When `rec.status != "🚧"` (i.e. ✅ / 📋 /
  💭), no `· Updated` span is emitted regardless of
  `lastTouchDates` content.
- **ANTS-1237-INV-3.** When `opts.lastTouchDates` is empty OR
  doesn't contain `rec.id` OR `rec.id` is empty, no `· Updated`
  span is emitted on that card.
- **ANTS-1237-INV-4.** The `humanAge(seconds)` ladder lower-side
  labels are: `today` (<86400), `yesterday` (<2·86400), `Nd ago`
  (<14·86400, N∈[2,13]), `Nw ago` (<60·86400, N∈[2,8]),
  `Nmo ago` (<365·86400, N∈[2,12]), `Ny ago` (otherwise, N≥1).
  `1d ago` / `1w ago` / `1mo ago` are unreachable.
- **ANTS-1237-INV-5.** `refreshLastTouchDatesIfStale` re-runs
  `parseLastTouchDates` only when ROADMAP.md's mtime
  (ms-resolution) differs from `m_lastTouchDatesMtime`. Public
  `lastTouchDatesMtime()` accessor exposes the cache mtime for
  observation.
- **ANTS-1237-INV-6.** `parseLastTouchDates` returns an empty
  `QHash` on every failure path: not a git repo, file not
  tracked, git not on PATH, file does not exist, or blame stdout
  empty. No crash, no error dialog.
- **ANTS-1237-INV-7 + ANTS-1237-INV-8.** For a `- 🚧 [ANTS-NNNN]`
  bullet with continuation lines, the inserted timestamp is
  `MAX(author-time)` over the bullet's block (bullet line +
  contiguous 2-space-indented continuation lines until the first
  blank line, next top-level bullet, or EOF). ID mentions
  elsewhere (e.g. audit-trail prose in another bullet's body)
  are NOT visited and contribute no timestamps.

## Test shape

Standalone C++ executable in the `test_dialogs` bundle.

- Renderer-layer assertions (INV-1, INV-2, INV-3, INV-4) call the
  static `RoadmapDialog::renderCardsHtml` / `humanAge` directly —
  no dialog instance, no git needed.
- Parser-layer assertions (INV-5, INV-6, INV-7, INV-8) build a
  real git checkout in a `QTemporaryDir` and exercise the public
  static `parseLastTouchDates` + the public
  `refreshLastTouchDatesIfStale` member.
- Parser tests early-skip with `GTEST_SKIP()` if `git` is not on
  PATH.
