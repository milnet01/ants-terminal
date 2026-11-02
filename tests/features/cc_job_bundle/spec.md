# cc-job bundle (ANTS-5629)

Ants Terminal ships `cc-job` at `<bindir>/cc-job`. It runs a long command in
its own `systemd --user` unit, so relaunching Ants does not kill it. The
source is `packaging/cc-job/cc-job`.

- **INV-1** — Installing the `cc-job` component puts an executable
  `bin/cc-job` under the prefix.
  *Breaks when:* the install rule is dropped, or installs without the
  execute bit.
- **INV-2** — `cc-job --help` names both needs: `systemd --user`, and
  Konsole for `watch`.
- **INV-3** — With `CC_JOB_DIR` unset, the log dir is
  `$XDG_STATE_HOME/cc-job`, created on first run.
- **INV-4** — `cc-job watch <name>` with no `konsole` on `PATH` exits `2`
  with a message naming Konsole.

- **INV-5** — A variable exported to `cc-job start` reaches the job with its
  exact value, and `--help` says so.
  *Breaks when:* a test job silently falls back to its defaults, such as a
  live database.
- **INV-6** — The session's own variables, such as `CLAUDE_CODE_SESSION_ID`
  and `ANTS_MCP_SOCKET`, do not reach the job.
- **INV-7** — Exported values travel in a mode 0600 file that is gone once the
  job has started, and not on `systemd-run`'s command line.
- **INV-8** — Plain `cc-job status` runs no per-job `systemctl is-active`.
  *Breaks when:* its cost grows with the jobs kept, as it did: 188 s for 4,012
  jobs, measured 2026-11-02.
- **INV-9** — Plain `status` lists running jobs, then the newest 20 others by
  log time, then a line counting the rest and naming `--all`. `status --all`
  lists every job by name, and `--help` names it. Both work where the state
  dir is a symlink.
- **INV-10** — If `list-units` fails, `status` asks per job, so a running job
  never reads as finished.

The test starts its job through a stand-in `systemd-run`, so it needs no
`systemd --user`.

Test: `test_cc_job_bundle.sh`, ctest name `cc_job_bundle`.
