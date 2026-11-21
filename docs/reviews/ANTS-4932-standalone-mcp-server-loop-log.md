# ANTS-4932 — Serve the project-scoped MCP verbs from a standalone process — review loop log

Record for [`../specs/ANTS-4932-standalone-mcp-server.md`](../specs/ANTS-4932-standalone-mcp-server.md) § Cold-eyes loop log.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-24 | 3, every lane held Q1–Q4 | 5 | 7 | 3 | 3 | Verified 18, fixed 18, dismissed 2 (INV-12's test directory; the verb-count label). Largest: the `MainWindow` coupling was undercounted (`!m_main` refusals and resolver reads missed), and the library split could not link. Design choices taken at the gate are recorded in § 9. |
| 2 | 2026-09-24 | 3, every lane held Q1–Q4 | 0 | 5 | 6 | 2 | Verified 13, fixed 13, dismissed 0. Cap reached (2 for a spec); shipped to implementation. Twelve of the thirteen anchored on text loop 1 wrote. The document was new, so that share cannot separate a calm cap from a violent one. Read by substance, they were consequences of loop 1's design choices not yet carried through (the shared pipeline against the forwarder, the `Required` gate against the fallback, the terminal-scoped definition), not repairs of repairs. Deferral filed: ANTS-5311. Disclosure: every lane's git snapshot named loop 1's commit and its finding count. |
| impl | 2026-10-16 | none dispatched | - | - | - | - | ANTS-5750 fold-back, not a review. Brought three passages in line with existing code: INV-2 scoped to shared verbs (trace registers in mcp::registerMcpdOnlyVerbs), § 2.1 now says a rebuilt ants-mcpd needs a client reconnect, § 2.3 drops get_git_status (now a deprecated alias of git_state). No gate: the amendment only records built code. |
| impl | 2026-11-13 | none dispatched | - | - | - | - | ANTS-6719 fold-back, not a review. § 2.3 said `token_usage` missed the calls `ants-mcpd` serves; ANTS-5311 shipped that, so the sentence now points at its spec. |

<!-- one row per review loop; written as the loops happen, never back-filled -->
