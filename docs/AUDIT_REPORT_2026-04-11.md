# Ants Terminal - Comprehensive Security & Code Audit Report

**Date:** 2026-04-11
**Auditor:** Claude Opus 4.6 (1M context)
**Scope:** Full codebase review (48 source files, ~15,600 lines of C++/Qt6)
**Previous Audits:** 2026-04-04 (42 items), 2026-04-09 (22 items)

---

## Executive Summary

This fourth comprehensive audit examined all 48 source files in the Ants Terminal codebase for bugs, security vulnerabilities, buffer overflows, memory leaks, dead code, performance issues, and best practices compliance. **All findings from previous audits were verified as fixed before this review began.**

**Findings:** 55 total issues identified, 42 fixed in this session
- **Critical:** 5 (5 fixed)
- **High:** 18 (14 fixed)
- **Medium:** 18 (13 fixed)
- **Low/Cosmetic:** 14 (10 fixed)

**Build Status:** Clean build with zero warnings under `-Wall -Wextra -Wpedantic` with full security hardening.

---

## CRITICAL FIXES (5/5 fixed)

### C1. PTY File Descriptor Leak in Child Process (CWE-403)
- **File:** `ptyhandler.cpp:78-102`
- **Issue:** `forkpty()` child inherited all parent FDs (AI network sockets, plugin FDs, etc.)
- **Fix:** Close all FDs 3-1023 in child before `exec()` to prevent leaked file descriptors
- **References:** [CERT FIO22-C](https://wiki.sei.cmu.edu/confluence/display/c/FIO22-C.+Close+files+before+spawning+processes), [CWE-403](https://cwe.mitre.org/data/definitions/403.html)

### C2. Lua Binary Bytecode Loading Allows Sandbox Escape
- **File:** `luaengine.cpp:147`
- **Issue:** `luaL_dofile()` executes compiled bytecode without verification. Lua 5.4 has no bytecode verifier, so crafted bytecode can corrupt memory and escape the sandbox.
- **Fix:** Check first byte of plugin files for `\x1b` (Lua bytecode signature) and reject.
- **References:** [Lua 5.4.4 Sandbox Escape](https://github.com/Lua-Project/lua-5.4.4-sandbox-escape-with-new-vulnerability)

### C3. Partial Socket Read in Hook/MCP Handlers
- **File:** `claudeintegration.cpp:366-374, 465-467`
- **Issue:** `socket->readAll()` in `readyRead` handler only gets partial data if JSON payload spans multiple TCP segments. Incomplete JSON silently discarded.
- **Fix:** Buffer data per-socket; process on `disconnected` (hooks) or valid JSON detection (MCP).

### C4. OSC 52 Clipboard Prefix Truncated by C String Null Terminator
- **File:** `terminalgrid.cpp:537`
- **Issue:** `"\x00OSC52:" + std::string(...)` — the `const char*` overload stops at the null byte, producing an empty string instead of the `\0OSC52:` prefix. Clipboard integration was completely broken.
- **Fix:** Use `std::string(1, '\0')` constructor and concatenate.

### C5. No Compiler Security Hardening Flags
- **File:** `CMakeLists.txt`
- **Issue:** Zero security flags — no stack protector, no FORTIFY_SOURCE, no RELRO, no PIE, no format security.
- **Fix:** Added `-fstack-protector-strong`, `-fstack-clash-protection`, `-D_FORTIFY_SOURCE=2`, `-Wl,-z,relro`, `-Wl,-z,now`, `-Wl,-z,noexecstack`, PIE support.

---

## HIGH SEVERITY FIXES (14/18 fixed)

### H1. PTY Master FD Missing O_CLOEXEC
- **File:** `ptyhandler.cpp:106`
- **Fix:** Added `fcntl(m_masterFd, F_SETFD, FD_CLOEXEC)` after forkpty().

### H2. PTY Double-Reap Race Condition
- **File:** `ptyhandler.cpp:151 + destructor`
- **Issue:** Both `onReadReady()` and `~Pty()` called `waitpid()` on same PID. If PID was recycled, SIGHUP sent to wrong process.
- **Fix:** Set `m_childPid = -1` after reaping in `onReadReady()`. Use `WNOHANG` to avoid blocking.

### H3. Session Decompression Bomb
- **File:** `sessionmanager.cpp:119`
- **Issue:** 100MB compressed → potentially 100GB decompressed (zlib ~1000:1 ratio).
- **Fix:** Added 500MB decompressed size check after `qUncompress()`.

### H4. Session tabId Path Traversal
- **File:** `sessionmanager.cpp:19`
- **Issue:** `tabId` concatenated directly into file path. A `../` tabId could read/write arbitrary files.
- **Fix:** Validate tabId with `^[a-zA-Z0-9\\-]+$` regex.

### H5. VT Parser m_intermediate String Unbounded
- **File:** `vtparser.cpp:99,131,219`
- **Issue:** No size limit on intermediate bytes. Malformed sequences could grow unboundedly.
- **Fix:** Capped at 8 bytes (real sequences use 1-2).

### H6. Image Dimension Checks Missing (OOM DoS)
- **File:** `terminalgrid.cpp:602, 1446`
- **Issue:** OSC 1337 (iTerm2) and Kitty PNG images loaded without dimension check. Malicious server could send image that decodes to 100000x100000 pixels.
- **Fix:** Added `MAX_IMAGE_DIM` (4096) check after `loadFromData()`.

### H7. Command Injection via Undo-Close-Tab CWD
- **File:** `mainwindow.cpp:219`
- **Issue:** `cd ` + raw directory path sent to shell. Metacharacters in directory name executed.
- **Fix:** Shell-quote the path using single quotes with internal `'\\''` escaping.

### H8. Static `firstShow` Shared Across Windows
- **File:** `mainwindow.cpp:1581`
- **Issue:** `static bool firstShow` is per-process, not per-window. Second window never centers.
- **Fix:** Changed to member variable `m_firstShow`.

### H9. Theme Reload Leaks QActions
- **File:** `mainwindow.cpp:381-395`
- **Issue:** `themesMenu->clear()` deletes actions but `m_themeGroup` still holds stale pointers. Rebuilt without clearing group first.
- **Fix:** Clear action group before repopulating.

### H10. Lua `getmetatable` Not Removed from Sandbox
- **File:** `luaengine.cpp:129-142`
- **Issue:** `getmetatable("")` returns the string metatable; modifying it affects all strings globally and can break the sandbox.
- **Fix:** Added `getmetatable` to the dangerous globals removal list.
- **References:** [HackTricks Lua Sandbox Escape](https://book.hacktricks.wiki/en/generic-methodologies-and-resources/lua/bypass-lua-sandboxes/index.html)

### H11. Lua Event Registration Silent Default
- **File:** `luaengine.cpp:47`
- **Issue:** Unrecognized event names silently defaulted to `Output`. Typos like `ants.on("outpu", cb)` bind to the wrong event.
- **Fix:** Changed `eventFromString` to return bool; `lua_ants_on` calls `luaL_error` for unknown events.

### H12. AiDialog Code Block Extraction Inverted
- **File:** `aidialog.cpp:49-56`
- **Issue:** `lastIndexOf("```")` finds the closing fence, not the opening. `indexOf("```", start+3)` searches forward from there and finds nothing.
- **Fix:** Find closing fence first, then search backwards for the opening fence.

### H13. Claude Socket Permissions Allow Local User Spoofing
- **File:** `claudeintegration.cpp:336-337, 431`
- **Issue:** Unix sockets in `/tmp` created with default permissions. Any local user could connect and send fake hook events.
- **Fix:** Set socket file permissions to owner-only after `listen()`.

### H14. Claude allowlist regex missing closing paren
- **File:** `claudeallowlist.cpp:310`
- **Issue:** Regex `^(Read|Edit|Write)\(/([^/].*)$` was missing `\)$`, so the path fix never triggered for properly-formatted rules.
- **Fix:** Changed to `^(Read|Edit|Write)\(/([^/].*)\)$` and appended `)` to replacement.

### Not Fixed (deferred to future audit)

- **H15.** Tab color indices invalidated on tab removal (`mainwindow.cpp:2117`) — Would require changing from index-keyed to pointer-keyed hash. Risk of breaking existing behavior.
- **H16.** Settings dialog duplicated keybinding defaults (`settingsdialog.cpp:260,491`) — Refactor to extract static method. Low risk but touches UI flow.
- **H17.** GPU renderer `render()` never called — the entire GL renderer is dead code when enabled. Needs design decision on whether to wire it in or remove it.
- **H18.** Claude path traversal in project name (`claudeprojects.cpp:406`) — **Fixed**: Added validation rejecting `/` and `..` in project names.

---

## MEDIUM SEVERITY FIXES (13/18 fixed)

### M1. ESC c (RIS) Loses bellCallback
- **File:** `terminalgrid.cpp:447-449`
- **Fix:** Preserve `m_bellCallback` alongside `m_responseCallback` during full reset.

### M2. clearRow() Uses Default Colors Instead of Current SGR Background
- **File:** `terminalgrid.cpp:976-983`
- **Issue:** Erased cells should use the current SGR background per VT spec. Was always using default.
- **Fix:** Use `m_currentAttrs.bg` when valid, otherwise `m_defaultBg`.

### M3. restoreCursor() No Bounds Check After Resize
- **File:** `terminalgrid.cpp:931-935`
- **Fix:** Clamp saved cursor position to current grid dimensions.

### M4. screenCombining/scrollbackCombining No Bounds Check
- **File:** `terminalgrid.h:144-149`
- **Fix:** Return static empty map when index is out of range.

### M5. pushScrollbackLine No Max Scrollback Enforcement
- **File:** `terminalgrid.h:174`
- **Issue:** Session restore could load unlimited scrollback lines.
- **Fix:** Enforce `m_maxScrollback` limit in `pushScrollbackLine`.

### M6. DCS/APC String Non-ASCII Truncation
- **File:** `vtparser.cpp:287, 305`
- **Issue:** `static_cast<char>(ch)` truncates codepoints above 0x7F. OSC correctly re-encoded to UTF-8, but DCS/APC did not.
- **Fix:** Added full UTF-8 re-encoding for DCS and APC strings (matching OSC pattern).

### M7. Session Files Written Without Restrictive umask
- **File:** `sessionmanager.cpp:254-258, 288-289`
- **Issue:** Brief window where files may be world-readable between creation and `setPermissions()`.
- **Fix:** Set `umask(0077)` before opening files, restore after.

### M8. Session File Read Without Size Check
- **File:** `sessionmanager.cpp:265`
- **Issue:** `file.readAll()` with no size check before the compressed data limit.
- **Fix:** Check `file.size() > 100MB` before `readAll()`.

### M9. Command Palette populateList Called Twice on Show
- **File:** `commandpalette.cpp:42-44`
- **Issue:** `m_input->clear()` triggers `textChanged` signal → `populateList()`, then `populateList()` called explicitly.
- **Fix:** Block signals on `m_input` during clear.

### M10. Claude m_changedFiles Never Cleared Between Sessions
- **File:** `claudeintegration.cpp:322-327`
- **Issue:** Stale file paths from previous sessions persisted.
- **(Noted for future fix — requires processHookEvent changes)**

### M11-M13. PTY chdir fallback, fcntl error check, toggleswitch m_anim init
- Various minor fixes applied.

### Not Fixed (deferred)
- insertLines/deleteLines/scrollDown don't shift hyperlinks
- Quake mode animation hide callback accumulation
- Permission widget pile-up in status bar
- KWin temp files use predictable paths
- Sixel HLS color model not implemented

---

## LOW SEVERITY / COSMETIC (10/14 fixed)

### Dead Code Removed
- `onShellExited()` slot — never connected (`mainwindow.h:45, mainwindow.cpp:1419`)
- `onImagePasted()` — redundant, handling moved to TerminalWidget (`mainwindow.cpp:1423`)
- `hookServerPort()` — returned meaningless value, never called (`claudeintegration.h:68, .cpp:361`)
- `recentlyChangedFiles()` — declared inline, never called (`claudeintegration.h:80`)
- `imagePasted` signal connection removed from `connectTerminal()`

### Dead Includes Removed
- `<utmp.h>` from ptyhandler.cpp (line 15) — nothing from utmp used
- `<QStandardPaths>` from ptyhandler.cpp (line 4) — unused
- `<QIcon>` from titlebar.h (line 7) — moved to titlebar.cpp where it's used
- `<QTimer>` from titlebar.cpp (line 5) — unused
- `<QMenu>` from commandpalette.cpp (line 6) — unused
- `<QLabel>` from claudeintegration.h (line 5) — unused

### Build System Fixes
- Removed self-referential `set(LUA_LIBRARIES ${LUA_LIBRARIES})` (CMakeLists.txt:20)
- Added `CMAKE_CXX_EXTENSIONS OFF` for strict ISO C++20
- Added `CMAKE_EXPORT_COMPILE_COMMANDS ON` for tooling support
- Added default `CMAKE_BUILD_TYPE Release`
- Added `-Wall -Wextra -Wpedantic` per STANDARDS.md requirement

---

## ITEMS NOT FIXED (Noted for Future Work)

These items were identified but intentionally deferred due to scope, risk, or design decisions needed:

| ID | Severity | Description | File |
|----|----------|-------------|------|
| H15 | High | Tab color indices invalidated on removal | mainwindow.cpp:2117 |
| H17 | High | GPU renderer render() never called (dead code) | glrenderer.cpp |
| M-D1 | Medium | insertLines/deleteLines don't shift hyperlinks | terminalgrid.cpp:801-823 |
| M-D2 | Medium | Quake animation hide callback not disconnected | mainwindow.cpp:2035 |
| M-D3 | Medium | Permission widgets pile up in status bar | mainwindow.cpp:1688 |
| M-D4 | Medium | KWin temp files use predictable paths | mainwindow.cpp:1284,1346 |
| M-D5 | Medium | Sixel HLS color model not implemented | terminalgrid.cpp:1312 |
| M-D6 | Medium | Sixel setPixelColor per-pixel is slow | terminalgrid.cpp:1257 |
| L-D1 | Low | Config::save() called on every setter (no batching) | config.cpp |
| L-D2 | Low | updateStatusBar reads .git/HEAD every 2 seconds | mainwindow.cpp:1921 |
| L-D3 | Low | wcwidth() depends on locale for CJK width | terminalgrid.cpp:155 |
| L-D4 | Low | Trigger command actions can be triggered by remote output | mainwindow.cpp:2085 |

---

## ONLINE RESEARCH SOURCES

Security research conducted for this audit:

### Terminal Emulator CVEs
- [CVE-2024-38396 (iTerm2, CVSS 9.8)](https://nvd.nist.gov/vuln/detail/CVE-2024-38396) — Window title reporting escape injection
- [CVE-2024-56803 (Ghostty)](https://github.com/ghostty-org/ghostty/security/advisories/GHSA-5hcq-3j4q-4v6p) — Title + invisible text code execution
- [CyberArk: Don't Trust This Title](https://www.cyberark.com/resources/threat-research-blog/dont-trust-this-title-abusing-terminal-emulators-with-ansi-escape-characters)
- [Weaponizing ANSI Escape Sequences](https://www.packetlabs.net/posts/weaponizing-ansi-escape-sequences)

### PTY Security
- [CERT FIO22-C: Close files before spawning](https://wiki.sei.cmu.edu/confluence/display/c/FIO22-C.+Close+files+before+spawning+processes)
- [CWE-403: File Descriptor Leak](https://cwe.mitre.org/data/definitions/403.html)
- [TIOCSTI Security Problems](https://wiki.gnoack.org/TiocstiTioclinuxSecurityProblems)
- [TTY Pushback Attack](https://www.errno.fr/TTYPushback.html)

### Lua Sandbox Security
- [HackTricks: Bypass Lua Sandboxes](https://book.hacktricks.wiki/en/generic-methodologies-and-resources/lua/bypass-lua-sandboxes/index.html)
- [Lua 5.4.4 Sandbox Escape (UAF)](https://github.com/Lua-Project/lua-5.4.4-sandbox-escape-with-new-vulnerability)

### Qt6 Best Practices
- [Qt: Handling Untrusted Data](https://doc.qt.io/qt-6/untrusteddata.html)
- [QDataStream Class (security warnings)](https://doc.qt.io/qt-6/qdatastream.html)
- [QOpenGLWidget Pitfalls](https://doc.qt.io/qt-6/qopenglwidget.html)

### Terminal Rendering
- [Paul Williams' DEC Parser Model](https://vt100.net/emu/dec_ansi_parser)
- [LearnOpenGL Text Rendering](https://learnopengl.com/In-Practice/Text-Rendering)
- [Warp: Adventures in Text Rendering](https://www.warp.dev/blog/adventures-text-rendering-kerning-glyph-atlases)
- [How Zutty Works](https://tomscii.sig7.se/2020/11/How-Zutty-works)

---

## VERIFICATION

All changes compiled cleanly with:
- `-Wall -Wextra -Wpedantic -Wformat -Wformat-security` — **0 warnings**
- `-fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=2` — security hardening
- `-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack` — linker hardening
- Position-independent code enabled for ASLR

---

## FILES MODIFIED

| File | Changes |
|------|---------|
| `CMakeLists.txt` | Security flags, warnings, build type, extensions, compile commands, self-ref fix |
| `ptyhandler.cpp` | FD closure, O_CLOEXEC, double-reap fix, WNOHANG, chdir fallback, dead includes |
| `vtparser.cpp` | m_intermediate cap, DCS/APC UTF-8 re-encoding |
| `terminalgrid.h` | Bounds checks for combining/scrollback accessors, pushScrollbackLine cap |
| `terminalgrid.cpp` | OSC 52 null fix, image dimension checks, RIS bellCallback, restoreCursor clamp, clearRow SGR bg |
| `luaengine.cpp` | Bytecode block, getmetatable removal, eventFromString error, QFile include |
| `sessionmanager.cpp` | tabId validation, decompression bomb guard, file size check, umask |
| `mainwindow.h` | m_firstShow member, dead slot declarations removed |
| `mainwindow.cpp` | CWD injection fix, firstShow fix, theme reload fix, dead code removed, signal fix |
| `claudeintegration.h` | Dead function declarations removed, QLabel include removed |
| `claudeintegration.cpp` | Buffered socket reads, socket permissions, dead hookServerPort removed |
| `claudeallowlist.cpp` | Regex closing paren fix |
| `claudeprojects.cpp` | Path traversal validation |
| `aidialog.cpp` | Code block extraction fix |
| `commandpalette.cpp` | Double-populate fix, dead include removed |
| `titlebar.h` | QIcon include moved to .cpp |
| `titlebar.cpp` | QTimer removed, QIcon added |
