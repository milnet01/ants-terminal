# ANTS-3833 — Split remotecontrol.cpp into per-family translation units

**Status:** spec draft (2026-08-06).
**Kind:** refactor.
**Source:** ROADMAP.md ANTS-3833 (user-question-2026-08-05; groundwork
measured 2026-08-06).
**Pairs with:** ANTS-1677 (`claudeintegration.cpp` / `mainwindow.cpp` /
`auditdialog.cpp` — the same problem in the sibling files this one does not
touch).

## 1. Problem

`src/remotecontrol.cpp` is one translation unit holding every MCP verb body.
It is the largest source in the tree by a factor of two, and its size is now
the dominant cost of the project's most common edit.

**Layman:** every Claude command lives in one enormous file, so changing one
command makes the computer re-read all of them — about a minute each time.
Cutting the file into eleven keeps each edit under a quarter of that.

Measured 2026-08-06 (`wc -l -c src/remotecontrol.cpp src/remotecontrol.h`):
24,803 lines / 1,174,447 bytes of implementation behind a 1,182-line header.
The ROADMAP bullet's "23,849 LoC / 1.1 MB" was measured on 2026-08-05 and
has since drifted; the figures above supersede it.

Three consequences, in the order they cost time:

1. **A one-verb edit costs a 54.66 s recompile.** Measured with ccache
   bypassed so the figure is real compiler work, not a cache hit:

   ```
   touch src/remotecontrol.cpp && cd build && CCACHE_DISABLE=1 \
     /usr/bin/time -f "wall=%e s  maxrss=%M KB" \
     ninja CMakeFiles/ants_core_lib.dir/src/remotecontrol.cpp.o
   → wall=54.66 s  maxrss=1642524 KB
   ```

   The same command against `src/terminalgrid.cpp` (3,547 lines) returns
   `wall=11.19 s  maxrss=743140 KB`. The 1.6 GB peak matters independently
   of the clock: `CMakeLists.txt`'s `compile_pool` is `max(2, nproc/4)` = 3
   on this 12-core host, and this project has an earlyoom history.

2. **No session can hold the file**, so every change to a verb is a blind
   targeted edit against a file nobody has read end to end.

3. **`ants_core_lib` has a 54.66 s serial pole.** One TU cannot be split
   across pool slots, so a full build waits on it regardless of how many
   slots are free.

The class itself is not the problem. `RemoteControl::dispatch()` is 103
lines routing an op string to a member; the 97 `cmd*` members it routes to
are independent of one another and of everything except the shared helpers
in § 2.3.

## 2. Surface

### 2.1 What moves, and what does not

| | |
|---|---|
| **Moves** | the bodies of `RemoteControl::` members, cut as contiguous slices into new `src/remotecontrol_<family>.cpp` TUs |
| **Does not move** | `src/remotecontrol.h` — no declaration changes, so no consumer recompiles for an API reason |
| **Does not move** | `RemoteControl::dispatch()` and the op→member routing chain, which stay in `src/remotecontrol.cpp` |
| **Does not move** | `src/claudeintegration.cpp` — the tools/list schema block and `kindForName` live there, not here (`grep -n kindForName src/*.cpp` returns four hits, all in `claudeintegration.cpp`) |
| **Not created** | no new class, no new library, no new build target; the TUs join `ants_core_lib` beside the file they came from |

The ROADMAP bullet names "registration order and `kindForName` bucketing" as
a trap. Both are in `claudeintegration.cpp` and are untouched by this item.
The trap that is real is § 2.4.

### 2.2 The eleven translation units

Each TU is **one contiguous slice** of the pre-split file, named by its first
and last `RemoteControl::` member. Contiguity is load-bearing: it is what
lets `ANTS_RC_SOURCES` (§ 2.4) declare an order whose concatenation preserves
every member's pre-split relative position, which two-anchor scrape windows
(`find(A)` then `find(B, posA)`) depend on.

| # | File | First member | Last member | Lines |
|---|---|---|---|---|
| 1 | `remotecontrol.cpp` | *(file head)* | `dispatch` | 2,261 |
| 2 | `remotecontrol_terminal.cpp` | `cmdLs` | `cmdGrabImage` | 1,404 |
| 3 | `remotecontrol_roadmap_query.cpp` | `roadmapStoreOrNull` | `cmdRoadmapLogAppendBatchForTest` | 2,890 |
| 4 | `remotecontrol_changelog.cpp` | `cmdChangelogQuery` | `cmdChangelogLogAddBatch` | 1,157 |
| 5 | `remotecontrol_roadmap_log.cpp` | `cmdRoadmapLogAppend` | `cmdRoadmapLogAppendBatch` | 4,885 |
| 6 | `remotecontrol_workspace.cpp` | `cmdWorkspaceSearch` | `cmdCodebaseIndex` | 1,789 |
| 7 | `remotecontrol_docs.cpp` | `cmdDocsIndex` | `cmdProjectSettings` | 960 |
| 8 | `remotecontrol_feedback.cpp` | `rlResolveForeignFeedbackIds` | `cmdAuditDismiss` | 2,334 |
| 9 | `remotecontrol_state.cpp` | `cmdGitState` | `cmdSimilarCode` | 3,336 |
| 10 | `remotecontrol_review.cpp` | `cmdIndieReviewPartition` | `cmdTokenUsage` | 1,975 |
| 11 | `remotecontrol_coldeyes.cpp` | `cmdColdEyesPartition` | `cmdTestResults` | 1,812 |

Sums to 24,803 — the whole file, no residue. Line counts are the pre-split
slice sizes; each TU gains an include preamble on top.

Two boundaries are named for what they contain rather than what the family
label suggests, because contiguity wins over tidiness:

- TU 3 carries `cmdRoadmapLog`'s op router and its seven `*ForTest` seams
  (each under 40 lines, all thin routers) ahead of the write bodies in TU 5.
- TU 1 is not a family. It is the file head — includes, the shared helper
  block, the constructor, `start`, `onNewConnection`, `dispatch`.

**Every seam lands outside an anonymous-namespace block.** Verified against
the 24 `namespace {` / `}  // namespace` pairs in the file: the ten seam
start lines (2262, 3666, 6556, 7713, 12598, 14387, 15347, 17681, 21017,
22992) each fall in open code. TU 8 wholly contains the delicate region at
`namespace ants {` where an anonymous namespace is closed early and reopened,
so no seam crosses it.

Each TU opens with an ordinal marker on its first line, which is what INV-3
checks:

```cpp
// ANTS-3833 TU 5/11 — roadmap_log write ops.
```

### 2.3 Shared helpers: `src/remotecontrol_internal.h`

The pre-split file's head anonymous namespace holds 59 free functions, 12
constants and 2 structs. Most are roadmap/changelog helpers that move with
TU 3–5. A minority are referenced across TU boundaries and must survive the
split with external linkage.

**Rule:** a helper referenced by more than one TU is declared in
`src/remotecontrol_internal.h` inside `namespace rcdetail`, keeping its
name and signature verbatim. Its **definition stays in a `.cpp`** — never
moves into the header — so a scrape asserting that a helper is defined in
the RemoteControl source still finds it (§ 2.4).

**Derivation, not a hand-written list.** The promotion set is computed after
the slices are cut, per helper:

```bash
# For each helper name H, the set of TUs referencing it:
grep -l "\bH\b" src/remotecontrol*.cpp
```

More than one file → promote. The header records the resulting list; the
spec records the rule, because the list is an artifact that will change the
first time a verb moves.

The linker enforces the direction that matters: a cross-TU reference to a
helper still holding internal linkage fails to link. Over-promotion (a
helper in the header used by one TU) costs nothing but tidiness and is not a
contract.

Sample of what the measurement already shows will promote —
`resolveRootCanonical`, `findRoadmapUnder`, `rcSetWriteBytes`,
`rcScrubLeakedToolXml`, `kUnrecognisedFormatExpected` — against
`rcApplyHeadlineOnly` / `rcClipMatchBytes` / `rcClipMatchTextFields`, which
are used only by `cmdWorkspaceSearch` and move into TU 6 unpromoted.
`resolveRootCanonical` is the widest: it appears in **nine of the eleven
slices** (all but TU 3 and TU 4), counted by running
`sed -n '<first>,<last>p' src/remotecontrol.cpp | grep -c resolveRootCanonical`
over each slice range in § 2.2.

### 2.4 The scrape seam — the real blast radius

**130 test files and 303 call sites read this file as text**, not the 72 the
ROADMAP bullet names. The bullet counted files carrying the literal string
`remotecontrol.cpp`; most tests reach the file through a CMake `-D` path
macro instead.

```bash
# 130 — union of literal-path and macro-path users
{ grep -rl 'src/remotecontrol\.cpp'          tests/ --include='*.cpp'
  grep -rl '\bSRC_REMOTECONTROL_CPP_PATH\b'  tests/ --include='*.cpp'
  grep -rl '\bSRC_RC_CPP\b'                  tests/ --include='*.cpp'; } | sort -u | wc -l

# 303 — call sites (282 + 21)
grep -rc 'slurpFile(SRC_REMOTECONTROL_CPP_PATH)' tests/ --include='*.cpp'
grep -rc 'slurpFile(SRC_RC_CPP)'                 tests/ --include='*.cpp'
```

The bullet's other stated trap — fixed byte windows — is **reduced but not
gone**, and the difference matters. `tests/_support/srcgrep.h` bounds regions
structurally (`slurpFunctionBody` brace-matches; `mcpToolDescriptor` runs to
the next registration), which ANTS-3681 / ANTS-3720 introduced for exactly
this failure mode — but that work did not sweep every existing window:

```bash
# 47 test files still take a numeric-length substr window
grep -rlE '\.substr\([A-Za-z_][A-Za-z0-9_]*, *[0-9]{2,}\)' tests/ --include='*.cpp' | wc -l
# 25 of those also read remotecontrol text
grep -lE 'SRC_REMOTECONTROL_CPP_PATH|SRC_RC_CPP|src/remotecontrol\.cpp' <those 47> | wc -l
```

**Concatenation is what makes those 25 safe, and it is exact rather than
approximate.** `slurpRemoteControl()` returns the original text with a
preamble inserted at each of the ten seams. A window `[A, A+N)` therefore
reads byte-identical content to pre-split *unless a seam falls inside it* —
so the whole risk reduces to one enumerable check (§ 6, migration-time),
not to an audit of 25 tests.

The seams were chosen with this in mind. The seven distinct anchors those
windows use against remotecontrol text — `RemoteControl::start()`,
`cmdSetTitle`, `cmdReadRegions`, `cmdLastAuditSummary`,
`cmdIndieReviewOrchestrate`, `cmdRoadmapBranchDrift`, and a
`sparse_partition_hint` offset — are all **mid-slice**; none anchors on a
TU-last member, which is the only position from which an ordinary window
could reach a seam. Where a window did span one, the failure is loud: it
would read an include preamble instead of code and redden a must-contain
assertion.

Beyond that, the coupling that survives is a single question: *which file is
"the RemoteControl source"?* Three changes settle it.

**(a) One list macro replaces the four path macros.** `CMakeLists.txt`
defines the TU list once and passes it, semicolon-separated, to each of the
four bundles that carry a path macro today (`test_core`, `test_claude`,
`test_audit`, `test_dialogs`):

```cmake
# ANTS-3833 — split order. The concatenation of these files, in this order,
# preserves the pre-split relative order of every RemoteControl member.
set(ANTS_RC_SOURCE_LIST
    src/remotecontrol.cpp
    src/remotecontrol_terminal.cpp
    # … TU 3 … TU 11, in slice order
)
```

**(b) `SRC_REMOTECONTROL_CPP_PATH` and `SRC_RC_CPP` are deleted.** This is
the point of the design. A scrape that reads one TU sees a fraction of the
class, in which a verb that moved to a sibling TU is indistinguishable from a
verb that was deleted — a silent, plausible, green-or-red-for-the-wrong-reason
failure. Deleting the macros converts every missed call site into a **compile
error**. There is no partial-scrape state to reach.

**(c) `tests/_support/srcgrep.h` gains one helper**, and the 303 sites become
a mechanical substitution to it:

```cpp
// ANTS-3833 — the RemoteControl implementation is N translation units. A
// scrape that reads one of them reads a fraction of the class. Read them
// all, in ANTS_RC_SOURCES order (the order they were cut from the original
// file), so two-anchor windows still see A before B.
inline std::string slurpRemoteControl();
```

`slurpFile(SRC_REMOTECONTROL_CPP_PATH)` → `ants_test::slurpRemoteControl()`,
and likewise for `SRC_RC_CPP`. Nothing else about a scrape changes:
`countOccurrences` totals are preserved by concatenation, negative
assertions ("must not include X") are preserved because no TU gains X, and
`slurpFunctionBody` anchors resolve as before.

The 57 test files that `#include "remotecontrol.h"` are a compile-time
dependency on an unchanged header and need no edit.

### 2.5 Git history

The cut lands as **one pure-motion commit**: no reformatting, no renames, no
edits to the moved lines. `git log -M -C` can then follow a verb across the
split. Any change that is not motion — the include preambles, the `rcdetail`
promotion, the CMake and test edits — lands in separate commits before and
after it.

## 3. Invariants

- **INV-1** — `src/remotecontrol.h` is byte-identical across the migration.
  The split changes no declaration, so no consumer recompiles for an API
  reason. *Test:* `git diff <pre>..<post> -- src/remotecontrol.h` is empty.
- **INV-2** — Every `RemoteControl::` member defined in the pre-split file is
  defined exactly once across the post-split TUs; none is added or removed.
  *Breaks when* a slice boundary drops or duplicates a member. *Test:*
  `grep -ohE 'RemoteControl::[A-Za-z0-9_]+\(' <sources> | sort | uniq -c`
  compares equal between the pre-split file and the concatenated TUs.
- **INV-3** — `ANTS_RC_SOURCES` lists the TUs in slice order, so the
  concatenation preserves every member's pre-split relative position.
  *Breaks when* a TU is appended to the list rather than inserted at its
  slice position — which silently reorders two-anchor scrape windows.
  *Test:* `tests/features/rc_tu_split/` asserts the eleven `TU k/11` ordinal
  markers appear at ascending offsets in `slurpRemoteControl()`.
- **INV-4** — No build target defines a macro naming a single remotecontrol
  TU as the RemoteControl source. *Breaks when* a bundle keeps
  `SRC_REMOTECONTROL_CPP_PATH` or `SRC_RC_CPP`, letting a scrape read a
  fraction of the class and read a moved verb as a deleted one. *Test:*
  `grep -rl 'SRC_REMOTECONTROL_CPP_PATH\|SRC_RC_CPP' CMakeLists.txt tests/ | wc -l`
  is 0, and the suite builds.
- **INV-5** — `src/remotecontrol_internal.h` is included only by
  `src/remotecontrol*.cpp`. *Breaks when* a sibling subsystem takes a
  dependency on a helper that was never API, making the next split a
  cross-subsystem change. *Test:* `tests/features/rc_tu_split/` — the
  including-file set is exactly the `remotecontrol*.cpp` set.
- **INV-6** — No TU in `ANTS_RC_SOURCES` exceeds 6,000 lines. *Breaks when*
  verbs accrete into one TU until it is the old file again — the regression
  this whole item exists to prevent, and the only one that returns silently.
  *Test:* `tests/features/rc_tu_split/` counts lines per listed source.
- **INV-7** — No helper retains internal linkage while being referenced from
  another TU. *Breaks when* a promoted helper is left `static` or inside an
  anonymous namespace. *Test:* `ants_core_lib` links (the linker is the
  check; an unresolved `rcdetail::` reference fails the build).
- **INV-8** — `RemoteControl::dispatch()` stays in `src/remotecontrol.cpp`
  and its op→member routing chain is byte-identical across the migration.
  *Breaks when* a slice takes routing with it, changing which op reaches
  which verb. *Test:* `slurpFunctionBody(rc, "RemoteControl::dispatch")`
  compares equal pre- and post-split.
- **INV-9** — The suite is green with no test-source change other than the
  `slurpFile(SRC_…)` → `slurpRemoteControl()` substitution. *Breaks when* the
  split changes observable behaviour — an assertion edited to accommodate the
  refactor is the signal. *Test:* `ctest --preset=default` green, and
  `git diff --stat tests/` reviewed against the substitution list; any other
  test edit is named with its reason in the commit message.
- **INV-10** — No seam offset falls inside a fixed-byte scrape window. A
  window `[A, A+N)` over `slurpRemoteControl()` reads byte-identical content
  to the pre-split file exactly when no preamble is inserted within it.
  *Breaks when* a seam is placed mid-window, so the window reads an include
  preamble instead of code. *Test:* for each of the 25 window-taking tests
  (§ 2.4), resolve its anchor offset in `slurpRemoteControl()` and assert no
  seam offset lies in `[A, A+N)`; migration-time, run once against the
  post-split tree.

## 4. RAM / build cost

**No runtime cost.** No new state, no new allocation, no new external
library, no new build target — the eleven TUs join `ants_core_lib`.

**Compile cost, measured** (commands in § 1):

| | Lines | Wall | Peak RSS |
|---|---|---|---|
| `remotecontrol.cpp` today | 24,803 | 54.66 s | 1,642,524 KB |
| `terminalgrid.cpp` (reference) | 3,547 | 11.19 s | 743,140 KB |

**Projected, not measured** — a two-point linear fit over those rows gives
≈ 3.9 s + 0.00205 s/line and ≈ 593 MB + 0.0423 MB/line. For the largest
post-split TU (#5, 4,885 lines) that is **≈ 14 s and ≈ 800 MB**. Two points
across two different files is a weak model; the build plan re-measures with
the § 1 command and records the real figure here.

**Full-build effect.** `compile_pool` is **3** on this 12-core host —
`CMakeLists.txt` sets `_ANTS_COMPILE_CAP` to `nproc / 4`, floored at 2, for
hosts with four or more cores — and a single TU cannot use more than one
slot, so today's 54.66 s is a serial pole
on the critical path. After the split the pole is the largest TU and the
other ten overlap with it and with unrelated work. Total CPU rises — eleven
include preambles instead of one — while the pole falls roughly four-fold.

**PCH and unity are already correct for this.** `ants_apply_qt_pch()` is
applied to `ants_core_lib`, so each new TU reuses the library's existing
precompiled Qt headers rather than reparsing them, which is what keeps the
per-TU fixed cost near the ~3.9 s intercept. `ants_core_lib` is in
`_ants_subset_linked_libs`, which sets `UNITY_BUILD OFF`, so an
`ANTS_UNITY_BUILD=ON` build cannot re-batch the eleven TUs back into one.

**AUTOMOC is unchanged.** `Q_OBJECT` is in `remotecontrol.h`; moc runs on the
header, so the split adds no moc work.

## 5. Out of scope

- **Splitting `src/remotecontrol.h` (1,182 lines)** — permanent exclusion,
  not deferred. It is the class API; 57 test files and every consumer include
  it, and splitting it buys no compile time because the declarations are
  needed wherever the class is used. It is also what INV-1 pins.
- **Extracting pure engines per verb** — the established
  `speclint.cpp` / `readregion.cpp` / `docdedup.cpp` pattern (ANTS-3660..3665
  and others) that leaves a thin verb body behind. Permanent exclusion *for
  this id*, not a rejection: it continues per-verb as verbs are touched. This
  item only decides which TU a body lives in, and composes with that work
  rather than duplicating or blocking it.
- **`src/claudeintegration.cpp`** (12,822 lines — the tools/list schemas,
  `kindForName`, the dispatch prelude) — deferred, tracked by **ANTS-1677**.
- **Any change to verb behaviour, response envelopes or refusal codes.**
  INV-9 is what holds this line.
- **`tests/coverage-map.json`** — deliberately unchanged. Its `_note` records
  that cross-cutting files including `remotecontrol.cpp` are unlisted by
  design so `focused_test` falls back to the full suite; the new TUs inherit
  that fallback with no edit.

## 6. Tests

Feature test: `tests/features/rc_tu_split/`. Covers INV-3, INV-4, INV-5,
INV-6. Label `features;fast`. Source added to **`test_core`**'s `SOURCES`
list — the test only reads files and needs Qt6::Core only — never as an
`add_executable` (see `tests/features/README.md`).

| Case | Invariant | Asserts |
|---|---|---|
| `TuOrdinalMarkersAscend` | INV-3 | the eleven `ANTS-3833 TU k/11` markers appear at ascending offsets in `slurpRemoteControl()` |
| `NoSingleTuPathMacro` | INV-4 | neither retired macro name appears in `CMakeLists.txt` or under `tests/` |
| `InternalHeaderStaysInternal` | INV-5 | the set of files including `remotecontrol_internal.h` is exactly the `remotecontrol*.cpp` set |
| `NoTuExceedsLineCap` | INV-6 | every source in `ANTS_RC_SOURCES` is ≤ 6,000 lines |

Must-fail-first, per the project convention — each case is verified RED
before the corresponding fix is in place:

- `TuOrdinalMarkersAscend` — against an `ANTS_RC_SOURCE_LIST` with two
  entries transposed.
- `NoSingleTuPathMacro` — against the pre-split `CMakeLists.txt`.
- `InternalHeaderStaysInternal` — against a scratch `#include
  "remotecontrol_internal.h"` added to an unrelated `src/*.cpp`.
- `NoTuExceedsLineCap` — against the pre-split `remotecontrol.cpp` (24,803
  lines) in the list.

**INV-7 has no case here and needs none** — its surface is that
`ants_core_lib` links, which every build already runs. A helper left with
internal linkage while another TU references it is an unresolved symbol, and
no test can be redder than a failed link.

INV-1, INV-2, INV-8, INV-9 and INV-10 are **migration-time checks with no
permanent surface** — they compare the pre- and post-split trees, a pairing
that exists only during the migration. Each is a recorded command run against
the migration commits (§ 3), and the build plan owns running them; none is a
standing test, and inventing one would be a test that can never fail again.

## 7. Cross-doc impact

| Doc | Change |
|---|---|
| `docs/subsystems.md` | the `remotecontrol` lane entry lists the eleven TUs — the lane catalogue `subsystem` and `indie_review_partition` derive from |
| `CHANGELOG.md` | one `Changed` entry |
| `CLAUDE.md` | none — the module map moved to `docs/subsystems.md` under ANTS-1292 |
| `README.md` / `PLUGINS.md` | none — no user-visible behaviour, no Lua surface change |
| `docs/standards/mcp-tools.md` | none — no contract changes; where a verb body lives is not part of the authoring checklist |

## Cold-eyes loop log

| Loop | Reviewer | Findings (C/H/M/L/I) | Outcome |
|---|---|---|---|
