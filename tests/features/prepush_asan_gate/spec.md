# Feature: pre-push hook — the ASan leg's cost gate

Test contract for ANTS-4118, extended by ANTS-4536 / ANTS-4943 /
ANTS-4883. Locks the behaviours that stop
`tools/hooks/pre-push`'s `build-asan` leg from being killed mid-build by a
caller's command timeout.

## Problem

The hook runs the Release suite, then does an incremental `cmake --build
build-asan` followed by the sanitized suite. On a cold or stale sanitizer
tree that build is minutes long. Measured 2026-08-11: a `git push` from an
agent session was SIGTERMed at the 600 s harness cap, mid-`ninja`, with the
commit unpushed.

Two costs, and the second is the one that bites:

1. The escape hatch (`ANTS_PREPUSH_NO_ASAN=1`) becomes the habit rather
   than the exception — which is how the ASan leg quietly stops running
   locally. The hatch was only ever printed in the *skip* branch, so a
   caller learned it existed only after already using it.
2. A SIGTERM lands mid-`ninja`, the case this project treats as leaving
   `.ninja_deps` untrustworthy. The hook had no trap, so the next run
   would do an incremental build over a tree nothing had marked as
   interrupted.

## Invariants under test

- **INV-1** — the leg is COST-GATED. Before building, the hook counts the
  sanitizer tree's pending ninja edges (`ninja -C <dir> -n`). Above
  `ANTS_PREPUSH_ASAN_MAX_EDGES` (default 25) it SKIPS the leg, naming the
  edge count and how to run it deliberately, and never invokes `cmake
  --build` on that tree. This is what prevents the timeout: a warm tree's
  incremental build is small, a cold one is not, and only the second is
  refused.
- **INV-2** — a warm tree (edges ≤ the cap) still RUNS the leg: the gate
  must not become a blanket skip, or it reintroduces the coverage loss it
  exists to prevent.
- **INV-3** — the hatch is announced BEFORE the leg starts, in the branch
  that runs it. A caller who cannot afford the wall-clock has to be able to
  learn that from the run they are watching, not from the skip message they
  will never see.
- **INV-4** — if a prior run left the interrupt marker
  (`<dir>/.ants-prepush-interrupted`), the leg is skipped regardless of
  edge count, and the message carries a single command that both heals the
  tree and clears the marker. An incremental result over a tree that took a
  SIGTERM mid-ninja is a false pass, not a cheap one.
- **INV-5** — the skip paths still exit 0 (the push proceeds; CI remains
  the backstop) and the Release leg is unaffected in every case.
- **INV-6** — a pending CMake regeneration is MEASURED, not skipped. When
  the dry run is just `[0/1] Re-running CMake...`, every real edge is hidden
  behind that regen, so the count reads as one cheap edge for what may be a
  full rebuild — precisely the case a changed `CMakeLists.txt` produces. The
  hook runs the regen (`ninja -C <dir> build.ninja`, a CMake re-run rather
  than a build) and re-counts against what it reveals, then gates on that
  count like any other. Treating the regen as unmeasurable and skipping was
  the original repair, and it was wrong in the other direction: it stood the
  leg down on every push touching `CMakeLists.txt`, which is the change most
  likely to introduce a sanitizer-visible defect (ANTS-4536). The skip
  survives only where the regen itself fails, or where the re-count still
  reads as a regen — those remain genuinely unmeasurable.

- **INV-8** — the interrupt-marker skip (INV-4) states the marker's AGE.
  The marker never expires and the skip is one line inside a long run that
  callers commonly tail, so a marker written days ago printed identically to
  one written this session, and the leg stayed dark across every push in
  between while each still ended "push allowed". Expiring the marker is
  deliberately NOT the repair — healing the tree stays the caller's call —
  but a stale skip must not be invisible (ANTS-4943).

- **INV-9** — the test's own result does not depend on the ambient
  environment. The hook reads `ANTS_PREPUSH_NO_ASAN`, `ANTS_PREPUSH_NO_QT62`
  and `ANTS_PREPUSH_ASAN_MAX_EDGES`; the hook runs as a child of whatever
  set them. A caller who exported the documented hatch to skip the slow leg
  for one push therefore failed this suite, so the documented escape hatch
  could not be used for the thing it documents. Each case scrubs those
  names and sets only what it is exercising (ANTS-4883).
- **INV-7** — a truncated deps log is REPORTED, never gated on. `ninja`
  warning `premature end of file; recovering` looks like the signature of a
  killed build, and an earlier draft of this gate skipped the leg on it.
  That was wrong twice over. Measured 2026-08-12 on this repo's own
  `build-asan`: the warning **survived a full `cmake --build --clean-first`
  rebuild**, so it describes the on-disk log rather than the run, and a gate
  keyed to it would never clear — permanently disabling the leg this whole
  feature exists to keep running. Ninja's recovery also errs toward
  rebuilding more, not less (a dropped dep record reads as dirty). So the
  hook prints a note and proceeds. The explicit marker (INV-4) remains the
  untrusted signal, because we write it ourselves and the printed heal
  command clears it.

## Unmeasured, deliberately

The 25-edge default is reasoned, not measured: inside a 600 s cap, the
Release suite takes ~40 s and the sanitized ctest ~200 s, leaving ~300 s for
the build; a Debug+ASan TU is ~10–15 s, so ~25 TUs is the most that fits.
It is an env-overridable guess at a wall-clock budget, and the invariant
under test is the gating behaviour, not the number.

## Test plan

`test_prepush_asan_gate.sh` drives the real hook in a throwaway git repo
with `ctest` / `cmake` / `ninja` stubbed on `PATH`, feeding the new-branch
ref line so the gate runs unconditionally. The stub `ninja` prints
`ANTS_TEST_NINJA_EDGES` dry-run lines, so cold vs warm is one variable. The
stub `cmake` appends its argv to a log, which is how "never invokes cmake
--build on that tree" is asserted rather than inferred.

Pre-fix the hook has no edge count, no marker check and prints the hatch
only when skipping, so INV-1/3/4 fail on assertions and INV-2/5 pass.

The stub `ninja` models the regen as a STATE, not a constant reading: mode
`regen` reports `[0/1] Re-running CMake...` until the hook actually runs the
regen edge, after which the dry run reports real edges. A stateless stub
would report the regen forever and could not tell INV-6's two arms apart.
Mode `regen_fails` never clears, which is the arm that still skips.

Label: `features;fast`.
