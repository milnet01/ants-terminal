# token-bench: the bug test runs against the starting tests

`tools/token-bench/bench.py` restores `tests/` from a run's starting commit
before it runs the bench's bug test, so a run that edited the test cannot
pass. The restore put back tracked files only; a file the run added under
`tests/`, such as a `conftest.py`, stayed and could change the result
(ANTS-6630).

- **INV-1 — every change the run made under `tests/` is gone.** After
  `bug_test_passes`, a tracked test file the run edited holds its starting
  text, and an untracked file the run added under `tests/` no longer exists.

Test: `test_token_bench_restore.py` builds a throwaway git repository, makes
the run's edits, and calls `bug_test_passes` with a bug test that passes only
when both are undone.
