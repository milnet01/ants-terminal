# Ants Terminal — Plugin Authoring Standards

> **Audience**: plugin authors writing against `ants-terminal` **v0.5.0**
> (Lua 5.4 runtime). For internal plugin-system architecture notes aimed at
> Ants-Terminal contributors, see `STANDARDS.md` → "Plugin System Standards".

Plugins extend the terminal through a sandboxed Lua 5.4 runtime. They can
react to terminal events (line of output, keypress, tab created, OSC 133
prompt marker), inject text into the PTY, show notifications, update the
status bar, and read recent scrollback — strictly through the `ants.*` API.
Direct access to Qt, the filesystem, the network, or other processes is
deliberately unavailable.

This document is the **source of truth** for plugin authors. The runtime
implementation is at `src/luaengine.cpp` + `src/pluginmanager.cpp`; when
those drift from this doc, the doc wins and an issue should be filed.

## Table of Contents

1. [Directory Layout](#directory-layout)
2. [Manifest (`manifest.json`)](#manifest-manifestjson)
3. [Entry Point (`init.lua`)](#entry-point-initlua)
4. [The `ants.*` API — Current (v0.5.0)](#the-ants-api--current-v050)
5. [Events](#events)
6. [Sandbox Boundaries](#sandbox-boundaries)
7. [Resource Limits](#resource-limits)
8. [Versioning & Compatibility](#versioning--compatibility)
9. [Error Handling](#error-handling)
10. [Distribution](#distribution)
11. [Examples](#examples)
12. [Roadmap (planned APIs — not yet available)](#roadmap-planned-apis--not-yet-available)

---

## Directory Layout

Every plugin lives in a directory under `~/.config/ants-terminal/plugins/`:

```
~/.config/ants-terminal/plugins/
├── hello-world/
│   ├── init.lua          # required — entry point
│   ├── manifest.json     # optional (recommended)
│   ├── README.md         # optional — shown in (planned) plugin browser
│   ├── LICENSE           # optional
│   └── assets/           # any additional Lua modules, data files, icons
└── git-aware-prompt/
    ├── init.lua
    └── manifest.json
```

**Rules:**

- The directory name is the plugin id when `manifest.json` is absent. Use
  lowercase-kebab-case: `git-branch-badge`, `k8s-context`, not `MyPlugin`.
- `init.lua` is the only file the loader executes directly. Any additional
  Lua you want to split into modules must be inlined via copy-paste (the
  sandbox blocks `require`) — plan your code shape accordingly.
- Non-Lua files (markdown, JSON, assets) are ignored by the loader and
  have no effect on behavior.

## Manifest (`manifest.json`)

Optional but strongly recommended. **Manifest v2** (shipped in 0.6.0) adds
`permissions`, `keybindings`, and `settings_schema` on top of the v1 metadata:

```json
{
  "name":        "Git Branch Badge",
  "version":     "1.2.0",
  "description": "Show git branch + dirty marker in the status bar.",
  "author":      "Your Name <you@example.com>",
  "permissions": ["clipboard.write", "settings"],
  "keybindings": {
    "open-palette": "Ctrl+Alt+G"
  },
  "settings_schema": {
    "poll_ms": { "type": "integer", "default": 2000, "min": 500, "max": 60000 }
  }
}
```

**Field contracts:**

| Field             | Type    | Required | Default    | Notes                                                       |
|-------------------|---------|----------|------------|-------------------------------------------------------------|
| `name`            | string  | no       | directory  | Human-readable display name                                 |
| `version`         | string  | no       | `"0.1.0"`  | **[SemVer](https://semver.org)** encouraged                 |
| `description`     | string  | no       | `""`       | One sentence; ~80 char soft limit                           |
| `author`          | string  | no       | `""`       | Name + email or URL                                         |
| `permissions`     | array   | no       | `[]`       | Capability strings (see below). Prompts user on first load. |
| `keybindings`     | object  | no       | `{}`       | `{"action-id": "Ctrl+Shift+X"}` — fires `keybinding` event. |
| `settings_schema` | object  | no       | `{}`       | JSON-Schema subset. UI auto-render is 0.7 follow-up.        |

**Permission strings (0.6):**

| Permission        | Grants                                                    |
|-------------------|-----------------------------------------------------------|
| `clipboard.write` | `ants.clipboard.write(text)` — write system clipboard     |
| `settings`        | `ants.settings.get(key)` / `ants.settings.set(key, v)` — per-plugin k/v store |

Un-granted permissions result in the corresponding `ants.*` table being
**absent** (not `nil`), so plugins can feature-detect with
`if ants.clipboard then ... end`. Grants persist in `config.json`
(`plugin_grants.<name>: [...]`); subsequent loads don't re-prompt unless
the manifest requests a new permission the user hasn't yet granted.

## Entry Point (`init.lua`)

Plugin load is a single execution of `init.lua` at the top level. The
Lua environment contains `ants.*` plus a curated set of safe standard
libraries (see [Sandbox Boundaries](#sandbox-boundaries)). Register
event handlers during this execution; they persist for the session.

Minimal valid plugin:

```lua
-- init.lua
ants.notify("Hello", "Hello from a plugin!")
```

A more typical shape:

```lua
-- init.lua
local config = {
    message = "[tip] Use Ctrl+Shift+P for the command palette",
}

ants.on("tab_created", function(tab_info)
    ants.notify("New tab", config.message)
end)

ants.on("line", function(line)
    if line:match("error: ") then
        ants.set_status("⚠ error in recent output")
    end
end)
```

**Lifecycle (0.6+):**

- `init.lua` runs **once**, synchronously, when the plugin's VM is created
  (at terminal startup for already-enabled plugins, or when the user toggles
  it on in Settings → Plugins, or when hot-reload fires with
  `ANTS_PLUGIN_DEV=1`).
- A `load` event fires **once** immediately after `init.lua` returns — use
  `ants.on("load", fn)` for deferred setup that needs the full ants API
  ready.
- An `unload` event fires right before `lua_close()` on shutdown, plugin
  disable, or hot-reload. Use it to save state (`ants.settings.set(...)`)
  or cancel any external work. The VM closes immediately after, so no
  further ants calls will run.
- Plugins run in **isolated VMs** — shared `lua_State` globals from other
  plugins are not visible. Each plugin gets its own 10 MB heap and 10 M
  instruction budget.
- Plugins should still be **idempotent on re-init**: hot reload tears the
  whole VM down and re-creates it, so re-registered handlers simply
  replace the old ones.

## The `ants.*` API — Current (v0.6.0)

This is the complete surface as of **0.6.0**. Functions not listed here
do not exist and will raise `attempt to call a nil value`. Permissioned
functions (marked 🔒) are only present when the corresponding permission
is granted.

### `ants.send(text)`

Write `text` to the terminal PTY verbatim. No trailing newline is added.

```lua
ants.send("date\n")                 -- runs `date`
ants.send("\x03")                   -- sends Ctrl+C
```

**Security:** does **not** check whether the PTY child is at a prompt
(OSC 133 not yet consulted). Writing during a long-running command
injects into that command's stdin. Use responsibly.

### `ants.notify(title, message)`

Shows a desktop notification (via the host's native notification service)
or falls back to the status bar.

```lua
ants.notify("Build finished", "Exit 0 · 1m 14s")
```

### `ants.get_output(n)`

Returns the last `n` lines of visible scrollback as a single newline-joined
string. Default `n = 50`. Capped at the current scrollback size.

```lua
local recent = ants.get_output(200)
if recent:find("FAIL") then ... end
```

### `ants.get_cwd()`

Returns the terminal's **reported** current working directory (via OSC 7
if the shell emits it, falling back to the process's CWD). May be empty.

```lua
local cwd = ants.get_cwd()
```

### `ants.set_status(text)`

Overwrites the status-bar text. A new `ants.set_status("")` restores the
default status. Plugins should scope their status updates to clear them
when no longer relevant.

```lua
ants.set_status("indexing…")
```

### `ants.log(message)`

Writes to the internal plugin log (visible in `journalctl` or the
terminal's stdout when launched from a shell). Use for debugging; not a
substitute for `ants.set_status()` for user-facing messages.

```lua
ants.log("plugin foo: loaded config from " .. cfg_path)
```

### `ants.on(event_name, handler)`

Registers a callback for a named event. Handler receives event data as a
string and may return `false` from keypress events to cancel propagation.
Multiple handlers per event are allowed; invocation order is
registration order.

```lua
ants.on("keypress", function(key)
    if key == "Ctrl+Shift+G" then
        ants.send("git status\n")
        return false            -- consume the keypress
    end
end)
```

### `ants._version` / `ants._plugin_name`

Read-only strings. `ants._version` is the terminal version (e.g. `"0.6.0"`);
`ants._plugin_name` is the plugin's declared manifest name. Use them for
feature detection without string-hardcoding:

```lua
if ants._version and ants._version >= "0.6" then
    -- 0.6+ API available
end
```

### 🔒 `ants.clipboard.write(text)`

Requires manifest permission `"clipboard.write"`. Writes to the system
clipboard. No return value. Only available if the user granted the
permission at load time.

```lua
if ants.clipboard then
    ants.clipboard.write("copied from plugin")
end
```

### 🔒 `ants.settings.get(key)` / `ants.settings.set(key, value)`

Requires manifest permission `"settings"`. Per-plugin key/value store,
persisted in the main `config.json`. Values are strings; encode structured
data with `string.format` / manual JSON. Returns `nil` for unset keys.

```lua
if ants.settings then
    local last_run = ants.settings.get("last_run") or "never"
    ants.settings.set("last_run", os.date())  -- (os removed in sandbox;
    -- this is just an illustration — in practice, use a string you already have)
end
```

## Events

| Event name       | Handler signature             | Cancellable | When it fires                                        |
|------------------|-------------------------------|-------------|------------------------------------------------------|
| `"output"`       | `function(chunk)`             | no          | Every chunk of PTY output, post-decode, pre-display  |
| `"line"`         | `function(line)`              | no          | A complete logical line arrives (newline-terminated) |
| `"prompt"`       | `function(marker_data)`       | no          | OSC 133 A/B/C/D marker parsed                        |
| `"keypress"`     | `function(key)` returns bool? | **yes**     | User pressed a key (before the grid sees it)         |
| `"title_changed"`| `function(new_title)`         | no          | OSC 0/1/2 title change                               |
| `"tab_created"`  | `function(tab_info)`          | no          | New tab opened (includes splits)                     |
| `"tab_closed"`   | `function(tab_info)`          | no          | Tab/pane closed                                      |
| `"keybinding"`   | `function(action_id)`         | no          | Manifest `"keybindings"` shortcut fired              |
| `"load"`         | `function(plugin_name)`       | no          | Once after `init.lua` returns — deferred init hook   |
| `"unload"`       | `function(plugin_name)`       | no          | Just before VM shutdown — save state here            |

**Return-value contract:** only `"keypress"` acts on the return value.
Returning `false` suppresses default handling; any other value (including
`nil`, `true`, or omitting the return) lets the event propagate. All
other events ignore the return value.

**Performance:** handlers run synchronously on the UI thread. A slow
handler stalls the terminal. Keep handler bodies under ~1 ms wall time;
offload expensive work (don't — you can't, the sandbox blocks threads).

## Sandbox Boundaries

The plugin Lua environment has the following libraries **removed** before
`init.lua` runs:

- `os`, `io` — no shell, no file I/O, no `getenv`
- `loadfile`, `dofile`, `load` — no dynamic code
- `require`, `package` — no module imports
- `debug` — no stack/frame introspection
- `coroutine` — no coroutines (a prior sandbox-escape vector)
- `rawget`, `rawset`, `rawequal`, `rawlen` — no metatable bypass
- `setmetatable`, `getmetatable` — no metatable modification
- `collectgarbage` — no GC manipulation

Available: `string`, `table`, `math`, `utf8`, plus the stripped-down
`_G`. `print()` is typically redirected to `ants.log`; don't rely on
stdout behaving as in a standalone Lua REPL.

**Bytecode rejection:** if `init.lua` begins with the Lua bytecode
signature byte (`0x1B`), the loader refuses to execute it. Ship Lua
**source**, not compiled `.luac`.

**What a plugin cannot do** (and a misbehaving plugin therefore cannot
abuse):

| Attack surface | Status | Notes |
|----------------|--------|-------|
| Read/write user files outside `~/.config/ants-terminal/plugins/<me>/` | blocked — no `io` | Settings persistence is planned; see Roadmap |
| Spawn processes | blocked — no `os.execute` | `ants.send` can run shell commands *in your terminal* — user-visible, not covert |
| Open network sockets | blocked — no `io`, no `socket` lib | Planned: capability-gated `ants.net` |
| Read other plugins' state | blocked — each plugin runs in its own `lua_State` (0.6+) | Hit `ants.settings` to persist your own state |
| Read arbitrary env vars | blocked — no `os.getenv` | Planned: curated `ants.env.get(name)` for whitelisted vars |
| Write to clipboard | not exposed yet | Planned: `ants.clipboard.write(text)` |
| Read clipboard | not exposed and not planned | OSC 52 read is disabled system-wide for security |

## Resource Limits

The runtime enforces two hard limits per plugin per event invocation:

- **Instruction budget:** 10,000,000 VM instructions per event handler.
  Enforced via `lua_sethook(LUA_MASKCOUNT, …)`. On exceed, the runtime
  calls `luaL_error` with a "Plugin timed out" message; the remaining
  handlers in the chain for that event run normally. Your plugin is not
  unloaded — but the **specific invocation** is aborted.
- **Heap budget:** 10 MB total Lua heap, enforced by a custom allocator.
  On exceed, allocations return `NULL`; Lua treats this as out-of-memory
  and typically propagates the error up the call stack. Again, the
  plugin is not unloaded, but the allocation fails.

**Author implications:**

- Don't accumulate unbounded state (e.g. appending every output line to
  a table). Use ring buffers or bounded caches.
- Don't loop over `ants.get_output(very_large_N)` — each call copies.
- Recursive patterns that hit the instruction budget are unrecoverable
  for that one invocation; the next event fires a fresh budget.

## Versioning & Compatibility

The `ants.*` API is versioned with the terminal. Backward-compatible
additions happen in **minor** releases (`0.5 → 0.6`). Breaking changes
happen only in **major** releases (`0.x → 1.0`).

Plugin authors are encouraged to declare their minimum supported
terminal version in `manifest.json` (field name **reserved** for a
future `"api_version"` / `"requires"` block — see Roadmap). Until that
lands, check `ants._version` (also reserved, not yet present):

```lua
-- forward-compat idiom (won't break on older terminals, just skips)
if ants._version and ants._version >= "0.6" then
    -- use 0.6-only API
end
```

## Error Handling

- Uncaught Lua errors in a handler are logged via `ants.log` and do not
  crash the terminal.
- Syntax errors in `init.lua` abort plugin load and surface in the log
  with file:line info.
- Bytecode-rejection errors fire at load and log a warning.
- Timeouts and OOM errors are logged per occurrence.

**The terminal will never crash because of your plugin.** If it does,
file a bug against the runtime — that's our invariant, not yours to
worry about.

## Distribution

Today there is **no plugin marketplace**. Distribute plugins as:

- Git repositories with `init.lua` + `manifest.json` at root. Users
  clone into `~/.config/ants-terminal/plugins/<name>/`.
- Tarballs. Users extract into the plugins dir.
- Copy-paste. Users create the dir and paste `init.lua`.

A signed marketplace is planned (see Roadmap). Until then, include a
clear `README.md` in your repo describing what the plugin reads (output?
keypresses?) and what it writes (PTY injection?) so users can do their
own threat assessment.

## Examples

### Example 1 — hello world

```lua
-- ~/.config/ants-terminal/plugins/hello/init.lua
ants.notify("Hello", "Ants Terminal plugin system is alive.")
```

### Example 2 — per-host banner

```lua
-- init.lua — show a banner tag on the status bar based on hostname
-- Uses ants.get_output() to sniff a prompt for hostname — imperfect but
-- illustrative.
ants.on("prompt", function(_)
    local recent = ants.get_output(5)
    if recent:find("@prod") then
        ants.set_status("⚠ PRODUCTION")
    elseif recent:find("@staging") then
        ants.set_status("staging")
    else
        ants.set_status("")
    end
end)
```

### Example 3 — fail-fast test runner

```lua
-- init.lua — re-run tests on save when you're in a test directory
-- Listens to OSC 133 markers; when a 'B' marker arrives (command start),
-- checks the command for matching patterns.
local last_cmd = ""

ants.on("prompt", function(marker)
    -- OSC 133's B marker carries the command; we just snapshot recent
    -- output since the marker data shape is loose today.
    local tail = ants.get_output(2)
    last_cmd = tail:match("%$ (.-)\n") or last_cmd
end)

ants.on("line", function(line)
    if line:match("^FAIL") then
        ants.notify("Test failed", last_cmd)
    end
end)
```

### Example 4 — keybinding override

```lua
-- init.lua — F5 inserts the last known git branch name
local last_branch = "main"

ants.on("line", function(line)
    local b = line:match("On branch (%S+)")
    if b then last_branch = b end
end)

ants.on("keypress", function(key)
    if key == "F5" then
        ants.send(last_branch)
        return false
    end
end)
```

## Roadmap (planned APIs — not yet available)

The following are targeted for upcoming minor releases. See
`ROADMAP.md` for the full schedule. **Do not ship plugins that rely
on these today** — they'll fail with `attempt to call a nil value`.

### 0.6 — capability manifest + clipboard + settings schema ✅ (shipped 2026-04-14)

**All of the below are live in 0.6.0.** See the
[manifest contract](#manifest-manifestjson) and the
[`ants.*` API](#the-ants-api--current-v060) section for concrete docs.

- ✅ **`manifest.json` v2** with declarative `permissions`, `keybindings`,
  and `settings_schema` fields. First-load prompt + persisted grants in
  `config.json`.
- ✅ **`ants.clipboard.write(text)`** — gated by `clipboard.write`.
- ✅ **`ants.settings.get/set`** — gated by `settings`; backed by
  `config.json` (`plugin_settings.<name>.<key>`). **Note:** the 0.6 store
  is a flat string k/v — the JSON-Schema-driven Settings UI auto-render
  is deferred to 0.7.
- ✅ **Per-plugin Lua VMs** — each plugin gets its own `lua_State` with
  independent 10 MB heap + 10 M instruction budget.
- ✅ **`ants._version`** and **`ants._plugin_name`** exposed.
- ✅ **Hot reload** via `QFileSystemWatcher` — enabled when
  `ANTS_PLUGIN_DEV=1`. Fires `load` / `unload` lifecycle events.
- ✅ **Plugin keybindings** — `manifest.json` `"keybindings"` block.
  Firing sends a `keybinding` event with the action id.

### 0.7 — richer events + command palette registration

- **`ants.palette.register({title, action, hotkey})`** — inject entries
  into the Ctrl+Shift+P palette.
- Events: `"output_line"` (post-OSC-133-grouped), `"command_finished"`
  (exit code + duration), `"pane_focused"`, `"theme_changed"`,
  `"window_config_reloaded"`.
- **User-vars channel**: WezTerm-style `OSC 1337;SetUserVar=NAME=<b64>`
  produces a `"user_var_changed"` event. Lets a shell plugin pipeline
  state (git branch, k8s context) into terminal UI without prompt
  parsing.

### 0.8 — trigger system + plugin marketplace

- **Trigger rules** as first-class config: `{regex, action, params}`
  evaluated per output line; `ants.trigger.register(…)` lets plugins
  add triggers programmatically.
- **Plugin marketplace**: signed manifest (Ed25519), public index
  served from a static site, inline install from Settings → Plugins →
  Browse.

### 0.9+ — WebAssembly plugins (opt-in)

- Language-agnostic plugins via `wasmtime` embed. Same `ants.*` API
  exposed as WASI imports. Lua plugins continue to work — WASM is
  additive, for authors who want Rust/Go/AssemblyScript.

---

## Contributing to this Document

The runtime lives in `src/luaengine.cpp` / `src/pluginmanager.cpp`.
When you change what the plugin API exposes, update this file in the
same commit. CI doesn't check doc drift today; we're on the honor
system. If you find a gap between this doc and the runtime, file a
GitHub issue — the doc is the contract.
