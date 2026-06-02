# mcp_build_identity — ANTS-1952

Surface the MCP server's build identity (git SHA + build time) so a
caller can detect a ship-vs-live binary gap. A SemVer string alone
cannot distinguish "same version, rebuilt with a fix" — exactly the
trap behind the ANTS-1632 / 1903 / 1947 stale-binary investigations,
where a committed fix sat on disk while the running MCP server was an
older binary, so investigations chased ghosts.

The build-stamp infrastructure already exists (ANTS-1222 + ANTS-1394):
`cmake/build_info.h.in` → generated `build/generated/build_info.h`
defines `ANTS_BUILD_COMMIT` (short git SHA), `ANTS_BUILD_DATE`,
`ANTS_BUILD_TIME`, `ANTS_BUILD_TYPE`. This feature wires those four
macros into the MCP `initialize` `serverInfo` object and the
`get_session_info` control-plane verb.

## Test scope

Source-scrape regression locks the wiring so a future refactor of
`claudeintegration.cpp` cannot silently drop the build stamp.

## Invariants checked

- **INV-1.** `claudeintegration.cpp` `#include`s `build_info.h`.
- **INV-2.** The `initialize` `serverInfo` object carries
  `build_commit`, `build_date`, `build_time`, `build_type`, each
  populated from the matching `ANTS_BUILD_*` macro (anchor:
  `ANTS-1952`).
- **INV-3.** The `get_session_info` envelope re-surfaces the same
  identity as `server_build_commit` / `server_build_date` /
  `server_build_time` / `server_build_type`, so a session that has
  already handshaked can re-confirm the running SHA without a second
  `initialize`.
- **INV-4.** `CMakeLists.txt` wires `ants_claude_lib` to the generated
  header dir and depends on `ants_build_info`, so the include resolves
  and the stamp refreshes on every build.
