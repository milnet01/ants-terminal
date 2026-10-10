# Feature: the podman compile guard configures as ci.yml does

Test contract. `tools/qt62-guard.sh` compiles the tree inside CI's image for
two ci.yml jobs, `qt62-baseline` and `build-test`. Its configure line and its
compile-cache settings are written in the script, not read from ci.yml, so
they can drift. They had: until 2026-10-10 the guard lacked
`-DANTS_COMPILE_POOL=4`, `-DANTS_REQUIRE_SELF_UPDATE=ON` and
`CCACHE_SLOPPINESS`.

The test reads ci.yml with PyYAML and the guard's compile block as text.

**INV-1 — the guard's compile configure sets exactly the `-D` cache
variables and the `-G` generator of each guarded job's `Configure` step.**
Paths (`-S`, `-B`) differ by design: the container mounts the tree and the
build volume elsewhere.

**INV-2 — every `CCACHE_*` variable in each guarded job's `env`, except
`CCACHE_DIR`, is passed to the compile's `podman run` with the same value.**
`CCACHE_DIR` is a path, and the guard mounts its own cache.

## Not covered

The `--run-job` leg runs ci.yml's own steps through `tools/ci_workflow.py`,
so it cannot drift this way. The package list is extracted from ci.yml at
run time (the script's header, § Keying).

The test exits 77 (skipped) when PyYAML is absent.
