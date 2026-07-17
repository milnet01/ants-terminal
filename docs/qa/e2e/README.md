# E2E harness — driving Ants Terminal as a user (ANTS-2049)

The e2e harness launches a **throwaway** Ants instance, drives it with
synthetic input over its RemoteControl socket, observes it, and tears it
down — without ever touching the instance you're running. It's how the
agent (or you) can "sit down and use" the app: type, click buttons, open
dialogs, resize, screenshot — and assert the result.

Spec + contract: [`docs/specs/ANTS-2049.md`](../../specs/ANTS-2049.md).

## Pieces

| File | What it is |
|---|---|
| `tools/e2e/run.sh` | `launch_e2e` / `call_e2e` / `teardown_e2e` shell functions. |
| `tools/e2e/smoke.sh` | The 8 smoke cases (spec §6). Sources `run.sh`. |
| `--e2e` flag | Forces the RemoteControl socket open **and** unlocks the inject verbs (the `m_e2eMode` gate). |
| `--remote-json '<obj>'` | Generic client passthrough: send any `{cmd, ...args}` to the socket, print the JSON reply. |

The inject verbs (`inject-key`, `inject-click`, `resize-window`,
`grab-image`) are **socket-only** and gated behind `--e2e` — they are *not*
MCP tools, so they can never be called against your live instance through
the agent's MCP toolset (a security property, spec §5).

## Run the smoke suite

```bash
# Headless (recommended for automation — the dialog case skips offscreen):
ANTS_E2E_BIN=build/ants-terminal QT_QPA_PLATFORM=offscreen bash tools/e2e/smoke.sh

# On-screen (a throwaway window flashes briefly; the dialog case can run):
ANTS_E2E_BIN=build/ants-terminal bash tools/e2e/smoke.sh

# Via ctest (opt-in label, excluded from the default run):
ctest --test-dir build -L e2e --output-on-failure
```

`ANTS_E2E_BIN` defaults to `build-fast/ants-terminal` (INV-7); point it at
whatever binary you built. The ctest target sets it to the freshly built
`ants-terminal` and runs offscreen.

## Drive it by hand

```bash
source tools/e2e/run.sh
export ANTS_E2E_BIN=build/ants-terminal      # or build-fast/…

launch_e2e                                   # spawn + wait for the socket
call_e2e '{"cmd":"send-text","text":"echo hi\n"}'
call_e2e '{"cmd":"get-text"}'                # observe the terminal
call_e2e '{"cmd":"resize-window","w":1200,"h":800}'
call_e2e '{"cmd":"inject-click","widget":"roadmapButton"}'
call_e2e "{\"cmd\":\"grab-image\",\"path\":\"$(e2e_art)/shot.png\"}"
teardown_e2e                                 # kill + rm the temp profile
```

`launch_e2e` isolates the instance completely — its own socket and all
three XDG dirs (config / data / cache) under a fresh `mktemp` — so it never
reads your config, restores your tabs, or binds your socket (INV-3). The
`EXIT` trap always reaps every instance + temp dir (INV-6).

### Verbs at a glance

| Verb | Args | Reply |
|---|---|---|
| `send-text` | `text` | `{ok}` — writes to the PTY (types into the shell) |
| `get-text` | `lines?` | `{ok, text}` — scrollback + screen |
| `tab-list` | — | `{ok, tabs:[…]}` |
| `inject-key` | `key?`/`text?`, `modifiers?[]`, `widget?` | `{ok}` — posts a `QKeyEvent` press+release |
| `inject-click` | `widget?`, `x?`,`y?`, `button?` | `{ok}` — posts a `QMouseEvent` press+release |
| `resize-window` | `w`, `h` | `{ok, w, h}` — echoes the post-clamp size |
| `grab-image` | `path`, `widget?` | `{ok, path}` — PNG under `ANTS_E2E_ARTIFACT_DIR` |

Refusal codes: `e2e_disabled` (no `--e2e`), `widget_not_found` (bad
`objectName`), `bad_path` (grab escapes the artifact dir), `bad_args`. See
[`docs/standards/mcp-error-codes.md`](../../standards/mcp-error-codes.md).

To address a specific widget, give it an `objectName` in the source and
pass it as `widget`. Current hooks: `roadmapButton`, `RoadmapDialog`
(ANTS-2049); add more as cases need them.

## Case format (the ANTS-2050 suite)

The feature-covering suite lives in [`cases.sh`](../../../tools/e2e/cases.sh)
(automated lanes) and [`cases.md`](cases.md) (the full checklist, marking each
case auto / manual / pending-hook). `cases.sh` runs all lanes or one:
`bash tools/e2e/cases.sh theme`.

Each case is **drive → observe → assert**, exit 0 on pass. Build them on
`run.sh`'s functions:

```bash
source tools/e2e/run.sh
launch_e2e
# drive
call_e2e '{"cmd":"…"}'
# observe
out=$(call_e2e '{"cmd":"get-text"}')
# assert (parse the JSON reply — a verb refusal is exit 2 with {ok:false,code},
# not a transport error, so check the fields, not just the exit code)
grep -q 'EXPECTED' <<<"$out" || { echo FAIL; exit 1; }
```

Conventions:

- **Skip, don't fail, when the platform can't host the check** — e.g. a
  dialog that needs an on-screen window is `SKIP` under `QT_QPA_PLATFORM=offscreen`.
- **Isolate second instances with a tag** — `launch_e2e gate --no-e2e`
  gives a distinct socket/profile; `call_e2e '<json>' gate` targets it.
- **Never build** inside a case (INV-7) — the harness runs an
  already-built binary.
- **Verify the negative path** — before trusting a guard assertion, revert
  the guard locally, rebuild, and confirm the case no longer refuses
  (spec §6).
