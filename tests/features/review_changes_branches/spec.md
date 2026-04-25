# Feature: Review Changes dialog — branch-aware + live updates

## Problem

`MainWindow::showDiffViewer()` ran three async git probes
scoped to the current branch's working tree:

1. `git status -b --short` — current branch porcelain.
2. `git diff --stat --patch HEAD` — worktree vs current HEAD.
3. `git log --oneline --decorate @{u}..HEAD` — current branch's
   unpushed commits.

A user with five feature branches each holding unpushed work
would see "no unpushed" if they happened to be sitting on a
clean branch when they opened the dialog. Branches without
upstreams, or with diverged ahead/behind state, were entirely
invisible. Specifically reported by user 2026-04-25: "the
Review Changes dialog doesn't consider changes in other
branches of the project."

## Fix

Two additional probes drop in alongside the existing three:

- `git for-each-ref refs/heads
   --format='%(refname:short)\t%(upstream:short)\t
   %(upstream:track)\t%(subject)'` — every local branch with
  its upstream, ahead/behind track string, and tip-commit
  subject. Renders as a sortable summary.
- `git log --branches --not --remotes --oneline --decorate` —
  every commit reachable from any local branch but NOT from
  any remote-tracking branch. The git-native shape of "what
  haven't I pushed *anywhere*?"

Both probes are O(refs) and finish in milliseconds even on
large repos. The dialog now shows five sections: Status,
Unpushed (current branch), Branches, Unpushed across all
branches, Diff.

## Contract

### Invariant 1 — ProbeState declares the new fields

`MainWindow::showDiffViewer` defines a local `struct ProbeState`
with `branches` and `crossUnpushed` QString members alongside
the pre-existing `status`, `diff`, `unpushed`. The pending
counter is initialized to 5 (matching the five async probes).

### Invariant 2 — both new probes are spawned

`runAsync` is called with `for-each-ref refs/heads` (writing into
`ProbeState::branches`) and `log --branches --not --remotes`
(writing into `ProbeState::crossUnpushed`).

### Invariant 3 — finalizer renders both new sections

The finalize lambda emits "Branches" and "Unpushed across all
branches" section headers when the corresponding state field is
non-empty.

### Invariant 4 — Copy Diff includes the new sections

The Copy Diff handler appends the branches summary and cross-
branch unpushed log to the clipboard payload, so a user pasting
into a chat / PR description gets the full picture.

### Invariant 5 — empty state is honest

The "no status, no diff, no unpushed" empty-state message only
fires when ALL FIVE state fields are empty. A repo with branches
but no current-HEAD changes still renders the Branches section
instead of falling through to the empty-state copy.

### Invariant 6 — runProbes lambda is re-callable

`MainWindow::showDiffViewer` body must factor probe spawning
into a `runProbes` lambda that constructs a fresh `ProbeState`
on each call. Without this, the second invocation would
decrement a previous-render's pending counter, race the two
finalizers, and render the union of both probe sets (including
stale data).

### Invariant 7 — QFileSystemWatcher armed on .git paths

The dialog must construct a `QFileSystemWatcher` watching:

- the working directory itself (untracked-file additions),
- `.git` (the directory),
- `.git/HEAD`, `.git/index`, `.git/refs/heads`,
  `.git/refs/remotes`, `.git/logs/HEAD`.

`fileChanged` and `directoryChanged` signals connect through a
debounce `QTimer` (single-shot, ~300 ms) that calls runProbes
on timeout. The 300 ms window collapses the burst of
fileChanged events that `git pull` / `git fetch` fire.

### Invariant 8 — atomic-rename re-watch

QFileSystemWatcher loses the watch on a file that's atomically
replaced (rename(2) — git's standard pattern for HEAD/index/
logs/HEAD updates). On `fileChanged`, the source must re-add
the path if the file still exists. Without this, the watcher
fires once per file and then goes silent.

### Invariant 9 — manual Refresh button bypasses debounce

A `QPushButton` with objectName `reviewRefreshBtn` invokes
`runProbes` directly on click. No debounce — the user clicked
to get an immediate response.

### Invariant 10 — live status indicator label

A `QLabel` with objectName `reviewLiveStatus` shows refresh
state: "● refreshing…" while probes run, "● live — auto-refresh
on git changes" when complete. Lets the user see the dialog is
actually wired up (not just stale).

## How this test anchors to reality

Source-grep on `src/mainwindow.cpp`:

1. ProbeState struct contains `branches` and `crossUnpushed`
   QString members and the pending count is 5.
2. Two `runAsync` calls reference the new git args
   (`for-each-ref`, `--branches --not --remotes`) and the
   matching ProbeState slot pointers.
3. Finalize emits both new section headers.
4. Copy handler concatenates both new sections.
5. Empty-state guard mentions all five fields.

## Regression history

- **Latent since `showDiffViewer` shipped.**
- **Reported:** 2026-04-25 — "the Review Changes dialog doesn't
  consider changes in other branches of the project."
- **Fixed:** 0.7.32.
