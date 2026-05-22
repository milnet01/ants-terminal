# rc_read_tool_byte_cap — MCP read-tool response byte cap (ANTS-1293)

Canonical spec: `docs/specs/ANTS-1293.md`.

Pins the server-side response byte cap on the structured read tools
(`file_outline` `symbols[]`, `workspace_search` `matches[]`) via the pure
`RemoteControl::capJsonArrayToBytes` helper, plus a source-grep on the two
handler bodies for the wiring.

## Assertions

- BC-1: a payload under the cap is returned unchanged (no `truncated`
  flip, no dropped-count field).
- BC-2: an oversized payload trims from the TAIL — leading item survives,
  `truncated:true`, dropped count correct, final serialized envelope
  within the cap.
- BC-3: a `max_bytes` above the 4 MiB ceiling clamps and sets
  `capClamped`; a tiny payload still isn't trimmed.
- BC-4: `max_bytes <= 0` falls back to the 512 KiB default.
- BC-5: a cap below the base envelope size drops all items without
  crashing and sets `truncated`.
- WI-1 / WI-2: `cmdFileOutline` / `cmdWorkspaceSearch` call
  `capJsonArrayToBytes` with `symbols_dropped` / `results_dropped`.

Why a pure helper: mirrors the ANTS-1348 `trimScrollbackForGetText`
pattern so the cap logic is unit-testable without a Qt event loop or the
MainWindow dependency chain.
