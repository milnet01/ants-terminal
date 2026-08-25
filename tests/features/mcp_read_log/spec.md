# Feature test: `read_log` MCP tool (ANTS-1855)

Test contract for the `read_log` MCP verb. Full design:
[`docs/specs/ANTS-1855.md`](../../../docs/specs/ANTS-1855.md).

The filtering logic lives in the pure `ReadLog::filter` helper
(`src/readlog.{h,cpp}`, Qt6::Core-only in `ants_core_lib`) so the
behavioural invariants are testable without `RemoteControl` /
`MainWindow`. The thin `cmdReadLog` wrapper + tools/list schema +
dispatch are locked by source-scrape.

## Invariants (mirror docs/specs/ANTS-1855.md)

- **W1 (INV-1/2/3)** — wiring: `cmdReadLog` declared in
  `remotecontrol.h`; the IPC dispatcher routes `read_log`/`read-log`;
  `callerCwdContractFor` returns `Required` for `read_log`; the handler
  calls `PathValidation::validatePath` (→ `bad_path`).
- **W2 (INV-11)** — `read_log` is in mcpprojection's compaction table
  (`mcpprojection.cpp`) and the tools/list schema registers `read_log`
  with `path`/`include`/`exclude`/`contains`/`since`/`tail`/`max_bytes`/
  `since_cursor`/`fields` + `registerToolProvider("read_log", …Required…)`
  in `mainwindow.cpp`.
- **B-INV-4** — `include`/`exclude` regex: a line is kept iff it matches
  `include` and not `exclude`; invalid regex → `bad_args`.
- **B-INV-5** — `contains:S` keeps only lines containing literal S.
- **B-INV-6** — `since:T` keeps lines whose `[..]` prefix is lexically
  `>= T`; no-prefix lines dropped when `since` set.
- **B-INV-7** — `tail:N` clamped (N>10000→10000, N<=0→no tail); returns
  the last N survivors; `matched` is the full pre-tail count.
- **B-INV-8** — byte cap drops the **oldest** survivors (newest kept),
  sets `truncated` + `lines_dropped`; over-ceiling sets
  `bytes_cap_clamped`.
- **B-INV-9** — envelope carries `matched` + `scanned`.
- **B-INV-10** — `since_cursor` reads only appended bytes + returns a
  fresh `cursor`; a rotated/garbage cursor soft-falls-back to
  `cursor_stale:true` + full re-read (never a refusal).
- **B-not_found** — an unopenable path → `{ok:false, code:"not_found"}`.

## Test plan

Behavioural invariants exercise `ReadLog::filter` against fixture logs
in a `QTemporaryDir`. Wiring invariants source-scrape
`remotecontrol.{h,cpp}` / `claudeintegration.cpp` / `mcpprojection.cpp`
/ `mainwindow.cpp`. Label `features;fast`. Verify the suite fails against
the stub helper + un-wired sources before implementing.
