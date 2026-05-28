# auto_switch_surfacing — feature-conformance spec

Tests for ANTS-1893 (switch-event surfacing). See
`docs/specs/ANTS-1893.md` for the full invariant catalogue
(INV-1..INV-15). Suite name: `AutoSwitchSurfacing`.

## Coverage status (2026-05-28)

Initial implementation ships with the most-easily-driven subset.
The remainder of the 15-invariant test surface needs a
TerminalWidget test seam (the helper takes `TerminalWidget*` for
`shellPid()` / `sendToPty()` / `shellCwd()` access). Tracked as a
follow-up roadmap item; the implementation itself is feature-
complete and exercised end-to-end via manual runtime verification
(see ANTS-1893 §10 acceptance log).

| Invariant | Test file | Status |
|-----------|-----------|--------|
| INV-11 (config defaults TRUE) | `test_config_surfacing_defaults.cpp` | shipped |
| INV-1..10, 12..15 | (various) | deferred — needs TerminalWidget seam |

## Bundle

All tests target `test_claude` per the spec's §7 plan.
