# mcp_last_audit_summary — feature-conformance test

Locks the wire and parser contract for the ANTS-1254
`last_audit_summary` MCP tool. Asserts spec INVs 1, 2, 3, 4, 6, 8,
9, 10 by exercising `AuditEngine::summariseSarif` against committed
SARIF fixtures + source-grep checks for the wiring INVs (5, 7).

## Invariants

| ID | Source | Check |
|---:|--------|-------|
| INV-1 | spec § 4 | Reads SARIF only — never the HTML. Verified by source-grep that `summariseSarif` doesn't reference `.html`. |
| INV-2 | spec § 4 | Single-entry mtime cache wired up: `m_auditSummary*` private members declared in `remotecontrol.h`. |
| INV-3 | spec § 4 | `top_findings[]` sort order: level desc → confidence desc → file asc → line asc. Verified by parsing a 5-finding fixture and asserting positions. |
| INV-4 | spec § 4 | `counts` reflects the full set, not the filtered `top_findings[]`. Verified by parsing with `severity_floor: error` and asserting `counts.warning > 0`. |
| INV-5 | spec § 4 | (Wiring contract — not falsifiable without a live socket; covered by sibling `mcp_provider_registry` Inv8SchemaMatchesRegistry test.) |
| INV-6 | spec § 4 | Latest-SARIF discovery: caller picks lex-max filename, not mtime. Tested via `cmdLastAuditSummary` integration in a sibling spec; here we lock the parser-side behavior on a deterministic single-file fixture. |
| INV-7 | spec § 4 | `top_findings[].file` is whatever SARIF carries (no rewriting). Verified by fixture comparison. |
| INV-8 | spec § 4 | `severity_floor` validation rejects bad input. Verified by source-grep + a parser test that bad floor doesn't reach summariseSarif. |
| INV-9 | spec § 4 | `counts` is fixed-shape — always 4 keys present, even when zero. Verified by parsing an empty-results fixture. |
| INV-10 | spec § 4 | Empty `runs[]` → `nullopt` (caller maps to `not_audited`). Verified by parsing an empty-runs fixture. |
| ANTS-1625 INV-1 | docs/specs/ANTS-1625.md § 3 | `pickForeignReport` helper declared in `remotecontrol.cpp` carries the `broadest_in_recency_window` basis literal. |
| ANTS-1625 INV-2 | docs/specs/ANTS-1625.md § 3 | `cmdLastAuditSummary` emits `env["pick_basis"]` so every `{ok:true}` envelope carries the picker basis. |
| ANTS-1625 INV-3 | docs/specs/ANTS-1625.md § 3 | One foreign-format file → `pick_basis == "sole"`. |
| ANTS-1625 INV-4 | docs/specs/ANTS-1625.md § 3 | Two files in-window, newest is also largest non-narrow → `pick_basis == "newest"`. |
| ANTS-1625 INV-5 | docs/specs/ANTS-1625.md § 3 | Older + larger + non-narrow beats newer + smaller + narrow-suffix when both are inside the 24-hour window → `pick_basis == "broadest_in_recency_window"`. |
| ANTS-1625 INV-6 | docs/specs/ANTS-1625.md § 3 | Broader candidate older than 24h is out-of-window; picker keeps the newest narrow file. |
| ANTS-1625 INV-7 | docs/specs/ANTS-1625.md § 3 | SARIF branch runs before the `pickForeign` lambda — SARIF naming sorts lex-max correctly. |
| ANTS-1625 INV-8 | docs/specs/ANTS-1625.md § 3 | Narrow-suffix set (`-postfix`, `-single`, `-narrow`) appears in both `classifyAuditScope` and `pickForeignReport`. |
| ANTS-3512 INV-1 | finbreak feedback 2026-07-14 | `cmdLastAuditSummary` reads the sibling `findings-<iso>-<sha>.json` sidecar's `scope` key and emits it as `requested_scope`, so the *requested* scope is authoritative over the distinct-file-count heuristic. |
| ANTS-3512 INV-2 | finbreak feedback 2026-07-14 | A confirmed full-tree request (`requested_scope == "full"`) suppresses `narrow_run_warning`/`narrow_run_files` even when the derived tag is narrow — a whole-tree sweep whose findings land in one file is not a single-file rerun. |

## Strategy

Two fixtures committed under this dir:

- `fixture_min.sarif` — 5 results spanning all 3 SARIF levels +
  one suppressed result. Lets us assert sort order, count
  invariants, severity-floor filtering, and rule-index resolution.
- `fixture_empty.sarif` — well-formed SARIF skeleton with
  `"runs": []`. Locks INV-10.

Test exercises `AuditEngine::summariseSarif` directly — no live
socket, no QProcess, no MainWindow. Source-grep checks lock the
wiring contracts (`m_auditSummary*` decls, `summariseSarif` impl
not opening `.html`).
