# ANTS-3833 — Split remotecontrol.cpp into per-family translation units

**Status:** spec draft (2026-08-06).
**Kind:** refactor.
**Source:** ROADMAP.md ANTS-3833 (user-question-2026-08-05; groundwork
measured 2026-08-06).
**Pairs with:** ANTS-1677 (`claudeintegration.cpp` / `mainwindow.cpp` /
`auditdialog.cpp` — the same problem in the sibling files this one does not
touch).

**Contents** — [1. Problem](#1-problem) · [2. Surface](#2-surface)
([2.1 What moves](#21-what-moves-and-what-does-not) ·
[2.2 The eleven TUs](#22-the-eleven-translation-units) ·
[2.3 Shared helpers](#23-shared-helpers-srcremotecontrol_internalh) ·
[2.4 The scrape seam](#24-the-scrape-seam--the-real-blast-radius) ·
[2.5 Git history](#25-git-history)) ·
[3. Invariants](#3-invariants) · [4. RAM / build cost](#4-ram--build-cost) ·
[5. Out of scope](#5-out-of-scope) · [6. Tests](#6-tests) ·
[7. Cross-doc impact](#7-cross-doc-impact)

## 1. Problem

`src/remotecontrol.cpp` is one translation unit holding every MCP verb body.
It is the largest source in the tree by a factor of two, and its size is now
the dominant cost of the project's most common edit.

**Layman:** every Claude command lives in one enormous file, so changing one
command makes the computer re-read all of them — about a minute each time.
Cutting the file into eleven brings a typical edit down to roughly a quarter
of that.

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
   of the clock: `CMakeLists.txt` derives `compile_pool` as `nproc / 4`,
   floored at 2 for hosts with four or more cores — **3** here — and this
   project has an earlyoom history. § 4 carries the post-split concurrent
   figure, which is the one that decides whether the split helps or hurts
   on that front.

2. **No session can hold the file**, so every change to a verb is a blind
   targeted edit against a file nobody has read end to end.

3. **`ants_core_lib` has a 54.66 s serial pole.** One TU cannot be split
   across pool slots, so a full build waits on it regardless of how many
   slots are free.

The class itself is not the problem. `RemoteControl::dispatch()` is 103
lines routing an op string to a member. The file defines 97 distinct `cmd*`
members (`grep -oE 'RemoteControl::cmd[A-Za-z0-9_]+' src/remotecontrol.cpp |
sort -u | wc -l`) — **not all of them op-routed**: seven `cmdRoadmapLog*ForTest`
seams and `cmdVerifyChangesImpl` / `cmdVerifyChangesWithRoot` are called from
other members, not from `dispatch`. Routed or not, they are independent of one
another and of everything except the shared helpers in § 2.3.

## 2. Surface

### 2.1 What moves, and what does not

| | |
|---|---|
| **Moves** | the bodies of `RemoteControl::` members, cut as contiguous slices into new `src/remotecontrol_<family>.cpp` TUs |
| **Does not move** | `src/remotecontrol.h` — no declaration changes, so no consumer recompiles for an API reason |
| **Does not move** | `RemoteControl::dispatch()` — its own body and the op→member routing chain inside it — which stays in `src/remotecontrol.cpp`. A verb's *inner* op router (`cmdRoadmapLog`, `cmdGitState`) is a verb body and moves with its family; INV-8 is scoped to `dispatch` alone |
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

- TU 3 carries `cmdRoadmapLog`'s op router and **six** of the seven
  `cmdRoadmapLog*ForTest` seams (each under 40 lines, all thin routers) ahead
  of the write bodies in TU 5. The seventh, `cmdRoadmapLogBundleRowForTest`,
  sits in TU 5 immediately before `cmdRoadmapLogBundleRow`.
- TU 1 is not a family. It is the file head — includes, **the shared helper
  block (§ 2.3)**, the constructor, `start`, `onNewConnection`, `dispatch`.
  It is therefore also the TU that owns every promoted helper's definition.

**Every seam lands outside an anonymous-namespace block.** This is the one
empirical claim the whole cut rests on, so it is established by a
**brace-aware scan** — one that tracks nesting depth and skips string
literals, character literals and both comment forms — and **not** by matching
`^namespace \{` / `^\}  // namespace` at column zero. A column-anchored grep
counts openings it can see and closings that happen to be commented, cannot
pair them, and cannot see an indented or nested opening at all; two
independent readers reached opposite conclusions from it. The scan reports
**24 anonymous namespaces**, and their extents are:

```
  112..  1926   7370.. 7474  12503..12551  14037..14328  16875..16959
17046.. 17505  17817..17900  18066..18288  18676..18725  19511..19637
19905.. 20023  20619..20704  20865..20922  21005..21015  21230..21252
21694.. 21719  21898..21932  22268..22308  22314..22462  22763..22791
22937.. 22990  23791..23816  24251..24279  24600..24610
```

Against that map, all ten seam start lines (2262, 3666, 6556, 7713, 12598,
14387, 15347, 17681, 21017, 22992) fall in open code — nesting depth zero,
zero seams inside an anonymous namespace. Two regions that look dangerous and
are not: TU 8 (15347–17680) wholly contains the close-early / reopen pair
around `namespace ants {` (16875–16959, then 17046–17505), and TU 11's seam
at 22992 clears the 22937–22990 block by two lines.

**A source comment misreads this same region and must not be trusted as
evidence.** `src/remotecontrol.cpp`'s closing brace at line 16959 is annotated
"*anonymous from line 1320*"; the scan puts that block's opening at 16875. The
comment is stale, it is the thing that made two readers believe seven seams
were unsafe, and correcting it is code — out of scope here, filed instead
(§ 7).

Each TU opens with an ordinal marker on its first line, which is what INV-3
checks. `N` is the TU count declared by `ANTS_RC_SOURCES`, not the literal 11
— a twelfth TU renumbers all of them, which is deliberate: the marker set and
the list must agree or INV-3 fails.

```cpp
// ANTS-3833 TU 5/11 — Roadmap write ops.
```

The marker carries **no snake_case identifier** — not `roadmap_log`, not a
refusal code, not a config key. § 2.4 explains why: the marker text joins the
concatenation every scrape reads, so a verb name inside it would inflate that
verb's occurrence count.

**Where a new verb goes.** Into the TU owning its family, at the end of that
TU's slice — never into `remotecontrol.cpp`, which is the dispatcher and the
helper pool. When a family TU would pass INV-6's 6,000-line cap, split it and
renumber every marker. This rule is what keeps INV-6 from being the only thing
standing between the tree and a second 24,803-line file.

### 2.3 Shared helpers: `src/remotecontrol_internal.h`

The pre-split file's head anonymous namespace (lines 112–1926) holds 59
symbols: free functions, 12 constants and 2 structs.

**They do not move, and that follows from § 2.2 rather than from preference.**
The block sits at 112–1926, wholly inside TU 1's slice (1–2,261), and every
TU is one contiguous slice — so the helpers stay in `src/remotecontrol.cpp`
by construction. TU 1 is the dispatcher **and** the shared helper pool. There
is no arrangement in which a helper "moves with its family": doing so would
make TU 1 non-contiguous and void INV-3, INV-10 and § 2.5's pure-motion
commit at once.

**What that costs is linkage, and the cost is large.** 52 of the 59 symbols
are referenced beyond line 2,261 — 88%, not a minority — so almost the whole
block needs external linkage:

```bash
# → total=59 promoted=52 ; $HEADSYMS is the head block's symbol list
tot=0; out=0
while read -r s; do
  tot=$((tot+1))
  n=$(grep -nE "\b${s}\b" src/remotecontrol.cpp | awk -F: '$1>2261' | wc -l)
  [ "$n" -gt 0 ] && out=$((out+1))
done < "$HEADSYMS"
echo "total=$tot promoted=$out"
```

The seven that stay internal are used only within the head block itself:
`rcClipMatchBytes`, `rcElideBody`, `rcEscapeUnclosedFence`,
`rcExtractBoldId`, `rcExtractCaretAnchor`, `rlLeafDirPrefix`,
`rxAntsV1IdBracket`.

**Rule:** a symbol referenced by more than one TU is declared in
`src/remotecontrol_internal.h` inside `namespace rcdetail`, keeping its name
and signature verbatim. Its **definition stays in a `.cpp`** — never moves
into the header — so a scrape asserting that a helper is defined in the
RemoteControl source still finds it (§ 2.4). Two forms, because the block is
not all functions:

| Kind | Declaration in the header | Definition |
|---|---|---|
| function | `QString rcNormaliseHeadline(QStringView);` | stays in `remotecontrol.cpp` |
| constant | `inline constexpr` in the header — never `extern const`, which would lose constant-expression usability at the call sites that need it | in the header, by definition of `inline constexpr` |

The constant row is the one exception to "definitions stay in a `.cpp`", and
it is forced: `kIdPrefixShape` and `kUnrecognisedFormatExpected` are used
where a constant expression is required.

**Derivation, not a hand-written list.** The promotion set is computed after
the slices are cut, per symbol — **against `ANTS_RC_SOURCES` membership, never
a `src/remotecontrol*.cpp` glob**, which also matches `src/remotecontrolgate.cpp`
(a separate `ants_core_lib` source that is not a TU of this class and must not
include the internal header):

```bash
# the TUs referencing symbol H, from the declared list rather than a glob
grep -l "\bH\b" $(tr ';' ' ' <<< "$ANTS_RC_SOURCES")
```

More than one file → promote. The header records the resulting list; the spec
records the rule, because the list is an artifact that changes the first time
a verb moves. Over-promotion (a symbol in the header used by one TU) costs
tidiness, not correctness, and is not a contract. The linker enforces the
direction that matters: a cross-TU reference to a symbol still holding
internal linkage fails to link (INV-7).

**This promotion edits text in place, which INV-10 depends on knowing.** Every
promoted definition loses its anonymous-namespace enclosure and gains
`rcdetail` qualification, so the post-split concatenation is *not* the
pre-split bytes plus seam preambles. § 2.5 keeps the promotion in its own
commit for exactly this reason, and INV-10 is evaluated against the
pure-motion commit alone.

### 2.4 The scrape seam — the real blast radius

**130 test files and 330 macro uses reach this file**, not the 72 the ROADMAP
bullet names. The bullet counted files carrying the literal string
`remotecontrol.cpp`; most tests reach the file through a CMake `-D` path macro
instead.

```bash
# 130 — union of literal-path and macro-path users
{ grep -rl 'src/remotecontrol\.cpp'          tests/ --include='*.cpp'
  grep -rl '\bSRC_REMOTECONTROL_CPP_PATH\b'  tests/ --include='*.cpp'
  grep -rl '\bSRC_RC_CPP\b'                  tests/ --include='*.cpp'; } | sort -u | wc -l

# the use forms, and how many of each
grep -rhoE '[A-Za-z_]+\((SRC_REMOTECONTROL_CPP_PATH|SRC_RC_CPP)\b' \
  tests/ --include='*.cpp' | sort | uniq -c
```

**Seven forms, and they do not all want the same thing** — which is the fact
that shapes the rest of this section. Three routes, decided by what the site
is actually asking for:

| Use form | Count | What it wants | Route |
|---|---|---|---|
| `slurpFile(SRC_REMOTECONTROL_CPP_PATH)` | 282 | the class's text | `slurpRemoteControl()` |
| `slurpFile(SRC_RC_CPP)` | 21 | the class's text | `slurpRemoteControl()` |
| `slurp(SRC_REMOTECONTROL_CPP_PATH)` | 12 | the class's text | `slurpRemoteControl()` |
| `readSource(SRC_RC_CPP)` | 5 | the class's text | `slurpRemoteControl()` |
| `QStringLiteral(SRC_REMOTECONTROL_CPP_PATH)` | 3 | one as text, two as a **dirname anchor** | split per site |
| `QFileInfo(SRC_REMOTECONTROL_CPP_PATH)` | 2 | `src/`'s **directory**, to locate a sibling file | `ANTS_RC_SRC_DIR` |
| `#if defined(SRC_REMOTECONTROL_CPP_PATH)` | 5 | proof the bundle carries the source-path defs | `defined(ANTS_RC_SOURCES)` |
| **total** | **330** | | |

The last three rows are why "substitute one call for another" is not the whole
change. `tests/features/mcp_path_anchor/` computes
`QFileInfo(SRC_REMOTECONTROL_CPP_PATH).absoluteDir().filePath("pathvalidation.h")`
— it never reads remotecontrol at all; it is using the macro as a handle on
`src/`. Those sites get `ANTS_RC_SRC_DIR`, a new definition carrying the source
directory, which is what they meant. The five `#if defined` guards assert their
bundle was given the source-path definitions and re-point at `ANTS_RC_SOURCES`.

The bullet's other stated trap — fixed byte windows — is **reduced but not
gone**, and the difference matters. `tests/_support/srcgrep.h` bounds regions
structurally (`slurpFunctionBody` brace-matches; `mcpToolDescriptor` runs to
the next registration). `mcpToolDescriptor` is ANTS-3720's;
`slurpFunctionBody` predates it (ANTS-1348, after a 2,500-char window broke on
`cmdGetText`'s growth); **ANTS-3681** is the sweep that replaced fixed-byte
windows with structural bounds — and that sweep did not reach every existing
window:

```bash
# 47 test files still take a numeric-length substr window
grep -rlE '\.substr\([A-Za-z_][A-Za-z0-9_]*, *[0-9]{2,}\)' tests/ --include='*.cpp' | wc -l
# 25 of those also read remotecontrol text
grep -lE 'SRC_REMOTECONTROL_CPP_PATH|SRC_RC_CPP|src/remotecontrol\.cpp' \
  $(grep -rlE '\.substr\([A-Za-z_][A-Za-z0-9_]*, *[0-9]{2,}\)' tests/ --include='*.cpp') | wc -l
```

**Concatenation is what makes those 25 tractable.** Across the pure-motion
commit alone (§ 2.5), `slurpRemoteControl()` returns the pre-split text with a
preamble inserted at each of the **eleven** TU heads — ten of them interior to
the old file, one at offset 0. A window `[A, A+N)` therefore reads
byte-identical content to pre-split *unless an insertion point falls inside
it*, so the risk reduces to one enumerable check (INV-10) rather than an audit
of 25 tests. The `rcdetail` promotion commit edits text elsewhere and is
verified separately (§ 2.3).

**What decides reach is window length against distance-to-seam, not where the
anchor sits.** The seven anchors those windows use against remotecontrol text
are `RemoteControl::start()`, `cmdSetTitle`, `cmdReadRegions`,
`cmdLastAuditSummary`, `cmdIndieReviewOrchestrate`, `cmdRoadmapBranchDrift`,
and a `find("sparse_partition_hint", fn)` offset. None is a TU-last member,
which helps, but it does not settle anything on its own: `RemoteControl::start()`
is at line 1961 and the nearest seam is 2262, roughly 300 lines away, and the
observed window lengths run to five digits. INV-10 is the check; this paragraph
is not a substitute for it.

**And the failure is not reliably loud**, which is why INV-10 is mandatory
rather than advisory. A *must-contain* assertion whose window slid onto an
include preamble goes red — visible. But a **negative** assertion ("this
region must not mention X") and a `countOccurrences` total over a shifted
window both stay **green while testing nothing**. Those are the classes INV-10
exists for.

Beyond that, the coupling that survives is a single question: *which file is
"the RemoteControl source"?* Three changes settle it.

**(a) Two new definitions replace the four path macros.** `CMakeLists.txt`
builds the TU list once and passes both definitions to each of the four
bundles that carry a path macro today — `test_claude` (≈ line 1833),
`test_audit` (≈ 2041), `test_dialogs` (≈ 2160) and `test_core` (≈ 2463):

```cmake
# ANTS-3833 — split order. The concatenation of these files, in this order,
# preserves the pre-split relative order of every RemoteControl member.
set(ANTS_RC_SOURCES_REL
    src/remotecontrol.cpp
    src/remotecontrol_terminal.cpp
    # … TU 3 … TU 11, in slice order
)
list(TRANSFORM ANTS_RC_SOURCES_REL PREPEND "${CMAKE_SOURCE_DIR}/"
     OUTPUT_VARIABLE _ants_rc_abs)
list(JOIN _ants_rc_abs ";" _ants_rc_joined)

# …then, per bundle:
target_compile_definitions(<bundle> PRIVATE
    ANTS_RC_SOURCES="${_ants_rc_joined}"
    ANTS_RC_SRC_DIR="${CMAKE_SOURCE_DIR}/src")
```

**One name, used everywhere.** The CMake list variable is
`ANTS_RC_SOURCES_REL`; the **C++-visible macro is `ANTS_RC_SOURCES`**, and it
is that macro every invariant and test in this spec names. Its contract:

| | |
|---|---|
| Type | string literal |
| Content | absolute paths to every TU, in slice order |
| Separator | `;` — no spaces, no trailing separator |
| Guarantee | at least one entry; entry *k* is TU *k* |

**(b) `SRC_REMOTECONTROL_CPP_PATH` and `SRC_RC_CPP` are deleted.** This is
the point of the design. A scrape that reads one TU sees a fraction of the
class, in which a verb that moved to a sibling TU is indistinguishable from a
verb that was deleted — a silent, plausible, green-or-red-for-the-wrong-reason
failure. Deleting the macros converts every missed call site into a **compile
error**. There is no partial-scrape state to reach.

Deletion is only safe once the nine non-text-read uses in the table above
(two of the three `QStringLiteral` sites, both `QFileInfo` sites, all five
`#if defined` guards) have
been re-routed to `ANTS_RC_SRC_DIR` and `defined(ANTS_RC_SOURCES)`, so the
build plan does that **before** the delete, not after. A `#if defined` guard
left pointing at a deleted macro fires its own `#error` — loud, but it stops
the bundle rather than the site.

**(c) `tests/_support/srcgrep.h` gains one helper**, and the 321 text-read
sites — the four pure-text forms (320) plus the one `QStringLiteral` site that
feeds `slurpAbsolute` — become a mechanical substitution to it:

```cpp
// ANTS-3833 — the RemoteControl implementation is eleven translation units.
// A scrape that reads one of them reads a fraction of the class: a verb that
// moved to a sibling TU is indistinguishable from a verb that was deleted.
// Reads every path in ANTS_RC_SOURCES, in that order — the order they were
// cut from the original file — joined with a newline, so a two-anchor window
// (find(A), then find(B, posA)) still sees A before B. A path that cannot be
// opened contributes "", matching slurpFile's behaviour rather than aborting
// the shared bundle.
inline std::string slurpRemoteControl();
```

`slurpFile(SRC_REMOTECONTROL_CPP_PATH)` → `ants_test::slurpRemoteControl()`,
and likewise for the `slurp(…)`, `readSource(…)` and `slurpFile(SRC_RC_CPP)`
forms. `slurpFunctionBody` anchors resolve as before.

**Two preamble contents can shift a count, and only one is controllable.**
The ordinal marker's own text is: `// ANTS-3833 TU 5/11 — roadmap_log write
ops.` would add an occurrence of `roadmap_log` to the concatenation. **The
marker therefore carries no snake_case identifier** — no verb name, op name,
refusal code or config key — which is why § 2.2's form reads *"Roadmap write
ops"*. The per-TU `#include` lines are not controllable: eleven preambles
duplicate include text that appeared once. No `countOccurrences` assertion
over include text is known to exist, and **INV-9's green suite is what
establishes that**, not this paragraph.

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
  *Breaks when* a slice boundary drops or duplicates a member. *Test:* the
  **definition-anchored** list below returns the same 123 names, each exactly
  once, from the pre-split file and from the post-split TUs:

  ```bash
  grep -ohE '^[A-Za-z_][A-Za-z0-9_:<>,& *]*RemoteControl::[A-Za-z0-9_]+\(' \
       "$@" | sed -E 's/.*(RemoteControl::[A-Za-z0-9_]+)\(/\1/' | sort | uniq -c
  ```

  The anchor is load-bearing. The unanchored form
  (`grep -ohE 'RemoteControl::[A-Za-z0-9_]+\('`) counts every **mention** —
  134 against 123 on the current file — so it is equal by construction under
  pure motion and would stay equal if a definition were duplicated and a call
  site removed. A test that cannot distinguish those is not testing INV-2.
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

  the command below **returns 0** post-split, and returns 136 against the
  current tree:

  ```bash
  grep -rl 'SRC_REMOTECONTROL_CPP_PATH\|SRC_RC_CPP' CMakeLists.txt tests/ \
    | grep -v '^tests/features/rc_tu_split/' | wc -l
  ```

  The exclusion is required, not cosmetic: the test that asserts this must
  contain both literals in order to search for them, and its own `spec.md`
  quotes them too. Without the filter the case matches itself and can never
  pass.
- **INV-5** — `src/remotecontrol_internal.h` is included **only** by files
  listed in `ANTS_RC_SOURCES`. *Breaks when* a sibling subsystem takes a
  dependency on a helper that was never API, making the next split a
  cross-subsystem change. *Test:* `tests/features/rc_tu_split/` — no file
  outside the `ANTS_RC_SOURCES` list includes the header. A **subset** check,
  matching the "only by" wording: a TU that happens to need none of the
  promoted helpers is entitled not to include it. The set is the declared
  list, **never a `src/remotecontrol*.cpp` glob**, which also matches
  `src/remotecontrolgate.cpp` — a separate `ants_core_lib` source that is not
  a TU of this class and must not include the header.
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
- **INV-9** — The suite is green, and the only changes under `tests/` are the
  four mechanical ones this spec authorises: the text-read substitution to
  `ants_test::slurpRemoteControl()` (§ 2.4's first four forms), the
  `ANTS_RC_SRC_DIR` re-route of the dirname anchors, the
  `defined(ANTS_RC_SOURCES)` re-point of the five `#if defined` guards, and
  the new `tests/features/rc_tu_split/` directory with its `SOURCES` wiring.
  *Breaks when* the split changes observable behaviour — an assertion edited
  to accommodate the refactor is the signal. *Test:* `ctest --preset=default`
  green, and `git diff --stat tests/` reviewed against those four categories;
  any edit outside them is named with its reason in the commit message.
- **INV-10** — No preamble insertion point falls inside a fixed-byte scrape
  window. Across the **pure-motion commit alone**, a window `[A, A+N)` over
  `slurpRemoteControl()` reads byte-identical content to the pre-split file
  exactly when none of the eleven TU-head preambles is inserted within it.
  *Breaks when* a seam is placed mid-window, so the window reads an include
  preamble instead of code — silently, for a negative or count-based
  assertion. *Test:* for each of the 25 window-taking tests (§ 2.4), resolve
  its anchor offset in `slurpRemoteControl()` and assert no insertion point
  lies in `[A, A+N)`; migration-time, run once against the post-split tree.
  Scoped to the pure-motion commit because the `rcdetail` promotion (§ 2.3)
  edits definitions in place, so the identity does not hold across it —
  the promotion commit is covered by INV-9's green suite instead.

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
other ten overlap with it and with unrelated work.

**Three numbers move in three different directions, and quoting only the
first would oversell this.** Every post-split figure is a projection from the
fit above, not a measurement:

| | Today | Projected | Change |
|---|---|---|---|
| One-verb incremental edit | 54.66 s | ≈ 14 s (the edited TU alone) | **≈ 3.9× better** — the win this item is for |
| Serial pole in a full build | 54.66 s | ≈ 14 s | ≈ 3.9× shorter |
| Total CPU for this source | 54.66 s | ≈ 94 s (11 × 3.94 s intercept + 0.002045 × 24,803) | **≈ 1.7× worse** |
| Full-build wall for this source over 3 slots | 54.66 s | ≈ 31 s | ≈ 1.8× better |

**Concurrent memory, which § 1 raises and a per-TU figure does not answer.**
The single-process peak is what earlyoom reaps, and it falls from a measured
1,642,524 KB to a projected ≈ 800 MB. The three-slot ceiling is bounded by
3 × the largest TU ≈ **2.4 GB**, against a build today reaching ≈ 1.64 GB plus
two ordinary sibling TUs in the other two slots. The split lowers the spike and
does not raise the ceiling.

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
INV-6. Label `features;fast` (an existing label pair — five other cases use
it). Source added to **`test_core`**'s `SOURCES` list — that bundle owns
remote-control features per `tests/features/README.md`'s bundle table, and
this case only reads files, so it needs Qt6::Core only. Never an
`add_executable`.

| Case | Invariant | Asserts |
|---|---|---|
| `TuOrdinalMarkersAscend` | INV-3 | the eleven `ANTS-3833 TU k/11` markers appear at ascending offsets in `slurpRemoteControl()` |
| `NoSingleTuPathMacro` | INV-4 | neither retired macro name appears in `CMakeLists.txt` or under `tests/`, **excluding `tests/features/rc_tu_split/`** — the case and its `spec.md` must both spell the literals in order to search for them, so without the exclusion it matches itself and can never pass |
| `InternalHeaderStaysInternal` | INV-5 | no file outside the `ANTS_RC_SOURCES` list includes `remotecontrol_internal.h` — a subset check against the declared list, never a `remotecontrol*.cpp` glob (which catches `remotecontrolgate.cpp`) |
| `NoTuExceedsLineCap` | INV-6 | every source in `ANTS_RC_SOURCES` is ≤ 6,000 lines |

Must-fail-first, per the project convention — each case is verified RED
before the corresponding fix is in place:

- `TuOrdinalMarkersAscend` — against an `ANTS_RC_SOURCES` with two
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
| `ROADMAP.md` (ANTS-3833) | three statements on the bullet are measured wrong and are corrected by this spec: the size (`23,849 LoC / 1.1 MB` → 24,803 / 1,174,447 B), the blast radius (`72` test files → 130 files / 330 macro uses), and the named trap (`kindForName` bucketing is in `claudeintegration.cpp` and does not move). Annotate the bullet rather than silently superseding it |
| `src/remotecontrol.cpp` (code, filed not fixed) | the closing brace at line 16959 is annotated "*anonymous from line 1320*"; a brace-aware scan puts that block's opening at 16875. The stale comment is what made two independent readers conclude seven seams were unsafe. Out of scope for a docs change — filed as its own item |
| `CHANGELOG.md` | one `Changed` entry |
| `CLAUDE.md` | none — the module map moved to `docs/subsystems.md` under ANTS-1292 |
| `README.md` / `PLUGINS.md` | none — no user-visible behaviour, no Lua surface change |
| `docs/standards/mcp-tools.md` | none — no contract changes; where a verb body lives is not part of the authoring checklist |

## Cold-eyes loop log

| Loop | Reviewer | Findings (C/H/M/L/I) | Outcome |
|---|---|---|---|
| 1 (2026-08-06) | 3 independent lanes, cold | 2/7/10/9/0 verified, 5 dismissed. Dimensions: 2×5, 4×4, 5×4, 7×4, 6×3, 15×3, 13×2, 9×1, 10×1, 11×1 | All 28 fixed. Two CRITICALs: § 2.3 had helpers "moving with TU 3–5", which § 2.2's contiguity rule makes impossible — they stay in TU 1, and 52 of 59 need promotion, not "a minority"; and § 2.4's macro retirement covered one use form when there are **seven** (330 uses), including nine that want a directory or a `#if defined` guard and cannot be substituted by a text reader. Dismissed: the two lanes concluding seven seams sit inside an anonymous namespace — a brace-aware scan puts 0 of 10 inside, and the source comment that misled them (`remotecontrol.cpp:16959`, "anonymous from line 1320") is itself stale, now filed; and three lanes reporting `test_core` absent from the bundle table, which was a truncation in the orchestrator's packet, not a defect. § 2.2's seam claim survived but its *evidence* did not: a column-anchored grep cannot pair namespace braces, and the real extents are now listed |
