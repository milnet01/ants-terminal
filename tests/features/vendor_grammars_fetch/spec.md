# Feature: vendor-grammars — a failed fetch changes nothing

Test contract for ANTS-6631. `tools/vendor-grammars.sh` runs `fetch` inside
`$(...)`, where `set -e` is off.

## Invariants under test

- **INV-1** — when the runtime's download fails under `--pin` with an empty
  `sha256`, the script exits non-zero, `third_party/tree-sitter/` is left as
  it was, and no hash is pinned. Before, `fetch` carried on past the failed
  `curl`, `sha256sum` and `tar` and printed a directory, and the script then
  deleted the vendored runtime before the copy failed.
- **INV-2** (control) — a good download replaces the runtime and pins the
  tarball's hash.
- **INV-3 (ANTS-6987)** — when the `jq` rewrite that pins the hash fails
  under `--pin`, the script exits non-zero, names the failure, and leaves
  `third_party/tree-sitter/` as it was. Red before ANTS-6987, which went on
  to replace the runtime. The case puts a stub `jq` on `PATH` that fails
  only that rewrite.

## Test plan

`test_vendor_grammars_fetch.sh` runs a copy of the real script in a
throwaway tree, with a stub `curl` first on `PATH`: it fails as a 404 does,
or copies a tarball the test built. Nothing is fetched and the repo's own
`third_party/` is never touched. Red before ANTS-6631 on INV-1's
runtime-kept check. The no-hash check does not tell old from new code: the
old script pinned an empty hash over an empty one.

Not covered: the grammar loop, whose `parser.c` check already stops before
it deletes anything.

The suite exits 77 (skipped) when jq, sha256sum or tar is missing.

Label: `features;fast`.
