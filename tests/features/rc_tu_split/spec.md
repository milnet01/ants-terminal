# rc_tu_split — the RemoteControl translation-unit split holds

**Parent spec:** [`docs/specs/ANTS-3833-remotecontrol-decomposition.md`](../../../docs/specs/ANTS-3833-remotecontrol-decomposition.md)
**Bundle:** `test_core` · **Suite:** `RcTuSplit` · **Label:** `features;fast`

## Why this exists

`src/remotecontrol.cpp` was one 24,752-line translation unit. ANTS-3833 cut it
into eleven, and the cut is only safe while six properties keep holding. Each
of them fails *silently* — the build stays green, the suite stays green, and
the tests quietly stop testing what they claim to.

The shared failure is that ~130 test files read the RemoteControl source as
**text** and assert things about it. `ants_test::slurpRemoteControl()` hands
them the eleven TUs concatenated in cut order, so those scrapes still see one
continuous class. Every case below defends one way that concatenation can stop
being faithful.

## Invariants

Each case derives its expectations from `ANTS_RC_SOURCES` — the compile
definition CMake builds from the `ANTS_RC_SOURCES_REL` list. **No case
hard-codes eleven.** § 2.2 of the parent spec expects a twelfth TU once one
reaches the line cap, and a case that has to be edited when that happens is a
case that will be edited wrongly.

- **INV-3 — `TuOrdinalMarkersAscend`.** Each listed TU carries a `TU k/N` head
  marker, `k` = 1…N in list order, `N` = the entry count, and the markers
  appear at *ascending* offsets in the concatenation, exactly once each.
  *Breaks when* a TU is appended to the list rather than inserted at its slice
  position — which silently reorders every two-anchor scrape window
  (`find(A)`, then `find(B, posA)`).

- **INV-4 — `NoSingleTuPathMacro`.** Two separate assertions:
  nothing under `tests/` names one TU as *the* RemoteControl source — neither
  retired macro (`SRC_REMOTECONTROL_CPP_PATH`, `SRC_RC_CPP`) nor a literal
  `src/remotecontrol.cpp` path — and `CMakeLists.txt` defines neither macro.
  *Breaks when* a test reads TU 1 alone, where a verb that **moved** to a
  sibling TU is indistinguishable from a verb that was **deleted**.

  The literal is asserted over `tests/` only. `CMakeLists.txt` must carry it
  forever — it is TU 1, and INV-11 requires the list naming it to exist.

- **INV-5 — `InternalHeaderStaysInternal`.** `src/remotecontrol_internal.h` is
  included only by files in the `ANTS_RC_SOURCES` list. A subset check: a TU
  needing none of the promoted helpers is entitled not to include it.
  *Breaks when* a sibling subsystem takes a dependency on a helper that was
  never API, making the next split a cross-subsystem change.

- **INV-6 — `NoTuExceedsLineCap`.** No listed TU exceeds 6,000 lines.
  *Breaks when* verbs accrete into one TU until it is the old file again —
  the regression this whole item exists to prevent, and the only one that
  returns silently.

- **INV-10 — `NoSeamInsideAScrapeWindow`.** No TU-head preamble is inserted
  inside a fixed-byte scrape window. The case **derives** its work-list: it
  scans `tests/` for `<subject>.substr(<ident>, <N>)` sites whose `<subject>`
  holds `slurpRemoteControl()` text, recovers each site's anchor from the
  `<subject>.find("…")` that produced `<ident>`, resolves that anchor's offset
  `A` in the concatenation, and asserts no TU-head offset lies in `[A, A+N)`.
  *Breaks when* a seam is placed mid-window, so the window reads an include
  preamble instead of code.

  **The derivation is the point.** Hard-coding today's anchor list would leave
  a window added tomorrow silently uncovered — the exact class this invariant
  exists for. Measured 2026-08-06: the scan returns **20 sites over 15 distinct
  anchors**, where § 2.4 of the parent spec enumerates seven.

  **And the failure is not reliably loud**, which is why the case is
  mandatory rather than advisory. A *must-contain* assertion whose window slid
  onto an include preamble goes red — visible. A **negative** assertion ("this
  region must not mention X") and a `countOccurrences` total over a shifted
  window both stay **green while testing nothing**. Those are the classes this
  case defends. One of the 20 sites
  (`token_usage_no_ci_diagnostic`, anchor `env["ok"] = true;`) is exactly a
  negative assertion.

- **INV-11 — `LibraryConsumesTheList`.** `CMakeLists.txt` carries no
  `src/remotecontrol*.cpp` literal outside the `set(ANTS_RC_SOURCES_REL …)`
  block. *Breaks when* a TU is added to `add_library()` and not to the list: it
  links, INV-3 through INV-6 all pass, and every scrape reads a fraction of
  the class in silence — the failure INV-4 abolishes, arriving by a route
  INV-4 cannot see.

  `src/remotecontrolgate.cpp` is a different file, not a TU, and does not
  match: the pattern requires `remotecontrol` to be followed by `.` or `_`.

## Deliberate self-exclusion

Both INV-4 and INV-5 scan `tests/`, and this directory has to contain the very
literals they search for — the assertion cannot look for a string without
naming it, and this `spec.md` quotes them too. **Every scan skips
`tests/features/rc_tu_split/`.** Without the filter each case matches itself
and can never pass.

## Out of scope

- **INV-7** (no internal linkage across TUs) — its surface is that the link
  succeeds, which every build already runs. A helper left `static` while
  another TU references it is an unresolved symbol; no test can be redder than
  a failed link. Note the link must be an *executable* — `ants_core_lib` is a
  `STATIC` archive and never resolves symbols.
- **INV-1, INV-2, INV-8, INV-9** — migration-time checks with no permanent
  surface. They compare the pre- and post-split trees, a pairing that exists
  only during the migration. Each is a recorded command in the parent spec's
  § 2.5 commits; a standing test for them would be a test that can never fail
  again.
- Anything about *behaviour*. Whether a verb still works is the ~130 existing
  remote-control feature tests' job. This case only asserts that those tests
  are still reading the whole class.

## Must-fail-first

Per `tests/features/README.md` step 6, each case was verified RED before the
property was restored — the mutations are recorded in the ANTS-3833 commit
message. Five of the six mutate only files the test reads **at run time**
(the TU sources, `tests/`, `CMakeLists.txt`), so they need no rebuild;
`TuOrdinalMarkersAscend` needs one, because the list reaches the test as a
compile definition.
