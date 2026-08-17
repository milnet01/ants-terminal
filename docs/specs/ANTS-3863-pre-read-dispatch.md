# ANTS-3863 — dispatch before reading, so a migrated project never loads its `ROADMAP.md` body to dispatch

**Status:** accepted (2026-08-08) — rule-14 gate run to its 3-loop cap,
converged, nothing verified left unfixed and no deferred tail. Ready to
implement; no further review gate is owed.

<!--
ANTS-3863: this line read "spec draft (2026-08-08)" until 2026-08-17, while the
review-loop log at the foot of this file recorded three completed cold-eyes
loops closing that same day. A session picking the item up read the status,
applied CLAUDE.md rule 14 ("a contract document runs through review-contract
before anyone builds under it"), and stopped — correctly, on the evidence it
had. The gate had in fact run.

Corrected to the sibling convention (see ANTS-3793 and ANTS-3808, both
"accepted (2026-08-04) — …"). No new gate is owed for this edit: it makes the
header agree with the log already in the document, and the gate whose absence
it implied is the one that demonstrably ran.

Worth noting that loop 3 listed "a stale `Status:` date" among its own MEDIUM
findings — so this field has drifted here before, and the drift is what a
reader keys on first.
-->

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

2. **One site reads it for nothing else at all, and a second reads it for
   almost nothing.** `src/remotecontrol_roadmap_log.cpp` opens the file and
   `readAll()`s it into a block-scoped `probe` (the `op:append_batch` path),
   passes it to `RemoteControl::roadmapWriteTarget()`, and lets it fall out of
   scope at the closing brace — on a migrated project that read has no consumer
   at all. The `op:append` path's `storeMarkdown` is the near-miss: it reaches
   `rlStoreCounterPrefix()` further down the same block, but that helper
   consults the text only after an explicit `id_prefix` argument and the store's
   own `idPrefixFor()` have both come up empty. A migrated project normally has
   a stored prefix, so the 3 MiB is read for a fallback that normally does not
   run. § 2.4 keeps that fallback working without paying for it up front.

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
dispatch. `storeLegend()` is the dialog's only caller of `migratedProject()`;
`RoadmapDialog::roadmapBullets()` reaches the same decision one level up,
through `bulletsFor()`.
`grep -rn 'migratedProject(' src/ --include=*.cpp --include=*.h | grep -vE
'^[^:]+:[0-9]+: *(//|\*)'` returns six lines: one declaration
(`src/roadmapsource.h`), one definition (`src/roadmapsource.cpp`) and those four
calls. The unfiltered grep returns 15 — the other nine are prose mentions in
comments, which is why the filter is part of the command rather than a detail
left to the reader.

Counting every site that hands roadmap text to a dispatch-taking entry point
gives **28**, not the three the bullet implies. **§ 2.4 owns that derivation** —
the command, what it can and cannot match, and the per-file split — and it is
not repeated here. The ROADMAP bullet carries the correction.

**Layman:** Every time a tool asks "is this project's roadmap in the database
yet?", it opens the 3 MB text file to find out — even though the answer is
visible in the first 21 KB, and even though a migrated project's text is never
used afterwards. This makes it read only what it needs.

## 2. Surface

### 2.1 `RoadmapText` — the seam's text parameter becomes a provider

**Six seam functions change signature, plus one helper below the seam.** § 2.2's
table has five rows; the four that take roadmap text change here, and
`RoadmapDialog::storeLegend()` — the fifth — takes none, so it changes only in
how it *builds* the text it passes on (§ 2.4). Those four plus § 2.3's two owner
wrappers make six. **The seventh is `rlStoreCounterPrefix()`** (§ 2.4), which is
not a dispatch-taking entry point and so is outside INV-7's grep — it is counted
here because "six" is the number an implementer works to, and leaving it behind
costs the `op:append` saving § 2.4 relies on. All seven stop taking `const
QString &markdown` and take `RoadmapText &` instead. `RoadmapText` is declared in
`src/roadmapsource.h` beside the seam it serves, and lives in
`ants_roadmapstore_lib` with the rest of it. INV-7 names five *symbols* for the
six **seam** functions — `roadmapBullets` covers both wrappers — and does not
reach the seventh, which is why it is called out above rather than left to the
grep.

```cpp
// The roadmap text, read as late as the caller's path actually needs it.
//
// The dispatch needs only the detector's window (§ 2.2); the unmigrated path
// needs the whole text. Splitting them behind one object is what lets the
// dispatch run BEFORE any body read, without the call sites in § 2.4 each
// having to know which of the two they will end up needing.
class RoadmapText {
public:
    // Move-only: it owns an open file handle (the read contract below), so a
    // copy would either read the file twice — breaking INV-6 — or double-close
    // it. Declared, not merely intended:
    RoadmapText(const RoadmapText &) = delete;
    RoadmapText &operator=(const RoadmapText &) = delete;
    RoadmapText(RoadmapText &&) noexcept;
    RoadmapText &operator=(RoadmapText &&) noexcept;
    ~RoadmapText();

    // Nothing is read at construction. `path` is the project's LIVE roadmap.
    static RoadmapText fromFile(QString path);

    // The caller already holds the text: RoadmapDialog's archive
    // concatenation (loadMarkdown() with includeArchive), and every test.
    // Costs nothing and reads nothing — full() hands back what it was given.
    static RoadmapText fromMemory(QString text);

    // At most kDetectorLineCap non-blank lines OR kDetectorByteCap bytes,
    // whichever comes first, in the detector's own shape. Memoised — INV-6
    // forbids a second read. The BYTE cap is not belt-and-braces; see below.
    // On a text whose open failed, returns an empty QStringList — which is
    // what INV-4's refusal chain (empty prefix -> sawSignal false) rests on.
    const QStringList &detectionPrefix();

    // The whole text, memoised. Only the unmigrated path calls it.
    // On a text whose open failed, returns an empty QString — which is what
    // today's sites already hand the seam when their QFile::open() fails.
    const QString &full();

    // True when a file-backed text's file would not open, OR when a read that
    // did open failed part-way. FORCES the open if it has not happened yet
    // (see below — the whole point is that consumers branch on this BEFORE
    // any accessor). Each consumer keeps its OWN answer to an unopenable file
    // (§ 2.4) — the two roadmap_log sites already disagree today, and this
    // spec preserves both.
    //
    // ALWAYS false for a fromMemory() text: it has no file to fail on, so a
    // site that branches on it takes its success path unconditionally. That is
    // what makes the archive-concatenating site (§ 2.4) safe to leave alone.
    bool openFailed();

    // Bytes actually read from disk so far — DISK bytes, not the QString's
    // in-memory size (§ 4 prices that separately, and in different units).
    // ALWAYS 0 for a fromMemory() text, which reads no disk at all, so INV-1's
    // assertion is trivially satisfied there rather than undefined.
    // Exists so INV-1 is assertable rather than argued.
    qint64 bytesRead() const;
};
```

**`kDetectorByteCap` exists because the line cap does not bound the read.**
`kDetectorLineCap` counts
**non-blank** lines, and the helper keeps blank lines in the list without
counting them (`src/roadmapsource.cpp`) — so a file of a million blank lines,
or one whose first 300 non-blank lines sit behind a long blank run, or one with
no `\n` at all, is read to EOF with `nonBlank` never reaching the cap. That is
harmless today, where the caller had already read the whole file anyway; it is
the entire claim once the read is the thing being bounded. **INV-2's own fixture
list names a file that is entirely blank lines**, so the unbounded case is one
this item's tests are required to exercise.

The reader therefore stops at whichever bound it reaches first. **Set
`kDetectorByteCap` to 1 MiB**, beside `kDetectorLineCap` in
`src/roadmapsource.cpp`: ~50× § 1's measured prefix, so no plausible roadmap
reaches it, and small enough that the pathological input costs a bounded read
rather than the file. Truncation lands on the **last complete line at or before
the cap**, so the detector is never handed a half-line — it sees a short list,
which is the case it already handles.

**Both producers get both caps, and that is what keeps INV-2 true.** The byte
cap goes into the existing in-memory `detectionPrefix()` helper as well as the
new file-backed reader. Capping only the reader would fork the two producers on
exactly the input this section is about, and INV-2 — the file reader and the
in-memory helper return the same `QStringList` — would be false by construction
on the all-blank fixture INV-2 itself names. Scoping INV-2 around the divergence
was the alternative and it is the worse trade: two producers free to disagree is
the shape this item exists to remove.

**Capping the in-memory helper changes no classification**, which is why it is
safe to change a shipped helper here. `RoadmapParse::detectRoadmapFormat()`
skips blank lines and breaks at its own `++seen >= 300`
(`src/roadmapparse.cpp`), so it never examines a line the caps would have
dropped. The helper has been returning list elements no consumer reads.

**It also preserves a cap that already existed.** `storeLegend()` reads today
through `loadMarkdown()`, which is capped at 8 MiB per file (§ 2.4); converting
it to an uncapped `fromFile` would have removed a bound rather than tightened
one. `kDetectorByteCap` is stricter than the cap it replaces on that path.

**`openFailed()` forces the open, and that is load-bearing rather than
incidental.** § 2.4 converts sites that branch on `QFile::open()` *before* the
dispatch, so they call `openFailed()` at the same point — before any accessor
has run. An implementation where it merely reports a flag some earlier accessor
set returns `false` on first call, and the `op:append` path silently loses the
`roadmap_read_failed` refusal INV-4 says is preserved. The open it forces is
**the same open** `detectionPrefix()` would have performed, not an extra one, so
it consumes no bytes of its own and INV-6 is unaffected.

**Accessor order is free, in both directions.** `detectionPrefix()` then
`full()` continues from where the prefix stopped. `full()` then
`detectionPrefix()` derives the prefix from the memoised body through the
existing in-memory helper — no disk read at all, since the bytes are already in
hand. Either order reads the file exactly once, which is what makes INV-6
order-independent rather than order-specific.

**The read contract, stated because three invariants rest on it and none of
them can hold without it.** A file-backed `RoadmapText` opens its file **once**,
with `QIODevice::ReadOnly | QIODevice::Text` — the mode every one of today's
call sites uses, and the one that decides CRLF handling, so INV-5's
byte-identity claim is meaningless without pinning it. It **retains the open
handle** until `full()` has run or the object is destroyed. `detectionPrefix()`
reads forward to whichever of the two caps it reaches first and stops; `full()`
continues from where the prefix stopped and appends, rather than seeking back
and re-reading. Hence:

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
Its four callers must not call it either — `bulletsFor()`,
`roadmapStoreServes()`, `roadmapWriteTarget()` and `storeLegend()`: each either
returns the store's records or reports "not migrated", and in neither case does
the seam itself parse markdown.

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
only `full()` calls in `src/roadmapsource.cpp` and these two wrappers (§ 2.4
adds two more below them: the `roadmap_query` index branch and
`rlStoreCounterPrefix()`'s fallback):

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

### 2.4 The 28 sites

Every site **outside `src/roadmapsource.cpp`** that hands roadmap text to one of
the entry points above. Enumerated by
`grep -rnE '(^|[^:[:alnum:]_])(roadmapBullets|roadmapWriteTarget|roadmapStoreServes|RoadmapSource::bulletsFor|RoadmapSource::migratedProject)\(' src/ --include=*.cpp`,
piped through `grep -vE '^[^:]+:[0-9]+: *(//|\*)'` — the same comment filter § 1
uses, which drops ` * ` continuation lines a bare `//` filter leaves in. The
per-file counts below are `… | cut -d: -f1 | sort | uniq -c`, and they sum to
**27** (re-derived 2026-08-08).

**The "outside `src/roadmapsource.cpp`" qualifier is a real exclusion, not
tidiness.** The two `RoadmapSource::`-qualified alternatives cannot match that
file's own in-namespace calls, which are written unqualified — so
`bulletsFor()`'s `migratedProject(store, projectRoot, markdown, …)` is a genuine
28th site that the command does not return — **the 28th**, and the reason this
section's heading and the command's own total differ by one. It is a
wrapper-internal pass-through like the four below, it changes with the
signature, and it is named here because a command whose output is presented as
"every site" must say what it cannot see.

**No definitions have to be removed by hand.** The command's leading
`(^|[^:[:alnum:]_])` already excludes every qualified definition — `:` is inside
the negated class, so `RemoteControl::roadmapBullets(` and its four siblings do
not match — and `RoadmapSource`'s own two definitions are written unqualified in
`src/roadmapsource.cpp`, which the `RoadmapSource::`-prefixed alternatives do
not reach either. The output is 27 call sites and nothing else, so the count is
reproducible by running the command and reading `wc -l`.

Four of the command's 27 are **wrapper-internal pass-throughs** — a seam function handing
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
| `src/remotecontrol_roadmap_log.cpp` | 7 | 0 | `RoadmapText::fromFile(roadmapPath)`. At `op:append_batch`'s `probe` the `QFile` + `readAll()` block is **deleted** outright. At `op:append`'s `storeMarkdown` it is replaced rather than deleted, because that text has a second consumer (below). The other five take their text as a function parameter and own no read block to delete — the caller's conversion is what reaches them |
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

**A site whose text is not a file on disk passes `fromMemory`.**
`RoadmapDialog::roadmapBullets()` is handed
`loadRoadmapMarkdown(includeArchive)`, which concatenates the live roadmap with
its archives — not a single file, and not what the dispatch classifies.

**`includeArchive` is a runtime value, so those sites branch rather than pick.**
Its three call sites pass `wantsHistoryLoad()` (`src/roadmapdialog.cpp`), which
is `false` whenever history is not loaded — the common case. On that branch the
text *is* the live file alone, so the dialog would otherwise read 3 MiB and hand
it over as `fromMemory`, forgoing the whole saving on its busiest path. Each
site therefore reads:

```cpp
    auto text = includeArchive
        ? RoadmapText::fromMemory(loadRoadmapMarkdown(true))
        : RoadmapText::fromFile(m_roadmapPath);
    const auto recs = roadmapBullets(text, includeArchive);
```

The `true` branch keeps today's cost because it genuinely needs every archive
byte concatenated before anything can parse them; the `false` branch is the one
this item exists for. **INV-1 is scoped to file-backed texts for exactly this
reason** — a `fromMemory` text reads no disk, so asserting a disk-byte bound on
it is vacuous rather than false.

The
dialog's own dispatch (`storeLegend()`) already passes
`loadRoadmapMarkdown(/*includeArchive=*/false)`, i.e. the live file alone, so it
becomes `fromFile(m_roadmapPath)` and the concatenating call stays `fromMemory`.

**`loadRoadmapMarkdown(false)` is not quite `readAll()`, and the difference is
stated rather than assumed.** It delegates to `loadMarkdown()`, which opens
`ReadOnly | Text` and calls `f.read(kPerFileCap)` with an **8 MiB per-file cap**
(`src/roadmapdialog.cpp`), returning an empty string if the open fails. For
`storeLegend()` the substitution is exact in every way that matters: the
dispatch reads inside the first 300 non-blank lines, ~400× inside the cap, and
an unopenable file yields an empty prefix on both shapes, so § 2.5's
classification outcome is unchanged. `storeLegend()` never calls `full()`
(§ 2.2), so the 8 MiB difference cannot reach it at all.

**Where that difference *does* reach is the wrapper below**,
`RoadmapDialog::roadmapBullets()` on its `includeArchive == false` branch: past
8 MiB `full()` returns more text than `loadMarkdown()` does today. That is a
*widening*, not a regression — the cap silently truncated the dialog's parse —
and § 5 carries it as user-visible, INV-5 as its bound. The dialog's 8 MiB cap
is not adopted into `RoadmapText`; ANTS-1125 INV-5a owns it for the
archive-concatenation path, which stays on `fromMemory` and keeps it.

**`rlStoreCounterPrefix()` takes the `RoadmapText &` too, and that is the one
helper below the seam this item touches.** Its signature
(`src/remotecontrol_roadmap_query.cpp`) ends in `const QString &markdown`, used
only on its last fallback — after `idPrefixArg` and `store.idPrefixFor()` have
both failed. Passing the provider instead of the text moves the `full()` onto
that fallback, so the common migrated `op:append` reads no body while the rare
prefix-sniff still gets the whole file. Converting it is what makes § 1
consequence 2's "near-miss" site pay; leaving it on `const QString &` would
force `storeMarkdown`'s read back to the top of the block and cost the saving
on the second-busiest write path.

**Each site keeps its existing answer to an unopenable file, and the two
`roadmap_log` sites answer differently.** `op:append` branches on
`QFile::open()` today and refuses `roadmap_read_failed`; it branches on
`openFailed()` instead. `op:append_batch` does **not** branch today — its
`if (sf.open(…)) { probe = …; }` simply leaves `probe` empty on failure and lets
it reach the seam — so it gains no guard at all, and the empty prefix a failed
`RoadmapText` yields reproduces exactly that. Both behaviours survive; only one
of them involves a branch.

### 2.5 What does not change

- **The parsed-bullet cache's keying, TTL and invalidation.**
  `src/remotecontrol_roadmap_query.cpp` keys a cache on path + mtime with a TTL
  and only reads on a miss; none of that logic moves, and no cache-miss site is
  excepted from § 2.4's conversion. This item makes the *miss* cheap, it does
  not change when one happens. What changes inside the miss block is only
  *when* the bytes are pulled — § 2.4's last paragraph has the branch detail.
- **`detectRoadmapFormat()`.** Untouched — no branch, no cap, no signature.
- **The in-memory `detectionPrefix()` helper, except for gaining
  `kDetectorByteCap`.** The bounded file reader is a second producer of the same
  `QStringList`, not a replacement — `fromMemory` still uses the existing
  helper, and § 2.1 adds the byte cap to it so the two producers cannot diverge.
  That is the helper's only change.
- **Every refusal code and message.** No `ReadError` value, envelope `code`, or
  message text moves. ANTS-3815's drift message included.
- **The migration's own classification.** `RoadmapMigrate::findRoadmaps()`
  classifies during migration and is not on this path.

## 3. Invariants

- **INV-1** — A dispatch against a **migrated** project, through a
  **file-backed** `RoadmapText`, reads at most `kDetectorLineCap` non-blank
  lines **or `kDetectorByteCap` bytes** from the live roadmap, never its body.
  (A `fromMemory` text reads no disk at all — `bytesRead()` is 0 — so the bound
  holds trivially there rather than being claimed of a path that cannot meet it;
  § 2.4 says which dialog branch is which.)
  *Test:* `RoadmapReadSeam` — migrate a project, append a multi-megabyte tail of
  valid `ants-v1` bullets to its rendered `ROADMAP.md`, dispatch, and assert the
  dispatch still returns the project id while `bytesRead()` stays inside the
  **fixture's own** prefix boundary. **The bound is derived, not chosen:** the
  test builds the fixture, so it computes the byte offset at which its 300th
  non-blank line ends and asserts `bytesRead()` is no more than that offset plus
  one line (the reader may overshoot to finish the line it is on, and may not
  overshoot further). A second assertion — `bytesRead() < fileSize / 100` —
  is a weaker but cap-independent bound, and is what fails loudly if the line
  cap is lost.
  **A third leg covers the byte cap, and it must be sized above it:** a fixture
  of **2 MiB of blank lines**, where the non-blank cap is never reached — assert
  `bytesRead() <= kDetectorByteCap` **and** `bytesRead() < fileSize`, and that
  the dispatch still returns. One million blank lines is 1,000,000 bytes, under
  a 1 MiB cap, so a fixture that size passes whether or not the byte cap exists;
  the second assertion is what makes the leg able to fail at all.
  *Breaks when:* any function in § 2.2's table calls `full()`, or the reader
  loses either cap and drains the file.
- **INV-2** — The **bounded file reader and the in-memory helper produce the
  same `QStringList`**, so swapping one for the other cannot change a
  classification: for each fixture,
  `detectRoadmapFormat(RoadmapText::fromFile(p).detectionPrefix())` equals
  `detectRoadmapFormat(RoadmapText::fromMemory(<whole file>).detectionPrefix())`,
  `sawSignal` included. *Test:* `RoadmapReadSeam`, over one fixture per dialect
  (`ants-v1`, `github-task-list`, `pass-headings`), one whose dialect signal sits
  after 300 non-blank lines, and one that is entirely blank lines **and larger
  than `kDetectorByteCap`** (2 MiB, matching INV-1's third leg). Five files, not
  a corpus sweep — the claim is exactly what those five exercise. The blank
  fixture must exceed the byte cap or it tests only the line cap, and the byte
  cap is the newer of the two and the one that could be applied to one producer
  and not the other.
  **This is reader equivalence, not prefix-versus-whole-file equivalence**, and
  the distinction is load-bearing: both sides of the comparison are capped at
  `kDetectorLineCap`, so on the after-300-lines fixture both correctly *miss* the
  late signal and agree. That the capped prefix classifies a whole file the same
  way is `detectionPrefix()`'s own pre-existing property — stated at its
  definition in `src/roadmapsource.cpp` and untouched here (§ 2.5) — so this
  item neither re-proves it nor may quietly weaken it.
  **The byte cap does not create an exception here, because § 2.1 gives it to
  both producers.** Had it gone to the file reader alone, this invariant would
  be false on its own all-blank fixture — which is why it did not.
  *Breaks when:* the reader counts blank lines toward the cap, stops at 300
  total lines rather than 300 non-blank ones, or applies `kDetectorByteCap` to
  one producer and not the other — each shifts one window away from the other's
  and the two sides diverge.
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
  what the site reads today, **for any roadmap under 8 MiB**. Above that the
  dialog's path widens rather than matches, because `loadMarkdown()`'s
  `kPerFileCap` truncates today and `full()` does not; § 2.4 records it and § 5
  carries it as a user-visible change. Every other consumer already uses
  `readAll()`, so the bound is the dialog's alone. *Test:* a **new** case,
  `RoadmapReadSeam.Ants3863Inv5FullMatchesReadAll` — not an extension of
  `Inv2BackendsAgree`, which § 7 requires to stay unedited but for its argument
  type, and one case cannot be both. It asserts `full()` equals a
  `QFile::readAll()` of the same path **opened
  `QIODevice::ReadOnly | QIODevice::Text`** — the mode § 2.1 pins and the one
  every current site uses — including for a file with no trailing newline and
  one with CRLF line endings. The mode is named in the assertion rather than
  assumed, because `Text` is exactly what decides the CRLF case, so a comparison
  against a bare `ReadOnly` read would fail on a correct implementation.
  *Breaks when:* the bounded reader's line splitting leaks into `full()`, or
  `full()` returns only the bytes after the prefix rather than the whole text.
- **INV-6** — No `RoadmapText` reads any byte of its file more than once,
  whichever order its accessors are called in. *Test:* `RoadmapReadSeam`, **two
  legs, one per order** — `detectionPrefix()` then `full()`, and `full()` then
  `detectionPrefix()` — each followed by calling both again, asserting
  `bytesRead()` equals the file size exactly and never grows on the repeats.
  Two legs because § 2.1 satisfies the orders by different mechanisms (continue
  the handle; derive the prefix from the memoised body), and a single-order test
  leaves one of them unexercised.
  **This and INV-5 are jointly satisfiable only under § 2.1's retained-handle
  rule** — `full()` continuing from where the prefix stopped is what lets it
  return the whole text (INV-5) without re-reading the head (INV-6). An
  implementation that re-opens and re-reads satisfies INV-5 and fails this;
  one that returns only the tail satisfies this and fails INV-5.
  *Breaks when:* memoisation is added to one accessor and not the other, or the
  handle is closed after `detectionPrefix()` and `full()` re-opens.
- **INV-7** — **No dispatch-taking entry point keeps a `const QString &`
  overload.** *Test:* source-grep — the matcher must be **multiline**, because
  C++ parameter lists wrap: **seven of the twelve occurrences** put `const
  QString &markdown,` on a line of its own, where a line-based `grep` cannot see
  it. Sum the per-file counts, because `rg --count-matches` prints one line per
  file and prints nothing at all when there are no matches — so the after-figure
  is only a number if something adds it up:

  ```
  rg -U --count-matches \
    '(roadmapBullets|roadmapWriteTarget|roadmapStoreServes|bulletsFor|migratedProject)\s*\([^)]*QString\s*&\s*markdown' \
    src/ | awk -F: '{s+=$2} END{print s+0}'
  ```

  **Expected 12 before the change and 0 after** (measured 2026-08-08).
  Twelve is not a figure to take on sight — it is six functions × two
  occurrences each, a declaration in the header and a definition in the `.cpp`,
  the six **seam** functions § 2.1 names (`rlStoreCounterPrefix()`, the seventh,
  is outside this pattern by design). A run reporting any other pre-change figure means
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
site holds today) plus a `QStringList` bounded by § 2.1's two caps — 300
non-blank lines, or `kDetectorByteCap` of text, whichever comes first. **The
byte cap is what makes this a ceiling rather than an estimate**, for the reason
§ 2.1 gives: the line cap alone bounds neither the element count nor the bytes.

**Two figures, and conflating them is what the byte cap makes possible.** The
*typical* case is § 1's prefix — the number to reason about, since it is what
every real dispatch on this project actually costs. The *worst* case is the cap:
`kDetectorByteCap` of text is ~2 MiB held as UTF-16, and on an all-blank file up
to ~1 M list elements at ~32 bytes each. **That worst case is a bound, not a
budget** — it is what the object cannot exceed on a hostile input, and it is
still an order of magnitude under the unbounded read it replaces on that same
input. Quoting the typical figure as if it were the ceiling is the error the cap
was added to make impossible.

**`QString` is UTF-16, so the RAM figure is not the byte figure**, and the two
must not be compared as if they were. Taking § 1's prefix measurement: that many
UTF-8 bytes of near-ASCII markdown becomes twice as many bytes in memory, and
the `QStringList`'s per-element overhead (a `QArrayData` header plus a pointer,
~32 bytes across ~350 elements) adds roughly 11 KiB on top. The object's ceiling
on the migrated path is therefore about **2.5× the prefix's on-disk size**,
which is the number to reason with — and it is still **~120× below** the whole
file the same call holds today (both sides in UTF-16, so the doubling cancels),
which is the point.

That ceiling is a **RAM** quantity and has nothing to do with INV-1's threshold,
which bounds **bytes read from disk**. Keep the two apart: a single figure
quoted in both sections would read as one budget.

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

**Three externally visible changes, only the first of which is the point.**
No verb's response shape changes in any of them.

1. **Latency**, downward, on the migrated path. This is the item.
2. **A roadmap whose first 300 non-blank lines exceed `kDetectorByteCap`** is
   classified from a truncated prefix. If its dialect signal sits past 1 MiB the
   detector no longer sees it, `sawSignal` is false, and a migrated project
   refuses `SourceUnrecognised` where it was previously served. This needs an
   average line length over ~3.5 KB across 300 lines — no roadmap in this
   corpus is close (§ 1's prefix is ~50× inside the cap) — but it is a refusal
   the cap creates, so it is recorded rather than assumed unreachable. The
   remedy is the existing one: re-run the migration.
3. **The dialog's unmigrated path past 8 MiB** parses more text than
   `loadMarkdown()`'s `kPerFileCap` allowed (§ 2.4, INV-5). A widening — the
   truncation it removes was silent — but a user-visible difference.

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
  streaming reader would cut § 1's prefix to that one line — a further ~660×,
  on top of the ~156× the bounded read already delivers.
  It is not worth a reader that must interleave with the detector's own loop:
  the bound is what makes the cost safe, and the ~21 KB residue it leaves is
  already small enough that cutting it further buys nothing measurable against
  the complexity. (§ 1 holds the figures; they are not repeated here.)
- **Removing the file open entirely on a migrated project** — a permanent
  exclusion, and INV-3 is why. The open is what produces the second witness
  ANTS-3815 § 6 requires; a store-only dispatch would make INV-6 of that spec
  unenforceable, which § 6 forbids without superseding it explicitly. The
  bullet's proposed "cheap existence/size stat" is strictly weaker than the
  bounded read for the same cost class, so it is rejected rather than deferred.
- **The parsed-bullet cache's TTL and keying** — untouched (§ 2.5), tracked by
  nothing, because nothing is wrong with it. This item changes the cost of a
  miss, not the miss rate.
- **The `readAll()` sites in `src/remotecontrol_roadmap_log.cpp` that are not
  dispatch inputs** — out of scope by definition. § 1 measures **18** `readAll()`
  calls in that file; **two** of them are the reads § 2.4 converts
  (`op:append`'s `storeMarkdown` and `op:append_batch`'s `probe`), leaving 16.
  Those 16 read the file to splice text into it, which is the markdown write
  path and needs the whole file. **The 18 and § 2.4's 7 are different things
  being counted** — `readAll()` calls versus call sites of a dispatch-taking
  entry point — and the five call sites that are neither take their text as a
  function parameter, so they own no `readAll()` at all.
- **Auditing whether any of the 23 consumer sites should not be reading a roadmap at
  all** — a separate question this item deliberately does not open. It changes
  how each site reads, not whether it should.

## 7. Tests

Feature test: `tests/features/roadmap_read_seam/`, extending the existing
`RoadmapReadSeam` suite and its `spec.md`. Covers INV-1 through INV-6; INV-7 is
a source-grep case in the same file. Label `features;fast`, **except the three
multi-megabyte fixtures**, which follow the suite's existing
`Ants3793LatencyCaseIsPerfLabelled` convention and carry the `perf` label so
they stay out of the default presets: INV-1's `ants-v1` tail, INV-1's 2 MiB
blank-line leg, and INV-2's entirely-blank fixture. Sizing them at megabytes is
what makes them meaningful and also what makes them too slow for `fast`; a
labelled-`fast` multi-megabyte case would be the same mistake in the other
direction.

Per the project test convention, **verify each new case fails against pre-change
source first** — for INV-1 that means asserting `bytesRead()` before the bounded
reader exists, so the case must be written against a stub that returns the file
size and seen to fail.

**The existing case names carry ANTS-3793's invariant numbering, not this
document's, and the two collide.** `Inv2BackendsAgree`, `Inv2Membership`,
`Inv3Ceiling` and `Inv3Latency` are ANTS-3793's INV-2 and INV-3 — nothing to do
with the INV-2 and INV-3 above. So **every case this item adds is prefixed
`Ants3863`** (`Ants3863Inv5FullMatchesReadAll` and its siblings), which is the
convention the suite already uses for ANTS-3815's additions and the only thing
keeping "INV-2's test" unambiguous in a file serving three specs.

Existing cases that must stay green unedited but for the new argument type:
`RoadmapReadSeam.Inv1DispatchMarker`,
`Ants3815Inv5UnrecordedFormatDispatchesAsBefore`,
`Ants3815Inv6StoredFormatDisagreeingWithTheFileRefuses`, `Inv2BackendsAgree`,
`Inv2Membership`, `Inv3Ceiling`, `Inv3Latency`.

## 8. Cross-doc impact

- **`docs/specs/ANTS-3815-store-source-format-column.md` § 6** — its
  "ANTS-3863 owes INV-6 a second witness" line is discharged by INV-3 here, and
  gains a pointer saying so. **Two further corrections are owed in the same
  bullet, and both are inventory this spec supersedes.** It describes this item
  as "a signature change across `bulletsFor()`'s two call sites plus
  `RemoteControl::roadmapStoreServes()`" — § 2.1 makes it six functions and
  § 2.4 counts 28 sites. And it offers "a cheap existence/size stat as the
  candidate" for keeping a second witness; § 6 here **rejects** that outright as
  strictly weaker than the bounded read for the same cost class, so it is not a
  live option to leave standing in another spec's prose.
- **`docs/specs/ANTS-3815-store-source-format-column.md` § 2.4 — already
  amended, 2026-08-08.** Its closing paragraph now scopes its "the live read is
  not removed and no signature changes" claim to that item and points here for
  what happens after, since that sentence describes the state *this* item
  leaves behind. The same edit corrected the paragraph's caller list, which
  named three callers and gave `RoadmapDialog::storeProjectRoot()` as one of
  them — the identical error § 1 corrects in the ROADMAP bullet, and it had
  propagated into a spec before anyone caught it. Nothing further is owed there.
- **`docs/specs/ANTS-3793-roadmap-consumer-cutover.md`** — its seam signatures
  are quoted in the header comment of `src/roadmapsource.h`, and that comment
  additionally says `markdown` "is never re-read from disk here" and that
  ANTS-3863 "owns removing the file read". Both become false at this item: the
  read is bounded rather than removed (§ 6), and `RoadmapText` reads from disk
  by design. **This item owns both edits** — the spec's § 2.2 signature block
  and the header comment — and they land in the implementation commit, not
  here, because the comment is code.
- **`ROADMAP.md`** — the ANTS-3863 bullet's corrected inventory (already
  annotated 2026-08-07) matches § 1; flip to 🚧 when implementation starts.
- **`CHANGELOG.md`** — one `Changed` entry in the release that carries it.
- **`CLAUDE.md`** — no change. The module map describes subsystems, not
  signatures, and the roadmap lane's entry stays true.

## Cold-eyes loop log

<!-- /cold-eyes writes one row per review loop as it closes. -->

| Loop | Date | Lanes | C/H/M/L/I | Dimensions | Outcome |
|---|---|---|---|---|---|
| 3 | 2026-08-08 | 2, cold — same shared-packet shape, no mention of loops 1–2; packet's verified-facts block extended with the source facts loops 1–2 had established | C 1 · H 4 · M 7 · L 8 · I 0 — verified 19, dismissed 1 | dim 7×5, dim 4×4, dim 5×4, dim 2×3, dim 6×3, dim 15×3, dim 9×1, dim 1×1, dim 8×1, dim 11×1 | **Converged by cap, and the split is why the cap is the right place to stop: 12 of the 19 were collateral from loop 2's own `kDetectorByteCap` fix, against 7 draft defects. CRITICAL 1 → 1 → 1, but this loop's is the previous loop's fix biting rather than anything the draft ever said. (1) CRITICAL, both lanes (A graded it HIGH, B CRITICAL; graded CRITICAL here): loop 2 gave `kDetectorByteCap` to the **file reader only** while § 2.5 kept the in-memory helper unchanged — which makes INV-2, "the two producers return the same `QStringList`", false by construction on the all-blank fixture INV-2 itself names. The two were mutually unsatisfiable. **Fixed at the root rather than scoped around:** the byte cap goes to *both* producers, so INV-2 survives verbatim. Safe because `detectRoadmapFormat()` skips blank lines and breaks at its own `++seen >= 300` (`src/roadmapparse.cpp`), so it never examines a line the caps would drop — the helper has been returning elements no consumer reads. Scoping INV-2 around the divergence was the alternative and it was the worse trade: two producers free to disagree is what this item exists to remove. (2) HIGH, lane B, and the best catch of the loop: INV-1's new byte-cap leg specified *one million blank lines* — 1,000,000 bytes against a 1,048,576-byte cap. **The only test written to prove the byte cap could not fail whether or not the cap existed.** Now 2 MiB, plus `bytesRead() < fileSize` so the assertion has something to be wrong about. INV-2's blank fixture pinned above the cap for the same reason. (3) HIGH, lane B: § 4's ceiling was still derived from § 1's ~21 KB prefix while § 2.1's cap permits ~2 MiB UTF-16 — the budget section quoting a typical figure as a bound, in the paragraph that had just claimed the cap is "what makes this a ceiling". Now states both, and which is which. (4) HIGH, lane B: the byte cap creates a **new refusal class** — a roadmap whose dialect signal sits past 1 MiB classifies from a truncated prefix, `sawSignal` false, `SourceUnrecognised` where it was served. § 5 said the one visible change was latency. § 5 now carries three, with the ~3.5 KB average line length that would be needed to reach it. (5) HIGH, both lanes: loop 2 added `rlStoreCounterPrefix()` as a **seventh** signature change in § 2.4 while § 2.1 still said six, and INV-7's grep is scoped to dispatch-taking entry points so it cannot detect the miss — an implementer counting to six leaves behind the one conversion the `op:append` saving depends on. **MEDIUM ×7:** the H1's absolute "never loads its body" against § 2.4's index-branch `full()`; 1 MiB called "two orders of magnitude" above a 21 KB prefix when it is ~50×; INV-5's byte-identity claim unscoped against the 8 MiB widening § 2.4 concedes; `op:append_batch` described as branching on `openFailed()` when it does not branch today at all (`if (sf.open(…))` with the failure ignored); the 8 MiB caveat attached to `storeLegend()`, which § 2.2 says never calls `full()`, instead of to the wrapper below it; `detectionPrefix()`'s open-failure result undefined though INV-4's whole refusal chain rests on it; and a stale `Status:` date. **LOW ×8**, all fixed: "the only `full()` calls in the seam's own code" against two more added below it; "states the same thing as a ratio" of a bound that is weaker, not equal; `full()`'s "halving it again" for a ~660× cut; the `perf` label named for one large fixture when there are three; move-only stated in prose with no declarations in the sketch (now five explicit lines); § 1 duplicating § 2.4's 27/28 derivation nearly line for line (deleted, pointer left); the blank-lines rule restated in four places (§ 2.1 owns it now); and "sites … change nothing else" immediately above a paragraph requiring those sites to branch. **Dismissed (1):** lane A's dim-11 "679-line spec with no TOC". Checked the two sibling specs — ANTS-3815 at 773 lines and ANTS-3793 at 968 — and **neither carries one**, so a TOC is not this project's convention and adding one here would be the outlier, not the fix. Doc 681 → 767 lines. **Stopping at the cap, and the trend says stop rather than merely permits it:** CRITICAL 1 → 1 → 1 looks flat and is not — loop 1's was the draft's, loop 2's was the draft's, loop 3's was loop 2's own fix, and collateral now outnumbers draft defects 12:7. The remaining seven draft defects this loop were all LOW/MEDIUM wording. A loop 4 would be answering this loop's own edits with a fresh cold dispatch, which is exactly the reflex the cap exists to stop. Nothing verified is left unfixed, so there is no deferred tail to file. |
| 2 | 2026-08-08 | 2, cold — identical byte-stable shared packet (~16k tok), no mention of loop 1's findings or fixes; packet carried bounded windows of all nine cited source files plus quoted ANTS-3815 / ANTS-3793 passages | C 1 · H 4 · M 6 · L 8 · I 0 — verified 19, dismissed 0 | dim 5×5, dim 4×4, dim 2×3, dim 7×3, dim 10×2, dim 6×2, dim 11×1, dim 12×1, dim 15×1 | **Draft defects outnumbered fix collateral 12:7, so no stop trigger fired — but the CRITICAL was a claim this document had been making since its first draft and neither loop had checked. (1) CRITICAL, lane A (lane B graded the same defect MEDIUM from the RAM side): § 2.1 called the file-backed prefix read "a bounded read that stops at the cap and never touches the tail". `kDetectorLineCap` counts **non-blank** lines and the helper keeps blank lines without counting them, so a file of blank lines — or one with no `\n` — is read to EOF and the bound is fiction. **INV-2's own fixture list names an all-blank file**, so the unbounded case was one the tests were required to hit. Compounding it, § 2.4 had just declined to carry the dialog's existing 8 MiB `kPerFileCap` into `RoadmapText`, which would have *removed* a cap rather than tightened one. New `kDetectorByteCap` (1 MiB), a third INV-1 leg on a million-blank-line fixture, and § 4's ceiling re-derived from both caps. (2) HIGH, both lanes independently: `openFailed()`'s open timing was undefined against § 2.1's "nothing is read at construction", while § 2.4 tells sites to branch on it exactly where they branch on `QFile::open()` today — before any accessor. The natural implementation returns `false` on first call and `op:append` silently loses the `roadmap_read_failed` refusal INV-4 claims is preserved. It now forces the open, and it is the same open `detectionPrefix()` would do. (3) HIGH, lane B: INV-5 said to extend `RoadmapReadSeam.Inv2BackendsAgree` while § 7 listed that case as one that must stay unedited — opposite instructions for one case, so an implementer following § 7 never writes INV-5's check. Now a new `Ants3863Inv5FullMatchesReadAll`, and § 7 gained the naming rule: the existing `Inv2…`/`Inv3…` cases are **ANTS-3793's** numbering, which collides with this document's INV-2/INV-3 in a file serving three specs. (4) HIGH, lane A: INV-1 was falsifiable on a live path — `RoadmapDialog::roadmapBullets()` takes `fromMemory`, having already read the file. Verified the branch: `includeArchive` is `wantsHistoryLoad()`, so it is **false on the common path**, and the dialog was forgoing the whole saving on its busiest call. Now a runtime branch — `fromFile` when false, `fromMemory` when true — and INV-1 is scoped to file-backed texts. (5) HIGH, lane B: `RoadmapText` owns a retained handle (loop 1's own contract) yet is returned by value with no copy/move rule — a copyable handle-owner reads twice or double-closes. Now move-only. **Found by verification rather than by either lane, and it corrected § 1:** consequence 2 claimed *two* sites read the file for nothing else. Only one does. `op:append`'s `storeMarkdown` reaches `rlStoreCounterPrefix()` further down the same block, on the **migrated** path — so the claim was false, and § 2.4's "the block above each is deleted" was false with it. Read the helper: it consults the text only after an explicit `id_prefix` and the store's own `idPrefixFor()` both come up empty, so § 2.4 now passes the provider down to it and the `full()` lands on that rare fallback instead of at the top of the block. **MEDIUM ×6:** the enumeration command cannot match `roadmapsource.cpp`'s own unqualified `migratedProject()` call, a genuine **28th** site the "every site" framing had to name (and the reason § 2.4's heading and its command's total differ by one); § 6's "the 18 `readAll()` sites that are not dispatch inputs" plus § 2.4's seven made 25 in an 18-call file, conflating `readAll()` calls with entry-point call sites; INV-6 defined only the prefix→body order, leaving body→prefix unspecified and untested; "§ 2.2's four dispatch-taking functions" against a five-row table, and "the three functions that wrap it" against four callers — both loop-1 collateral from adding `storeLegend()` to that table; and § 8 owed ANTS-3815 § 6 two corrections it had missed, since that bullet still describes this item as "`bulletsFor()`'s two call sites plus `roadmapStoreServes()`" and still offers the existence/size stat § 6 here **rejects**. **LOW ×8**, all fixed: three "orders of magnitude" claims that arithmetic puts at ~120×, ~156× and ~400× — two orders, not three, now stated as ratios; § 2.4's comment filter differed from § 1's and missed ` * ` continuation lines; `rg --count-matches` prints per-file counts and nothing at all on zero, so INV-7's before/after figures needed the `awk` sum (**executed as written: 12, and 0 on a no-match pattern**); "four of the six signatures wrap" is seven of the twelve occurrences; `bytesRead()` was undefined for `fromMemory` and `full()` for a failed open; § 8's ANTS-3793 row was agentless about an edit this item owns; and "dispatch site" carried two senses two sentences apart. **Consolidation pass:** four "an earlier draft said X" asides removed — drafting history is context tax on the implementer and this table is its home. Doc 532 → 681 lines. |
| 1 | 2026-08-08 | 1 (general-purpose, strong model, cold); packet was `references/review-brief.md` plus bounded windows of `roadmapsource.{h,cpp}`, `roadmapparse.cpp`, `remotecontrol_roadmap_{query,log}.cpp`, `remotecontrol_terminal.cpp`, `roadmapdialog.{h,cpp}` and quoted ANTS-3815 / ANTS-3793 passages | C 1 · H 5 · M 5 · L 5 · I 0 — verified 16 (one in part), dismissed 0 | dim 5×4, dim 4×3, dim 15×3, dim 9×2, dim 1×1, dim 2×1, dim 7×1, dim 10×1 | **Every finding verified against current source before any edit, and the run's centre of gravity was that this document's own arithmetic did not reproduce. (1) CRITICAL: § 2.4's table excepted "the cached-parse path" while § 2.5 said the cache is untouched — read as written, the saving never reached the largest consumer. Re-derived from `remotecontrol_roadmap_query.cpp`: the cache-miss block spends one `readAll()` on *either* the dispatch (`section.isEmpty()`) *or* `RoadmapIndex::buildIndex()` on the `else`, so no site is excepted — the index branch simply calls `full()`. § 2.4 now names the branch and § 2.5 is scoped to the cache's keying and TTL. (2) MEDIUM, and the worst reproducibility defect: § 2.4 claimed the enumeration summed to **25** after "five definitions removed by hand", then listed six names. Re-running it returns **27**, and *no* definitions are removable — the command's leading `(^\|[^:[:alnum:]_])` already excludes every qualified definition, and `RoadmapSource`'s two are written unqualified where the `RoadmapSource::`-prefixed alternatives cannot reach them. The true split is 23 consumer sites plus 4 wrapper-internal pass-throughs; `roadmap_query` is 8, not 6. All six other mentions of "25" swept in the same pass. (3) HIGH ×2, resolved together by a read contract § 2.1 never had: INV-6 (read each byte once) and INV-5 (`full()` is byte-identical to today's read) were jointly unsatisfiable, and INV-5's byte-identity was undefined without an open mode — every current site uses `ReadOnly \| Text`, which is exactly what decides its CRLF fixture. § 2.1 now pins the mode, a retained handle, `full()` continuing from where the prefix stopped, and — closing the MEDIUM TOCTOU finding — states that prefix and body therefore come from one file description. (4) HIGH: § 2.3's "exactly one use of the body" is three in `RoadmapDialog::roadmapBullets()` — an empty root, a null store, and the fall-through past `bulletsFor()`; an implementer converting "the one site" leaves two that do not compile. (5) HIGH: § 2.2's "three wrappers" omitted `storeLegend()`, the fourth `migratedProject()` caller, which INV-1's "any *seam* function" clause does not reach — it is dialog code. Added to the table with the reason it belongs there. (6) HIGH **verified only in part, and the correction matters more than the finding**: the lane said INV-7's grep "returns zero before any change, so the invariant is unfalsifiable". It returns **5**. The real hole is narrower and worse-hidden — line-based `grep` cannot see the four signatures that wrap `const QString &markdown,` onto its own line, which includes `migratedProject()` and `bulletsFor()`, the two functions the item is about. INV-7 is now `rg -U` with a derived before-figure of **12** (six functions × declaration + definition) and 0 after. (7) MEDIUM ×2 on § 4: the RAM ceiling ignored that `QString` is UTF-16, so the prefix costs ~2× its byte count plus ~11 KiB of `QStringList` headers; and its "well under 64 KiB" collided with INV-1's *unrelated* 64 KiB, which bounds **disk bytes** and was asserted with no derivation. INV-1's threshold now derives from the fixture the test builds, and § 4 states the two budgets are different quantities. (8) MEDIUM: § 4 and § 6 restated figures § 4 itself declares live only in § 1 — three times. Both now derive by reference. (9) LOW ×5: § 2.1's "four dispatch-taking functions" read as the whole change set (it is six functions / five symbols); `openFailed()` was undefined for `fromMemory` and for a mid-read error; § 5's "latency, downward" carried no target (now INV-1's byte assertion, stated as *why* a millisecond figure would be worse); INV-2's claim said "the bounded prefix and the whole file classify identically" while its test caps **both** sides — on the after-300-lines fixture both correctly miss the signal, so the claim was false by construction and is now reader-equivalence, with the prefix-vs-whole-file property left where it lives, at `detectionPrefix()`'s definition. **Found by verification rather than by the lane, and the run's best catch:** § 2.4 assumed `storeLegend()`'s `loadRoadmapMarkdown(false)` is `m_roadmapPath`'s bytes. It is not — `loadMarkdown()` reads under an **8 MiB per-file cap** — immaterial for a dispatch that stops inside 300 lines, but a widening for any roadmap past the cap, and now stated instead of assumed. **Cross-doc, and it is where the same error had already spread:** ANTS-3815 § 2.4's closing paragraph named three callers including `RoadmapDialog::storeProjectRoot()` — the identical wrong function this spec's § 1 corrects in the ROADMAP bullet — and claimed "no signature changes" unscoped. Both amended there; § 8 records it. **Nothing deferred, nothing dismissed.** Doc 359 → 532 lines. |
