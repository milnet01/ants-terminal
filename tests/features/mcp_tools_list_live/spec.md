# mcp_tools_list_live

ANTS-1677 INV-6, for ANTS-4919's split of `claudeintegration.cpp`.

## Contract

The live `tools/list` reply does not change when the class's text moves
between files.

| # | Statement |
|---|-----------|
| 1 | A `ClaudeIntegration` started with `startMcpServer()` on a temporary socket answers `initialize`, then `tools/list` with no `_meta.shape`, with the full descriptor set. When `ANTS_TOOLS_LIST_DUMP` is set, the raw `tools/list` reply line is written to that path. |
| 2 | The reply's tool-name set equals the set of names on the class's source lines that match `tools/check-readme-claims.sh`'s MCP-tool pattern, and is not empty. A line inside an `#ifdef ANTS_LUA_PLUGINS` block counts only when this test is built with `ANTS_LUA_PLUGINS`. |

The test registers a stand-in `trace` provider, because the `trace`
descriptor is listed only by a process that registered the verb.

## Migration-time use

Run before and after each `claudeintegration` cut or promotion commit:

```bash
ANTS_TOOLS_LIST_DUMP=/path/before.json ctest --test-dir build -R McpToolsListLive
# … build the cut …
ANTS_TOOLS_LIST_DUMP=/path/after.json ctest --test-dir build -R McpToolsListLive
cmp /path/before.json /path/after.json
```

## Must fail first

- Statement 1: a scratch build with one descriptor's description edited.
  The two dumps differ under `cmp`.
- Statement 2: a scratch build whose source list omits a file holding tool
  descriptors. The name sets differ.
