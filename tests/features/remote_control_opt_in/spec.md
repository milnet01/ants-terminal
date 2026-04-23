# Feature: remote-control opt-in default + `send-text` control-char filter

## Problem

The 2026-04-23 `/indie-review` multi-agent sweep flagged the remote-
control subsystem as a local-UID-scope RCE chain:

1. The listener starts unconditionally on every Ants launch
   (`mainwindow.cpp:788`).
2. Authentication is absent (`remotecontrol.cpp:66-113`).
3. `send-text` writes raw bytes to the PTY with no control-char
   filter (`remotecontrol.cpp:235`).

Any process running under the user's UID (a compromised browser tab,
a malicious `pip install` post-exec hook, a rogue VS Code extension)
can therefore inject `\nrm -rf ~\n` into the next tab's shell via
bracketed-paste-disable + newline-containing bytes. Kitty closes this
by defaulting `allow_remote_control=no` and requiring an explicit
password for privileged operations; Ants did neither.

## External anchors

- [Kitty's remote control docs](https://sw.kovidgoyal.net/kitty/remote-control/)
  — `allow_remote_control` defaults to `no`. Users must explicitly
  enable it (and optionally set a password) to use `@` commands.
- CWE-829 — Inclusion of Functionality from Untrusted Control Sphere.
- ECMA-48 — defines the C0 (0x00–0x1F) and C1 (0x7F–0x9F) control
  sets. 0x09 (HT), 0x0A (LF), 0x0D (CR) are *graphic* control
  characters that are safe in a keystroke stream; the rest of C0
  (especially 0x1B ESC — the vector for DECSET/DECRST,
  bracketed-paste toggles, cursor reprogramming, OSC 52 clipboard
  overwrites) are not.

## Contract

### Invariant 1 — remote-control listener defaults to OFF

The `remote_control_enabled` config key MUST exist, default to
`false`, and gate whether `RemoteControl::start()` is invoked on
MainWindow construction. When the key is `false` or absent, the
Unix-domain socket MUST NOT be created and no external process can
control the terminal.

### Invariant 2 — `send-text` strips dangerous C0 controls by default

When a `send-text` request arrives without an explicit `raw: true`
field, bytes in the set `{0x00..0x08, 0x0B..0x1F, 0x7F}` MUST be
stripped from the `text` payload before it reaches `sendToPty()`.

Preserved bytes (safe in a keystroke stream):

| Byte | Name | Why kept |
|---|---|---|
| 0x09 | HT (TAB) | Regular keyboard keystroke |
| 0x0A | LF | End-of-line for scripted commands |
| 0x0D | CR | Some shells expect CR, e.g. in line-editing contexts |

Stripped bytes that matter most for defense:

| Byte | Name | Attack |
|---|---|---|
| 0x1B | ESC | Injection of CSI/OSC/DCS sequences; bracketed-paste disable; OSC 52 clipboard writes |
| 0x00 | NUL | Truncates downstream C-string handling |
| 0x08 | BS  | Backspace over user's existing typing |
| 0x7F | DEL | Same class as BS in many shells |

### Invariant 3 — `raw: true` opt-in bypass

When the request JSON contains `"raw": true`, the filter is skipped
and the `text` payload is written to the PTY byte-for-byte. This
preserves the prior Kitty-compat behavior for callers that genuinely
need it (terminal test harnesses, escape-sequence driven plugins) —
they just have to ask.

### Invariant 4 — response reports whether filtering happened

The `send-text` response envelope SHOULD include a `stripped`
counter (bytes removed) when the filter is active and non-zero, so
scripts can detect when their payloads are being altered. Missing
from the response when filter was in pass-through mode or when
nothing was stripped.

## Regression history

- **Introduced:** by design; the initial remote-control implementation
  (0.7.x arc) mirrored Kitty's byte-faithful `send-text`. The absence
  of an auth layer made this a local-UID RCE chain.
- **Discovered:** 2026-04-23 via `/indie-review`. Two independent
  reviewers (IPC subsystem + threat-model cross-cut) flagged the
  same issue.
- **Fixed:** 0.7.12 — opt-in default + filter. X25519 auth layer
  (ROADMAP 💭) deferred to 0.8.0.
