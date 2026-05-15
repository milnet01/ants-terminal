# ANTS-1357 — MCP idempotent-read response cache

Direct-cache-API tests for the 100 ms TTL cache that wraps four MCP
read verbs at `claudeintegration.cpp::processTools`. See the parent
spec at `docs/specs/ANTS-1357.md` for full rationale and invariants.

The dispatch integration is exercised in `mcp_tools_list_schema` /
other MCP feature lanes; this lane locks the cache helpers' contract
in isolation via the test-only seam (`tryGetIdempotentReadCacheForTest`,
`putIdempotentReadCacheForTest`, `idempotentReadCacheSizeForTest`,
`idempotentReadCacheLruForTest`).

## Invariants exercised

- **INV-1** Hit response is byte-identical to the put response (no
  transform).
- **INV-2** TTL ≤ 100 ms wall-clock. Entries past 100 ms read as miss.
- **INV-3** Cache size ≤ 32 entries. Insert past cap evicts LRU.
- **INV-4** Allowlist enforced at lookup AND insert; non-allowlisted
  tools never enter the cache.
- **INV-5(a)** Empty response strings are not inserted.
- **INV-5(b)** The `kMcpRcUnavailable` literal is not inserted.
- **INV-9** Args participate in the cache key — two calls with
  different args produce two cache entries.

## Out of scope here

- Wire-byte integration with `processTools` (rides on
  `mcp_tools_list_schema` + INV-1 byte-identity).
- `wrapMcpData` / `recordCall` interaction (rides on existing MCP
  lanes).
- Real provider semantics (`get_cwd` etc.) — irrelevant to cache
  mechanics.
