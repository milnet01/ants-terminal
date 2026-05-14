# session_memory_engine — feature-conformance spec

**Owner:** ANTS-1283 (`docs/specs/ANTS-1283.md`)
**Engine:** `src/sessionmemoryengine.{h,cpp}`

Exercises `SessionMemoryEngine::execute` plus its helpers
(`cwdHash`, `isValidKey`, `loadStore`, `saveStore`,
`serializedSize`). Standalone GoogleTest binary linking only the
engine's compile units through `ants_core_lib`.

## Cases (ENG-1..ENG-10)

| # | Case | Asserts |
|---|---|---|
| ENG-1 | `cwdHash` deterministic and 16-hex | same cwd → same hash; different cwds → different hashes |
| ENG-2 | `isValidKey` accepts canonical alphabet | `[A-Za-z0-9._-]`, length 1–64 |
| ENG-3 | `isValidKey` rejects out-of-band | `/`, `..`, control chars, empty, length 65 |
| ENG-4 | `Get` on empty store returns `found=false` | INV-6 |
| ENG-5 | `Set` then `Get` round-trip | value preserved verbatim |
| ENG-6 | `Delete` removes key | subsequent `Get` returns `found=false` |
| ENG-7 | `List` returns keys-only (INV-5) | no inline values in response |
| ENG-8 | Total-store cap enforced (INV-2) | `Set` that would push past 100 KiB → `cap_exceeded` |
| ENG-9 | Per-value cap enforced (INV-8) | value > 16 KiB → `bad_value` |
| ENG-10 | Corrupt store treated as empty (INV-11) | mangled JSON on disk → load returns `{}`, set succeeds |

## Build wiring

`tests/features/session_memory_engine/test_session_memory_engine.cpp`
joins the `test_audit` bundle in `CMakeLists.txt` next to
`tests/features/cold_eyes_engine/test_cold_eyes_engine.cpp`.

## Test workspace

Every case uses a `QTemporaryDir` as the base directory (passed via
the engine's `baseDirOverride` arg) — no writes to `~/.cache/`.
