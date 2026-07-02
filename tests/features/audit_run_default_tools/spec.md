# ANTS-3418 — audit_run drops mypy from the auto-detect default tool set

## Problem

`audit_run` auto-detects its tool set when the caller omits `tools`: it
takes every known tool that is runnable on PATH. The audit runner invokes
each tool DEPS-LESS (no `uv sync` / venv), so a full-sweep `mypy` on any
project that imports third-party libraries emits dozens of
`import-not-found` / `import-untyped` findings — the libraries simply aren't
importable in the runner's environment. The project's REAL, deps-installed
`mypy` (run in CI and pre-commit) is clean, so these are pure tool-env false
positives that dominate every sweep's raw count and force a re-triage each
`/close-phase` (MAME Curator feedback 2026-07-02).

## Fix

Drop mypy from the AUTO-DETECT default set only. It stays a known tool, so an
explicit `tools:["mypy"]` still runs — a caller who has arranged a
deps-installed environment can still request it. CI + pre-commit already run
the real deps-installed mypy, so a deps-less sweep mypy adds no signal.

## Invariants

### INV-1 — mypy stays a known tool

`kKnownTools()` still lists `mypy`, so an explicit `tools:["mypy"]` passes
the allowlist validation (`!kKnownTools().contains(t)` → `bad_tool`) and runs.

### INV-2 — auto-detect set excludes mypy

`kAutoDetectTools()` derives from `kKnownTools()` and removes `mypy`. It is
the set used to seed `wantedTools` when the caller omits `tools`.

### INV-3 — empty tools uses the auto-detect set

`runAudit` assigns `kAutoDetectTools()` (not `kKnownTools()`) when
`req.tools` is empty.

## Test plan

Source-scrape of `src/auditrunner.cpp`: INV-1 asserts `kKnownTools()` still
contains `mypy`; INV-2 asserts `kAutoDetectTools()` exists, derives from
`kKnownTools()`, and drops `mypy`; INV-3 asserts the empty-`tools` default in
`runAudit` references `kAutoDetectTools()`.
