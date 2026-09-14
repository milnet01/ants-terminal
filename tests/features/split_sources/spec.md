# split_sources — a split class's source list stays true

**Parent spec:** [`docs/specs/ANTS-1677-large-file-decomposition.md`](../../../docs/specs/ANTS-1677-large-file-decomposition.md) § 3, INV-2, INV-9, INV-11
**Bundle:** `test_core` · **Suite:** `SplitSources` · **Label:** `features;fast`

## Why this exists

ANTS-1044, ANTS-1043 and ANTS-4919 split `auditdialog.cpp`, `mainwindow.cpp`
and `claudeintegration.cpp` into pieces. Two routes name a class's files:

- `CMakeLists.txt`'s `ANTS_<STEM>_SOURCES_REL` list, which the build and the
  C++ scrapes read;
- the glob `src/<stem>.cpp src/<stem>_*.cpp`, which shell and Python readers
  read.

When the two drift apart, some reader sees part of the class and nothing goes
red. Each case below defends one way that happens.

## Invariants

- **INV-2 — `ListMatchesTheGlob`.** For each class that has a list or a piece
  file:
  - the list exists and starts with `src/<stem>.cpp`;
  - as a set, it equals the files the glob matches;
  - the owning library's `add_library()` consumes `${ANTS_<STEM>_SOURCES_REL}`;
  - this bundle's `ANTS_<STEM>_SOURCES` definition exists, and split on `;` it
    equals the list, in order, as absolute paths.

  *Breaks when* a piece is added to the build and not the list, a file matches
  the glob and is not built, or an unescaped `;` collapses the definition.

- **INV-9 — `NoListedFileExceedsTheCap`.** No file of a class in the case's
  capped-class list exceeds 4,000 lines. The list starts empty. The last cut
  commit of a class's last item adds its stem.
  *Breaks when* code regrows in one file until the split is undone.

- **INV-11 — `InternalHeaderStaysInternal`.** Only files in a class's list
  include `src/<stem>_internal.h`. The scan covers `src/` and `tests/`.
  *Breaks when* another subsystem depends on a helper that was never API.

## Proof that each check can fail

On today's tree no class has a list or a piece, so the three standing cases
have nothing to check. **`DetectsEachDefectInAFixtureTree`** runs the same
checks over a temporary tree. A clean tree must report nothing. The tree is then
broken, and each defect the parent spec's must-fail list names must be reported:

- a piece the list misses;
- a stray `src/<stem>_x.cpp` outside the list;
- a listed file one line over the cap;
- a file outside the list including the internal header;
- a compiled definition collapsed by an unescaped `;`;
- a library that does not consume the list;
- pieces with no list at all.

## Deliberate self-exclusion

The INV-11 scan skips `tests/features/split_sources/`. This `spec.md` names the
header the scan looks for.
