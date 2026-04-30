# Roadmap dialog — split-by-version archive (ANTS-1125)

User request 2026-04-30: ROADMAP.md has crossed 260 KiB and is becoming
unwieldy in tooling that loads it whole — the Read-tool 256 KiB cap, the
roadmap-query IPC cache, and `RoadmapDialog::rebuild` itself. Rather
than treat a single growing file as immutable, split it by minor
version: keep the small "open work" surface in the canonical
`ROADMAP.md`, and rotate the closed-minor sections into per-minor
archive files in a directory adjacent to that file. The viewer pulls
archives in **only on demand** — when the user asks for History or
runs a search — so the default render stays cheap.

## Layout convention

Archive files live at `<dirname(canonical-roadmap)>/docs/roadmap/`,
where `canonical-roadmap` is `m_roadmapPath` after symlink resolution.
The dialog never re-discovers the project — it uses only
`dirname(canonical(m_roadmapPath))`. (For most repos that *is* the
project root, but the dialog doesn't know that and doesn't depend
on it.)

```
<dirname(canonical-roadmap)>/
├── ROADMAP.md              ← current minor in flight + [Unreleased]
└── docs/
    └── roadmap/
        ├── 0.5.md          ← shipped 0.5.x sections, snipped from ROADMAP.md
        ├── 0.6.md          ← shipped 0.6.x sections
        └── 0.7.md          ← shipped 0.7.x sections
```

Archive files use **`<MAJOR>.<MINOR>.md`** verbatim — no leading `v`,
no `roadmap-` prefix, no padding (`0.7.md` not `00.07.md`), no patch
suffix (`0.7.0.md` is rejected). The naming rule is enforced by a
case-sensitive regex `^[0-9]+\.[0-9]+\.md$` applied to each entry the
loader sees; non-matching entries are skipped silently. This filter
defends against `latest.md`, `README.md`, `0.7.md.bak`, hidden files,
and the `0.7.0.md`/patch-suffix mistake.

Archives are **sorted numerically** by the (major, minor) integer
tuple parsed from the filename, descending. This is critical: lexical
descending puts `0.10.md` before `0.9.md` (string `1` < string `9`),
which is the wrong order. Numeric sort handles minor-10 cleanly.

Rotation happens at `/bump` time on a minor or major version bump:
the just-closed minor's section is snipped out of `ROADMAP.md` and
written to `docs/roadmap/<closed>.md`. Patch bumps don't rotate. The
`/bump` recipe is the sole writer; `bump.json` (project recipe at
`.claude/bump.json`) owns the rotation step. The `roadmap-format.md`
standard owns the layout / naming / sort contract via § 3.9
"Archive rotation"; this spec defers to it.

## Viewer behaviour

The dialog's existing two-stage pipeline (read markdown → render HTML)
gains one helper between the read and the render:

```
m_roadmapPath ──→ loadRoadmapMarkdown(includeArchive) ──→ renderHtml(...)
                          │
                          └─ if includeArchive && historyArchiveDir():
                               for each archive (numeric desc by version):
                                 emit "\n\n---\n\n"     ← thematic break
                                 emit "<!-- archive: 0.7.md -->\n\n"
                                 emit cap-truncated archive contents
```

The thematic break (`---`) plus HTML comment is a **sentinel
separator**: any list / heading / bullet truncated at the per-file
cap cannot bleed into the next file's content because Markdown's
thematic break terminates every open block context.

End-to-end shape of the assembled buffer (synthetic example, current
file + two archives):

```text
[ROADMAP.md content, up to 8 MiB]


---

<!-- archive: 0.7.md -->

[0.7.md content, up to 8 MiB]


---

<!-- archive: 0.6.md -->

[0.6.md content, up to 8 MiB]
```

If the total exceeds 64 MiB partway through, the loader stops adding
archives and emits one final
`<!-- archive: truncated past 64 MiB cap -->` line in place of the
next archive's content (see INV-5a).

`includeArchive` is computed by `wantsHistoryLoad()`:

| Active preset                       | wantsHistoryLoad? |
|-------------------------------------|-------------------|
| Full / Current / Next / FarFuture   | false             |
| History                             | true              |
| Custom                              | false             |
| (any preset) + search box non-empty | true (overrides)  |

The Full preset has every `Show*` bit on, including `ShowDone`, so
auto-loading on `Done`-checked alone would force archive-load on the
default view — defeating the purpose of the split. The trigger is the
explicit user gesture (selecting the History preset or typing in
search), keyed against `Preset::History` the enumerator (not a tab
index literal — tab order may change).

## Read caps

- **Per-file cap: 8 MiB.** `read(qint64)` cap on each
  `QFile::read()` call — applies independently to `m_roadmapPath`
  and to each archive file. Defends against `/dev/zero` symlinks
  and against accidental binary content in an archive.
- **Total assembled-buffer cap: 64 MiB.** After concatenation, if
  the assembled buffer would exceed 64 MiB, the loader stops adding
  archives and emits one final
  `<!-- archive: truncated past 64 MiB cap -->` line in place of
  further archive content. 8 archives × 8 MiB = 64 MiB, so the 9th
  archive triggers truncation. No hard archive-count cap — the byte
  budget alone is the limit, so legitimately small archives can
  stack many deep without truncation.

## Watcher

The dialog's existing `QFileSystemWatcher` is extended to include
`historyArchiveDir()` as a watched directory when one exists.
**`directoryChanged` is connected to the same `scheduleRebuild`
debounce slot that `fileChanged` already uses** — the 200 ms
debounce coalesces back-to-back rotation writes (snip-out +
new-file create) into a single rebuild, preserving the
scroll-restore guarantees from `roadmap_viewer` § Live update.
`directoryChanged` fires on add, remove, AND rename of any entry
in the watched directory — so external events (a `git checkout`
that switches branches with different archives) trigger the same
rebuild path as `/bump`-time rotation.

Per-file watches on individual archive files are not added —
`/bump` is the only sanctioned writer, and the cost of N watches
isn't warranted.

## IPC scope (guard-rail)

The new helpers — `historyArchiveDir`, `loadRoadmapMarkdown`,
`wantsHistoryLoad` — are private to `RoadmapDialog`. **No code
path outside `RoadmapDialog::loadRoadmapMarkdown` reads from
`historyArchiveDir()`.** This is the assertion: archives don't
bleed into the IPC verb, the test harness, or any future
consumer. INV-12 below is the source-grep guard.

## Backward compatibility

Projects without a `docs/roadmap/` directory keep working unchanged:
`historyArchiveDir()` returns empty, `loadRoadmapMarkdown(true)` is
identical to `loadRoadmapMarkdown(false)`, no extra watcher path is
registered, and the dialog behaves exactly as pre-0.8.

## Invariants

Source-grep + pure-helper drive. Same harness pattern as
`roadmap_viewer` and `roadmap_viewer_tabs`.

- **INV-1** `RoadmapDialog::historyArchiveDir()` returns the absolute,
  symlink-resolved path to `<dirname(canonical(m_roadmapPath))>/docs/roadmap/`
  iff that path exists AND `S_ISDIR` AND is readable. Returns the
  empty string in **every** other case: missing, regular file, broken
  symlink, symlink cycle, unreadable, `docs/` itself missing or a
  non-directory. Pure helper; no side effects beyond the stat.

- **INV-1a** `historyArchiveDir()` is symlink-safe: a symlink-cycle
  for `docs/roadmap/` resolves to a canonical path or returns empty
  rather than looping.

- **INV-2** `loadRoadmapMarkdown(false)` returns the contents of
  `m_roadmapPath` only (subject to the per-file 8 MiB cap) — the
  result does not depend on whether the archive directory exists or
  what it contains.

- **INV-3** `loadRoadmapMarkdown(true)` against a project with no
  `docs/roadmap/` directory returns content identical to
  `loadRoadmapMarkdown(false)` — the missing-archive case is a no-op,
  not an error.

- **INV-3a** Missing-archive-dir negation: when `historyArchiveDir()`
  returns empty, **no path is added to the dialog's
  `QFileSystemWatcher`** for the archive directory. Asserted by
  source-grep that the `m_watcher.addPath` call for archives is
  guarded on a non-empty result.

- **INV-3b** Empty-archive-dir == missing-archive-dir: an existing
  but empty `docs/roadmap/` directory (zero matching `*.md` entries)
  yields the same output as INV-3 — only the current file's content,
  with no separator emitted.

- **INV-4** `loadRoadmapMarkdown(true)` with archive files
  `0.5.md`, `0.6.md`, `0.7.md`, `0.10.md` returns the current file,
  followed by `0.10.md`, then `0.7.md`, then `0.6.md`, then `0.5.md`
  — **numeric descending by `(major, minor)` tuple parsed from the
  filename**. Each archive is preceded by `"\n\n---\n\n<!-- archive:
  <filename> -->\n\n"` (thematic break + HTML comment sentinel) so
  a truncated trailing bullet in the previous block cannot bleed
  into the next archive's heading. Asserted by composing four
  archives with known content and checking `loadRoadmapMarkdown` for
  the exact emitted order and presence of the sentinel string.

- **INV-4a** Filename filter is the case-sensitive regex
  `^[0-9]+\.[0-9]+\.md$`. Non-matching entries — `0.7.0.md`,
  `latest.md`, `README.md`, `0.7.md.bak`, `0.7.MD`, hidden files,
  non-`.md` — are skipped silently. Asserted by populating the
  archive dir with one matching file and several non-matching
  entries and checking that only the matching content appears.

- **INV-5** Per-file 8 MiB read cap. A current ROADMAP.md of 16 MiB
  is truncated to 8 MiB; an 8 MiB cap applies independently to each
  `QFile::read()` call inside `loadRoadmapMarkdown`. The cap defends
  against symlinked `/dev/zero` files (the per-`read()` budget never
  yields more than 8 MiB regardless of how the file claims to size).
  Asserted as a `qint64` literal in the helper — source-grep guards
  against silent removal.

- **INV-5a** Total assembled-buffer cap of 64 MiB. After
  concatenating the current file plus zero-or-more archives, the
  loader stops adding new archives once the assembled buffer would
  exceed 64 MiB. A final sentinel `<!-- archive: truncated past
  64 MiB cap -->` marks the truncation point in the returned string.
  Asserted by composing nine 8 MiB archive files plus an 8 MiB
  current and checking that the assembled length is ≤ 64 MiB and
  the truncation sentinel appears.

- **INV-6** `wantsHistoryLoad()` returns `true` iff the dialog's
  active preset equals **`Preset::History`** (the enumerator —
  asserted via a stored `m_activePreset` member or equivalent
  helper, NOT via tab-index literal) **or** the search box's
  trimmed text is non-empty. Returns `false` for every other preset
  when the search box is empty — the Done filter alone is not a
  trigger. The `Preset::History` enumerator name is the same one
  pinned by `roadmap_viewer_tabs/spec.md` § INV-2; renaming it
  requires a lock-step update to both specs.

- **INV-7** `rebuild()` calls `loadRoadmapMarkdown(wantsHistoryLoad())`
  — the archive load is gated on the policy helper, not always-on.

- **INV-8** The `QFileSystemWatcher` adds `historyArchiveDir()` when
  it returns non-empty, and connects the `directoryChanged` signal
  to the **same debounce slot** that `fileChanged` is already routed
  to (slot name not pinned — a future rename of the dialog's debounce
  slot must update this INV in lockstep, but doesn't break the
  contract). The 200 ms debounce coalesces back-to-back rotation
  writes (snip + create on `/bump`) into one rebuild. Add and
  remove of any entry in the watched dir fire `directoryChanged`
  on every supported platform; rename within the same directory
  is *typically* delivered as a coalesced add/remove pair via the
  underlying inotify (Linux) layer — Qt does not formally guarantee
  rename coalescing, so the test asserts add+remove only and
  treats rename as a platform-best-effort.

- **INV-9** Cross-archive ID search reuses `roadmap_viewer_tabs`
  INV-11's `id:NNNN` parser — there is **one** parser, exercised
  on the archive-augmented input. Asserted by feeding
  `loadRoadmapMarkdown(true)` to the same `parseBullets` /
  predicate-matching pipeline used in the peer spec, and checking
  that an ID present only in an archive matches exactly once.

- **INV-10** History-preset rebuild grows the rendered output: with
  any non-empty archive in `docs/roadmap/`, switching from Full to
  History (which flips `m_activePreset` to `Preset::History` →
  `wantsHistoryLoad` returns `true`) triggers a rebuild whose
  `loadRoadmapMarkdown` result is strictly larger (by at least the
  archive's first 100 bytes plus the sentinel separator) than the
  Full preset's load.

- **INV-11** Numeric-sort guarantee for minor versions ≥ 10. With
  archives `0.9.md` and `0.10.md`, the loader emits `0.10.md` first,
  then `0.9.md`. This is the case lexical sort gets wrong; the
  invariant pins the numeric-sort fix. Negative-case complement to
  INV-4's multi-archive ordering — INV-4 covers single-digit minors
  (where lexical and numeric sort happen to agree); INV-11 is the
  explicit regression guard for the case they diverge.

- **INV-12** **Guard-rail: archives stay inside the dialog.**
  Asserted by source-grep — the regex `\bhistoryArchiveDir\b`
  matches in `src/roadmapdialog.{h,cpp}` and
  `tests/features/roadmap_viewer_archive/` only. Specifically
  excludes `remotecontrol.cpp` (the IPC verb), `mainwindow.cpp`,
  the audit pipeline, and any future consumer — archives are
  dialog-only by contract.

- **INV-13** `roadmap-format.md § archive rotation` exists as a
  documented amendment to the format standard. Asserted by
  source-grep that `docs/standards/roadmap-format.md` contains the
  literal heading `archive rotation` (case-insensitive). The
  standard is the contract owner for the layout / naming rule
  (INV-4a, INV-11); this spec defers to it. The amendment lands in
  the same commit as this spec — INV-13 is not retroactive.

- **INV-14** `/bump` rotation contract.
  `.claude/bump.json`'s `todos` array contains a rotation step
  keyed on **minor and major bumps only — not patch**. The recipe
  step invokes `packaging/rotate-roadmap.sh <closed-minor>` (the
  shell-script form is the contract; a future port to Python or
  another language must keep the same CLI). The step is
  content-preserving: composing a faux ROADMAP.md with two minor
  sections, running `rotate-roadmap.sh` for the closed minor,
  then asserting:
  (a) every bullet under the closed minor's `## <closed>.0 — …`
  heading and its sub-headings appears byte-identical in
  `docs/roadmap/<closed>.md`, and
  (b) the same bullets *and the closed minor's
  `## <closed>.0 — …` heading* are absent from the post-rotation
  ROADMAP.md, and
  (c) the post-rotation ROADMAP.md still parses cleanly via
  `parseBullets`.

- **INV-14a** `rotate-roadmap.sh` CLI shape:
  `rotate-roadmap.sh <closed-minor> [<roadmap-path>] [<archive-dir>]`.
  `closed-minor` is required and must match `^[0-9]+\.[0-9]+$`
  (rejecting `0.7.0`, `latest`, `v0.7`, etc.). The script exits 1
  with a usage line on bad arguments, and exits 2 on degenerate
  section bounds (start ≥ end). Defaults: `roadmap-path` =
  `./ROADMAP.md`; `archive-dir` = `./docs/roadmap` — both are
  relative to the cwd, so invocation from outside the project root
  surfaces a "ROADMAP.md not found" error rather than silently
  rotating the wrong file. The script tolerates spaces in path
  arguments (every shell expansion is double-quoted). The
  closed-minor's `.` characters are regex-escaped before being
  fed to `awk` so `## 0X7` cannot accidentally match
  `^## 0.7[. ]` — INV is asserted via source-grep that the
  awk pattern uses an `ESCAPED=...sed 's/\./...'` form rather
  than feeding `$CLOSED` raw.
  Asserted by source-grep + behavioural fixture round-trips.

- **INV-14b** Idempotence (exit code AND content). Invoking
  `rotate-roadmap.sh 0.5` twice in a row exits 0 both times. The
  first run rotates; the second run finds no `## 0.5.` heading
  remaining and returns "nothing to rotate" without modifying
  ROADMAP.md. Additionally — and this is the load-bearing
  property — the **archive file is not clobbered between runs**.
  If `docs/roadmap/0.5.md` already exists when `rotate-roadmap.sh`
  starts, the script preserves it byte-for-byte (logs a
  "preserving it (no-clobber)" notice on stderr) and only
  performs the snip-out from ROADMAP.md if a matching section is
  still present. This is the realistic edge case where someone
  manually re-pastes the closed minor's section back into
  ROADMAP.md after a rotation: they get the snip; their
  hand-edited archive is untouched. Asserted by a fixture that
  runs the script, hand-edits the archive, runs the script
  again, and compares hashes.

- **INV-14b1** Sequential rotation of two different minors:
  `rotate-roadmap.sh 0.5 && rotate-roadmap.sh 0.6` both exit 0
  and produce two distinct archive files. Closed-minor sections
  are independent; rotating 0.5 does not affect 0.6's section.
  Asserted by a fixture with three sections (`## 0.5.0`,
  `## 0.6.0`, `## 0.7.0`) and verifying both archives land with
  no cross-contamination.

- **INV-14c** Atomic write — applies to **both** the post-rotation
  ROADMAP.md and the new archive file. Both are written via
  `mktemp` + `mv` (POSIX rename) — never via direct
  truncate-then-write. A crash mid-write leaves the previous
  on-disk content intact. Same atomic-replace pattern Ants uses
  for SessionManager and the audit-export sites (ANTS-1016 /
  ANTS-1017). Asserted by source-grep that `rotate-roadmap.sh`
  references `mktemp` for the archive write path AND for the
  ROADMAP.md write path, with `mv` rename completing each.

- **INV-14d** Section-bound discipline. The script walks from the
  first `## <closed-minor>[. ]` heading forward to the next `## `
  heading that does *not* start with `## <closed-minor>[. ]`.
  Multiple sub-headings within the closed minor (e.g. `## 0.7.0`,
  `## 0.7.7`, `## 0.7.12`) all rotate together while a sibling
  `## 0.8.0` heading stays put. Asserted with a fixture that has
  three sub-headings in 0.7 and one in 0.8 — the test checks that
  exactly the 0.7 trio moves and 0.8 remains.

  **EOF case:** when the closed minor is the last section in
  ROADMAP.md (no trailing `## ` heading after it), the walk
  treats EOF as the end marker and rotates the section to its
  final line. Asserted by a separate fixture with only a
  `## 0.7.0` section and no trailing heading.

  **Single-line section case:** a section consisting of just the
  `## 0.7.0 — title` heading with no body lines (followed
  immediately by `## 0.8.0 …`) rotates cleanly with a one-line
  archive body. The sanity guard `END_LINE > START_LINE` allows
  single-line sections through.

## How to verify pre-fix code fails

```bash
# Replace <impl-sha> with the commit that introduced
# loadRoadmapMarkdown / historyArchiveDir / wantsHistoryLoad.
git checkout <impl-sha>~1 -- src/roadmapdialog.cpp src/roadmapdialog.h
cmake --build build --target test_roadmap_viewer_archive
ctest --test-dir build -R roadmap_viewer_archive
# Expect: every INV fails — pre-split source has no
# loadRoadmapMarkdown / historyArchiveDir / wantsHistoryLoad
# helpers, no archive-dir watcher, and rebuild reads
# m_roadmapPath inline.
```

## Out of scope

- **Live editing of archive files.** `/bump` is the sole sanctioned
  writer; manual edits work but aren't part of the contract.
- **Configurable archive directory.** Hard-coded relative path
  matches the App-Build documentation-folder layout (ANTS-1107) and
  isn't worth a Config knob.
- **Per-file archive watching.** Dir-watching catches add / remove /
  rename; per-file `fileChanged` would only matter if archives were
  edited in place, which the contract discourages.
- **Lazy-load on scroll.** The whole assembled markdown is rendered
  at once when archives are pulled in. Rendered HTML is cached in
  `m_lastHtml`; the cost is paid once per History switch.
- **Per-patch archive files (`X.Y.Z.md`).** Archives are per-minor
  only — `0.7.md` covers all 0.7.x patch releases. INV-4a's
  `^[0-9]+\.[0-9]+\.md$` filter rejects `0.7.0.md` precisely so a
  future contributor doesn't accidentally introduce a parallel
  per-patch convention.
