# Feature: pre-push hook — the ASan leg's cost gate

Test contract for ANTS-4118. Locks the three behaviours that stop
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
- **INV-6** — a pending CMake regeneration is NOT a warm reading. When the
  dry run is just `[0/1] Re-running CMake...`, every real edge is hidden
  behind that regen, so the count is 1 for what may be a full rebuild —
  precisely the case a changed `CMakeLists.txt` produces. The hook must
  treat it as unmeasurable and skip, not as one cheap edge.
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

Label: `features;fast`.
