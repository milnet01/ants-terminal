# Ants Terminal — Discovery

> **Purpose — so that later, anyone can tell whether the thing being
> built is still the thing that was wanted.**

This is what every feature is checked against. It passes as a gate
(`~/.claude/workflow.md` § 3) when a stranger could read it and say
whether a given feature serves it.

**Status:** agreed (2026-09-28).

## The problem

Claude Code in an ordinary terminal spends more tokens than the work
needs, and the user pays for every one. The terminal knows nothing about
the sessions running in it, so the user cannot see what each is doing or
costing. And a fix to the terminal's Claude tools means restarting it,
which kills every Claude session running inside.

## Who it is for

- A person who runs Claude Code on Linux every day and pays for its
  tokens. The first such person is the project's author.

A feature that serves only people who never use Claude Code is
secondary: welcome, but not what the project is judged on.

## Signs it is working

Labels are `SIGN-<n>`. They are never reused or renumbered.

- **SIGN-1 — Cheaper.** On a named, repeatable Claude Code task, a
  session in Ants costs at least 20% less than the same task in a plain
  terminal with no Ants tools, averaged over repeated runs. Cost is the
  tokens weighted at their price. The design names the task.
- **SIGN-2 — Fast.** On the same machine, Ants prints a large burst of
  output no slower than Konsole, and typing does not lag while Claude
  Code streams output.
- **SIGN-3 — No relaunch.** A change to how Ants' Claude tools behave
  takes effect in a running terminal without a restart, and the Claude
  sessions running in it survive. Only a new kind of terminal state for
  the tools to read needs a restart.
- **SIGN-4 — Programs just work.** Claude Code, vim, htop, tmux and less
  display and respond correctly, and Ants passes vttest's core screens at
  a pass level the design sets.
- **SIGN-5 — You can see what Claude is doing.** Without leaving the
  terminal, you can see each Claude session's status, its token cost so
  far, and the project's roadmap.

## What it deliberately does not do

- **Never:** be a code editor or an IDE. It is the window Claude Code runs
  in.
- **Never:** run or host AI models itself. It makes Claude Code cheaper;
  it does not replace it.
