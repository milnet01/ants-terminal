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

No case starts a job, so the test needs no `systemd --user`.

Test: `test_cc_job_bundle.sh`, ctest name `cc_job_bundle`.
