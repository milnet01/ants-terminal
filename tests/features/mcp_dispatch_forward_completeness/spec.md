# ANTS-1642 — MCP dispatch-forward completeness

## Context

Three live instances of the same bug shape have shipped to users
unnoticed:

- **ANTS-1437** — `roadmap_query` MCP-dispatch lambda silently dropped
  the new `mode` arg.
- **ANTS-1398** — same lambda silently dropped `include_section_headers`.
- **ANTS-1586** — same lambda silently dropped `include_body`.

In each case the schema in `src/claudeintegration.cpp` advertised a new
property, but the corresponding `registerToolProvider` lambda in
`src/mainwindow.cpp` didn't forward the property to the underlying IPC
verb. The verb then read the default at every call and the new option
silently did nothing — discovered only when a downstream session
reported the missing field.

## Invariants

### INV-1 — every schema-advertised prop is read by the dispatch lambda

For each tool registered in `claudeintegration.cpp` via
`t["name"] = "TOOL"` … `tools.append(t)`, collect the set of property
names declared as `props["X"] = …`. For each such tool, locate the
matching `registerToolProvider("TOOL", [...]{ … })` block in
`mainwindow.cpp` and assert that the lambda body reads each property
via `args.value("X")` (literal or `QStringLiteral` wrapper).

### INV-2 — direct-args lambdas count as forwarding everything

A lambda that hands `args` directly to a `cmd…(args)` invocation
implicitly forwards every property. The IPC handler reads the input
object directly, so per-arg forwarding inside the lambda would be
redundant. Such lambdas are exempt from INV-1's per-prop assertion.

### INV-3 — transport-handled props are exempt

`etag_match` is consumed by `ClaudeIntegration::applyEtagPattern` at
the dispatcher layer before the lambda runs. Lambdas may legitimately
ignore it.

### INV-4 — explicit allow-list for known-good ignored props

A small allow-list of `(tool, prop)` pairs covers cases where a prop
is intentionally not forwarded (e.g. deprecated args still listed in
the schema for back-compat). Each entry carries a `Reason:` comment
naming the rationale. Empty as of ANTS-1642 — added only as escape
hatch.

## Failure mode

When INV-1 fails the test prints the full `(tool, missing_prop)` list
so a fix lands in one pass instead of a re-run per prop.

## Maintenance

When adding a new MCP tool with a selective-forward lambda, either:

- ensure every new `props["X"]` has a matching `args.value("X")` in
  the lambda, OR
- pass `args` directly to the cmd handler (INV-2 then exempts the tool).

When adding a deliberately-ignored prop, add the `(tool, prop)` pair
to the test's allow-list with a `Reason:` comment.

## Source

In-session 2026-05-19 follow-up to ANTS-1586 (the third instance of
the same dispatch-forward bug shape).
