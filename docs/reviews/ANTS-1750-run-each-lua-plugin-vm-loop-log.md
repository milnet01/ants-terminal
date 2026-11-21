# ANTS-1750 — Run each Lua plugin VM on its own worker thread — review loop log

Record for [`../specs/ANTS-1750-run-each-lua-plugin-vm.md`](../specs/ANTS-1750-run-each-lua-plugin-vm.md) § Cold-eyes loop log.

## Cold-eyes loop log

**Loop 1 (2026-05-22)** — independent reviewer, briefed cold against
live source. Grounding clean (all `path:line` refs verified). Findings
folded in:

- **BLOCKER** — the `Keybinding` shortcut in `MainWindow`
  (`mainwindow.cpp:537-539`) calls `engineFor(...)->fireEvent(...)`
  directly on the GUI thread, bypassing `PluginManager::fireEvent`; a
  freeze vector the original draft missed → added § 2.2.1 (targeted
  `dispatchTo`) + INV-10.
- **BLOCKER** — `loadPlugin` constructs `new LuaEngine(this)`
  (`pluginmanager.cpp:333`); a parented `QObject` cannot `moveToThread`
  → added the parentless-engine + explicit-lifetime rule to § 2.1 +
  INV-11.
- **SHOULD-FIX** — inline `Load` event ordering → § 2.2.2.
- **SHOULD-FIX** — `firePaletteAction` direct call → § 2.2.1.
- **SHOULD-FIX** — health tick mis-attributed to `MainWindow`'s 2 s
  timer → § 2.5 now gives `PluginManager` its own `QTimer`.
- **SHOULD-FIX / NIT** — over-strong "only cross-thread access" claim
  (§ 2.5 reworded); `settings.set` unaddressed (§ 2.3 line added);
  INV-1 grep weak (now leans on the runtime affinity assert); 512 KiB
  stack stated as a chosen cap, not derived (§ 4); PLUGINS.md:391 named
  as the concrete edit (§ 7); test bundle named (`lua_sandbox_hardening`,
  § 6).

**Loop 2 (2026-05-22)** — fresh reviewer, same cold brief, *not* told
what loop 1 changed. **None of loop 1's findings recurred** — those
fixes held. Two new (deeper) BLOCKERs surfaced, now folded:

- **BLOCKER** — the queued `initialize`/`loadScript` posts return `void`,
  so the GUI can no longer gate `Load` on a load failure (a `loadScript`
  script error leaves `m_state` valid → `Load` fires into a half-loaded
  VM) → § 2.2.2 now specifies worker-side `m_state`-latch gating +
  worker-side `shutdown()` on the loadScript failure path.
- **BLOCKER** — `shutdown()`/`~LuaEngine` call `lua_close` (a `lua_*`
  call) on the GUI thread today (`unloadAll` `pluginmanager.cpp:65`,
  load-failure `:351`, destructor `luaengine.cpp:91`), violating INV-1 at
  teardown → § 2.5.1 rewritten to keep all `lua_close` on the worker +
  new INV-12.
- **SHOULD-FIX** — `settings.get` is a two-hop relay (engine → PM →
  MainWindow `mainwindow.cpp:566`); only the first hop is BlockingQueued
  (§ 2.3 clarified). Test bundle is `test_lua` not `lua_sandbox_hardening`
  (§ 6 fixed). PLUGINS.md `:393` "sandbox blocks threads" also now false
  (§ 7 widened to 391-393).
- **NIT** — keybinding line is `:538` (§ 2.2.1); health-timer cite
  trimmed to `:737` (§ 2.5).

Grounding verified clean by loop 2 (all `path:line` refs correct).

**Loop 3 (2026-05-22)** — fresh reviewer, same cold brief, not told what
loop 2 changed. **No BLOCKERs; none of loop 2's findings recurred** —
the spec has converged. Refinements folded:

- **SHOULD-FIX** — `settings.get`'s worker-side block is unbounded; spec
  now states this is intentional and distinct from the § 2.6 GUI-block
  risk (§ 2.3).
- **SHOULD-FIX** — abort latency clarified: fires at the next hook fire
  (sub-ms, bounded by the 100k/line cadence); no racy cross-thread
  `lua_sethook` (§ 2.5).
- **SHOULD-FIX** — health detection switched from post-time to
  `eventStarted`-based *execution* time, so a backed-up healthy worker
  isn't falsely demoted (§ 2.5 + single-C-call bullet).
- **SHOULD-FIX** — `dispatchTo` / `healthyEngines()` / `healthTick()` now
  have one-line declarations (§ 2.2.1); `firePaletteAction` GUI-side
  callers (`mainwindow.cpp:6686`/`:6699`/`:2138`) named as unchanged.
- **NIT** — `test_lua` bundle line corrected to `CMakeLists.txt:1384`
  (§ 6).

Grounding re-verified clean by loop 3.

**Loop 4 (2026-05-22)** — fresh reviewer, same cold brief, not told what
loop 3 changed. **Clean across all 7 categories; no BLOCKERs; none of
loop 3's findings recurred.** Only a line-number NIT (`ants_lua_lib`
source is `CMakeLists.txt:424`, lib decl `:423` — § 4 clarified) and a
documentation-completeness SHOULD-FIX (enumerate the five broadcast
`fireEvent` sites — already covered transitively by § 2.2; § 2.6 now
lists them explicitly + confirms it is the *complete* engine-invoking
set). Both folded; no design change. Convergence confirmed: the
threading model, deadlock asymmetry, teardown/detach path, and
execution-time watchdog are all sound against current source.

**Sign-off (2026-05-22):** per the user's standing rule — a clean
cold-eyes pass *is* the sign-off — loop 4's clean pass accepts the spec.
Status → accepted; next step is the implementation plan (writing-plans).

**Implementation row (2026-10-08, ANTS-5107)** — not a review loop; no
reviewer was dispatched. A performance review found teardown waiting
`kTeardownMs` per plugin, a blocked `settings.get` / `settings.set`
deadlocking it, and a queued backlog holding `Unload` back. § 2.5.1
steps 1 and 2 now record the fix: one shared deadline, blocking calls
delivered during the wait, and a teardown flag that skips the backlog.
The code and its tests (`LuaThreading.Ants5107*`) land in the same
commit.
