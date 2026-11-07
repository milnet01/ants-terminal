# audit_project_code_lanes_off — audit lanes that run project code start off

**Bundle:** `test_dialogs` · **Suite:** `AuditProjectCodeLanesOff` · **Label:** `features;fast`

## Problem

Some audit lanes run code the project supplies. `cargo clippy` builds the
crate, which runs its `build.rs` and procedural macros. `pylint` reads the
project's pylint configuration, whose `init-hook` and `load-plugins` run
Python. On a cloned repository, a lane that is selected when the Audit
dialog opens runs that code on the first click of Run (ANTS-6446).
`luacheck` runs the project's `.luacheckrc`, a Lua script with full access
to `os` and `io`. `checkov` reads the project's `.checkov.yaml`, which can
set `external-checks-dir` to the project's own Python checks (ANTS-6696).

## Invariants

- **INV-1** — With the tool installed and the project type detected, the
  `cargo_clippy`, `pylint`, `luacheck` and `checkov` lanes are available but
  not selected.
- **INV-2** — A lane that runs no project code is still selected:
  `cargo_audit`, under the same conditions.

## Test surface

The test builds a temporary project holding `Cargo.toml`, a `Dockerfile`
and one `.py` and one `.lua` file, puts stub `cargo`, `cargo-audit`,
`pylint`, `luacheck` and `checkov` executables first on `PATH`, constructs
an `AuditDialog` and reads `checksForTest()`.
