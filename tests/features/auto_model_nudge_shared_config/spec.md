# Feature: the auto-model nudge saves through MainWindow's Config

## Problem

`MainWindow::showClaudeAutoModelNudge` (`src/mainwindow_tabs.cpp`)
recorded its answer through a fresh `Config` object. That write never
reaches `m_config.lastWrittenBytes()`, so `onConfigFileChanged` reads it
as an external edit (`docs/standards/config-hot-reload.md` C1, C2): it
reloads `m_config`, closes a cached Settings dialog, and shows the
"Config reloaded" toast. Until that reload lands, `m_config` holds the
old values, and its next save can write them back over the answer.

## Contract

The nudge writes both settings through `m_config`.

## Invariants

**INV-1 — `showClaudeAutoModelNudge` constructs no `Config`.**

**INV-2 — it still writes both settings through `m_config`.** The body
calls `m_config.setClaudeAutoModelNudgeShown` and
`m_config.setClaudeAutoModelSwitch`, so the fix cannot pass by
deleting the writes.

## Scope

In scope: `showClaudeAutoModelNudge` only. Other fresh `Config`
objects in `src/` are out of scope here.
