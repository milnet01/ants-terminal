# Feature: config parse-failure guard

## Problem

The 2026-04-23 `/indie-review` multi-agent sweep flagged this silent-
data-loss pattern in three separate subsystems:

```
1. Read file.
2. Parse.
3. If parse fails, `root` stays default-constructed (empty).
4. Next save() writes the empty default over the user's file.
```

The user's prior settings are irretrievably destroyed by whatever
setter happened to run next (window-move, theme-switch, tab-open —
all of which call save() unconditionally). There's no backup, no
error surface, no way to recover.

This spec covers the `Config` callsite (`src/config.cpp` load/save).
The pattern applies identically to `claudeallowlist.cpp` and
`sessionmanager.cpp` — each gets its own spec and fix.

## External anchors

- [Qt docs — QSaveFile](https://doc.qt.io/qt-6/qsavefile.html) —
  prescribes commit-after-full-write semantics; Qt recommends using
  `QSaveFile` rather than manual rename for exactly this class of bug.
  We don't switch wholesale because we already have fsync+atomic-rename
  in place; the missing piece was the parse-failure guard.
- [CWE-20](https://cwe.mitre.org/data/definitions/20.html) —
  Improper Input Validation. Failing to distinguish "no file" from
  "corrupt file" is the textbook shape.

## Contract

### Invariant 1 — load() distinguishes three states

On construction, `Config::load()` must recognise:

- **File does not exist** → clean defaults, `loadFailed() == false`.
  First run on a fresh user account. save() proceeds normally.
- **File exists and parses as a JSON object** → `m_data` populated,
  `loadFailed() == false`. The happy path.
- **File exists but fails to parse (or can't be opened)** →
  `loadFailed() == true`. The dangerous path.

### Invariant 2 — on parse failure, the corrupt file is rotated aside

When load() enters the "exists but failed to parse" path, the
corrupt file MUST be copied to `config.json.corrupt-<ms_timestamp>`
in the same directory before any write path can run. This preserves
the user's prior bytes so they can hand-fix or copy back.
`loadFailureBackupPath()` returns the path of the rotated backup.

### Invariant 3 — save() is suppressed when loadFailed is latched

Once `m_loadFailed == true`, every call to `save()` MUST be a no-op
until the Config is re-constructed (app restart). This prevents the
next setter — which always calls save() — from overwriting the
corrupt-but-intact config.json with fresh defaults and destroying
the user's recovery path.

### Invariant 4 — std::rename return value is checked

When save() does run (happy path), the `std::rename(tmp, final)` call
MUST have its return value checked. On failure:

- The tmp file MUST be removed so it doesn't accumulate across
  session lifetimes.
- A `DebugLog::Config` entry MUST be written with errno context.
- `config.json` MUST be left untouched (which atomic-rename
  guarantees on success, and is what we want on failure too).

### Invariant 5 — `.corrupt-<ms>` backup retention is capped

`rotateCorruptFileAside` (shared helper in `secureio.h`) MUST cap
the number of retained `.corrupt-*` siblings at the newest 5.
A user who repeatedly opens Ants with a broken config would
otherwise accumulate one backup per launch forever; the cap keeps
five recovery snapshots while bounding the footprint. Older
siblings are ranked by mtime (not filename timestamp) so files with
skewed clocks still prune deterministically.

## How this test anchors to reality

The test constructs a `Config` against an isolated `XDG_CONFIG_HOME`,
plants a malformed `config.json` in it, constructs a fresh `Config`,
then:

- Asserts `loadFailed()` is true and the backup file exists.
- Calls a setter (triggering save()) — asserts the corrupt file is
  NOT overwritten.

If either invariant drifts, this test fires. The anchor is the
on-disk byte contents of the corrupt file — a direct, objective
signal.

## Regression history

- **Introduced:** since the initial config implementation. The bug
  is the `if (doc.isObject()) m_data = doc.object();` pattern with
  no else-branch; harmless-looking and universal across Qt/JSON
  codebases. Took a fresh-eyes review to name.
- **Discovered:** 2026-04-23 via `/indie-review`. Config subsystem
  reviewer flagged the load-without-else; IPC subsystem reviewer
  independently flagged the same shape in claudeallowlist.cpp and
  sessionmanager.cpp (cross-cutting theme).
- **Fixed:** 0.7.12 — load() three-way branch, backup rotation,
  save() suppression, std::rename return check. Allowlist and
  session-manager analogs ship in separate specs.
