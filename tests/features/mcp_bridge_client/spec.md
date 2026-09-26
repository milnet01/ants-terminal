# tools/mcp-bridge.py connects only to a socket it owns, and survives bad input

The bridge relays Claude Code's MCP requests to the Ants socket in /tmp.
/tmp is world-writable, so the socket it picks is a trust decision, and the
bridge is the client's only process — anything that raises kills it.

## Invariants

- **INV-1** — the picker never follows a symlink. An entry named like an
  Ants socket that is a link — even to a real socket the user owns — is
  skipped. `os.stat` follows the link and would accept it.
- **INV-2** — an override of `ANTS_MCP_CONNECT_TIMEOUT_S` /
  `ANTS_MCP_READ_TIMEOUT_S` that is not a positive finite number (`nan`,
  `inf`, `-1`) falls back to the default instead of raising at the first
  request.
- **INV-3** — a request line that is valid JSON but not an object (a batch
  array, a scalar) gets a JSON-RPC `-32600` error reply; the bridge keeps
  running.

## How it is tested

`test_mcp_bridge_client.py` imports the bridge as a module for INV-1 and
INV-2 (pointing `SOCK_GLOB` at a temp dir), and runs it as a subprocess
for INV-3.
