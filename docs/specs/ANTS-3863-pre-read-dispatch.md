# ANTS-3863 — dispatch before reading, so a migrated project never loads its `ROADMAP.md` body

**Status:** spec draft (2026-08-07).
**Kind:** refactor.
**Source:** ROADMAP.md ANTS-3863 (split from ANTS-3815 § 6 by user decision,
2026-08-07; scope confirmed by the user the same day after the call-site
inventory came back 8× the bullet's figure).
**Blocked by:** ANTS-3815 (the `project.source_format` column the dispatch reads).
**Composes with:** ANTS-3793 (the read seam this narrows), ANTS-3809 (the
write-side dispatch the bullet's inventory missed).

## 1. Problem

The read seam asks one cheap question — *is this project migrated, and does the
file still look like what the store says it is?* — and every caller pays 3.12 MiB
to ask it.

`RoadmapSource::migratedProject()` (`src/roadmapsource.cpp`) takes the roadmap
text `const QString &markdown` and uses it for exactly one thing: it hands
`detectionPrefix(markdown)` to `RoadmapParse::detectRoadmapFormat()`. That helper
caps the detector's input at 300 **non-blank** lines (`kDetectorLineCap`,
`src/roadmapsource.cpp`), so the dispatch has never looked past the head of the
file. But `markdown` arrives by value, which means the caller has already read
the whole file before the dispatch can run.

Three consequences, and only the first is a cost:

1. **Every dispatch reads the whole file.** Measured 2026-08-07:
   `stat -c%s ROADMAP.md` → **3,276,756 bytes**. The bytes the detector can
   actually reach — 300 non-blank lines — end at byte **21,046** (line 346),
   from `awk 'BEGIN{n=0;b=0}{b+=length($0)+1; if($0 ~ /[^ \t]/) n++; if(n>=300)
   {print b; exit}}' ROADMAP.md`. On a migrated project the remaining
   **3,255,710 bytes — 99.36% of the file** — are read, converted from UTF-8,
   and discarded unexamined.

   **This figure moves every time anyone edits the roadmap** — it grew 1,781
   bytes between the first measurement this session and this draft, from an
   annotation added to the ANTS-3863 bullet itself. Nothing in this spec turns
   on the exact value; what it turns on is the ratio, and the ratio only worsens
   as the file grows.

2. **Two sites read it for nothing else at all.**
   `src/remotecontrol_roadmap_log.cpp` opens the file, `readAll()`s it into a
   block-scoped `storeMarkdown` (the `op:append` path) and into a block-scoped
   `probe` (the `op:append_batch` path), passes each to
   `RemoteControl::roadmapWriteTarget()`, and lets it fall out of scope at the
   closing brace. On a migrated project those two reads have no consumer.

3. **There is no shared reader to fix.** Each consumer opens its own `QFile`.
   `grep -c 'readAll()'` returns 9 in `src/remotecontrol_roadmap_query.cpp`, 18
   in `src/remotecontrol_roadmap_log.cpp` and 1 in `src/roadmapdialog.cpp`, so
   the change introduces the shared reader rather than editing one.

**The bullet's inventory was wrong twice, and this spec supersedes it.**
`migratedProject()` has **four** callers, not three:
`RoadmapSource::bulletsFor()`, `RemoteControl::roadmapStoreServes()`,
`RemoteControl::roadmapWriteTarget()` (ANTS-3809, added after the bullet's
inventory was taken) and `RoadmapDialog::storeLegend()`. **The bullet names
`RoadmapDialog::storeProjectRoot()` for that last one and it is the wrong
function** — `storeProjectRoot()` (`src/roadmapdialog.cpp`) walks up from
`m_roadmapPath` to the nearest `.git` and returns a string; it holds no
dispatch. The dialog's two dispatch sites are `storeLegend()` and
`RoadmapDialog::roadmapBullets()`.
`grep -rn 'migratedProject(' src/ --include=*.cpp --include=*.h | grep -vE
'^[^:]+:[0-9]+: *(//|\*)'` returns six lines: one declaration
(`src/roadmapsource.h`), one definition (`src/roadmapsource.cpp`) and those four
calls. The unfiltered grep returns 15 — the other nine are prose mentions in
comments, which is why the filter is part of the command rather than a detail
left to the reader. Counting every site
that hands roadmap text to a dispatch-taking entry point — `roadmapBullets` /
`roadmapWriteTarget` / `roadmapStoreServes` / `bulletsFor` / `migratedProject` —
gives **25**, listed in § 2.4. The ROADMAP bullet carries the correction.

**Layman:** Every time a tool asks "is this project's roadmap in the database
yet?", it opens the 3 MB text file to find out — even though the answer is
visible in the first 21 KB, and even though a migrated project's text is never
used afterwards. This makes it read only what it needs.

## 2. Surface

### 2.1 `RoadmapText` — the seam's text parameter becomes a provider

The four dispatch-taking functions stop taking `const QString &markdown` and
take `RoadmapText &` instead. It is declared in `src/roadmapsource.h` beside the
seam it serves, and lives in `ants_roadmapstore_lib` with the rest of it.

```cpp
// The roadmap text, read as late as the caller's path actually needs it.
//
// The dispatch needs only the detector's window (§ 2.2); the unmigrated path
// needs the whole text. Splitting them behind one object is what lets the
// dispatch run BEFORE any body read, without the 25 call sites in § 2.4 each
// having to know which of the two they will end up needing.
class RoadmapText {
public:
    // Nothing is read at construction. `path` is the project's LIVE roadmap.
    static RoadmapText fromFile(QString path);

    // The caller already holds the text: RoadmapDialog's archive
    // concatenation (loadMarkdown() with includeArchive), and every test.
    // Costs nothing and reads nothing — full() hands back what it was given.
    static RoadmapText fromMemory(QString text);

    // At most kDetectorLineCap non-blank lines, in the detector's own shape.
    // File-backed: a bounded read that stops at the cap and never touches the
    // tail. Memoised — INV-6 forbids a second read.
    const QStringList &detectionPrefix();

    // The whole text, memoised. Only the unmigrated path calls it.
    const QString &full();

    // True when a file-backed text's file would not open. Each consumer keeps
    // its OWN answer to that (§ 2.4) — the two roadmap_log sites already
    // disagree today, and this spec preserves both.
    bool openFailed();

    // Bytes actually read from disk so far. Exists so INV-1 is assertable
    // rather than argued: it is the figure § 1 consequence 1 quotes.
    qint64 bytesRead() const;
};
```

**Why a type and not two overloads.** An overload taking a path would put "which
granularity do I need?" at all 25 sites, and a site that guesses wrong either
re-reads the file or refuses on a file it never opened. One object answers both
questions and remembers what it already did, so no site has to know.

**Why the accessors are non-`const` and the parameter is `RoadmapText &`.**
They memoise, so they mutate. A `const RoadmapText &` parameter would force
`mutable` members, which hides the read from the signature — the exact thing
this item exists to make visible.

### 2.2 The dispatch stops touching the body

`migratedProject()` changes one line of body: `detectionPrefix(markdown)`
becomes `text.detectionPrefix()`. Everything else in it — the canonicalisation
refusal, `readProjectByRoot()`, the `sawSignal` refusal, ANTS-3815's drift
comparison, the `ants-v1` gate — is untouched.

**It must never call `full()`, and that is the whole of this item's saving.**
The three functions that wrap it — `bulletsFor()`, `roadmapStoreServes()`,
`roadmapWriteTarget()` — must not call it either: each either returns the
store's records or reports "not migrated", and in neither case does the seam
itself parse markdown.

| Function | File | Body read? |
|---|---|---|
| `RoadmapSource::migratedProject()` | `src/roadmapsource.cpp` | never |
| `RoadmapSource::bulletsFor()` | `src/roadmapsource.cpp` | never |
| `RemoteControl::roadmapStoreServes()` | `src/remotecontrol_roadmap_query.cpp` | never |
| `RemoteControl::roadmapWriteTarget()` | `src/remotecontrol_roadmap_query.cpp` | never |

### 2.3 The two owner wrappers

`RemoteControl::roadmapBullets()` (`src/remotecontrol_terminal.cpp`) and
`RoadmapDialog::roadmapBullets()` (`src/roadmapdialog.cpp`) take `RoadmapText &`
in place of their `const QString &markdown`. Each has exactly one use of the
body — its unmigrated fall-through — and it becomes the only `full()` call in
the seam's own code:

```cpp
    return RoadmapParse::parseBullets(text.full());
```

This is the line that makes the laziness pay: it is reached only when the
project is *not* migrated, which is the one case where the whole file was
always going to be needed.

### 2.4 The 25 consumer sites

Every site that hands roadmap text to one of the entry points above. Enumerated
by `grep -rnE '(^|[^:[:alnum:]_])(roadmapBullets|roadmapWriteTarget|roadmapStoreServes|RoadmapSource::bulletsFor|RoadmapSource::migratedProject)\(' src/ --include=*.cpp`,
piped through `grep -vE '^[^:]+:[0-9]+: *//'` to drop comment mentions and with
the seam's own five definitions removed by hand
(`RemoteControl::roadmapBullets`, `roadmapStoreServes`, `roadmapWriteTarget`,
`RoadmapSource::bulletsFor`/`migratedProject`, `RoadmapDialog::roadmapBullets`).
The per-file counts below are `… | cut -d: -f1 | sort | uniq -c`, and they sum
to 25:

| File | Sites | Change |
|---|---|---|
| `src/remotecontrol_roadmap_log.cpp` | 7 | `RoadmapText::fromFile(roadmapPath)`; the `QFile` + `readAll()` block above each is deleted |
| `src/remotecontrol_roadmap_query.cpp` | 6 | same, except the cached-parse path (§ 2.5) |
| `src/roadmapdialog.cpp` | 5 | `fromFile` for the dispatch, `fromMemory` for the archive-concatenated text |
| `src/remotecontrol_feedback.cpp` | 3 | `fromFile` |
| `src/remotecontrol_changelog.cpp` | 2 | `fromFile` |
| `src/remotecontrol_coldeyes.cpp` | 1 | `fromFile` |
| `src/remotecontrol_terminal.cpp` | 1 | the wrapper's own pass-through |

**Sites whose text is not a file on disk pass `fromMemory` and change nothing
else.** `RoadmapDialog::roadmapBullets()` is handed
`loadRoadmapMarkdown(includeArchive)`, which concatenates the live roadmap with
its archives — not a single file, and not what the dispatch classifies. The
dialog's own dispatch (`storeLegend()`) already passes
`loadRoadmapMarkdown(/*includeArchive=*/false)`, i.e. the live file alone, so it
becomes `fromFile(m_roadmapPath)` and the concatenating call stays `fromMemory`.

**Each site keeps its existing answer to an unopenable file.** The two
`roadmap_log` sites already differ — the `op:append` path refuses
`roadmap_read_failed`, the `op:append_batch` path tolerates the failure and lets
the empty text reach the seam — and both are preserved by branching on
`openFailed()` where they branch on `QFile::open()` today.

### 2.5 What does not change

- **The parsed-bullet cache.** `src/remotecontrol_roadmap_query.cpp` keys a
  cache on path + mtime with a TTL and only reads on a miss; that logic is
  untouched. This item makes the *miss* cheap, it does not change when one
  happens.
- **`detectRoadmapFormat()` and the in-memory `detectionPrefix()` helper.** The
  bounded file reader is a second producer of the same `QStringList`, not a
  replacement — `fromMemory` still uses the existing helper.
- **Every refusal code and message.** No `ReadError` value, envelope `code`, or
  message text moves. ANTS-3815's drift message included.
- **The migration's own classification.** `RoadmapMigrate::findRoadmaps()`
  classifies during migration and is not on this path.

## 3. Invariants

- **INV-1** — A dispatch against a **migrated** project reads at most
  `kDetectorLineCap` non-blank lines from the live roadmap, never its body.
  *Test:* `RoadmapReadSeam` — migrate a project, append a multi-megabyte tail of
  valid `ants-v1` bullets to its rendered `ROADMAP.md`, dispatch, and assert
  `bytesRead()` is under 64 KiB while the dispatch still returns the project id.
  *Breaks when:* any seam function calls `full()`, or the bounded reader loses
  its cap and drains the file.
- **INV-2** — The bounded prefix and the whole file **classify identically**
  over the five fixtures its test names: for each,
  `detectRoadmapFormat(RoadmapText::fromFile(p).detectionPrefix())` equals
  `detectRoadmapFormat(RoadmapText::fromMemory(<whole file>).detectionPrefix())`,
  `sawSignal` included. *Test:* `RoadmapReadSeam`, over one fixture per dialect
  (`ants-v1`, `github-task-list`, `pass-headings`), one whose dialect signal sits
  after 300 non-blank lines, and one that is entirely blank lines. Five files, not
  a corpus sweep — the claim is exactly what those five exercise.
  *Breaks when:* the reader counts blank lines toward the cap, or stops at 300
  total lines rather than 300 non-blank ones — either shifts the window and can
  cut a signal the whole-file path sees.
- **INV-3** — ANTS-3815 INV-6 still holds through the new signature: a set
  `source_format` disagreeing with the live file's detected format refuses with
  `ReadError::SourceUnrecognised` and serves neither backend. *Test:* the
  existing `RoadmapReadSeam.Ants3815Inv6StoredFormatDisagreeingWithTheFileRefuses`
  stays green unedited but for its `RoadmapText` argument. *Breaks when:* the
  dispatch stops consulting the file at all — which is the failure ANTS-3815 § 6
  names as this item's debt, and this invariant is the second witness it is owed.
- **INV-4** — A migrated project whose live roadmap is **absent or unopenable**
  still refuses, and each consumer keeps the code it emits today. *Test:*
  `RoadmapReadSeam` for the seam half (empty prefix → `sawSignal` false →
  `SourceUnrecognised`), plus an `mcp_roadmap_*` case asserting `op:append`
  still answers `roadmap_read_failed` and `op:append_batch` still reaches the
  seam. *Breaks when:* `openFailed()` is folded into the seam's own refusal, which
  would collapse two consumer behaviours that differ today into one.
- **INV-5** — On an **unmigrated** project the text parsed is byte-identical to
  what the site reads today. *Test:* `RoadmapReadSeam.Inv2BackendsAgree` extended
  to assert `full()` equals a direct `QFile::readAll()` of the same path,
  including for a file with no trailing newline and one with CRLF line endings.
  *Breaks when:* the bounded reader's line splitting leaks into `full()`, or the
  prefix read consumes bytes `full()` then fails to re-read.
- **INV-6** — No `RoadmapText` reads its file more than once, whichever order
  its accessors are called in. *Test:* `RoadmapReadSeam` — call
  `detectionPrefix()`, then `full()`, then both again, and assert `bytesRead()`
  equals the file size exactly once over and never grows on the repeat calls.
  *Breaks when:* memoisation is added to one accessor and not the other, making
  a prefix-then-body path read the head twice.
- **INV-7** — **No dispatch-taking entry point keeps a `const QString &`
  overload.** *Test:* source-grep — `grep -rnE
  '(roadmapBullets|roadmapWriteTarget|roadmapStoreServes|bulletsFor|migratedProject)\s*\([^)]*QString\s*&\s*markdown'
  src/` returns zero. *Breaks when:* a site is left on the old shape behind a
  compatibility overload, which is the half-migrated seam — two ways to ask one
  question, free to disagree — that § 2.4's all-25 scope exists to prevent.

## 4. RAM / build cost

**Peak RAM falls; nothing new is held.** A `RoadmapText` holds at most one
memoised `QString` (the body, only on the unmigrated path — exactly what the
site holds today) plus a `QStringList` of at most 300 non-blank lines. Using
§ 1's measurements: the prefix is 21,046 bytes of text over 346 lines, whose
`QString` headers add roughly 12 KiB, so the object's ceiling on the migrated
path is well under 64 KiB against the whole file the same call holds today.

The saving is per dispatch, not per session — one file read per
`roadmap_query` / `roadmap_log` cache miss, cut to § 1's ratio. The figures are
stated once, in § 1, because they move with every roadmap edit and two copies
would disagree within the day.

**No new build targets and no new libraries.** `RoadmapText` joins
`ants_roadmapstore_lib` in the existing `src/roadmapsource.{h,cpp}`; it needs
`QFile` and `QString`, both already included there.

## 5. Migration / compatibility

**No on-disk format, schema or wire contract moves.** This is an in-process
signature change: no store column, no export record, no MCP envelope field, and
no config key is touched. A store written before this item and one written after
are identical, so there is no ladder rung and `kSchemaVersion` does not move.

The one externally visible change is **latency**, downward, on the migrated
path. No verb's response shape changes.

## 6. Out of scope

- **Streaming the detector so it stops at the format marker** — a permanent
  exclusion, not deferred. `detectRoadmapFormat()` returns at
  `<!-- ants-roadmap-format: 1 -->` on line 1 of a rendered roadmap (byte 32 on
  this project), so a streaming reader would cut 21,046 bytes to ~32 — from
  0.642% of the file to 0.001%. The bounded read already removes 99.36%; the
  remaining 21 KB is not worth a reader that must interleave with the detector's
  own loop. The bound is what makes the cost safe.
- **Removing the file open entirely on a migrated project** — a permanent
  exclusion, and INV-3 is why. The open is what produces the second witness
  ANTS-3815 § 6 requires; a store-only dispatch would make INV-6 of that spec
  unenforceable, which § 6 forbids without superseding it explicitly. The
  bullet's proposed "cheap existence/size stat" is strictly weaker than the
  bounded read for the same cost class, so it is rejected rather than deferred.
- **The parsed-bullet cache's TTL and keying** — untouched (§ 2.5), tracked by
  nothing, because nothing is wrong with it. This item changes the cost of a
  miss, not the miss rate.
- **The 18 `readAll()` sites in `src/remotecontrol_roadmap_log.cpp` that are not
  dispatch inputs** — out of scope by definition. They read the file to splice
  text into it, which is the markdown write path and needs the whole file.
  § 2.4's seven are the subset that feed a dispatch.
- **Auditing whether any of the 25 sites should not be reading a roadmap at
  all** — a separate question this item deliberately does not open. It changes
  how each site reads, not whether it should.

## 7. Tests

Feature test: `tests/features/roadmap_read_seam/`, extending the existing
`RoadmapReadSeam` suite and its `spec.md`. Covers INV-1 through INV-6; INV-7 is
a source-grep case in the same file. Label `features;fast`, except INV-1's
multi-megabyte fixture, which follows the suite's existing
`Ants3793LatencyCaseIsPerfLabelled` convention and carries the `perf` label so
it stays out of the default presets.

Per the project test convention, **verify each new case fails against pre-change
source first** — for INV-1 that means asserting `bytesRead()` before the bounded
reader exists, so the case must be written against a stub that returns the file
size and seen to fail.

Existing cases that must stay green unedited but for the new argument type:
`RoadmapReadSeam.Inv1DispatchMarker`,
`Ants3815Inv5UnrecordedFormatDispatchesAsBefore`,
`Ants3815Inv6StoredFormatDisagreeingWithTheFileRefuses`, `Inv2BackendsAgree`,
`Inv2Membership`, `Inv3Ceiling`, `Inv3Latency`.

## 8. Cross-doc impact

- **`docs/specs/ANTS-3815-store-source-format-column.md` § 6** — its
  "ANTS-3863 owes INV-6 a second witness" line is discharged by INV-3 here, and
  gains a pointer saying so. Its § 2.4 sentence "The live read is not removed and
  no signature changes" becomes "the live read is bounded and the signature
  changes at ANTS-3863", because that sentence describes the state this item
  leaves behind.
- **`docs/specs/ANTS-3793-roadmap-consumer-cutover.md`** — its seam signatures
  are quoted in the header comment of `src/roadmapsource.h`; both move together.
- **`ROADMAP.md`** — the ANTS-3863 bullet's corrected inventory (already
  annotated 2026-08-07) matches § 1; flip to 🚧 when implementation starts.
- **`CHANGELOG.md`** — one `Changed` entry in the release that carries it.
- **`CLAUDE.md`** — no change. The module map describes subsystems, not
  signatures, and the roadmap lane's entry stays true.
