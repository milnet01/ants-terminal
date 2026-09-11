# Feature: qt62-guard.sh — a killed compile must not orphan its container

Test contract for ANTS-5124. Locks the containment `tools/qt62-guard.sh` must
give its compile container once a push that is running it gets killed.

## Problem

`tools/hooks/pre-push` runs `tools/qt62-guard.sh --warm-only` as part of the
push gate. The guard's compile step is a foreground `podman run` with no
container name, no `--init`, and no signal trap. On 2026-09-11 a background
`git push` was stopped for low memory while the guard's `podman run` was
compiling. The push process died; the podman container did not, because
nothing told it to stop, and it kept running ninja and cc1plus against the
named build volume holding memory. Killing it by hand kills ninja mid-build,
which this project treats as tree corruption (`.ninja_deps` becomes
untrustworthy — the same class `build-asan`'s `.ants-prepush-interrupted`
marker exists to catch), so the only safe move that session had was to wait.

`build-asan`'s leg in `tools/hooks/pre-push` already solves the general shape
of this problem: a `trap` on `TERM INT` around the sanitizer build writes an
interrupted marker and exits, and a later run refuses to trust a tree behind
that marker. `qt62-guard.sh` has no equivalent for its own compile step.

## Scope

In scope: that the compile's `podman run` names and inits its container, that
a signal trap wraps the compile and stops that container, that the trap's
handler marks the build volume interrupted and exits non-zero, and that a
later `--warm-only` run refuses a marked volume rather than trusting it — the
same shape `build-asan`'s marker already gives the sanitizer leg.

Out of scope: driving the real `podman` container end to end (needs podman
and a multi-minute cold build; not something a fast unit-style test can pay
for), and the exact marker path or container-naming scheme — the test reads
the script's own text for the shapes named above rather than requiring one
spelling.

## Regression history

ANTS-5124 (2026-09-11). INV-1 to INV-4 failed against the script before the
fix; INV-5 is a guard and passed throughout.

## Invariants

- **INV-1** — the compile's `podman run` line names its container (`--name`)
  and passes `--init`, so a `podman stop` reaches the container's process
  tree (ninja's children included) rather than only the top-level shell.
- **INV-2** — a `trap` naming `INT`, `TERM` and `HUP` is set immediately
  before the compile's `podman run` and cleared (`trap -`) immediately after
  it succeeds, and the trap's handler stops the named container
  (`podman stop`) rather than leaving it running unattended.
- **INV-3** — between the trap and the compile, an interrupted marker for the
  build volume is written; it is removed only once the compile finishes, and
  the trap's handler exits non-zero and leaves it. The same shape
  `build-asan`'s `.ants-prepush-interrupted` marker gives the sanitizer leg.
- **INV-4** — the `--warm-only` branch checks for that marker and skips
  (exit 0) when it is present, so a later push does not trust an incremental
  build over a tree a killed compile left in an unknown state.
- **INV-5** (guard) — the `--warm-only` branch still skips when the cached
  image or build volume is missing, whether or not a marker exists. The
  existing `missing=` check is the reason `--warm-only` never pays a cold
  build inside a push, and the marker check added for INV-4 must not have
  displaced it.

## What this check does NOT cover, stated so it is not mistaken for coverage

- It does not run `podman`, so it cannot prove a `podman stop` on the named
  container actually reaches ninja's children — only that the script issues
  one.
- It does not prove the marker's path is a real, writable host directory —
  only that the text between the trap and the compile names "interrupted"
  and that `--warm-only` checks for it.
- It does not cover `--clean`, `--print`, or the image-build step
  (`qt62_ensure_image`) — those are unaffected by this fix and untouched by
  this spec.

## Build

Compiled into the **`test_claude`** bundle, the same one
`prepush_docs_only_parity` and `ci_workflow_deps` use for their `pre-push` /
`ci.yml` source scrapes. Reads `tools/qt62-guard.sh` through a
`SRC_QT62_GUARD_PATH` compile definition. Label `features;fast`.

Comment lines (first non-space character `#`) are stripped before matching,
the same way `ci_workflow_deps` strips them — `qt62-guard.sh`'s own header
discusses `podman run`, traps and markers in prose, so a raw substring match
would pass on the commentary alone with the real fix absent.
