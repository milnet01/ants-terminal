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
gives **27** — 23 consumer sites plus four wrapper-internal pass-throughs —
listed in § 2.4. The ROADMAP bullet carries the correction.

**Layman:** Every time a tool asks "is this project's roadmap in the database
yet?", it opens the 3 MB text file to find out — even though the answer is
visible in the first 21 KB, and even though a migrated project's text is never
used afterwards. This makes it read only what it needs.

## 2. Surface

### 2.1 `RoadmapText` — the seam's text parameter becomes a provider

**Six functions change signature**, not four: § 2.2's four dispatch-taking
functions and § 2.3's two owner wrappers. They stop taking `const QString
&markdown` and take `RoadmapText &` instead. `RoadmapText` is declared in
`src/roadmapsource.h` beside the seam it serves, and lives in
`ants_roadmapstore_lib` with the rest of it. INV-7 names five *symbols* for the
same six functions — `roadmapBullets` covers both wrappers.

```cpp
// The roadmap text, read as late as the caller's path actually needs it.
//
// The dispatch needs only the detector's window (§ 2.2); the unmigrated path
// needs the whole text. Splitting them behind one object is what lets the
// dispatch run BEFORE any body read, without the call sites in § 2.4 each
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

    // True when a file-backed text's file would not open, OR when a read that
    // did open failed part-way. Each consumer keeps its OWN answer to that
    // (§ 2.4) — the two roadmap_log sites already disagree today, and this
    // spec preserves both.
    //
    // ALWAYS false for a fromMemory() text: it has no file to fail on, so a
    // site that branches on it takes its success path unconditionally. That is
    // what makes the archive-concatenating site (§ 2.4) safe to leave alone.
    bool openFailed();

    // Bytes actually read from disk so far — DISK bytes, not the QString's
    // in-memory size (§ 4 prices that separately, and in different units).
    // Exists so INV-1 is assertable rather than argued.
    qint64 bytesRead() const;
};
```

**The read contract, stated because three invariants rest on it and none of
them can hold without it.** A file-backed `RoadmapText` opens its file **once**,
with `QIODevice::ReadOnly | QIODevice::Text` — the mode every one of today's
call sites uses, and the one that decides CRLF handling, so INV-5's
byte-identity claim is meaningless without pinning it. It **retains the open
handle** until `full()` has run or the object is destroyed. `detectionPrefix()`
reads forward to the cap and stops; `full()` continues from where the prefix
stopped and appends, rather than seeking back and re-reading. Hence:

- **Every byte is read at most once** (INV-6), so `bytesRead()` after both
  accessors equals the file size exactly and not the file size plus the prefix.
- **The prefix and the body come from one file description** — the deferred
  read introduces no TOCTOU window that reading the whole file up front did not
  already have. A writer that replaces the file between the caller's `stat` and
  the dispatch is the same race as today, unchanged and out of scope; a writer
  that rewrites it *between the two accessors* is the race this contract closes.
- **A mid-read I/O error** leaves `openFailed()` true and `full()` returning
  what was read before the failure, which is the behaviour of the `readAll()`
  it replaces — `QFile::readAll()` on a truncated read returns the partial bytes
  and sets the device's error, and no current site checks for that.

A caller that never reaches `full()` leaves the handle open until the object
dies. Every consumer in § 2.4 is a stack-scoped local inside the request it
serves, so that is one descriptor for the length of one verb call.

**Why a type and not two overloads.** An overload taking a path would put "which
granularity do I need?" at all 23 consumer sites, and a site that guesses wrong either
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
| `RoadmapDialog::storeLegend()` | `src/roadmapdialog.cpp` | never |

**`storeLegend()` is in that table although it is dialog code, not seam code.**
It is `migratedProject()`'s fourth caller (§ 1), it dispatches and then reads
the store's `legendText` — it has no use for markdown on either branch — and
INV-1's *Breaks when* clause says "any seam function", which on a literal
reading leaves the one dispatch site outside the seam free to drain the file
and satisfy the invariant anyway. It is the same rule, and the table is where
it binds.

### 2.3 The two owner wrappers

`RemoteControl::roadmapBullets()` (`src/remotecontrol_terminal.cpp`) and
`RoadmapDialog::roadmapBullets()` (`src/roadmapdialog.cpp`) take `RoadmapText &`
in place of their `const QString &markdown`. Every use of the body in either
wrapper is an unmigrated fall-through, and each becomes a `full()` call — the
only `full()` calls in the seam's own code:

```cpp
    return RoadmapParse::parseBullets(text.full());
```

This is the line that makes the laziness pay: it is reached only when the
project is *not* migrated, which is the one case where the whole file was
always going to be needed.

**The dialog wrapper has three such lines, not one**, and an implementer
converting "the one site" leaves two that no longer compile.
`RoadmapDialog::roadmapBullets()` returns `RoadmapParse::parseBullets(markdown)`
at three separate exits — an empty `storeProjectRoot()`, a null `m_roadmapStore`
after the open attempt, and the final fall-through past `bulletsFor()` — which
`grep -c 'return RoadmapParse::parseBullets(markdown);' src/roadmapdialog.cpp`
confirms as 3. All
three are the same "not migrated, parse the text" outcome and all three convert
identically. `RemoteControl::roadmapBullets()` has one.

### 2.4 The 27 sites

Every site that hands roadmap text to one of the entry points above. Enumerated
by `grep -rnE '(^|[^:[:alnum:]_])(roadmapBullets|roadmapWriteTarget|roadmapStoreServes|RoadmapSource::bulletsFor|RoadmapSource::migratedProject)\(' src/ --include=*.cpp`,
piped through `grep -vE '^[^:]+:[0-9]+: *//'` to drop comment mentions. The
per-file counts below are `… | cut -d: -f1 | sort | uniq -c`, and they sum to
**27** (re-derived 2026-08-08).

**No definitions have to be removed by hand, and an earlier draft of this
section said five and then named six.** The command's leading
`(^|[^:[:alnum:]_])` already excludes every qualified definition — `:` is inside
the negated class, so `RemoteControl::roadmapBullets(` and its four siblings do
not match — and `RoadmapSource`'s own two definitions are written unqualified in
`src/roadmapsource.cpp`, which the `RoadmapSource::`-prefixed alternatives do
not reach either. The output is 27 call sites and nothing else, so the count is
reproducible by running the command and reading `wc -l`.

Four of the 27 are **wrapper-internal pass-throughs** — a seam function handing
its own parameter to the next one down — and they change with the signature
rather than as consumer edits: the `bulletsFor()` call inside each owner
wrapper (`RemoteControl::roadmapBullets()` in `src/remotecontrol_terminal.cpp`
and `RoadmapDialog::roadmapBullets()` in `src/roadmapdialog.cpp`), and the
`migratedProject()` call inside each of `RemoteControl::roadmapStoreServes()`
and `RemoteControl::roadmapWriteTarget()` (both
`src/remotecontrol_roadmap_query.cpp`). The other **23** are true consumer
sites that construct the text.

| File | Sites | of which internal | Change |
|---|---|---|---|
| `src/remotecontrol_roadmap_log.cpp` | 7 | 0 | `RoadmapText::fromFile(roadmapPath)`; the `QFile` + `readAll()` block above each is deleted |
| `src/remotecontrol_roadmap_query.cpp` | 8 | 2 | `fromFile` at all six consumer sites; the two that share one read with a section path call `full()` on that branch (below) |
| `src/roadmapdialog.cpp` | 5 | 1 | `fromFile` for the dispatch, `fromMemory` for the archive-concatenated text |
| `src/remotecontrol_feedback.cpp` | 3 | 0 | `fromFile` |
| `src/remotecontrol_changelog.cpp` | 2 | 0 | `fromFile` |
| `src/remotecontrol_coldeyes.cpp` | 1 | 0 | `fromFile` |
| `src/remotecontrol_terminal.cpp` | 1 | 1 | the wrapper's own pass-through |

**Two `roadmap_query` sites read once and spend the text twice, and only one of
their two branches can go lazy.** In the cache-miss block
(`remotecontrol_roadmap_query.cpp`, the `if (!fresh)` body) a single `readAll()`
feeds either the dispatch — `section.isEmpty()`, which needs nothing but the
detector's window — or, on the `else`, `RoadmapIndex::buildIndex(markdown)`,
which needs every byte. The `section_index` branch below it has the same shape.
So each becomes one `RoadmapText::fromFile(path)` whose `full()` is called on
the index branch and never on the dispatch branch. This is not an exception to
the change; it is the change working as designed — a site that needs the body
asks for it, and the site next to it that does not, does not. § 2.5 records that
the cache's *keying and TTL* are what stay untouched.

**Sites whose text is not a file on disk pass `fromMemory` and change nothing
else.** `RoadmapDialog::roadmapBullets()` is handed
`loadRoadmapMarkdown(includeArchive)`, which concatenates the live roadmap with
its archives — not a single file, and not what the dispatch classifies. The
dialog's own dispatch (`storeLegend()`) already passes
`loadRoadmapMarkdown(/*includeArchive=*/false)`, i.e. the live file alone, so it
becomes `fromFile(m_roadmapPath)` and the concatenating call stays `fromMemory`.

**`loadRoadmapMarkdown(false)` is not quite `readAll()`, and the difference is
stated rather than assumed.** It delegates to `loadMarkdown()`, which opens
`ReadOnly | Text` and calls `f.read(kPerFileCap)` with an **8 MiB per-file cap**
(`src/roadmapdialog.cpp`), returning an empty string if the open fails. For
`storeLegend()` the substitution is exact in every way that matters: the
dispatch reads inside the first 300 non-blank lines, three orders of magnitude
inside the cap, and an unopenable file yields an empty prefix on both shapes,
so § 2.5's classification outcome is unchanged. It is **not** exact for a
hypothetical roadmap past 8 MiB, where `full()` would return more text than
`loadMarkdown()` does today — and that difference is a *widening*, not a
regression, since the cap silently truncated the dialog's parse. The dialog's
8 MiB cap is not adopted into `RoadmapText`; ANTS-1125 INV-5a owns it for the
archive-concatenation path, which stays on `fromMemory` and keeps it.

**Each site keeps its existing answer to an unopenable file.** The two
`roadmap_log` sites already differ — the `op:append` path refuses
`roadmap_read_failed`, the `op:append_batch` path tolerates the failure and lets
the empty text reach the seam — and both are preserved by branching on
`openFailed()` where they branch on `QFile::open()` today.

### 2.5 What does not change

- **The parsed-bullet cache's keying, TTL and invalidation.**
  `src/remotecontrol_roadmap_query.cpp` keys a cache on path + mtime with a TTL
  and only reads on a miss; none of that logic moves, and no cache-miss site is
  excepted from § 2.4's conversion. This item makes the *miss* cheap, it does
  not change when one happens. What changes inside the miss block is only
  *when* the bytes are pulled — § 2.4's last paragraph has the branch detail.
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
  valid `ants-v1` bullets to its rendered `ROADMAP.md`, dispatch, and assert the
  dispatch still returns the project id while `bytesRead()` stays inside the
  **fixture's own** prefix boundary. **The bound is derived, not chosen:** the
  test builds the fixture, so it computes the byte offset at which its 300th
  non-blank line ends and asserts `bytesRead()` is no more than that offset plus
  one line (the reader may overshoot to finish the line it is on, and may not
  overshoot further). A second assertion — `bytesRead() < fileSize / 100` —
  states the same thing as a ratio and is what fails loudly if the cap is lost.
  Neither figure is 64 KiB; an earlier draft asserted that constant with no
  derivation, and it collided with § 4's unrelated RAM ceiling.
  *Breaks when:* any function in § 2.2's table calls `full()`, or the bounded
  reader loses its cap and drains the file.
- **INV-2** — The **bounded file reader and the in-memory helper produce the
  same `QStringList`**, so swapping one for the other cannot change a
  classification: for each fixture,
  `detectRoadmapFormat(RoadmapText::fromFile(p).detectionPrefix())` equals
  `detectRoadmapFormat(RoadmapText::fromMemory(<whole file>).detectionPrefix())`,
  `sawSignal` included. *Test:* `RoadmapReadSeam`, over one fixture per dialect
  (`ants-v1`, `github-task-list`, `pass-headings`), one whose dialect signal sits
  after 300 non-blank lines, and one that is entirely blank lines. Five files, not
  a corpus sweep — the claim is exactly what those five exercise.
  **This is reader equivalence, not prefix-versus-whole-file equivalence**, and
  the distinction is load-bearing: both sides of the comparison are capped at
  `kDetectorLineCap`, so on the after-300-lines fixture both correctly *miss* the
  late signal and agree. That the capped prefix classifies a whole file the same
  way is `detectionPrefix()`'s own pre-existing property — stated at its
  definition in `src/roadmapsource.cpp` and untouched here (§ 2.5) — so this
  item neither re-proves it nor may quietly weaken it.
  *Breaks when:* the reader counts blank lines toward the cap, or stops at 300
  total lines rather than 300 non-blank ones — either shifts the window away
  from the helper's and the two sides diverge.
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
  to assert `full()` equals a `QFile::readAll()` of the same path **opened
  `QIODevice::ReadOnly | QIODevice::Text`** — the mode § 2.1 pins and the one
  every current site uses — including for a file with no trailing newline and
  one with CRLF line endings. The mode is named in the assertion rather than
  assumed, because `Text` is exactly what decides the CRLF case, so a comparison
  against a bare `ReadOnly` read would fail on a correct implementation.
  *Breaks when:* the bounded reader's line splitting leaks into `full()`, or
  `full()` returns only the bytes after the prefix rather than the whole text.
- **INV-6** — No `RoadmapText` reads any byte of its file more than once,
  whichever order its accessors are called in. *Test:* `RoadmapReadSeam` — call
  `detectionPrefix()`, then `full()`, then both again, and assert `bytesRead()`
  equals the file size exactly and never grows on the repeat calls.
  **This and INV-5 are jointly satisfiable only under § 2.1's retained-handle
  rule** — `full()` continuing from where the prefix stopped is what lets it
  return the whole text (INV-5) without re-reading the head (INV-6). An
  implementation that re-opens and re-reads satisfies INV-5 and fails this;
  one that returns only the tail satisfies this and fails INV-5.
  *Breaks when:* memoisation is added to one accessor and not the other, or the
  handle is closed after `detectionPrefix()` and `full()` re-opens.
- **INV-7** — **No dispatch-taking entry point keeps a `const QString &`
  overload.** *Test:* source-grep — the matcher must be **multiline**, because a
  C++ parameter list wraps and four of the six signatures put `const QString
  &markdown,` on a line of its own, where a line-based `grep` cannot see it:

  ```
  rg -U --count-matches \
    '(roadmapBullets|roadmapWriteTarget|roadmapStoreServes|bulletsFor|migratedProject)\s*\([^)]*QString\s*&\s*markdown' \
    src/
  ```

  **Expected 12 before the change and 0 after** (`rg -U`, measured 2026-08-08).
  Twelve is not a figure to take on sight — it is six functions × two
  occurrences each, a declaration in the header and a definition in the `.cpp`,
  the same six § 2.1 names. A run reporting any other pre-change figure means
  the change set moved and this invariant is measuring the wrong thing. Stating
  the before-figure is what makes the after-figure evidence: the same pattern
  under line-based `grep` returns **5**, not 12, so a `grep` run reaching 0
  would prove only that the five single-line signatures moved, leaving
  `migratedProject()` and `bulletsFor()` — the two the whole item is about —
  unchecked. *Breaks when:* a site is left on the old shape behind a
  compatibility overload, which is the half-migrated seam — two ways to ask one
  question, free to disagree — that § 2.4's all-sites scope exists to prevent.

## 4. RAM / build cost

**Peak RAM falls; nothing new is held.** A `RoadmapText` holds at most one
memoised `QString` (the body, only on the unmigrated path — exactly what the
site holds today) plus a `QStringList` of at most 300 non-blank lines.

**`QString` is UTF-16, so the RAM figure is not the byte figure**, and the two
must not be compared as if they were. Taking § 1's prefix measurement: that many
UTF-8 bytes of near-ASCII markdown becomes twice as many bytes in memory, and
the `QStringList`'s per-element overhead (a `QArrayData` header plus a pointer,
~32 bytes across ~350 elements) adds roughly 11 KiB on top. The object's ceiling
on the migrated path is therefore about **2.5× the prefix's on-disk size**,
which is the number to reason with — and it is still three orders of magnitude
below the whole file the same call holds today, which is the point.

That ceiling is a **RAM** quantity and has nothing to do with INV-1's threshold,
which bounds **bytes read from disk**. An earlier draft put 64 KiB in both
places, which made two unrelated budgets look like one.

The saving is per dispatch, not per session — one file read per
`roadmap_query` / `roadmap_log` cache miss, cut to § 1's ratio. **The
measurements themselves live only in § 1**, because they move with every roadmap
edit and two copies would disagree within the day; this section derives from
them by reference rather than restating them.

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

**The target is stated as bytes, not milliseconds, and deliberately.** Wall-clock
on this path is dominated by page-cache state and by whatever else the host is
doing, so a millisecond figure would be unreproducible on the machine that has
to check it. INV-1's `bytesRead()` assertion is the acceptance criterion — it is
derived from the fixture, exact, and fails deterministically if the saving is
not delivered. A latency number here would be a softer restatement of it.

## 6. Out of scope

- **Streaming the detector so it stops at the format marker** — a permanent
  exclusion, not deferred. `detectRoadmapFormat()` returns at
  `<!-- ants-roadmap-format: 1 -->` on line 1 of a rendered roadmap, so a
  streaming reader would cut § 1's prefix to that one line — a further three
  orders of magnitude, on top of the three the bounded read already delivers.
  It is not worth a reader that must interleave with the detector's own loop:
  the bound is what makes the cost safe, and the residue it leaves is already
  small enough that halving it again buys nothing measurable. (§ 1 holds the
  figures; they are not repeated here.)
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
- **Auditing whether any of the 23 consumer sites should not be reading a roadmap at
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
  gains a pointer saying so.
- **`docs/specs/ANTS-3815-store-source-format-column.md` § 2.4 — already
  amended, 2026-08-08.** Its closing paragraph now scopes its "the live read is
  not removed and no signature changes" claim to that item and points here for
  what happens after, since that sentence describes the state *this* item
  leaves behind. The same edit corrected the paragraph's caller list, which
  named three callers and gave `RoadmapDialog::storeProjectRoot()` as one of
  them — the identical error § 1 corrects in the ROADMAP bullet, and it had
  propagated into a spec before anyone caught it. Nothing further is owed there.
- **`docs/specs/ANTS-3793-roadmap-consumer-cutover.md`** — its seam signatures
  are quoted in the header comment of `src/roadmapsource.h`; both move together.
- **`ROADMAP.md`** — the ANTS-3863 bullet's corrected inventory (already
  annotated 2026-08-07) matches § 1; flip to 🚧 when implementation starts.
- **`CHANGELOG.md`** — one `Changed` entry in the release that carries it.
- **`CLAUDE.md`** — no change. The module map describes subsystems, not
  signatures, and the roadmap lane's entry stays true.

## Cold-eyes loop log

<!-- /cold-eyes writes one row per review loop as it closes. -->

| Loop | Date | Lanes | C/H/M/L/I | Dimensions | Outcome |
|---|---|---|---|---|---|
| 1 | 2026-08-08 | 1 (general-purpose, strong model, cold); packet was `references/review-brief.md` plus bounded windows of `roadmapsource.{h,cpp}`, `roadmapparse.cpp`, `remotecontrol_roadmap_{query,log}.cpp`, `remotecontrol_terminal.cpp`, `roadmapdialog.{h,cpp}` and quoted ANTS-3815 / ANTS-3793 passages | C 1 · H 5 · M 5 · L 5 · I 0 — verified 16 (one in part), dismissed 0 | dim 5×4, dim 4×3, dim 15×3, dim 9×2, dim 1×1, dim 2×1, dim 7×1, dim 10×1 | **Every finding verified against current source before any edit, and the run's centre of gravity was that this document's own arithmetic did not reproduce. (1) CRITICAL: § 2.4's table excepted "the cached-parse path" while § 2.5 said the cache is untouched — read as written, the saving never reached the largest consumer. Re-derived from `remotecontrol_roadmap_query.cpp`: the cache-miss block spends one `readAll()` on *either* the dispatch (`section.isEmpty()`) *or* `RoadmapIndex::buildIndex()` on the `else`, so no site is excepted — the index branch simply calls `full()`. § 2.4 now names the branch and § 2.5 is scoped to the cache's keying and TTL. (2) MEDIUM, and the worst reproducibility defect: § 2.4 claimed the enumeration summed to **25** after "five definitions removed by hand", then listed six names. Re-running it returns **27**, and *no* definitions are removable — the command's leading `(^\|[^:[:alnum:]_])` already excludes every qualified definition, and `RoadmapSource`'s two are written unqualified where the `RoadmapSource::`-prefixed alternatives cannot reach them. The true split is 23 consumer sites plus 4 wrapper-internal pass-throughs; `roadmap_query` is 8, not 6. All six other mentions of "25" swept in the same pass. (3) HIGH ×2, resolved together by a read contract § 2.1 never had: INV-6 (read each byte once) and INV-5 (`full()` is byte-identical to today's read) were jointly unsatisfiable, and INV-5's byte-identity was undefined without an open mode — every current site uses `ReadOnly \| Text`, which is exactly what decides its CRLF fixture. § 2.1 now pins the mode, a retained handle, `full()` continuing from where the prefix stopped, and — closing the MEDIUM TOCTOU finding — states that prefix and body therefore come from one file description. (4) HIGH: § 2.3's "exactly one use of the body" is three in `RoadmapDialog::roadmapBullets()` — an empty root, a null store, and the fall-through past `bulletsFor()`; an implementer converting "the one site" leaves two that do not compile. (5) HIGH: § 2.2's "three wrappers" omitted `storeLegend()`, the fourth `migratedProject()` caller, which INV-1's "any *seam* function" clause does not reach — it is dialog code. Added to the table with the reason it belongs there. (6) HIGH **verified only in part, and the correction matters more than the finding**: the lane said INV-7's grep "returns zero before any change, so the invariant is unfalsifiable". It returns **5**. The real hole is narrower and worse-hidden — line-based `grep` cannot see the four signatures that wrap `const QString &markdown,` onto its own line, which includes `migratedProject()` and `bulletsFor()`, the two functions the item is about. INV-7 is now `rg -U` with a derived before-figure of **12** (six functions × declaration + definition) and 0 after. (7) MEDIUM ×2 on § 4: the RAM ceiling ignored that `QString` is UTF-16, so the prefix costs ~2× its byte count plus ~11 KiB of `QStringList` headers; and its "well under 64 KiB" collided with INV-1's *unrelated* 64 KiB, which bounds **disk bytes** and was asserted with no derivation. INV-1's threshold now derives from the fixture the test builds, and § 4 states the two budgets are different quantities. (8) MEDIUM: § 4 and § 6 restated figures § 4 itself declares live only in § 1 — three times. Both now derive by reference. (9) LOW ×5: § 2.1's "four dispatch-taking functions" read as the whole change set (it is six functions / five symbols); `openFailed()` was undefined for `fromMemory` and for a mid-read error; § 5's "latency, downward" carried no target (now INV-1's byte assertion, stated as *why* a millisecond figure would be worse); INV-2's claim said "the bounded prefix and the whole file classify identically" while its test caps **both** sides — on the after-300-lines fixture both correctly miss the signal, so the claim was false by construction and is now reader-equivalence, with the prefix-vs-whole-file property left where it lives, at `detectionPrefix()`'s definition. **Found by verification rather than by the lane, and the run's best catch:** § 2.4 assumed `storeLegend()`'s `loadRoadmapMarkdown(false)` is `m_roadmapPath`'s bytes. It is not — `loadMarkdown()` reads under an **8 MiB per-file cap** — immaterial for a dispatch that stops inside 300 lines, but a widening for any roadmap past the cap, and now stated instead of assumed. **Cross-doc, and it is where the same error had already spread:** ANTS-3815 § 2.4's closing paragraph named three callers including `RoadmapDialog::storeProjectRoot()` — the identical wrong function this spec's § 1 corrects in the ROADMAP bullet — and claimed "no signature changes" unscoped. Both amended there; § 8 records it. **Nothing deferred, nothing dismissed.** Doc 359 → 532 lines. |
