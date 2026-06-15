# Feature test — roadmap_log pass-headings writer (ANTS-2126)

Drives the `cmdRoadmapLog*ForTest` seams against a seeded
`#### Pass N.M` heading-format `ROADMAP.md` in a `QTemporaryDir`, plus
pure-helper unit cases for `PassHeadingWrite`. Verifies the writer that
ANTS-2031 punted (it shipped only the `format_mismatch` refusal). The
spec these invariants implement is `docs/specs/ANTS-2126.md`; the INV
numbers below are this test file's own namespace (distinct from the
design spec's INV-1..16 and from the format-mismatch test's INVs).

- **INV-2** — `op:append` with a valid `pass` writes a `#### Pass <pass>
  <headline>` + `- **Status**: <keyword>` block at the end of the named
  section and returns `id:"PASS-<major>-<minor>[-<sub>]"` +
  `format:"pass-headings"`. The written block re-parses through the
  reader to the intended id/status/headline (round-trip, INV-12).
- **INV-3** — `op:append` refuses `bad_args` when `pass` is absent, when
  `pass` is malformed, or when `status` is absent; the file is untouched.
- **INV-4** — `op:flip` by `PASS-N-M` id rewrites the located pass's
  `- **Status**:` line to the new keyword; every other byte is identical.
- **INV-5** — flip preserves the Status line's style: keyword-only stays
  keyword-only, emoji-only stays emoji-only, emoji+keyword keeps both.
- **INV-6** — flip of a pass with no `- **Status**:` line inserts one
  under the heading.
- **INV-7** — `op:flip_batch` flips N passes by id in one commit; an
  unresolvable id lands in `skipped[]` while the rest apply.
- **INV-8** — `op:annotate` appends `note` as a body line at the end of
  the pass block, status unchanged; an empty note refuses `bad_args`.
- **INV-10** — no pass write path mutates `.roadmap-counter` (byte-equal
  before/after an append).
- **INV-11/12** — `passStatusKeyword` round-trips through the reader to
  the intended emoji; an appended pass re-parses to its reported id.
- **INV-13** — when a pass has two `- **Status**:` lines, flip targets
  the first; the second is byte-identical.
- **INV-14** — `op:append_batch` skip-and-continue: a malformed entry
  lands in `skipped[]` while valid entries commit; an all-invalid batch
  leaves the file untouched.
- **INV-15** — `op:append` into an empty section inserts the block under
  the section heading.
- **INV-16** — a stray `pass` arg on a GFM/ants-v1 roadmap is ignored,
  not refused (the normal bullet still writes).

Pure-helper unit cases additionally cover `passStatusKeyword` (the total
4→keyword map) and `passIdFromDesignator` (leading-zero stripping).
