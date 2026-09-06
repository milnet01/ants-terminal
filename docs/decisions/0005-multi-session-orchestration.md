# ADR-0005: Multi-session orchestration — one orchestrator, N workers in worktrees

- **Status:** Proposed
- **Date:** 2026-09-06
- **Deciders:** Project lead, Claude
- **Related:** ROADMAP.md ANTS-4881 (the substrate investigation this
  builds on), ANTS-4839 (roadmap render publishes onto the checked-out
  branch), ANTS-1979 / ANTS-2195 (the parked keystroke actuator),
  CLAUDE.md rule 17 (worktrees), `docs/standards/local-gate.md`.

## Context

The ask: run several Claude Code sessions on one project at once, with an
orchestrator dealing out work, verifying it, and merging it back — so
development goes faster than one session can go.

The interesting part is not the fan-out. It is that **four of the five
hard problems are already solved in this repo, and the fifth is the one
that decides the design.** This ADR is mostly an inventory of what exists,
so the build is small.

### What already exists

| Need | What serves it today |
|---|---|
| See which sessions are alive and what they are doing | `tab_list` — per tab: `cwd`, `shell_pid`, `claude_running`, `claude_state` (`idle`/`thinking`/`tool_use`/`compacting`), `awaiting_input`, active `tool` (ANTS-1865) |
| Watch a session without disturbing it | `get_text {tab}` — another tab's trailing scrollback |
| Start a session, in a directory, running a command | `launch` / `new-tab` over the remote-control socket (`src/remotecontrol_terminal.cpp`) — control-char filtered, `cwd` validated |
| Durable cross-session messaging with read receipts | `session_message` (ANTS-4622) — store-backed, ack'd, never auto-pruned while unread |
| Shared work record | the machine-global roadmap store (WAL; SQLite serialises the row writes) |
| Mechanical verification | `git_state`, `build_status`, `test_results`, `focused_test`, `verify_changes`, `mutation_probe`, and the project's own `tools/ci-parity.sh` + pre-push hook |
| Isolation between concurrent lines of work | git worktrees — CLAUDE.md rule 17 already prescribes them |

### The five problems

**1. File and build isolation.** Two sessions in one working tree
overwrite each other's edits, and their build directories collide.
*Solved by one worktree per worker* — own checkout, own branch, own build
dir. Rule 17's route, and ANTS-4881 already names it as the option that
"removes the build and push collisions outright".

**2. Shared records.** `ROADMAP.md` and `CHANGELOG.md` are whole-file
read-modify-write, and a `roadmap_log` write re-renders the WHOLE store
into whichever branch is checked out (ANTS-4839). Worktrees make this
worse rather than better: every worker's roadmap file is a different
checkout of the same generated document, so each write produces a large
unrelated diff and a guaranteed conflict on merge.

**3. Getting work INTO a worker.** Ants can only drive a *running* Claude
session by typing into its PTY. That actuator is parked and
code-enforced (`ModelAutoSwitch::kAutoSwitchActuatorParked`), because
mid-tool injection cancels the running command and idle injection risks
unrequested billable work.

**4. Verification and merge.** A worker's report is a claim. The
orchestrator must decide from the tree, not from the transcript.

**5. The machine.** 32 GiB with an earlyoom history; `JOB_POOLS` caps
compile and link parallelism *within* one build, and nothing caps builds
*across* worktrees. Two concurrent pre-push hooks already produce what
looks like linker corruption (`mold: unknown file type`), recorded as a
standing caution.

## Decision

### D1 — One worktree per worker, one branch per task

A worker never shares a checkout. Its branch is named for the task's
roadmap id. This is the decision that makes every other one simple.

### D2 — Workers do not write shared records. The orchestrator does.

**This is the load-bearing decision.** Workers write *code and tests* in
their own worktree, and report outcomes through a store-backed channel.
Only the orchestrator writes `ROADMAP.md`, `CHANGELOG.md` and the review
records — on `main`, in the main checkout, one writer at a time.

It costs nothing a worker needs, and it dissolves problem 2 rather than
solving it: no concurrent whole-file re-render, no roadmap diff on any
worker branch, nothing to merge but source. It also makes ANTS-4839 and
the ANTS-4881 write-collision work *desirable* rather than *blocking*.

### D3 — Spawn a fresh session per task; never inject into a live one

The orchestrator starts a worker with `launch`, in the worktree,
running `claude` with the task brief. Three reasons, in order:

1. **It sidesteps the parked actuator entirely.** No keystrokes into a
   session that might be mid-tool.
2. **Fresh context per task is a throughput lever, not a side effect.** A
   long-lived worker spends an increasing share of every task on
   compaction; a per-task session starts at zero.
3. It makes a stalled worker cheap to diagnose and cheap to kill.

The queue stays the source of truth, so a session the user started by
hand can also *pull* a task. Spawn is the default, not the only route.

### D4 — The work queue lives in the store, with a lease, and needs NO schema bump

A task is a roadmap item plus a claim. The claim lives in the existing
`item.extras` JSON column (present since user_version 1, already carrying
data on 191 of 6455 rows), taken by an atomic conditional `UPDATE`.

This matters because **a `kSchemaVersion` bump is a one-way door across
every project on the machine** — the first binary to upgrade the store
locks every older build out of every project (CLAUDE.md). A parallel-work
feature is not worth that, and it does not need it.

A lease carries an owner, an expiry and a heartbeat. An expired lease
returns the task to the queue; that is the whole answer to a worker that
dies, hangs, or is closed by the user.

### D5 — The orchestrator merges only on mechanical evidence

Per task, before merge: the branch builds, the suite is green, and the
diff stays inside the lane the task named. The worker's report is used to
*route* the check, never to replace it.

Merges are serial, and **the gate re-runs after each merge** — because at
volume the dominant source of new defects stops being the original code
and becomes the previous fixes (the premise `close-findings` is built on,
measured in this repo).

### D6 — A global build lease, because the machine is the real limit

At most N concurrent builds machine-wide (N=1 initially), held in the
store and acquired by any session about to build. Everything else about
the design scales with tokens; this one scales with RAM, and this host
has an earlyoom history and a documented two-builds-at-once failure.

### D7 — Workers start on an explicit act and stop when the queue empties

No session spawns itself. When the queue is empty a worker exits rather
than inventing work. This keeps the standing billing-safety rule intact:
the parallelism is requested, the work inside it is not invented.

## Consequences

**Good.** Isolation is a checkout, not a lock. No new schema. No
keystroke injection. The verification path already exists. Most of the
build is plumbing between parts that already work.

**Costs and risks.**

- **Tokens scale linearly with workers** and the orchestrator pays a
  supervision overhead on top. Needs a cap and a spend report.
- **Each worktree pays its own build.** ccache absorbs most of it
  (`ccache -M 20G` is already the project's recommendation); the first
  build in a fresh worktree is cold.
- **Partitioning is now a first-class skill.** Two tasks touching one
  file will conflict however good the machinery is. The orchestrator must
  refuse to deal a task whose lane overlaps one in flight.
- **A worker can be wrong confidently.** D5 is the whole answer, and it
  is only as good as the suite.
- **`launch` becomes reachable from the MCP.** It is control-char
  filtered and `cwd`-validated today and is same-UID trusted (ADR-0004),
  but exposing it widens what a confused session can do. It should refuse
  to launch outside a registered worktree of the calling project.

## Alternatives considered

**One working tree, a lock per file.** Rejected: it does not solve the
build directory, it does not solve the push gate, and ANTS-4881 warns
against a blanket mutex — most verbs are reads, and a global lock costs
every session to protect a rare write.

**Keystroke injection into standing sessions.** Rejected: ANTS-1979 and
ANTS-2195 settled it, and nothing about parallel work changes the
mechanism. D3 gets the same result without it.

**A separate task database.** Rejected: `item.extras` costs no
migration, and D4's one-way-door reasoning is decisive.

**Claude Code subagents instead of sessions.** Not a replacement — they
share the parent's tree and its context budget, which is exactly the
constraint this ADR is trying to lift. They remain the right tool inside
a task.
