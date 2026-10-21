# git_hooks_report — `git_state op:"hooks"`

Feature contract for ANTS-5505. `git_state op:"hooks"` answers in one call
whether a repository's push gate is wired up. It reports facts and no
verdict; `~/.claude/tools/ci-gate` keeps the verdicts.

The machine-wide hook is `${ANTS_GLOBAL_HOOKS:-$HOME/.claude/githooks}/pre-push`.

## What this locks

- **INV-1** — `hooks_path` carries `core.hooksPath`'s `value`, `scope` and
  `origin` as git reports them, all null when it is unset, and `dir`: the
  absolute hooks directory git uses either way.
- **INV-2** — `pre_push.kind` is `none` when the hooks directory holds no
  pre-push, and `machine_wide` when it is the machine-wide hook's file. It
  is `hands_off` when it names the shared hook and `exec`s a variable with
  `"$@"`, and `own` otherwise. A hook that names the shared hook but does
  not exec it is `own`.
- **INV-3** — `gate_keys` holds every `ants.gate.*` key with its `value` and
  `source`: `git_config` or `.ants/gate.conf`. Git config wins where both
  set a key. Key names are as git prints them, in lower case.
- **INV-4** — `gate` resolves as the hook does. `ants.gate.command` gives
  `source:"config"`, with a leading `./` dropped and an absolute path inside
  the repository made relative. One outside it sets `outside_repo:true`.
  With no command, the hook's `--gate-candidates` list is asked
  (`candidates_source:"machine_hook"`, or `"unavailable"`), and the first
  executable candidate gives `source:"discovered"`. None found: `path` is
  null.
- **INV-5** — `workflows` lists `.github/workflows/*.yml` and `*.yaml`.
  `gate.workflows_calling` names those whose text contains the gate's
  basename, and `gate.called_by_ci` says whether any does.
- **INV-6** — `gate_passed.tree` is HEAD's tree. `passed` is true, and
  `record` is the file's first line, when
  `<git-common-dir>/ants-gate-passed/<tree>` exists.
- **INV-7** — a directory that is not a repository refuses `not_git_repo`.

## Test

`test_git_hooks_report.cpp` builds temporary repositories with global and
system git config switched off. A fixture machine-wide hook prints a short
`--gate-candidates` list. Each case calls `RemoteControl::cmdGitState`.
