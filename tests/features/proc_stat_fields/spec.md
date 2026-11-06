# proc_stat_fields — feature contract

`TerminalWidget::foregroundProcess()` names the program running in the
foreground of a tab. It reads the terminal's foreground process group,
`tpgid`, from the shell's `/proc/<pid>/stat`. proc(5) orders the fields after
`pid (comm)` as state, ppid, pgrp, session, tty_nr, tpgid. `src/procstat.h`
holds that order once.

## Invariants

| INV | Statement |
|---|---|
| INV-1 | `procstat::tpgid` returns the sixth field after the comm, which is `tpgid`, not `tty_nr`. A comm holding `) ` does not shift it. A line too short or with no `)` returns -1. *Test:* `ProcStatFields.TpgidIsTheSixthFieldAfterComm` |
| INV-2 | `procstat::fieldsAfterComm` matches the running kernel: on `/proc/self/stat`, the third and fourth fields equal `getpgrp()` and `getsid(0)`. *Test:* `ProcStatFields.FieldOrderMatchesTheKernel` |
| INV-3 | `foregroundProcess()` reads the group through `procstat::tpgid`, not by its own index. *Test:* `ProcStatFields.ForegroundProcessUsesTheHelper` |
