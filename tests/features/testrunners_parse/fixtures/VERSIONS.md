# Fixture provenance

Each `.txt` here is the verbatim output of a real run, captured
2026-10-21 on openSUSE Tumbleweed. The scratch path is replaced by
`/work`; nothing else is edited. Re-capture a fixture when its runner's
version changes.

| Runner | Version | Fixtures |
|--------|---------|----------|
| pytest | 9.1.1 (Python 3.13.16), `PYTEST_DISABLE_PLUGIN_AUTOLOAD=1` | `pytest*.txt` |
| cargo | 1.99.0 | `cargo.txt` |
| go | 1.27.2 | `go*.txt` |
| jest | 30.5.2 (node 24.21.0) | `jest*.txt`, `npx_missing.txt` |
| vitest | 5.0.5 (node 24.21.0) | `vitest*.txt` |

Commands, run in each sample project:

- `python3 -m pytest -rfE -p no:cacheprovider` — `pytest.txt`;
  with `-q` — `pytest_q.txt`; `test_sample.py -k "not deselect"` —
  `pytest_desel.txt`; in an empty directory — `pytest_none.txt`.
- `cargo test --no-fail-fast` — `cargo.txt`.
- `go test -v ./...` — `go.txt`, `go_panic.txt`, `go_none.txt`; without
  `-v` — `go_nov.txt`; with `-timeout 1s` — `go_timeout.txt`.
- `npx --no-install jest --ci` — `jest.txt`; with `--findRelatedTests
  ./math.js` — `jest_related.txt`; with `--findRelatedTests
  ./package.json` — `jest_related_none.txt`; where jest is not
  installed — `npx_missing.txt`.
- `npx --no-install vitest run` — `vitest.txt`, and in a project with no
  test file — `vitest_none.txt`; `vitest related --run ./math.js` —
  `vitest_related.txt`.
