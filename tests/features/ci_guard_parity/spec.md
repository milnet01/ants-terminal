# Feature: the podman guard runs ci.yml's own job steps

Test contract. `tools/qt62-guard.sh` runs two ci.yml jobs, `qt62-baseline`
and `build-test`, inside CI's image. Since ANTS-5324 it runs each job's steps
through `tools/ci_workflow.py`, so it holds no configure or build line that
could drift from ci.yml.

The test reads ci.yml with PyYAML, reads the guard's code lines (comments
dropped) as text, and runs the guard's `--print` for each job.

**INV-1 — the guard runs no `cmake -S`, `cmake -B` or `cmake --build` of its
own.** A line starting with `echo` is a message, not a run, and is skipped.

**INV-2 — the guard has one `podman run`, and it executes
`tools/ci_workflow.py run "$job"`.**

**INV-3 — for each guarded job, `--print` mounts the build volume at
`/src/<dir>`, where `<dir>` is the `-B` directory of that job's `Configure`
step in ci.yml.** A CMake cache cannot move between build directories, so a
volume mounted anywhere else would fail the job's configure.

## Not covered

The package list is extracted from ci.yml at run time (the script's header,
§ Keying). Whether the job passes inside the image is the guard's own run, not
this test's.

The test exits 77 (skipped) when PyYAML is absent.

## Regression history

- **2026-10-10:** the guard's hand-written configure lacked
  `-DANTS_COMPILE_POOL=4`, `-DANTS_REQUIRE_SELF_UPDATE=ON` and
  `CCACHE_SLOPPINESS`. The earlier form of this test matched the guard's
  flags against ci.yml's.
- **ANTS-5324 (2026-11-09):** the guard runs ci.yml's steps instead, so the
  test checks that it has no compile of its own.
