# ADR-0005: Colony — one orchestrator, N worker sessions in worktrees

- **Status:** Proposed
- **Date:** 2026-09-06
- **Deciders:** Project lead, Claude
- **Related:** ROADMAP.md ANTS-4881 (the substrate investigation this
  builds on), ANTS-4887 (a worktree resolves to its main checkout),
  ANTS-4839 (the render publishes onto the checked-out branch),
  ANTS-1979 / ANTS-2195 (the parked `/model` actuator), CLAUDE.md rule 17
  (worktrees), `docs/standards/local-gate.md`.

## Name

**Colony.** A group of ants is a colony and its members are literally
workers, so the metaphor is exact rather than decorative. It names the
verbs (`colony_*`), the skills (`colony-orchestrator`, `colony-worker`)
and the roadmap section. Chosen by the project lead, 2026-09-06.

## Context

The ask: run several Claude Code sessions on one project at once, with an
orchestrator dealing out work, verifying it, and merging it back — so
development goes faster than one session can go.

The interesting part is not the fan-out. **Most of the parts already
exist in this repo**, so the build is smaller than it looks — and the
parts that do NOT exist are named here precisely, because the first
review of this document found that several of them had been assumed.

### What already exists, and what it actually does

| Need | What serves it today | The catch |
|---|---|---|
| See which sessions are alive and what they are doing | `tab_list` (MCP) — per tab `cwd`, `shell_pid`, `claude_running`, `claude_state` (`not_running`/`idle`/`thinking`/`tool_use`/`compacting`), `awaiting_input`, active `tool` (ANTS-1865) | — |
| Watch a session without disturbing it | `get_text {tab}` (MCP) | — |
| Start a session, in a directory, running a command | `launch` / `new-tab` over the remote-control socket (`src/remotecontrol_terminal.cpp:459`) | **Not MCP verbs.** `cwd` is validated against the FOCUSED PROJECT ROOT, so a sibling worktree is refused unless the caller passes `allow_outside_root:true`, which is a blanket bypass. `command` is control-char filtered *by default*, with `raw:true` as a documented opt-out |
| Durable cross-session messaging | `session_message` (ANTS-4622) | **Addressed to a PROJECT, never to a session** — and ANTS-4887 folds a worktree back into its main checkout, so the orchestrator and every worker share ONE inbox with no sender identity. Not usable as a worker→orchestrator channel without a discriminator |
| Shared work record | the machine-global roadmap store, WAL, `busy_timeout` set (explicit `PRAGMA`, and the Qt plugin defaults to 5000 ms) | Concurrent writers block and retry rather than failing, so no retry logic is owed. The one uncovered statement is the WAL switch itself, which takes an EXCLUSIVE lock below the busy handler |
| Mechanical verification | `git_state`, `build_status`, `test_results`, `focused_test`, `verify_changes`, `tools/ci-parity.sh`, `tools/hooks/pre-push` | — |
| Isolation between concurrent lines of work | git worktrees — CLAUDE.md rule 17 | — |

### The five problems

**1. File and build isolation.** Two sessions in one working tree
overwrite each other's edits and collide in the build directory.
*Solved by one worktree per worker.* ANTS-4881 already names this as the
option that "removes the build and push collisions outright".

**2. Shared records.** `ROADMAP.md` and `CHANGELOG.md` are whole-file
read-modify-write, and a roadmap write re-renders the WHOLE store.

**The failure is not the obvious one.** Since ANTS-4887 a worktree
resolves to its registered MAIN checkout, so a worker's roadmap write
does not dirty its own branch — **it writes the store and re-renders into
the orchestrator's `ROADMAP.md`, in the main checkout, under the
orchestrator's feet.** That is worse than a conflict, because nothing on
the worker's side shows it happened.

ANTS-4887's own report is the evidence that this is live: it came from "a
session using the roadmap as the ONLY coordination state between two
parallel sessions, where a claim written that way is invisible and then
silently lost."

**3. Getting work INTO a worker.** Ants can drive a *running* Claude
session only by typing into its PTY, over the rc socket's `send-text`.
That path is live and hardened; what is parked is narrower —
`ModelAutoSwitch::kAutoSwitchActuatorParked` gates the auto-switcher's
`/model` injection alone. The reasoning behind that parking is what
generalises: mid-tool injection cancels the running command, and idle
injection risks unrequested billable work.

**4. Verification and merge.** A worker's report is a claim. The
orchestrator must decide from the tree, not the transcript.

**5. The machine.** 32 GiB with an earlyoom history. `JOB_POOLS` caps
compile and link parallelism *within* one build and nothing caps builds
*across* worktrees. Two concurrent `pre-push` hooks already produce what
looks like linker corruption (`mold: unknown file type`) — and **hooks
are not sessions**, which decides where the lease has to live.

## Decision

### D1 — One worktree per worker, one branch per task

A worker never shares a checkout. Worktrees live at
`<project-parent>/<project>-colony/<task-id>/`, outside the project root
and beside it — so `git status` in the main checkout stays clean and the
tree is not walked by the project's own tooling. The branch is
`colony/<task-id>`.

Placement is stated because it is not free: it is outside the project
root, which is exactly what `launch`'s `cwd` validation refuses (D3).

### D2 — Workers do not write the rendered shared FILES. The orchestrator does.

**The load-bearing decision.** Workers write *code and tests* in their own
worktree. Only the orchestrator writes `ROADMAP.md`, `CHANGELOG.md` and
the review records — on `main`, in the main checkout, one writer at a
time.

**The ban is on the RENDERED FILES, not on the store.** A worker must
write the store to take a lease (D4), and that write MUST NOT go through
the render-and-publish path: it is a direct conditional `UPDATE` of one
row. Anything routed through `roadmap_log` republishes the whole store
into the orchestrator's checkout, which is problem 2 exactly.

This costs a worker nothing it needs, and dissolves problem 2 rather than
locking it.

### D3 — Spawn a fresh session per task; never inject into a live one

The orchestrator starts a worker with `launch`, in the worktree, running
`claude` with the task brief. **A spawned worker handles exactly one task
and exits.** Three reasons, in order:

1. **No injection into a running session**, so no mid-tool cancellation
   and no unrequested-turn hazard. The `send-text` path stays unused by
   Colony deliberately; this is policy, not something the parked constant
   enforces.
2. **Fresh context per task is a throughput lever**, not a side effect: a
   long-lived worker spends a growing share of every task on compaction.
3. A stalled worker is cheap to diagnose and cheap to kill.

**Two things must change in `launch` before this works**, and both are
Phase 2's actual content rather than incidental:

- **`cwd` validation.** It anchors to the focused project root today, so
  a D1 worktree is refused. The MCP surface must accept a *registered
  worktree of the calling project* — NOT by passing
  `allow_outside_root:true`, which is a blanket bypass and would let a
  confused session launch anywhere.
- **`raw`.** The MCP surface must not expose it. Control-char filtering
  is the property that makes this safe to reach from a verb.

### D4 — The work queue lives in the store, with a lease, and needs NO schema bump

A task is a roadmap item plus a claim. The claim lives in the `item`
table's existing `extras` JSON column, taken by an atomic conditional
`UPDATE` — measured today, 191 of 6455 items already carry `extras`.

This matters because **a `kSchemaVersion` bump is a one-way door across
every project on the machine**: `RoadmapStore::open()` refuses outright
when the store's `user_version` exceeds the build's, so the first binary
to upgrade it locks every older build out of every project. A
parallel-work feature does not earn that.

A lease carries an owner, an expiry and a heartbeat. An expired lease
returns the task to the queue — the whole answer to a worker that dies,
hangs, or is closed by the user.

**The worker's report goes in the task record, not in the mail.**
`session_message` addresses projects, and every worker resolves to the
same project as the orchestrator (ANTS-4887), so a shared inbox with no
sender identity is the wrong shape: any worker could read and `ack`
another's report. The claim row already identifies the task and its
owner, so the outcome belongs there.

### D5 — The orchestrator merges automatically, on mechanical evidence

Per task, before merge, all three:

1. the branch builds;
2. **`ctest --preset=default` is green** — the full correctness suite,
   named rather than left to the implementer, because D8's measurement is
   meaningless if two runs mean different things. Not
   `ci-parity.sh --full`: containerised Qt 6.2 and ASan are a release
   gate, and re-running them after every merge would dominate the
   wall-clock this whole design exists to reduce;
3. the diff stays inside the lane the task named.

A task meeting all three **merges without asking** (project lead,
2026-09-06). The three checks are mechanical, so a human approving them
adds latency without adding judgement. What a human is asked about is a
task that FAILS one.

Merges are serial, and **the gate re-runs after each merge**, because at
volume the dominant source of new defects stops being the original code
and becomes the previous fixes.

**When the post-merge re-run goes red**, the orchestrator reverts that
merge, requeues its task with the failure attached, and **stops dealing
until a human answers**. Continuing would cut every subsequent worker
branch from a known-broken `main`.

This raises the cost of a weak suite: with auto-merge, the suite is the
only thing between a confidently-wrong worker and `main`.

### D6 — A build lease as a file lock, because hooks are not sessions

At most N concurrent builds machine-wide (N=1 initially).

**It is an `flock` on a well-known path under `$XDG_STATE_HOME`, not a
store row.** Three reasons, and the first is decisive:

- **The documented failure is produced by git hooks**, and a hook is a
  shell script with no MCP session and no store connection. `flock(1)` is
  available to it in one line; a store lease is not.
- A build lease has no owning item, so `extras` — D4's whole
  no-migration argument — is not a home for it.
- An `flock` dies with the process holding it, so a killed build cannot
  wedge the machine. A stored lease would need an expiry and a reaper.

`tools/hooks/pre-push` and `tools/ci-parity.sh` acquire the same lock. A
hook that cannot acquire it waits rather than failing: a push that blocks
for a minute is correct, and one that aborts trains people to
`--no-verify`.

### D7 — Workers start on an explicit act, and a pull-mode worker stops when the queue empties

No session spawns itself. A **spawned** worker does one task and exits
(D3). A **hand-started pull-mode** session — one the user opened and
pointed at the queue — claims tasks until the queue is empty and then
exits rather than inventing work.

Both keep the standing billing-safety rule intact: the parallelism is
requested, and the work inside it is not invented.

### D8 — Build phases 0-2, then measure, before building 3-5

Project lead, 2026-09-06.

| Phase | Delivers | D-numbers |
|---|---|---|
| 0a | Build lease (`flock`), hooks included | D6 |
| 0b | Orchestrator owns the rendered files; worker store writes bypass the render | D2 |
| 1 | Queue + lease + task record in `extras` | D4 |
| 2 | `colony_spawn`: `launch` as a guarded MCP verb, worktree-aware `cwd` | D1, D3 |
| 3 | `colony-worker` skill — claim, work, report, exit | D3, D7 |
| 4 | `colony-orchestrator` skill — partition, deal, verify, merge, revert | D5 |
| 5 | Fleet view — worker state, spend, queue depth | — |

Phases 0-2 each stand on their own merit and fix hazards that exist
today. **Phases 3-5 are the bet** that N sessions finish more work per
hour than one, and that is unmeasured. ANTS-4913 runs between them: a
batch of independent backlog items, once serially and once with two
workers, compared on wall-clock to merged, tokens spent, and defects
found in the follow-up sweep. If two workers are barely faster than one,
phases 3-5 are not built and the valuable half is still delivered.

## Consequences

**Good.** Isolation is a checkout, not a lock. No schema migration. No
keystroke injection. The verification path already exists.

**Costs and risks.**

- **Tokens scale linearly with workers**, plus the orchestrator's
  supervision overhead. Needs a cap and a spend report.
- **Each worktree pays its own build.** ccache absorbs most of it
  (`ccache -M 20G` is the project's standing recommendation); the first
  build in a fresh worktree is cold.
- **D6 serialises builds machine-wide**, which caps the parallelism the
  whole design is chasing. This is deliberate — the machine is the
  binding constraint, not the design — and it is the strongest argument
  for running D8's measurement before building phases 3-5.
- **Partitioning is a first-class job.** Two tasks touching one file
  conflict however good the machinery is, so the orchestrator refuses to
  deal a task whose lane overlaps one in flight.
- **A worker can be wrong confidently.** D5 is the whole answer, and it
  is only as good as the suite.
- **`launch` becomes reachable from the MCP**, with two guards it does
  not have today (D3). Same-UID trusted (ADR-0004), so this is about
  confusion rather than attack.

## Alternatives considered

**One working tree, a lock per file.** Rejected: it does not solve the
build directory or the push gate, and ANTS-4881 warns against a blanket
mutex — most verbs are reads, and a global lock costs every session to
protect a rare write.

**Keystroke injection into standing sessions.** Rejected: ANTS-1979 and
ANTS-2195 settled the reasoning and nothing about parallel work changes
the mechanism. D3 gets the same result without it.

**`session_message` as the worker→orchestrator channel.** Rejected: it
addresses projects, and every worktree resolves to the orchestrator's own
project, so there is one inbox and no sender. D4 puts the report in the
task record instead.

**A separate task database.** Rejected: `extras` costs no migration, and
D4's one-way-door reasoning is decisive.

**Claude Code subagents instead of sessions.** Not a replacement — they
share the parent's tree and its context budget, which is the constraint
this ADR is lifting. They remain the right tool inside a task.
