# ANTS-3833 — Split remotecontrol.cpp into per-family translation units

**Status:** accepted (2026-08-06) — rule-14 gate run to its 3-loop cap, 3 independent cold lanes per loop, 85 findings verified and all 85 fixed; deferred tail empty.
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
[2.4 The scrape coupling](#24-the-scrape-coupling--the-real-blast-radius) ·
[2.5 Git history](#25-git-history)) ·
[3. Invariants](#3-invariants) · [4. RAM / build cost](#4-ram--build-cost) ·
[5. Out of scope](#5-out-of-scope) · [6. Tests](#6-tests) ·
[7. Cross-doc impact](#7-cross-doc-impact)

## 1. Problem

`src/remotecontrol.cpp` is one translation unit holding every MCP verb body.
It is the largest source in the tree by nearly a factor of two (24,803 lines
against `claudeintegration.cpp`'s 12,822), and its size is now
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
   of the clock: this host's `compile_pool` is **3** (§ 4 derives it) and the
   project has an earlyoom history. § 4 also carries the post-split concurrent
   figure, which is what decides whether the split helps or hurts on that
   front.

2. **No session can hold the file**, so every change to a verb is a blind
   targeted edit against a file nobody has read end to end.

3. **`ants_core_lib` has a 54.66 s serial pole.** One TU cannot be split
   across pool slots, so a full build waits on it regardless of how many
   slots are free.

The class itself is not the problem. `RemoteControl::dispatch()` is 103
lines routing an op string to a member. The file defines 97 distinct `cmd*`
members (`grep -oE 'RemoteControl::cmd[A-Za-z0-9_]+' src/remotecontrol.cpp |
sort -u | wc -l`) — **not all of them op-routed**. At least nine are called from other members
rather than reached through `dispatch`: the seven `cmdRoadmapLog*ForTest`
test hooks and
`cmdVerifyChangesImpl` / `cmdVerifyChangesWithRoot`. The verbs behind an inner
op router (`cmdRoadmapLog`'s eight write bodies, `cmdGitState`'s ops) are not
`dispatch`-routed either. Routed or not, they are independent of one
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
slice sizes; each TU gains a preamble on top.

**The preamble is a contract, because its size feeds § 4's per-TU projection
and every INV-10 insertion point.** For TUs 2–11, in order: the ordinal marker
(below), `#include "remotecontrol.h"`, `#include "remotecontrol_internal.h"`
where the slice uses a promoted symbol (INV-5 makes it optional), one
`using namespace rcdetail;`, then the **minimum** set of further includes that
slice needs — never a copy of the
file head's current include block (lines 1–111), which would put ~110 lines of
unused includes into all eleven TUs and spend the compile-time win this item
exists to collect.

**TU 1 is carved out**: it *is* the file head, so it keeps its existing include
block untouched and gains only the ordinal marker. Its insertion at offset 0 is
still one of INV-10's eleven — a marker line is a marker line — but it is one
line, not a preamble.

Three boundaries are named for what they contain rather than what the family
label suggests, because contiguity wins over tidiness:

- TU 3 carries `cmdRoadmapLog`'s op router and **six** of the seven
  `cmdRoadmapLog*ForTest` test hooks (each under 40 lines, all thin routers) ahead
  of the write bodies in TU 5. The seventh, `cmdRoadmapLogBundleRowForTest`,
  sits in TU 5 immediately before `cmdRoadmapLogBundleRow`.
- TU 1 is not a family. It is the file head — includes, **the shared helper
  block (§ 2.3)**, the constructor and destructor (lines 1928 and 1936),
  `setVerifyTrustClient`, `defaultSocketPath`, `start`, `onNewConnection`,
  `dispatch`. It is therefore also the TU that
  owns most promoted helpers' definitions.
- TU 9 carries `runClient` — the client-side loop, a sibling of `start` and
  `onNewConnection` which TU 1 holds. It sits at 17713, inside TU 9's slice,
  and contiguity keeps it there.

**Every seam lands outside an anonymous-namespace block.** ("Seam" means a TU
boundary throughout this document, and nothing else.) This is the one
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
zero seams inside an anonymous namespace. Three regions that look dangerous
and are not: TU 8 (15347–17680) wholly contains the close-early / reopen pair
around `namespace ants {` (16875–16959, then 17046–17505); TU 10's seam at
21017 clears the 21005–21015 block by two lines; and TU 11's seam at 22992
clears 22937–22990 by the same margin. Two lines is the tightest clearance in
the file, which is the reason the next paragraph exists.

**Every line number in § 2.2 is evidence as of 2026-08-06, not a coordinate to
cut on.** This is by the document's own argument the project's most-edited
file, so the slice boundaries, the namespace extents and the seam lines will
all have moved by the time anyone implements this — and two of the seams clear
their nearest anonymous namespace by two lines. **Precondition on the cut:**
re-derive every seam from the *member names* in the table above, then re-run
the scan and the containment check before any code moves — with
**`tools/rc-namespace-scan.py`**, which is that scanner, shipped rather than
described precisely so nobody re-derives it by hand:

```bash
tools/rc-namespace-scan.py src/remotecontrol.cpp \
  --seams 2262,3666,6556,7713,12598,14387,15347,17681,21017,22992
# → 24 anonymous namespaces; seams inside an anonymous namespace: 0 of 10
# → exits non-zero if any seam is not in open code
```
If a seam no longer falls in open code, move it to the next member boundary
that does and record the change.

**The same applies to every count this document asserts** — 127 in INV-2, 328
and the five categories in INV-9, 165 in INV-4, 337 / 330 in § 2.4, 74 in
§ 2.3. Each was measured on 2026-08-06 and each drifts the moment a verb is
added. Re-run their commands at cut time; the numbers here are evidence that
the design fits the tree, not constants to assert against blindly.

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

The marker carries **no snake_case identifier**; § 2.4(c) owns the rule and
the reason.

**Where a new verb goes.** Its *body* goes into the TU owning its family, at
the end of that TU's slice — never into `remotecontrol.cpp`, which is the
dispatcher and the helper pool. Its `dispatch` routing entry always goes into
`remotecontrol.cpp`, because that is where `dispatch` lives (§ 2.1). When a family TU would pass INV-6's 6,000-line cap, split it and
renumber every marker. This rule is what keeps INV-6 from being the only thing
standing between the tree and a second 24,803-line file.

### 2.3 Shared helpers: `src/remotecontrol_internal.h`

The pre-split file's head anonymous namespace (lines 112–1926) holds 59
symbols: 45 free functions, 12 constants and 2 structs. **It is not the only
one** — § 2.2's scan found 24 anonymous namespaces, and the promotion problem
belongs to all of them (see *All 24 blocks*, below).

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
| function | `QString rcNormaliseHeadline(QStringView);` | stays in a `.cpp` |
| constant | `inline constexpr` where a call site needs a constant expression; otherwise **`extern const T X;`** — never a bare `const T X;` | in the header for `inline constexpr`; else `const T X = …;` in one `.cpp` |
| struct / type | the whole definition moves into `namespace rcdetail` in the header | in the header — forced, and there is no alternative |

**The `extern` is load-bearing and INV-7 cannot catch its absence.** A
namespace-scope `const` object has **internal linkage** in C++ unless declared
`extern`, so a bare `const T X;` in the internal header gives every TU its own
private copy — and *links clean*. INV-7 relies on the linker, which sees
nothing wrong here, so the 12 head-block constants are the one promotion class
with no automatic backstop. Write `extern`, and note it when reviewing the
promotion commit.

**Two kinds break the "definitions stay in a `.cpp`" rule, and both are
forced.** A type used by value across TUs *must* have one definition visible
to each, or the TUs compile two distinct types that do not interoperate; an
`inline constexpr` constant is a definition by construction.

**That has a consequence § 2.4 must state rather than discover.**
`slurpRemoteControl()` reads the `.cpp` paths in `ANTS_RC_SOURCES` and **not**
`remotecontrol_internal.h`, so any symbol whose text moves into the header
disappears from everything a scrape can see — silently, for the negative and
count-based assertions § 2.4 identifies as the dangerous class. Two rules
follow:

- **Promote a constant to `inline constexpr` only where a constant expression
  is genuinely required at a call site**, established per symbol rather than
  assumed. Everything else keeps its definition in the `.cpp`.
- **Before the promotion commit lands, grep the test tree for each symbol
  whose text leaves `ANTS_RC_SOURCES`.** A hit means a scrape asserts on that
  text and the promotion would silence it; that symbol stays in the `.cpp`, or
  the scrape moves to the header explicitly. This is the check INV-9's green
  suite cannot perform, and it is why it is written here as a step rather than
  left to the suite.

**All 24 blocks, not just the head one.** The head block is the biggest and
the one whose numbers are quoted above, but the other 23 sit inside a single
TU and still hold symbols a *later* TU references — which is a link error the
moment the seam exists. Measured over the 23 non-head blocks: **22 of their 73
symbols are referenced outside their own TU**, among them `resolveRootCanonical`
(reaching eight other TUs), `runGit`, `collectGitSnapshot`, `isValidSpecId`,
`parseStatusHeader`, `runDiffOp` / `runLogOp` / `runStatusOp`, and the
`ceErr` / `csErr` / `fbErr` / `gitErr` / `irErr` / `wsErr` refusal helpers.

So the promotion set is roughly **74 symbols** (52 head + 22 non-head), not 52,
and the derivation below runs over every anonymous namespace in the file. The
figure is approximate on purpose: it was measured by a regex over
definition-shaped lines, which is good enough to size the work and not good
enough to *be* the list — the linker produces the exact set (INV-7).

**Derivation, not a hand-written list.** The promotion set is computed after
the slices are cut, per symbol — **against `ANTS_RC_SOURCES` membership, never
a `src/remotecontrol*.cpp` glob**, which also matches `src/remotecontrolgate.cpp`
(a separate `ants_core_lib` source that is not a TU of this class and must not
include the internal header). This is the one place that caveat is stated;
INV-5 and § 6 point here:

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

**Call sites are not qualified one by one.** Each TU's preamble carries a
single `using namespace rcdetail;`, so the ~74 promotions change the
*definitions* and leave every call site's text alone. That is the minimum-diff
option and it is what keeps commit 1's blast radius reviewable; explicit
`rcdetail::` at ~hundreds of call sites would be a far larger edit for no
benefit the linker cares about.

**This promotion edits text in place, which INV-10 depends on knowing.** Every
promoted definition loses its anonymous-namespace enclosure and gains
`rcdetail` enclosure, so the post-split concatenation is *not* the
pre-split bytes plus seam preambles. § 2.5 keeps the promotion in its own
commit for exactly this reason, and INV-10 is evaluated against the
pure-motion commit alone.

### 2.4 The scrape coupling — the real blast radius

**130 test files and 337 sites reach this file** — 330 through a macro and 7
through a hard-coded literal path — not the 72 the ROADMAP bullet names. The
bullet counted files carrying the literal string `remotecontrol.cpp`; most
tests reach the file through a CMake `-D` path macro instead.

```bash
# 130 — union of literal-path and macro-path users
{ grep -rl 'src/remotecontrol\.cpp'          tests/ --include='*.cpp'
  grep -rl '\bSRC_REMOTECONTROL_CPP_PATH\b'  tests/ --include='*.cpp'
  grep -rl '\bSRC_RC_CPP\b'                  tests/ --include='*.cpp'; } | sort -u | wc -l

# the use forms, and how many of each
grep -rhoE '[A-Za-z_]+\((SRC_REMOTECONTROL_CPP_PATH|SRC_RC_CPP)\b' \
  tests/ --include='*.cpp' | sort | uniq -c
```

**Eight forms, and they do not all want the same thing** — which is the fact
that shapes the rest of this section. Four routes, decided by what the site is actually asking for:

| Use form | Count | What it wants | Route |
|---|---|---|---|
| `slurpFile(SRC_REMOTECONTROL_CPP_PATH)` | 282 | the class's text | `slurpRemoteControl()` |
| `slurpFile(SRC_RC_CPP)` | 21 | the class's text | `slurpRemoteControl()` |
| `slurp(SRC_REMOTECONTROL_CPP_PATH)` | 12 | the class's text | `slurpRemoteControl()` |
| `readSource(SRC_RC_CPP)` | 5 | the class's text | `slurpRemoteControl()` |
| `QStringLiteral(SRC_REMOTECONTROL_CPP_PATH)` | 3 | one as text, two as a **dirname anchor** | split per site |
| `QFileInfo(SRC_REMOTECONTROL_CPP_PATH)` | 2 | `src/`'s **directory**, to locate a sibling file | `ANTS_RC_SRC_DIR` |
| `#if defined(SRC_REMOTECONTROL_CPP_PATH)` | 5 | proof the bundle carries the source-path defs | `defined(ANTS_RC_SOURCES)` |
| **total, macro** | **330** | | |
| `slurpFile(srcPath("src/remotecontrol.cpp"))` and one `ANTS_SOURCE_DIR + "/src/remotecontrol.cpp"` | 7 (in 6 files) | the class's text, via a **literal path** that never touches a macro | `slurpRemoteControl()` |
| **total, all routes** | **337** | | |

**The literal-path row is the one that breaks the "no partial-scrape state"
claim below**, because deleting a macro cannot fail a site that never used
one: those seven reads would silently keep scraping TU 1 alone. They are
re-routed by hand, and INV-4's `tests/` arm covers the literal too. All six
files are in **`test_claude`** — one of the four bundles in (a) — so the
re-route compiles against a defined `ANTS_RC_SOURCES`. That was checked rather
than assumed: a file that never used a path macro had no particular reason to
sit in a macro-carrying bundle.

The `QStringLiteral` / `QFileInfo` / `#if defined` rows are why "substitute one
call for another" is not the whole change. `tests/features/mcp_path_anchor/` computes
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

**(a) Three new definitions replace the two path macros, at each of their
four bundle definition sites.** `CMakeLists.txt`
builds the TU list once and passes all three definitions to each of the four
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

# ants_core_lib consumes the SAME list — one source of truth, so a TU cannot
# exist in the library and be invisible to every scrape.
add_library(ants_core_lib STATIC ${ANTS_RC_SOURCES_REL} …)

# …then, per bundle:
target_compile_definitions(<bundle> PRIVATE
    ANTS_RC_SOURCES="${_ants_rc_joined}"
    ANTS_RC_SRC_DIR="${CMAKE_SOURCE_DIR}/src"
    ANTS_RC_ROOT_DIR="${CMAKE_SOURCE_DIR}")
```

**The library must consume the list, not parallel it.** If `add_library()`
names the TUs separately, a twelfth TU can be added to the build and omitted
from `ANTS_RC_SOURCES`: it links, every invariant here still passes, and every
scrape silently reads 11/12 of the class — the exact failure (b) exists to
abolish, reintroduced through the back door. INV-11 pins it.

`ANTS_RC_ROOT_DIR` exists because INV-4 and INV-5 scan `CMakeLists.txt` and
`tests/`, and neither of the other two definitions can reach them.

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
feeds `slurpAbsolute` (a local helper in
`tests/features/roadmap_fold_in/test_roadmap_fold_in.cpp`) — become a
mechanical substitution to it:

```cpp
// ANTS-3833 — the RemoteControl implementation is eleven translation units.
// A scrape that reads one of them reads a fraction of the class: a verb that
// moved to a sibling TU is indistinguishable from a verb that was deleted.
// Reads every path in ANTS_RC_SOURCES, in that order — the order they were
// cut from the original file — joined with a newline, so a two-anchor window
// (find(A), then find(B, posA)) still sees A before B. A path that cannot be
// opened contributes "", matching slurpFile's behaviour rather than aborting
// the shared bundle.
#if defined(ANTS_RC_SOURCES)
inline std::string slurpRemoteControl() { /* split ANTS_RC_SOURCES on ';',
    slurpFile each, join with '\n' */ }
#endif
```

**The `#if` guard is required, not defensive.** `tests/_support/srcgrep.h` is
included by **278** test sources across every bundle, while `ANTS_RC_SOURCES`
is defined for the four in (a). An unguarded body referencing the macro fails
to compile every bundle that does not carry it — and the spec's own five
`#if defined` guards are the standing evidence that some do not.

`slurpFile(SRC_REMOTECONTROL_CPP_PATH)` → `ants_test::slurpRemoteControl()`,
and likewise for the `slurp(…)`, `readSource(…)` and `slurpFile(SRC_RC_CPP)`
forms. `slurpFunctionBody` anchors resolve as before.

**Two preamble contents can shift a count, and only one is controllable.**
The controllable one is the ordinal marker's own text: writing
`// ANTS-3833 TU 5/11 — roadmap_log write ops.` would add an occurrence of
`roadmap_log` to the concatenation. **The
marker therefore carries no snake_case identifier** — no verb name, op name,
refusal code or config key — which is why § 2.2's form reads *"Roadmap write
ops"*. The per-TU `#include` lines are not controllable: eleven preambles
duplicate include text that appeared once. No `countOccurrences` assertion
over include text is known to exist, and **INV-9's green suite is what
establishes that**, not this paragraph.

The 57 test files that `#include "remotecontrol.h"` are a compile-time
dependency on an unchanged header and need no edit.

### 2.5 Git history

**Three commits, and every one of them builds and tests green.** The order is
forced rather than stylistic: the promotion must precede the cut, because
~74 symbols cross a TU boundary (§ 2.3) and a cut made before they have
external linkage produces a tree that does not link.

| # | Contents | State after |
|---|---|---|
| 1 | The `rcdetail` promotion, **while the file is still one TU**: symbols leave the anonymous namespaces, `remotecontrol_internal.h` appears, `remotecontrol.cpp` includes it. Plus the pre-promotion grep of § 2.3 for any symbol whose text would leave `ANTS_RC_SOURCES`. | builds, suite green |
| 2 | **The cut.** Under `src/`: pure motion — the moved lines are byte-identical, no reformatting, no renames — plus each TU's preamble. Under `CMakeLists.txt`: the `ANTS_RC_SOURCES_REL` list, the library binding, the three new definitions, and the deletion of the two old path macros. Under `tests/`: the `slurpRemoteControl()` helper in `_support/srcgrep.h`, then all 337 site migrations. | builds, suite green |
| 3 | `tests/features/rc_tu_split/` and its `SOURCES` wiring. | builds, suite green |

**INV-10 measures commit 1 against commit 2** — that pairing is what "the
pure-motion commit" means everywhere in this document. The identity it asserts
is: the concatenation at 2 equals the file at 1, plus eleven preambles and the
ten newlines `slurpRemoteControl()` joins on. It does not span commit 1, which
edits definitions in place.

The `src/` half of commit 2 being pure motion is what lets `git log -M -C`
follow a verb across the split. Bundling the test migration into the same
commit does not weaken that — `-M -C` works per file pair — and separating them
would leave a commit whose suite is red, because the tests would still be
reading TU 1 alone through macros the cut has deleted.

## 3. Invariants

- **INV-1** — `src/remotecontrol.h` is byte-identical across the migration.
  The split changes no declaration, so no consumer recompiles for an API
  reason. *Test:* `git diff <pre>..<post> -- src/remotecontrol.h` is empty.
- **INV-2** — Every `RemoteControl::` member defined in the pre-split file is
  defined exactly once across the post-split TUs; none is added or removed.
  *Breaks when* a slice boundary drops or duplicates a member. *Test:* the
  **definition-anchored** list below returns the same 127 names, each exactly
  once, from the pre-split file and from the post-split TUs:

  ```bash
  grep -ohE '^([A-Za-z_][A-Za-z0-9_:<>,&* ]*[ *&])?RemoteControl::~?[A-Za-z0-9_]+\(' \
       "$@" | sed -E 's/.*(RemoteControl::~?[A-Za-z0-9_]+)\(/\1/' | sort | uniq -c
  ```

  Two details are load-bearing. **The anchor**: the unanchored form
  (`grep -ohE 'RemoteControl::[A-Za-z0-9_]+\('`) counts every **mention** —
  134 against 127 on the current file — so it is equal by construction under
  pure motion and would stay equal if a definition were duplicated and a call
  site removed. **The optional prefix and the `~`**: a required return-type
  prefix cannot match `RemoteControl::RemoteControl(` (line 1928) or
  `RemoteControl::~RemoteControl()` (1936), both of which are defined in this
  file and land in TU 1. **Run it twice: once piped through `sort` for the set
  comparison above, and once *without* `sort` for the ordered sequence.** The
  sorted run is INV-2's own claim; the unsorted run is the only check anywhere
  that INV-3's ordering property actually held, since INV-3's standing case
  asserts marker order and not member order, and a slice whose contents were
  reordered would pass it. Making the prefix optional and admitting `~` takes
  the set from 123 to the true **127** — the four it adds are
  `RemoteControl::RemoteControl`, `RemoteControl::~RemoteControl`,
  `RemoteControl::roadmapBullets` and `RemoteControl::roadmapWriteTarget`,
  all four real definitions the stricter form could not reach.
- **INV-3** — `ANTS_RC_SOURCES` lists the TUs in slice order, so the
  concatenation preserves every member's pre-split relative position.
  *Breaks when* a TU is appended to the list rather than inserted at its
  slice position — which silently reorders two-anchor scrape windows.
  *Test:* `tests/features/rc_tu_split/` derives `N` from the `ANTS_RC_SOURCES`
  entry count and asserts one `TU k/N` marker per entry, k = 1…N, at ascending
  offsets in `slurpRemoteControl()`. `N` is never the literal 11 in the test —
  a twelfth TU is an expected event (§ 2.2) and must not require editing the
  case.
- **INV-4** — Nothing under `tests/` names a single remotecontrol TU as the
  RemoteControl source: neither retired macro, and no hard-coded
  `src/remotecontrol.cpp` path. In `CMakeLists.txt`, neither retired macro
  survives. *Breaks when* a bundle keeps `SRC_REMOTECONTROL_CPP_PATH` or
  `SRC_RC_CPP`, or a test opens the literal path — either lets a scrape read a
  fraction of the class and read a moved verb as a deleted one. *Test:* rooted
  at `ANTS_RC_ROOT_DIR`, **both** commands **return 0** post-split; against the
  current tree they return **165** (files) and **6** (lines):

  ```bash
  # tests/ — macros AND the literal path
  grep -rl 'SRC_REMOTECONTROL_CPP_PATH\|SRC_RC_CPP\|src/remotecontrol\.cpp' \
       tests/ | grep -v '^tests/features/rc_tu_split/' | wc -l
  # CMakeLists.txt — the two macro names only
  grep -c 'SRC_REMOTECONTROL_CPP_PATH\|SRC_RC_CPP' CMakeLists.txt
  ```

  **The split into two commands is required, not stylistic.** `CMakeLists.txt`
  must carry the literal `src/remotecontrol.cpp` forever — it is TU 1, and
  § 2.4(a)'s `ANTS_RC_SOURCES_REL` list names it, which INV-11 in turn
  *requires* to exist. A single command matching the literal across both trees
  can never return 0, so it would be an invariant whose test reddens on the
  very block another invariant mandates.

  The literal arm over `tests/` is what closes the eighth route in § 2.4's
  table: deleting a macro cannot fail a site that never used one, so without it
  those seven reads keep scraping TU 1 in silence.

  The 165 decomposes as 130 `.cpp` + 35 `.md` (feature `spec.md` files quoting
  the path in prose, cleared by rewording rather than by code). The 6 is
  `CMakeLists.txt`'s macro-definition lines, counted by line rather than by
  file because there is only ever one file.

  The `rc_tu_split` exclusion is required, not cosmetic: the test that asserts
  this must contain the literals in order to search for them, and its own
  `spec.md` quotes them too. Without the filter the case matches itself and can
  never pass.
- **INV-5** — `src/remotecontrol_internal.h` is included **only** by files
  listed in `ANTS_RC_SOURCES`. *Breaks when* a sibling subsystem takes a
  dependency on a helper that was never API, making the next split a
  cross-subsystem change. *Test:* `tests/features/rc_tu_split/` — no file
  outside the `ANTS_RC_SOURCES` list includes the header, scanning `src/` and
  `tests/` from `ANTS_RC_ROOT_DIR`. A **subset** check, matching the "only by"
  wording: a TU that happens to need none of the promoted helpers is entitled
  not to include it. The set is the declared list, never a glob — § 2.3 says
  why.
- **INV-6** — No TU in `ANTS_RC_SOURCES` exceeds 6,000 lines — the cap is set
  from § 4's fit, at which point a TU costs ≈ 16 s to compile, still under a
  third of today's 54.66 s. *Breaks when*
  verbs accrete into one TU until it is the old file again — the regression
  this whole item exists to prevent, and the only one that returns silently.
  *Test:* `tests/features/rc_tu_split/` counts lines per listed source.
- **INV-7** — No **function or type** retains internal linkage while being
  referenced from another TU. *Breaks when* a promoted helper is left `static`
  or inside an anonymous namespace. *Test:* `ants-terminal` and the test
  bundles link — **not** `ants_core_lib`, which is a `STATIC` archive whose
  build only archives objects and never resolves symbols, so
  `--target ants_core_lib` is green with unresolved `rcdetail::` references.
  **Constants are excluded by construction**: a bare namespace-scope `const`
  has internal linkage and links clean per TU, so § 2.3's `extern` rule is the
  only thing standing behind them.
- **INV-8** — `RemoteControl::dispatch()` stays in `src/remotecontrol.cpp`
  and its op→member routing chain is byte-identical across the migration.
  *Breaks when* a slice takes routing with it, changing which op reaches
  which verb. *Test:* `slurpFunctionBody(rc, "RemoteControl::dispatch")`
  compares equal pre- and post-split.
- **INV-9** — The suite is green, and the only changes under `tests/` are the
  five mechanical ones this spec authorises: (1) the substitution of all **328**
  text-read sites to `ants_test::slurpRemoteControl()` — 321 macro-routed plus
  the 7 literal-path reads; (2) the `ANTS_RC_SRC_DIR` re-route of the four dirname anchors;
  (3) the `defined(ANTS_RC_SOURCES)` re-point of the five `#if defined`
  guards; (4) the `slurpRemoteControl()` helper added to
  `tests/_support/srcgrep.h`; (5) the new `tests/features/rc_tu_split/`
  directory with its `SOURCES` wiring. *Breaks when* the split changes
  observable behaviour — an assertion edited to accommodate the refactor is
  the signal. *Test:* `ctest --preset=default` green, and
  `git diff --stat tests/` reviewed against those five categories; any edit
  outside them is named with its reason in the commit message.
- **INV-10** — No preamble insertion point falls inside a fixed-byte scrape
  window. Across the **pure-motion commit alone**, a window `[A, A+N)` over
  `slurpRemoteControl()` reads byte-identical content to the pre-split file
  exactly when none of the eleven TU-head preambles is inserted within it.
  *Breaks when* a seam is placed mid-window, so the window reads an include
  preamble instead of code — silently, for a negative or count-based
  assertion. *Test:* a standing case in `tests/features/rc_tu_split/` that
  **derives** its work-list rather than embedding one. It scans `tests/` for
  `.substr(<ident>, <N>)` sites in files that also read remotecontrol text,
  recovers each site's anchor from the `find(...)` that produced `<ident>`,
  resolves that anchor's offset `A` in `slurpRemoteControl()`, and asserts no
  TU-head offset lies in `[A, A+N)`. § 2.4's seven anchors are what that scan
  returns *today*; hard-coding them would leave a window added tomorrow
  silently uncovered — the exact class this invariant exists for.
  **Standing rather than migration-time** for two reasons: § 2.2 mandates
  splitting a TU that reaches INV-6's cap, and every split adds an insertion
  point; and ANTS-3681's sweep did not reach every existing window, so new ones
  still appear. **Remedy when it fires:** move the seam to the next member
  boundary that clears every window, or convert the offending window to a
  structural bound (`slurpFunctionBody`) — the fix ANTS-3681 was already making.
  Scoped to commit 2 of § 2.5: commit 1's promotion edits definitions in place,
  so the byte-identity does not span it — that commit's silent-scrape risk is
  handled by § 2.3's pre-promotion grep instead.
- **INV-11** — `ants_core_lib`'s remotecontrol sources are exactly the
  `ANTS_RC_SOURCES_REL` list; the library consumes the list rather than
  restating it. *Breaks when* a TU is added to `add_library()` and not to the
  list — it links, INV-3 through INV-6 all pass, and every scrape reads a
  fraction of the class in silence, which is the failure INV-4 exists to
  abolish arriving by a route INV-4 cannot see. *Test:*
  `tests/features/rc_tu_split/` asserts `CMakeLists.txt` contains no
  `src/remotecontrol_*.cpp` literal outside the `ANTS_RC_SOURCES_REL`
  `set(...)` block.

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
| Full-build wall for this source over 3 slots | 54.66 s | ≈ 31 s at perfect packing; ≈ 33 s under longest-processing-time scheduling of the eleven | ≈ 1.7× better |

**Concurrent memory, which § 1 raises and a per-TU figure does not answer.**
The single-process peak is what earlyoom reaps, and it falls from a measured
1,642,524 KB to a projected ≈ 800 MB. The three-slot ceiling is bounded by
3 × the largest TU ≈ **2.4 GB**. Today's worst case is the 1,642,524 KB TU
sharing the pool with two ordinary siblings; taking `terminalgrid.cpp`'s
measured 743,140 KB as the sibling term gives ≈ **3.1 GB**. So the ceiling
falls too, by roughly 0.7 GB, and the single-process spike — the thing earlyoom
actually reaps — halves.

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
INV-6, INV-10, INV-11. Label `features;fast` (an existing label pair — five
other cases use it). Source added to **`test_core`**'s `SOURCES` list — that
bundle owns remote-control features per `tests/features/README.md`'s bundle
table, and this case only reads files, so it needs Qt6::Core only. Never an
`add_executable`.

| Case | Invariant | Asserts |
|---|---|---|
| `TuOrdinalMarkersAscend` | INV-3 | `N` = the `ANTS_RC_SOURCES` entry count; one `TU k/N` marker per entry, k = 1…N, at ascending offsets in `slurpRemoteControl()` |
| `NoSingleTuPathMacro` | INV-4 | two assertions, not one: under `tests/`, neither retired macro name nor a literal `src/remotecontrol.cpp` path, **excluding `tests/features/rc_tu_split/`**; in `CMakeLists.txt`, neither macro name — the literal is **not** asserted there, because § 2.4(a)'s `ANTS_RC_SOURCES_REL` list must carry it and INV-11 requires that list to exist (see INV-4) |
| `InternalHeaderStaysInternal` | INV-5 | no file outside the `ANTS_RC_SOURCES` list includes `remotecontrol_internal.h`, scanning `src/` and `tests/` from `ANTS_RC_ROOT_DIR` |
| `NoTuExceedsLineCap` | INV-6 | every source in `ANTS_RC_SOURCES` is ≤ 6,000 lines |
| `NoSeamInsideAScrapeWindow` | INV-10 | for each of § 2.4's seven anchors, no TU-head offset lies inside that window's `[A, A+N)` |
| `LibraryConsumesTheList` | INV-11 | `CMakeLists.txt` carries no `src/remotecontrol_*.cpp` literal outside the `ANTS_RC_SOURCES_REL` `set(...)` block |

Must-fail-first, per the project convention — each case is verified RED
before the corresponding fix is in place:

- `TuOrdinalMarkersAscend` — against an `ANTS_RC_SOURCES` with two
  entries transposed.
- `NoSingleTuPathMacro` — against the pre-split `CMakeLists.txt` (macro arm),
  and against the pre-split `tests/` tree (literal arm).
- `InternalHeaderStaysInternal` — against a scratch `#include
  "remotecontrol_internal.h"` added to an unrelated `src/*.cpp`.
- `NoTuExceedsLineCap` — against the pre-split `remotecontrol.cpp` (24,803
  lines) in the list.
- `NoSeamInsideAScrapeWindow` — against scratch TU *files* re-cut so a seam
  lands a few hundred bytes after the `cmdReadRegions` anchor. A list alone
  cannot relocate a seam; only the cut can.
- `LibraryConsumesTheList` — against a `CMakeLists.txt` naming one TU directly
  in `add_library()`.

**INV-7 has no case here and needs none** — its surface is that
`ants_core_lib` links, which every build already runs. A helper left with
internal linkage while another TU references it is an unresolved symbol, and
no test can be redder than a failed link.

INV-1, INV-2, INV-8 and INV-9 are **migration-time checks with no permanent
surface** — they compare the pre- and post-split trees, a pairing that exists
only during the migration. Each is a recorded command run against § 2.5's
commits 1 and 2, and the build plan owns running them; inventing a standing
test for them would be a test that can never fail again. **INV-10 and INV-11
are deliberately not on that list**: both stay reachable after the migration,
because § 2.2 expects a twelfth TU eventually and each new TU adds an
insertion point and another chance to bypass the list.

## 7. Cross-doc impact

| Doc | Change |
|---|---|
| `docs/subsystems.md` | the `remotecontrol` lane entry gains the eleven TUs **alongside** its existing verb list, which it keeps — the lane catalogue `subsystem` and `indie_review_partition` derive from |
| `ROADMAP.md` (ANTS-3833) | three statements on the bullet are measured wrong and are corrected by this spec: the size (`23,849 LoC / 1.1 MB` → 24,803 / 1,174,447 B), the blast radius (`72` test files → 130 files / 337 sites), and the named trap (`kindForName` bucketing is in `claudeintegration.cpp` and does not move). Annotate the bullet rather than silently superseding it |
| `src/remotecontrol.cpp` (code, filed not fixed) | the closing brace at line 16959 is annotated "*anonymous from line 1320*"; a brace-aware scan puts that block's opening at 16875. The stale comment is what made two independent readers conclude seven seams were unsafe. Out of scope for a docs change — filed as its own item |
| `CHANGELOG.md` | one `Changed` entry |
| `CLAUDE.md` | none — the module map moved to `docs/subsystems.md` under ANTS-1292 |
| `README.md` / `PLUGINS.md` | none — no user-visible behaviour, no Lua surface change |
| `docs/standards/mcp-tools.md` | none — no contract changes; where a verb body lives is not part of the authoring checklist |

## Cold-eyes loop log

| Loop | Reviewer | Findings (C/H/M/L/I) | Outcome |
|---|---|---|---|
| 3 (2026-08-06) — cap | 3 independent lanes, cold | 1/5/8/14/0 verified, 0 dismissed. Origin split: 19 fix collateral, 9 draft defects — the first loop where collateral led. Dimensions: 5×8, 4×7, 15×4, 6×4, 7×3, 2×2, 1×2, 9×2, 10×2 | All 28 fixed; deferred tail empty. **All three lanes led on the same CRITICAL, and it was loop 2's own collateral**: INV-4 gained a literal-`src/remotecontrol.cpp` arm in the same loop that INV-11 mandated a `CMakeLists.txt` block containing that literal, so its test could never return 0. Now two commands — the literal is asserted over `tests/` only. Also: INV-9 said "all 321 sites, literal-path reads included" when 321 provably excludes them (**328**); the constant-promotion row said "a plain declaration", which at namespace scope has internal linkage and **links clean**, so INV-7's linker check is blind to it (now `extern const`, stated as the one class with no automatic backstop); INV-7's surface was `ants_core_lib`, a `STATIC` archive that never resolves symbols; and INV-10's standing case hard-coded seven anchors measured on one date, so it now derives them. Resolved by measurement rather than assumption: all six literal-path files are in `test_claude` (a macro-carrying bundle, so the re-route compiles); INV-2's 127 is 123 + ctor + dtor + `roadmapBullets` + `roadmapWriteTarget`; INV-4's 165 is 130 `.cpp` + 35 `.md` |
| 2 (2026-08-06) | 3 independent lanes, cold | 1/6/10/12/0 verified, 0 dismissed. Origin split: 13 fix collateral, 16 draft defects. Dimensions: 5×7, 4×4, 6×4, 7×3, 15×3, 10×3, 1×1, 9×1, 13×1 | All 29 fixed; INV-11 added. Four structural draft defects, each measured rather than argued: the promotion analysis covered 1 of 24 anonymous namespaces, and **22 of 73 non-head symbols cross a TU boundary** (set is ~74, not 52); **seven literal-path reads** bypass the macro route entirely, so "no partial-scrape state to reach" was false and INV-4 now matches the literal; `ANTS_RC_SOURCES_REL` was not bound to `ants_core_lib`, letting a twelfth TU be invisible to every scrape while passing every invariant (INV-11); and `slurpRemoteControl()` sat unguarded in a header **278** test sources include while the macro reaches 4 bundles. The CRITICAL was collateral: § 2.5 put the preambles outside the pure-motion commit while § 2.4 and INV-10 measured them inside it — § 2.5 is now three named commits, each green. Also corrected: INV-2's anchor could not match the ctor (1928) or dtor (1936), so its 123 was really **127** |
| 1 (2026-08-06) | 3 independent lanes, cold | 2/7/10/9/0 verified, 5 dismissed. Dimensions: 2×5, 4×4, 5×4, 7×4, 6×3, 15×3, 13×2, 9×1, 10×1, 11×1 | All 28 fixed. Two CRITICALs: § 2.3 had helpers "moving with TU 3–5", which § 2.2's contiguity rule makes impossible — they stay in TU 1, and 52 of 59 need promotion, not "a minority"; and § 2.4's macro retirement covered one use form when there are **seven** (330 uses), including nine that want a directory or a `#if defined` guard and cannot be substituted by a text reader. Dismissed: the two lanes concluding seven seams sit inside an anonymous namespace — a brace-aware scan puts 0 of 10 inside, and the source comment that misled them (`remotecontrol.cpp:16959`, "anonymous from line 1320") is itself stale, now filed; and three lanes reporting `test_core` absent from the bundle table, which was a truncation in the orchestrator's packet, not a defect. § 2.2's seam claim survived but its *evidence* did not: a column-anchored grep cannot pair namespace braces, and the real extents are now listed |
