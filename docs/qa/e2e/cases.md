# Feature case suite — user-level QA pass (ANTS-2050)

A repeatable "sit down and use the app" checklist covering every user-facing
feature, grouped by subsystem. It runs on the ANTS-2049 harness
([README](README.md), [spec](../../specs/ANTS-2049.md)).

Each case is **drive → observe → expected observable**. A run is either:

- **Automated** — a lane in [`tools/e2e/cases.sh`](../../../tools/e2e/cases.sh),
  asserted headlessly through the harness. Run all lanes or one:
  ```bash
  ANTS_E2E_BIN=build/ants-terminal QT_QPA_PLATFORM=offscreen bash tools/e2e/cases.sh
  ANTS_E2E_BIN=build/ants-terminal QT_QPA_PLATFORM=offscreen bash tools/e2e/cases.sh theme
  ctest --test-dir build -L e2e --output-on-failure     # via ctest
  ```
- **Manual / on-screen** — driven by hand with `run.sh`'s functions (see the
  README "Drive it by hand"), checked with your eyes. These need a real display
  (geometry, colour, glyph shaping) or a resource the harness can't fake yet.

## Status legend

| Mark | Meaning |
|---|---|
| ✅ **auto** | Asserted by `cases.sh <lane>` — runs in `ctest -L e2e`. |
| 🖐 **manual** | Needs a human eye or a real display (visual, geometry, colour). |
| 🔧 **pending-hook** | Automatable, but needs a harness `objectName`/verb not yet present. Tracked by the harness-extension follow-up (see foot of page). |

Why not everything is automated: the harness observes the terminal through
`get-text` (grid text), liveness through `tab-list`, size through the
`resize-window` echo, and a rendered frame through `grab-image` (a *non-zero
PNG* — it proves the widget painted without crashing, it does **not** inspect
pixels). Anything whose only signal is a colour, a glyph shape, a pixel, or a
window rectangle is therefore 🖐; anything that needs input the harness can't
yet address (a specific dialog button, a paste, a second-profile relaunch) is
🔧 until a hook lands.

Harness-contract guards (bad widget → `widget_not_found`, non-`--e2e` gate →
`e2e_disabled`, grab escape → `bad_path`) are covered by
[`tools/e2e/smoke.sh`](../../../tools/e2e/smoke.sh), not repeated here.

---

## 1. Terminal basics — `cases.sh terminal`

| Case | Steps → Expected observable | Status |
|---|---|---|
| Echo + shell | Type `echo TB_$((6*7))_END` → `get-text` shows `TB_42_END`. | ✅ |
| ANSI SGR | `printf '\033[1;31mCOLORSAFE\033[0m\n'` → text `COLORSAFE` survives (escape consumed). | ✅ |
| CR overwrite | `printf 'XXXX\rYY\n'` → row reads `YYXX`. | ✅ |
| UTF-8 / CJK width | `echo CJK_中文_END` → both `中` and `文` present; each padded to 2 cells (`中 文 `). | ✅ |
| Long-line integrity | Print a 200-char line → all 200 chars survive (wraps, none dropped). | ✅ |
| Combining chars | Print `e` + U+0301 (é via combining accent) → renders as one cell, no width drift on the rest of the row. | 🖐 |
| Ligatures | In a ligature font, type `=>` `!=` `->` → glyphs shape as ligatures, cursor still lands on the right cell. | 🖐 |

## 2. Scrollback, search & navigation — `cases.sh scrollback`

| Case | Steps → Expected observable | Status |
|---|---|---|
| History retained | `echo SCROLLTOP; seq 1 300; echo SCROLLBOT` → both markers in `get-text -lines 400`. | ✅ |
| Find-in-scrollback | Open find, search a known string → match highlighted, count shown, next/prev cycles matches. | 🔧 |
| Back-to-bottom | Scroll up, then press the back-to-bottom control → view returns to the prompt. | 🔧 |
| Prompt-jump | With shell-integration marks, jump prev/next prompt → viewport lands on each prompt line. | 🔧 |

## 3. Tabs — (partly observable via `tab-list`)

| Case | Steps → Expected observable | Status |
|---|---|---|
| Open / close | New-tab shortcut → `tab-list` grows by one; close → shrinks by one. | 🔧 |
| Rename | Rename a tab → `tab-list` reflects the new title. | 🔧 |
| Reorder | Drag a tab left/right → order in `tab-list` changes. | 🔧 |
| Title colour | Set a tab title-colour → the tab header paints that colour. | 🖐 |

## 4. Splits & multiplexing

| Case | Steps → Expected observable | Status |
|---|---|---|
| Split horizontally / vertically | Split shortcut → two panes share the window; each has its own PTY. | 🔧 |
| Focus follows split | Type after a split → text lands in the focused pane only. | 🔧 |
| Close pane | Close one pane → the other expands to fill; window stays alive. | 🔧 |

## 5. Alt-screen apps

| Case | Steps → Expected observable | Status |
|---|---|---|
| Enter/leave alt-screen | `printf '\033[?1049h'` write, then `\033[?1049l` → primary-screen content restored intact. | 🔧 |
| Full-screen app | Run `vim`/`htop` → app paints full-screen; on quit the shell prompt + scrollback return. | 🖐 |

## 6. Dialogs (D1–D4 geometry + content)

Per [`docs/standards/dialogs.md`](../../standards/dialogs.md): themed, resizable,
size persisted, re-centred on open.

| Case | Steps → Expected observable | Status |
|---|---|---|
| Roadmap dialog opens | Click `roadmapButton` → `RoadmapDialog` visible; `grab-image widget=RoadmapDialog` yields a non-zero PNG. | 🖐 (smoke case 3; on-screen) |
| Audit dialog | Open Audit → dialog shows findings list; themed chrome. | 🔧 |
| Settings dialog | Open Settings → tabs render; theme combo lists built-ins. | 🔧 |
| About dialog | Open About → version string matches `project(VERSION)`. | 🔧 |
| Review-changes dialog | Open Review Changes with a dirty tree → diff renders. | 🔧 |
| Geometry D1–D4 | Resize a dialog, close, reopen → size persisted, re-centred, theme-consistent. | 🖐 |

## 7. Status-bar widgets

| Case | Steps → Expected observable | Status |
|---|---|---|
| Git branch chip | `cd` into a repo → chip shows the branch; switch branch → chip updates. | 🔧 |
| Repo-visibility badge | Open a PUBLIC vs PRIVATE repo → badge reflects visibility. | 🔧 |
| Update notifier | With an update available → notifier chip appears. | 🖐 |
| Tasks chip | Drive a task list to `done/total` → chip shows the count; batch-reset clears it. | 🔧 |
| Model chip + pulse/undo | Model switch → chip updates with a pulse; undo restores the prior model. | 🖐 |

## 8. Themes & opacity — `cases.sh theme`

| Case | Steps → Expected observable | Status |
|---|---|---|
| Restyle liveness | Thrash all built-in themes via config-reload → app stays alive (`tab-list` ok) and re-renders (non-zero PNG). | ✅ |
| Menu-driven theme switch | View → Theme → pick each theme → applies without crashing. **The Wayland menu-popup race (ANTS-3556) is regression-guarded by the source test `tests/features/theme_switch_popup_defer`** — the ✅ lane drives the config-reload path, a *different* code path from the menu QAction. | 🖐 |
| Opacity | Set terminal opacity < 100% → terminal area shows through; chrome stays opaque. | 🖐 |

## 9. Image paste auto-save

| Case | Steps → Expected observable | Status |
|---|---|---|
| Paste an image | Copy an image, paste into the terminal → it's saved to disk and the filepath is inserted at the cursor. | 🔧 |

## 10. Session persistence

| Case | Steps → Expected observable | Status |
|---|---|---|
| Save / restore | With N tabs open, quit gracefully, relaunch the same profile → the N tabs (and titles) come back. | 🔧 (needs relaunch-same-profile + graceful-quit save) |

## 11. SSH bookmarks & ControlMaster

| Case | Steps → Expected observable | Status |
|---|---|---|
| Bookmark connect | Pick an SSH bookmark → a session opens to that host. | 🖐 (needs a reachable host) |
| ControlMaster reuse | Open a second session to the same host → reuses the master connection (no re-auth). | 🖐 |

## 12. Lua plugin sandbox

| Case | Steps → Expected observable | Status |
|---|---|---|
| Plugin loads | Drop a plugin using the `ants.*` surface → it loads and its effect is observable. | 🔧 |
| Sandbox strips globals | A plugin touching `os.execute`/`io` → blocked; dangerous globals absent. | 🔧 |
| Instruction-count timeout | A plugin with an infinite loop → interrupted by the instruction-count cap, app stays alive. | 🔧 |

## 13. Claude Code integration surface

| Case | Steps → Expected observable | Status |
|---|---|---|
| MCP chrome | With `claude.mcp_enabled`, the socket is bound; a verb round-trips. (When false, verbs refuse `mcp_disabled`.) | 🔧 |
| Task list | Claude drives a task list → the tasks chip / panel reflects it. | 🔧 |
| Spinner de-dup | Rapid state changes → a single spinner, not a stutter of duplicates. | 🖐 |
| Hooks | A SessionStart hook fires → its prelude is present. | 🖐 |

---

## Coverage summary

- **Automated now** (✅, in `ctest -L e2e`): terminal basics (5 cases),
  scrollback retention, resize (3 cases), theme restyle-liveness (2 cases).
  These need **no** new harness hooks — `send-text`/`get-text`/`resize-window`/
  `tab-list`/`grab-image` cover them.
- **Manual / on-screen** (🖐): anything whose signal is a pixel, glyph, colour,
  or window rectangle, or that needs a real host/display.
- **Pending-hook** (🔧): automatable once the harness gains the addressing it
  lacks today — per-dialog and status-bar `objectName`s, key/shortcut injection
  with reliable focus, a relaunch-same-profile helper, a paste path, and a
  plugin-load path. These are the next increment of harness work, tracked as a
  follow-up to ANTS-2049/2050; grow `cases.sh` lane-by-lane as each hook lands
  (the README's "add more as cases need them").

When you add a hook and promote a 🔧 case to ✅, add its lane to `cases.sh`,
flip the mark here, and keep the two in sync.
