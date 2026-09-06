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
| Shared work record | the machine-global roadmap store, WAL, `busy_timeout` set (explicit `PRAGMA`; Qt's QSQLITE default is 5000 ms, and the store carries a second bulk profile) | **A contended writer blocks up to `busy_timeout` and then FAILS.** ANTS-3756 INV-16 makes that a contract: a write that cannot take the lock in time "fails and reports; it is never retried silently and never dropped". So every worker store write owes a failure branch — see D4. The same section requires `BEGIN IMMEDIATE`: a deferred read-then-write returns `SQLITE_BUSY` on the upgrade WITHOUT honouring `busy_timeout` at all. The WAL switch is separately uncovered, taking an EXCLUSIVE lock below the busy handler |
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

**The ban is on the RENDERED FILES, not on the store.** A worker makes
THREE store writes — the lease claim and its heartbeat (D4), the outcome
report (D4), and the declaration (D10) — and **none of them may go
through the render-and-publish path.** Each is a direct conditional
`UPDATE` of one row. Anything routed through `roadmap_log` republishes
the whole store into the orchestrator's checkout, which is problem 2
exactly. Naming all three rather than only the lease, because a rule
stated about one of them invites the other two through the front door.

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

**The lease record.** `owner` is the worker's **worktree path**, which is
unique per task, stable for the worker's life, and — unlike a shell PID —
neither ephemeral nor OS-reused. (D3 rules out addressing a worker by
session, and `session_message` cannot name one either, so this is the
only identity both sides can agree on.) It carries an expiry and a
heartbeat, and **the expiry must exceed a D6 lock wait**: a worker
legitimately blocks behind another build for minutes, and a reaper that
cannot tell that from a hang will reclaim live work.

**Every store write on this path owes a failure branch**, because a
contended write FAILS at the deadline rather than retrying (see the table
above, and ANTS-3756 INV-16). A claim that silently returns nothing is
indistinguishable from losing the race, which is how two workers end up
believing they own one task. Open the claim, the heartbeat, the report
and D10's declaration with `BEGIN IMMEDIATE`.

**An expired lease returns the task to the queue**, and requeue is a
common path rather than an edge — it is also where an unresolvable rebase
(D9) and a red post-merge (D5) land. So it must say what becomes of the
prior worktree and branch, both of which D1 keys on the task id and both
of which still exist after a hang: **fence first, then reap.** The
orchestrator confirms the holder is gone (`tab_list` shows no live
session at that `cwd`), then removes the worktree and deletes the branch,
then re-deals. Deleting a directory under a worker that is still writing
is the failure this ordering exists to prevent.

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
3. the diff stays inside the lane the task named. **A lane is a set of
   file paths** — that is what phase 1's task record stores and what this
   check evaluates, because a diff is a set of paths and anything else
   needs a translation step neither side has. (D9's symbol granularity
   narrows which tasks are PREFERRED together; it does not change what
   this gate compares. Sharpening the lane to symbols is ANTS-4914's
   successor work.)

A task meeting all three **merges without asking** (project lead,
2026-09-06). The three checks are mechanical, so a human approving them
adds latency without adding judgement. What a human is asked about is a
task that FAILS one.

Merges are serial, and **the gate re-runs after each merge**, because at
volume the dominant source of new defects stops being the original code
and becomes the previous fixes.

**When the post-merge re-run goes red**, the orchestrator **resets `main`
to the pre-integration commit** — not `git revert -m 1`, which needs a
merge commit that D9's rebase path never creates — requeues the task with
the failure attached, and **stops dealing until a human answers**.
Continuing would cut every subsequent worker branch from a known-broken
`main`. The reset is safe because integration is serial: nothing else has
landed since.

**Where the pre-merge gate runs: in the WORKER's worktree, not the main
checkout.** The orchestrator's checkout must stay on `main` — D2 has it
rendering `ROADMAP.md` there, and ANTS-4839 publishes that render onto
whichever branch is checked out, so borrowing it for a branch build would
publish the roadmap onto a worker branch. The worktree is warm from the
worker's own narrow builds (D6), so this is also the cheaper tree. The
orchestrator's own checkout runs only the POST-merge gate, on `main`.

This raises the cost of a weak suite: with auto-merge, the suite is the
only thing between a confidently-wrong worker and `main`.

### D6 — One build at a time machine-wide; workers build narrow, the orchestrator builds full

**Builds are the RAM hog, and sessions are not** (project lead,
2026-09-06). Several Claude sessions sitting idle cost little; two
`cc1plus` storms at once is what reaches the earlyoom ceiling on a 32 GiB
host. `JOB_POOLS` already caps `compile_pool` and pins `link_pool = 1`
WITHIN one build — the pools are per-invocation, so a second concurrent
build doubles the peak the first one was capped to.

**So: at most ONE build process machine-wide (N=1), and the work is split
by weight.**

- **A worker may build its own narrow target and run focused tests.** It
  still takes the lock. This is what lets a worker watch a test FAIL
  before writing the fix, which `testing.md` § 1 and `write-test` both
  require — a worker that cannot build hands back code that has never
  executed, and every defect then surfaces at merge, serially, in the
  most expensive place to find one.
- **Only the orchestrator runs the FULL build and the FULL suite**, at
  merge (D5). It runs in one warm tree, so ccache stays hot where it
  matters most.

Peak RAM is identical to forbidding worker builds outright — one build,
ever — and the difference is bought entirely from ordering rather than
from concurrency. Measured on this machine with ccache hot: a narrow
target build is 5-60 s, a full build 1-2 minutes, the full suite ~55 s at
`-j4`. Waiting for a turn is cheap; discovering a defect at merge is not.

The orchestrator-only variant was considered and declined for the reason
above: it is simpler to state and it makes every worker's output
unverified until it lands.

**It is an `flock` on `${XDG_STATE_HOME:-$HOME/.local/state}/ants-terminal/build.lock`,
not a store row.**

**The literal path is part of the decision, not an implementation
detail.** Its acquirers are separate artefacts built in different phases
— a shell hook and `ci-parity.sh` in phase 0a, the worker's narrow build
in phase 3, the orchestrator's gate in phase 4 — and mutual exclusion
exists only if every one of them names the identical file. Two spellings
of the same intent exclude nothing, and the failure is silent until two
`cc1plus` storms meet the earlyoom ceiling. `$XDG_STATE_HOME` is commonly
unset, so the fallback is named too rather than left to each caller to
invent.

**Extent: the lock covers the BUILD only, and is released before the test
run and before any `git push`.** An orchestrator holding it across D5's
gate would then block on its own `pre-push` hook, which waits rather than
failing — a deadlock with no timeout. Tests are not the RAM hog the lock
exists for; concurrent `cc1plus` is.

Three reasons it is a file lock rather than a store row, and the first is
decisive:

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

### D9 — Two workers MAY share a file. Overlap is handled at merge, not forbidden at dealing.

Raised by the project lead, 2026-09-06: *how do two sessions work on one
file — both take a copy, submit back, and the orchestrator merges?*

That is already what D1 gives us: each worker's worktree holds its own
copy of every file, so two workers on one file are two branches, and the
orchestrator merges. The decision is what happens at merge, and it is
three different problems wearing one name.

**1. Different regions of one file — merge cleanly, and that is the
common case.** Git handles it with nothing added. An earlier draft of
this ADR refused to deal any task whose FILE overlapped one in flight;
that is too strict, and it serialises exactly the work the oversized
files already serialise. Withdrawn.

**2. Overlapping regions — a textual conflict, visible and cheap.** The
second task is REBASED onto the merged `main`, not merge-committed: it
replays against current code, so the conflict is resolved in the place
that has both halves. Where the rebase cannot be resolved mechanically
the task is requeued with the failure attached, and its brief now names
the change that landed underneath it.

**3. No textual conflict, but a semantic one.** Worker A renames a
function; worker B adds a caller of the old name. Both diffs apply
cleanly. **No partitioning scheme detects this**, because the conflict is
not in the text either worker wrote — only something reading the MERGED
result can see it.

**In C++ the compiler usually is that something, and an earlier draft of
this ADR overstated the danger by ignoring it.** A stale caller of a
renamed function does not compile; nor does a changed signature or return
type. D5's post-merge build catches those without anything else being
built.

**The residue is the part the compiler never type-checks**, and it is
where this project has actually been bitten: the Lua plugin surface
(`ants.*` names), JSON config keys, MCP verb and argument names, string
literals that tests match on, doc citations and spec `INV-` references,
and a changed DEFAULT or behaviour behind an unchanged signature. Those
merge clean, compile clean, and are wrong. D10 is what covers them.

**So the dealing rule is a preference, not a prohibition.** The
orchestrator prefers non-overlapping tasks when it has the choice —
ordering two overlapping tasks costs one wait, while running them
concurrently and requeueing costs one task's work twice.

**It refuses nothing on the grounds of a rename. D10 supersedes the
carve-out an earlier draft had here**, and the draft was wrong twice
over: it refused exactly the work Colony exists to parallelise, and it
used a FILE-overlap test to catch case 3 — which this section has just
finished saying no partitioning scheme can detect. A file-overlap
refusal blocks safe pairs and deals the disjoint-file pairs that are
actually dangerous. Declare-and-sweep (D10) is the mechanism; preferring
disjoint work is a cost heuristic and nothing more.

Partitioning at SYMBOL rather than file granularity narrows case 2 —
`codebase_index` already knows which symbols live where — and is what
ANTS-4914's glossary would sharpen. It does nothing for case 3.

### D10 — A worker DECLARES a change the compiler cannot check, and the orchestrator verifies the declaration

Project lead, 2026-09-06: rather than forbidding the rename case, require
the worker to tell the orchestrator the old name and the new one, so the
merge can check other workers' code for references to the old.

This is better than forbidding it, and the reason is that **renames are
most frequent during exactly the work Colony exists to parallelise** —
the structural refactors of ANTS-1043 / ANTS-1044. A rule that refuses
them refuses the main use case.

**What must be declared is decided by two questions, in this order.** The
kind of thing renamed — variable, constant, function, method, class,
field, module, type, enum member, macro — is NOT the axis; a list of
kinds goes stale and invites the absurd reading that a loop counter needs
declaring.

**Q1 — REACH: can anything outside this file refer to it by name?**
A local, a private helper used in one file, a lambda's parameter: no.
Nothing else can hold a stale reference, so nothing is owed however many
of them a task renames. Everything below assumes the answer is yes.

**Q2 — CHECKEDNESS: would this language's own checker catch a stale
reference, in a run we already make?** If yes, D5's post-merge build
covers it. If no, declare it.

**The answer to Q2 is per-language, and that is the point rather than a
caveat** — the same rename is free in one language and dangerous in
another:

| Language | Stale reference to a renamed public name | So a rename |
|---|---|---|
| C++, C, Rust, Go, Java | fails to BUILD | needs no declaration |
| TypeScript | fails to type-check, where the project runs `tsc` | needs none if it does |
| Python, Lua, JavaScript, Ruby | fails at RUNTIME, only on a path that executes — so only where a test covers it | **must be declared** |
| Shell | fails at runtime, silently in many cases | **must be declared** |

So a Python or Lua worker declares nearly every non-local rename, and a
Rust worker declares almost none. That is not an inconsistency to iron
out; it is the checker doing more work in one language than the other.

**And some names are unchecked in EVERY language**, because nothing
type-checks a string. These are always declared, whatever the source
language is:

- a name reached across a language boundary — a Lua binding, an FFI or
  extension symbol, an MCP verb or argument name;
- a **config key**, JSON/YAML field, environment variable or CLI flag;
- a **string literal a test matches on**, which this project has been
  bitten by more than once;
- a renamed or moved **file** that documents, tests or build files cite
  by path;
- a **doc citation** or spec `INV-` id another document quotes;
- a changed **default value or behaviour** behind an unchanged signature
  — the one entry here that is not a rename at all, and the least
  visible of them.

The declaration is `{old, new, kind, language}` and rides the task record
(D4), which the orchestrator already reads.

**`language` narrows the sweep for a SOURCE-language rename only — never
for the always-declare list above.** Those entries are declared precisely
because they cross a boundary: a Lua binding's stale references live in
`.cpp`, an MCP verb name lives in schema strings and docs, a config key
lives in JSON and prose. **They are swept unscoped**, across source,
tests, docs and config. Scoping them by the language they were written in
would skip exactly the references the entry exists to catch.

For an entry with no source language — a JSON key, a moved file path, a
doc citation — `language` is `null`, which reads as "sweep everything"
rather than as a missing field.

**The orchestrator verifies rather than trusts.** On each merge it sweeps
the merged tree for every declared `old` token still present, across
source, tests, docs and config — `workspace_search` is the instrument and
it is one call per declaration. A survivor is a merge failure and takes
D5's post-merge path.

**And it derives what it can, so an undeclared change is still caught.**
A declaration is a hint that makes the check precise and cheap, never the
only line of defence: the post-merge build and suite catch the checked
cases regardless, and a token sweep over the symbols a branch REMOVED
catches the common undeclared rename without anyone remembering to say
so. A standard that relies on being remembered is a standard that fails
silently, which is the failure this whole design is built to avoid.

**The derive side is already multi-language here**, which is what makes
that backstop credible rather than aspirational: `find_definition`
resolves C++, Python, Lua, shell and GLSL, and `codebase_index` covers
the brace family plus Python. The languages where Q2 says *declare* are
largely the languages the index can still enumerate symbols for.

**This wants its own standard**, not a paragraph here — ANTS-4915. An ADR
records the decision; the rules a worker conforms to belong where a
worker will read them.

### D8 — Build phases 0-2, then measure, before building 3-5

Project lead, 2026-09-06.

| Phase | Delivers | D-numbers |
|---|---|---|
| 0a | Build lease (`flock` at the named path), build-only extent, hooks and `ci-parity.sh` included | D6 |
| 0b | Orchestrator owns the rendered files; worker store writes bypass the render, with `BEGIN IMMEDIATE` and a failure branch | D2, D4 |
| 1 | Queue + lease + task record in `extras` — owner, expiry, heartbeat, **lane as a file-path set, and D10's `{old, new, kind, language}` declaration slot** | D4, D5, **D10** |
| 2 | `colony_spawn`: `launch` as a guarded MCP verb, worktree-aware `cwd`, no `raw` | D1, D3 |
| 3 | `colony-worker` skill — claim, work (narrow builds under the lease), **write the declaration**, report, exit | D3, D6, D7, **D10** |
| 4 | `colony-orchestrator` skill — partition, deal, verify in the worker's worktree, **sweep declared tokens**, merge serially, rebase or requeue, reset on red | D5, **D9**, **D10** |
| 5 | Fleet view — worker state, spend, queue depth | — |

**D9 and D10 are split across phases rather than owning one**, which is
why an earlier draft of this table omitted them entirely: the declaration
is a FIELD in phase 1, a worker OBLIGATION in phase 3, and an
orchestrator CHECK in phase 4. A builder scheduling from the table alone
would otherwise ship a task record with no slot for it and a merge with
no sweep.

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
- **Partitioning is a preference, not a guarantee** (D9). Two tasks in
  different regions of one file merge cleanly; overlapping ones rebase or
  requeue; and a semantic conflict is invisible to any partition and is
  caught only by the post-merge gate.

  **This makes file SIZE a throughput variable, which is the strongest
  argument this project has yet had for its structural refactors**
  (project lead, 2026-09-06). **`mainwindow.cpp` is 8,020 lines and
  `auditdialog.cpp` 6,332**, measured 2026-09-06; every task touching
  either one excludes every other task that would. (ANTS-1043 and
  ANTS-1044 still carry the 2026-04-27 review's figures of 6,162 and
  5,749 — both files have grown by roughly a third since, which sharpens
  the argument rather than weakening it.) Splitting them does not merely tidy the tree —
  it converts one serialised lane into several parallel ones, so
  ANTS-1043, ANTS-1044 and ANTS-1049 are Colony ENABLERS rather than
  unrelated tier-3 work. The corollary is worth stating too: measuring
  Colony's speedup (D8) on a codebase whose two largest files are
  undivided measures the partition, not the design.
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
