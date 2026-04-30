# AUDIT snapshot — Ants Terminal (historical, 2026-04-13)

> **Historical snapshot, not current.** Generated against `HEAD =
> be261d9` (2026-04-13). For the current audit pipeline, run the
> Project Audit dialog; for the most recent dated audit reports,
> see `docs/AUTOMATED_AUDIT_REPORT_<date>_*.md`.
>
> Relocated from `/AUDIT.md` to `docs/journal/` per ANTS-1121 theme T3
> (and renamed to avoid collision with the `docs/AUDIT_REPORT_*.md`
> historical reports already in `docs/`).

## Original audit-Phase-2 snapshot (commit `be261d9`, 2026-04-13)

**Verification legend:**
- **[V]** = I directly read the cited lines and confirmed the issue.
- **[S]** = Suspected — flagged by sub-agent or static analyzer, but needs Phase 3 cross-check against external docs or a deeper read.
- **[F]** = False positive — investigated and dismissed (kept in doc for audit trail).

No fixes have been applied in this phase.

---

## Critical

*(none this pass — the three most recent audits removed the obvious critical bugs)*

---

## High

### H1. OSC 8 hyperlink URIs opened without scheme validation  **[V]**

- **Location:** `src/terminalwidget.cpp:2135` (Ctrl+Click), `:3371` (context-menu "Open Link"), `:2510-2513` (OSC 8 spans populated without scheme filter).
- **Evidence:**
  ```cpp
  // terminalwidget.cpp:2510-2513
  if (hlSpans) {
      for (const auto &hl : *hlSpans) {
          spans.push_back({hl.startCol, hl.endCol,
                           QString::fromStdString(hl.uri), false, true});
      }
  }
  // …later, at :2135 and :3371:
  QDesktopServices::openUrl(QUrl(s.url));
  ```
- **Impact:** `STANDARDS.md §Security` and `RULES.md §Security #7` both mandate "URIs from OSC 8 hyperlinks sanitized before opening (restrict to http/https/file/mailto schemes)." The regex-detected URL path at `:2487` does restrict to `https?://|ftp://|file://`, but OSC 8 spans bypass that filter and flow straight into `QDesktopServices::openUrl`. A hostile program can emit `OSC 8 ;; javascript:…` (or any `xdg-open`-handled scheme) and execute on Ctrl+Click or context-menu activation.
- **Proposed fix:** Before pushing OSC 8 spans into `detectUrls()`, reject URIs whose scheme is not in `{http, https, file, mailto, ftp}`. Apply once at the detection site so both click paths share one enforcement point.
- **Verification method:** Open a test terminal, run `printf '\e]8;;javascript:alert(1)\e\\CLICK ME\e]8;;\e\\\n'`, Ctrl+Click the text, confirm nothing happens and a status-bar message is shown.
- **Risk of fix:** Low — single chokepoint; no other code reads `s.url` before the filter.
- **Source:** STANDARDS.md l.132; RULES.md §Security rule 7.

### H2. Alt screen switch (DECSET 1049/47/1047) doesn't save/restore SGR attributes  **[V]**

- **Location:** `src/terminalgrid.cpp:320-348` (enter), `:373-384` (exit).
- **Evidence:**
  ```cpp
  // enter alt screen (:323-332)
  m_altScreenActive = true;
  m_altCursorRow = m_cursorRow;
  m_altCursorCol = m_cursorCol;
  m_altScrollTop = m_scrollTop;
  m_altScrollBottom = m_scrollBottom;
  m_altScreen = m_screenLines;
  m_altScreenHyperlinks = m_screenHyperlinks;
  m_altInlineImages = m_inlineImages;
  m_altPromptRegions = m_promptRegions;
  // …cursor is reset to (0,0); m_currentAttrs untouched.
  ```
- **Impact:** xterm's DECSET 1049 is defined as *"Save cursor as in DECSC"* + switch buffer + clear. DECSC saves cursor position **and SGR attributes, origin mode, selective-erase, and charset state**. This implementation only saves cursor position. When an app using 1049 (`vim`, `less`, `htop`) exits alt screen, the bold/italic/fg/bg that were active before entering the alt screen are lost; text typed after exit renders with whatever attrs were last set inside the alt screen.
- **Proposed fix:** Add a companion pair `m_altAttrs` / `m_altOriginMode` / `m_altCharset*` on entry, restore on exit. Alternatively, call the existing `saveCursor()` / `restoreCursor()` path on 1049 transitions (DECSC already persists attrs — verify it does).
- **Verification method:** `printf '\e[1;31mRED \e[?1049h\e[31mALT\e[?1049lTAIL\n'` — `TAIL` should remain red+bold. Today it renders default.
- **Source:** xterm control-sequences doc ("Save cursor and use Alternate Screen Buffer"), to be cited properly in Phase 3.

### H3. `/tmp/ants-claude-event-<pid>.json` hook-command path is dead code  **[V]**

- **Location:** `src/claudeintegration.cpp:813-862` (`ensureHooksConfigured`).
- **Evidence:**
  ```cpp
  QString hookCmd = QString(
      "jq -c '{hook_event_name: .hook_event_name, tool_name: .tool_name, "
      "tool_input: .tool_input, session_id: .session_id}' "
      "> /tmp/ants-claude-event-%1.json"
  ).arg(QApplication::applicationPid());
  ```
- **Impact:**
  1. **Dead code.** The hook server (`onHookConnection` at `:387`) consumes events from a **QLocalSocket** (`/tmp/ants-claude-hooks-<pid>`). Nothing reads `/tmp/ants-claude-event-*.json`. These files accumulate indefinitely and leak Claude tool-invocation details (commands, file paths, session IDs) in a world-traversable directory.
  2. **Wrong permissions.** `/tmp` is shared; even with `O_EXCL` the file created by the shell here inherits the user's umask — on most systems 0644, world-readable. That exposes pre-tool-use inputs (bash commands, edited file paths) to any local user.
  3. **Fails silently** if `jq` isn't installed.
- **Proposed fix:** Either wire the hook commands to pipe into the Ants socket (`nc -U /tmp/ants-claude-hooks-$PID`) and delete the `/tmp/ants-claude-event-*.json` output entirely, or delete `ensureHooksConfigured` if the socket path already receives events from a different transport. Investigate which transport is actually in use; I expect the file-write side is stale.
- **Verification method:** Start Ants, run `claude`, trigger any tool, then `ls /tmp/ants-claude-event-*.json` — the file will exist and never be read.
- **Risk of fix:** Medium — changes an integration contract. Confirm before rewriting the hook transport.

---

## Medium

### M1. Tab rename lambda captures stale `tabIndex`  **[V]**

- **Location:** `src/mainwindow.cpp:2276-2290`.
- **Evidence:**
  ```cpp
  connect(renameAction, &QAction::triggered, this, [this, tabIndex]() {
      QLineEdit *editor = new QLineEdit(m_tabWidget->tabBar());
      editor->setText(m_tabWidget->tabText(tabIndex));
      …
      connect(editor, &QLineEdit::editingFinished, this, [this, editor, tabIndex]() {
          QString newName = editor->text().trimmed();
          if (!newName.isEmpty())
              m_tabWidget->setTabText(tabIndex, newName);
          editor->deleteLater();
      });
  });
  ```
- **Impact:** If a tab is closed or moved between "Rename" menu click and the user pressing Enter, the captured `tabIndex` points at the wrong tab. Result: silently renaming a different tab (best case) or out-of-bounds `setTabText` on an invalidated index (harmless on Qt — no-op — but still a correctness bug).
- **Proposed fix:** Capture the tab's `QWidget*` instead, and resolve the index via `m_tabWidget->indexOf(w)` at edit-finish time, early-return if -1.
- **Risk of fix:** Low.

### M2. Tab color menu lambda captures stale `tabIndex`  **[V]**

- **Location:** `src/mainwindow.cpp:2312-2320`.
- **Evidence:**
  ```cpp
  connect(a, &QAction::triggered, this, [this, tabIndex, ce]() {
      if (ce.color.isValid()) {
          m_tabColors[tabIndex] = ce.color;
          m_tabWidget->tabBar()->setTabTextColor(tabIndex, ce.color);
      } else {
          m_tabColors.remove(tabIndex);
          m_tabWidget->tabBar()->setTabTextColor(tabIndex, QColor());
      }
  });
  ```
- **Impact:** Same class of bug as M1 — also leaks `m_tabColors` entries when the tab is closed (the hash key is never removed in `closeTab`). Low probability but trivial to hit with mouse-heavy usage.
- **Proposed fix:** Same pattern — capture `QWidget*`, resolve index via `indexOf`. Also add `m_tabColors.remove(...)` to `closeTab` using the key derived from the QWidget. (Consider changing the hash to keyed by `QWidget*` to avoid stale indices altogether.)
- **Risk of fix:** Low.

### M3. Reflow on resize discards combining characters  **[V]**

- **Location:** `src/terminalgrid.cpp:1140-1186` (scrollback reflow), `:1191-1241` (screen reflow).
- **Evidence:**
  ```cpp
  for (auto &sl : m_scrollback) {
      auto &cells = sl.cells;
      int len = static_cast<int>(cells.size());
      while (len > 0 && cells[len - 1].codepoint == ' ') --len;
      current.insert(current.end(), cells.begin(), cells.begin() + len);
      if (!sl.softWrapped) { … }
  }
  ```
  `sl.combining` (per-line `unordered_map<int, vector<uint32_t>>`) is never read, never propagated to the rebuilt lines.
- **Impact:** Any line containing combining characters (accents, Devanagari, zero-width joiners, emoji ZWJ sequences) loses its diacritics on every window resize. Wide CJK cells retain `isWideChar`/`isWideCont` flags only because those are on the `Cell` struct, not on the side table.
- **Proposed fix:** Carry combining data through reflow. Two options:
  1. Track `(col-in-source → combining)` per logical line, then remap to new columns as each cell is emitted.
  2. Collapse combining into per-cell side data before reflow (temporary), then un-split afterward.
  Option 1 is simpler: a parallel `std::vector<std::vector<uint32_t>>` keyed by position in `logical`.
- **Verification method:** Paste `café` into a terminal, resize the window so the word wraps. The combining acute accent disappears today.
- **Risk of fix:** Medium — touches reflow, which is a hot spot prone to regressions. Must exercise both scrollback and screen paths, plus resize with `softWrapped` spans.

### M4. Kitty image cache evicts *entire* cache on overflow  **[V]**

- **Location:** `src/terminalgrid.cpp:1607`.
- **Evidence:**
  ```cpp
  if (imageId > 0 && !image.isNull()) {
      if (static_cast<int>(m_kittyImages.size()) >= MAX_KITTY_CACHE) m_kittyImages.clear();
      m_kittyImages[imageId] = image;
  }
  ```
- **Impact:** Hitting the 200-entry cap nukes 199 warm images (each potentially megabytes). On a Kitty graphics-heavy workflow (`kitten icat`, `chafa -f kitty`), every 201st image discards the prior 200, causing a sudden reload storm and a perceptible UI hitch.
- **Proposed fix:** Evict the oldest single entry. Cheap implementation: maintain a `std::deque<int> m_kittyInsertOrder` alongside the map; pop front when size >= MAX.
- **Risk of fix:** Low.

### M5. `Config::load()` does not validate numeric ranges; setters do  **[V]**

- **Location:** `src/config.cpp:21-29` (load), `:64-72` (fontSize getter/setter pattern, replicated across ~30 getters).
- **Evidence:**
  ```cpp
  int Config::fontSize() const { return m_data.value("font_size").toInt(11); }
  void Config::setFontSize(int size) {
      size = qBound(4, size, 48);
      m_data["font_size"] = size;
      …
  }
  ```
  Setter clamps; getter returns whatever is on disk. A hand-edited `config.json` with `"font_size": 9999` returns 9999 unconstrained.
- **Impact:** Corrupted config file → UI anomalies (huge fonts, zero-opacity window, multi-GB scrollback allocation). Not exploitable by external input (only the user's own file), but fragile.
- **Proposed fix:** Either validate in `load()` (single pass, rewriting invalid keys to defaults), or make getters apply `qBound` with the same range the setters use. The latter is cheaper and keeps `load` simple.
- **Risk of fix:** Low. Touch only the ~6 numeric/opacity/padding/scrollback getters with known ranges; leave string keys alone.

### M6. Claude-process detection matches substring "claude" loosely  **[V]**

- **Location:** `src/claudeintegration.cpp:75-86`.
- **Evidence:**
  ```cpp
  for (pid_t pid : childPids) {
      QFile cmdFile(QString("/proc/%1/cmdline").arg(pid));
      …
      QString cmdline = QString::fromUtf8(cmdFile.readAll()).replace('\0', ' ').toLower();
      if (cmdline.contains("claude") || cmdline.contains("claude-code")) { … }
  ```
- **Impact:** Any child process with "claude" anywhere in argv matches — `grep claude file.txt`, a user running an unrelated tool at `~/bin/claude-search`, or a process whose cwd is a directory named `claude/…` (`cmdline` is argv, not cwd — so that last case is fine). The first two are real false-positive vectors that flip the status bar into a phantom "Claude running" state and then wedge state machine until the real process is detected.
- **Proposed fix:** Match the argv[0] basename against an exact list `{claude, claude-code}` and require it at the start of `cmdline` (after splitting on `\0`). Keep the fallback for `claude-code` being invoked via `node /path/to/claude-code/cli.js` by additionally checking for a `node` arg0 + `claude` in the script path.
- **Risk of fix:** Low.

### M7. MCP server response missing JSON-RPC envelope  **[S]**

- **Location:** `src/claudeintegration.cpp:495-610` (`onMcpConnection`).
- **Evidence:** Each response is assembled as a bare JSON object: `{"tools": [...]}` or `{"content": [...]}`. No `jsonrpc`, no `id`, no `result` wrapper. The request `id` is read but never echoed back.
- **Impact:** Real MCP clients correlate requests via `id`. Without it, the integration may be silently inert when a real MCP client (Claude Code's stdio/socket MCP transport) talks to it. This function may have been partially implemented.
- **Verification needed (Phase 3):** Consult the Model Context Protocol spec for the wire envelope. Confirm whether Claude Code uses the JSON-RPC framing over sockets or a custom line protocol.
- **Proposed fix:** Wrap every response as `{"jsonrpc":"2.0","id":<echoed>,"result":{…}}` and add error replies for unknown methods.
- **Risk of fix:** Low — additive wrapping.

---

## Low

### L1. `ToggleSwitch::m_anim` not initialized at declaration  **[V]**

- **Location:** `src/toggleswitch.h:30`.
- **Evidence:** cppcheck 2.20.0 reports `ToggleSwitch::m_anim is not initialized`. Assignment happens in the constructor, but a crash between construction and assignment (unlikely) would read garbage. Style-grade issue; easy to fix with `= nullptr` at declaration.
- **Proposed fix:** `QPropertyAnimation *m_anim = nullptr;`
- **Risk of fix:** None.

### L2. GlRenderer glyph-quad UV computation may double-count  **[S]**

- **Location:** `src/glrenderer.cpp:364`.
- **Evidence:**
  ```cpp
  float u0 = q.u0, v0 = q.v0, u1 = q.u0 + q.u1, v1 = q.v0 + q.v1;
  ```
  `placeAndUploadToAtlas` (`:515-516`) writes absolute coords (`(m_atlasX + w) / m_atlasWidth`) to its out-parameters. If the producer of `GlyphQuad` stores those as absolute `u1`/`v1`, then line 364 is adding absolute+absolute and scrolling UVs off the atlas.
- **Needs verification:** Read the callers that construct `GlyphQuad` — if they store `q.u1` as `(absolute_u1 - u0)` (i.e. a delta), the code is correct and my concern is wrong. Sub-agent flagged this; I didn't confirm the producer.
- **Proposed action:** Phase 3 — trace the two callers that build `GlyphQuad` and confirm the semantics.

### L3. `closeTab` does not null-check `m_tabWidget->widget(index)` before `deleteLater()`  **[V]**

- **Location:** `src/mainwindow.cpp:1263-1277`.
- **Evidence:** `QWidget *w = m_tabWidget->widget(index); … w->deleteLater();` with no guard.
- **Impact:** In normal use `index` is always in range (comes from `closeCurrentTab()` or a tab-close click). A defensive guard is cheap and removes a class of crashes from future misuse.
- **Proposed fix:** `if (!w) return;` immediately after line 1263.
- **Risk of fix:** None.

### L4. Claude transcript 32 KB window may miss the state-determining event  **[S]**

- **Location:** `src/claudeintegration.cpp:217-233`.
- **Evidence:** Tail of transcript read is hard-capped at 32 KB. If the last N events before the live one are huge (a full file-read tool result can be 60–100 KB in one JSON object), the window starts mid-line and the first line is discarded as "likely truncated". If the remaining events are all in the metadata-skip set, the state stays stale.
- **Verification needed:** Measure typical Claude Code transcript event sizes on a Read/Bash output tool_result.
- **Proposed fix:** Bump the window to 256 KB, or better — walk *backward* from EOF by scanning for newlines, collecting events until we find one non-metadata type.
- **Risk of fix:** Low — read-only path.

### L5. Cppcheck shadow/const-ref / static nits  **[V]**

- **Locations:** `src/luaengine.cpp:154,163,195,200,215`; `src/pluginmanager.cpp:36,87,89`; `src/terminalgrid.cpp:135,146,554,1159,1208`; `src/titlebar.cpp:122`; `src/auditdialog.cpp:757`; `src/themes.cpp:274`; `src/config.cpp:14`.
- **Evidence:** All flagged by cppcheck baseline in DISCOVERY.md §6.
- **Impact:** None functional. Signal-emit parameters shadow the signal name in lambdas; style-only.
- **Proposed action:** Fold into a single cleanup commit late in the fix plan, or skip if the noise is acceptable.

### L6. `cleanupOldSessions(30)` called only at startup  **[V]**

- **Location:** `src/sessionmanager.cpp:371-379`.
- **Evidence:** Function exists and is sound, but `rg` of the codebase shows it's not called from any long-lived timer. Sessions older than 30 days get cleaned up only when the app launches.
- **Impact:** A user who keeps Ants running for weeks without restart accumulates session files. Minor.
- **Proposed fix:** Hook a 24 h QTimer in `MainWindow` constructor to call it periodically, or leave as-is (startup is adequate for most users).
- **Risk of fix:** None.

---

## False positives (investigated, dismissed)

- **`wheelEvent` mouse row formula (`:1482`)** — sub-agent claimed the sign is inverted. Walking the math: `pixelToCell` returns `globalLine = scrollbackSize - scrollOffset + vr`; line 1482 subtracts that back out to recover `vr+1`. Formula is correct.
- **Scrollbar inversion (`:195`, `:2004`)** — agent called it "semantically backwards". It is intentionally inverted because Qt scrollbars run 0=top/max=bottom, while `m_scrollOffset` runs 0=live/max=deepest-scrollback. Round-trip is correct and stable.
- **Word-boundary condition `right < cols-1` (`:2331`)** — condition `right < cols - 1` means `right+1 < cols`, which is the correct access bound. Words ending at the last column are selected correctly; traced case-by-case.
- **`searchNext` modulo underflow (`:2713-2714`)** — guarded by `if (m_searchMatches.empty()) return;` one line above; `isCellCurrentMatch` also bounds-checks. Dismissed.
- **Paste sanitizer split-escape (`:1712`)** — `pasteToTerminal()` receives the full clipboard payload in a single call; `replace()` operates on the complete buffer before writing. PTY chunking doesn't re-introduce split escapes because the bytes are already sanitized. Dismissed.
- **OSC 52 "redundant" pre- vs post-decode check (`:620-626`)** — pre-check is a fast-path rejection that avoids the base64 allocation for obviously-oversized payloads; post-check catches the exact-size case. Defense in depth, not redundancy.
- **SGR 38/48 off-by-one (`:885`, `:894`)** — walked the bounds: `i+2 < size` correctly guards access at `i+2`; `i+4 < size` guards access at `i+4`. Agent's claim was wrong.
- **`insertLines`/`deleteLines` index corruption (`:930-951`)** — size is preserved across each iteration (one `erase` + one `insert`), so `bottom` stays valid. Traced.
- **Lua `load()` sandbox escape** — `sandboxEnvironment()` sets `load`, `loadfile`, `dofile` to `nil` before `loadScript` ever runs. String.dump is still available, but with `load` gone the bytecode can't be executed. Dismissed.
- **GL atlas second-overflow corruption** — after doubling to 4096×4096, the next overflow clears caches and zero-uploads the atlas, restarting placement. Not ideal for perf, but not a crash or corruption. Acceptable behaviour.

---

## Categories flagged as **N/A** for this codebase

- **CI/CD pipelines** — none exist. Dropping a minimal workflow (cmake build + cppcheck + clang-tidy gate) is discussed in `FIXPLAN.md` but is a net-new, not a fix.
- **API versioning / schema migration** — single-user desktop app; session file format has a `VERSION` constant (currently 2) with backward-read path.
- **i18n** — app is English-only by design. No hard-coded strings scheduled for translation.
- **a11y** — Qt widgets provide default screen-reader labels; terminal surface itself is a graphics widget (QOpenGLWidget) with no accessibility tree. Non-trivial retrofit; track as experimental in `EXPERIMENTAL.md`.

---

## Test-coverage posture

There are **no automated tests**. Every finding above will be verified by manual repro against the live binary. A minimal test harness (Qt Test + a PTY-driver that feeds escape sequences into `VtParser`) is proposed in `EXPERIMENTAL.md`, but is out of scope for this fix pass.

---

## Summary counts

| Severity | Verified | Suspected | Total |
|---|---|---|---|
| Critical | 0 | 0 | 0 |
| High     | 3 | 0 | 3 |
| Medium   | 6 | 1 | 7 |
| Low      | 4 | 2 | 6 |

---

## References (Phase 3)

Primary sources consulted for High / confirmed-Medium findings:

- **H1 (OSC 8 scheme allowlist).** Egmont Koblinger, *Hyperlinks in terminal emulators* — the de-facto OSC 8 spec referenced by VTE, iTerm2, WezTerm. [gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda](https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda).
  > "It's up to the terminal emulator to decide what schemes it supports and which applications it launches for them. Terminal emulators might decide to whitelist only some well known schemes and ask for the user's confirmation on less known ones."

  The gist explicitly discusses only `http://`, `https://`, `ftp://`, `file://`, `mailto:`. `javascript:` is not in the discussed set. The whitelist approach `{http, https, file, mailto, ftp}` used in this audit matches that guidance.

- **H2 (DECSET 1049 must restore SGR attrs).** Two sources:
  - xterm *Control Sequences* (Thomas Dickey), `CSI ? 1049 h` entry: *"Save cursor as in DECSC, xterm. After saving the cursor, switch to the Alternate Screen Buffer"*. [invisible-island.net/xterm/ctlseqs/ctlseqs.html](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html).
  - DEC VT510 *Reference Manual*, DECSC entry — lists the saved state as: cursor position, **character attributes set by SGR**, character sets (GL/GR), wrap flag, origin mode (DECOM), selective-erase attribute, and single-shift (SS2/SS3) state. [vt100.net/docs/vt510-rm/DECSC.html](https://vt100.net/docs/vt510-rm/DECSC.html).

  Together these confirm that `1049 h` must checkpoint SGR + charsets + origin + wrap, not cursor alone.

- **M7 (MCP requires JSON-RPC 2.0 envelope).** MCP specification, *Base Protocol*, 2025-11-25: *"All messages between MCP clients and servers MUST follow the JSON-RPC 2.0 specification … Result responses MUST include the same ID as the request they correspond to. Result responses MUST include a `result` field."*. [modelcontextprotocol.io/specification/2025-11-25/basic](https://modelcontextprotocol.io/specification/2025-11-25/basic).

  Confirms that the current bare-object responses (`{"tools":[...]}`) are non-compliant; the required shape is `{"jsonrpc":"2.0","id":<echoed>,"result":{...}}`.

- **Supporting (QTextLayout caching).** Qt 6 `QTextLayout` class reference. [doc.qt.io/qt-6/qtextlayout.html](https://doc.qt.io/qt-6/qtextlayout.html). Documents `setCacheEnabled(true)` for repeated-draw scenarios and notes `QStaticText` as the right primitive when text itself is unchanging across paints. Feeds into the performance note in `EXPERIMENTAL.md`, not a bug here.

- **Context (bracket-paste CVE referenced in RULES.md).** CVE-2021-28848 (ConEmu / bracket-paste injection). Retained in `RULES.md #20` — the current code already sanitizes `\e[200~` / `\e[201~` in `pasteToTerminal()`; no change needed for this pass.

Phase 4 (fix plan) next — then Phase 5 execute. **No code will change until the plan is approved.**
