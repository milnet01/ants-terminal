# GitWrap stdout budget (ANTS-1839)

`GitWrap::run` capped stderr (`kStderrCapBytes`) but read stdout unbounded.
Current callers are bounded (git status / branch / log --oneline), but a future
caller running `git diff` / `log -p` would feed an unbounded blob into a JSON
envelope. ANTS-1839 adds a per-caller stdout budget.

## Surface

- `GitWrap::run(workingDir, argv, maxStdoutBytes = kStdoutCapBytes)` — stdout is
  truncated to `maxStdoutBytes`; `Result::stdoutTruncated` flags a truncated
  read. Default `kStdoutCapBytes` is 1 MiB (generous for the current callers).

## Invariants

- **INV-1** With a small `maxStdoutBytes`, output longer than the cap is
  truncated to exactly the cap and `stdoutTruncated` is set.
- **INV-2** With a cap larger than the output, nothing is truncated and
  `stdoutTruncated` is false.
