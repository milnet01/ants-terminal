# Feature: OSC 10/11/12 default color queries

## Contract

Applications use the OSC 10/11/12 **query form** (`OSC <n> ; ? ST`) to
auto-detect the terminal's default foreground, background, and cursor
colors — primarily for dark-vs-light theme detection (delta, neovim,
bat, lazygit, fzf, jj, starship) that can't rely on `COLORFGBG` across
terminals.

For OSC 10/11/12 specifically, the grid MUST:

1. **INV-1** — On `OSC 10 ; ? ST`, emit a response of exactly
   `ESC ] 10 ; rgb:RRRR/GGGG/BBBB ESC \` via the registered
   `ResponseCallback`, where `RRRR/GGGG/BBBB` are the **16-bit-per-channel**
   hex components of the grid's current `m_defaultFg` (each 8-bit
   component `c` is replicated as `c * 0x0101` = `cc` in hex).

2. **INV-2** — `OSC 11 ; ?` behaves analogously, responding with the
   16-bit encoding of `m_defaultBg`.

3. **INV-3** — `OSC 12 ; ?` (cursor color query) returns an rgb-format
   response. We do not track a separate cursor color, so we fall back
   to `m_defaultFg`; the wire shape is unchanged from INV-1 except the
   OSC number is `12`.

4. **INV-4** — The **set form** of OSC 10/11/12 (e.g. `OSC 10 ; #ff0000 ST`
   or `OSC 11 ; rgb:ff/00/00 ST`) MUST NOT trigger any response. The
   grid deliberately ignores set attempts: default fg/bg/cursor are
   theme-driven, and honoring them from untrusted in-terminal processes
   would be a theme-injection vector.

5. **INV-5** — OSC 13 (highlight color) and other OSC color numbers
   outside the 10/11/12 triplet must not take the same query path. No
   response is emitted.

## Rationale

xterm's `operating system controls` page documents OSC 10/11/12 as the
canonical "query/set dynamic colors" mechanism. Modern CLI tools
increasingly rely on it at startup — if our response shape is wrong
(missing ST, 8-bit-per-channel instead of 16, wrong OSC number echoed,
etc.) those tools silently fall back to assuming a dark terminal even
on a light theme, producing unreadable output.

The 16-bit encoding is load-bearing: xterm's spec uses
`rgb:RRRR/GGGG/BBBB`; many parsers accept 8-bit (`rgb:RR/GG/BB`) too
but some (notably `xterm-query` and termenv) reject the 8-bit form.
Emitting the wider form is the conservative choice.

INV-4 is a deliberate security / UX policy: a remote process inside
a PTY (malicious SSH session, compromised container, hostile
`less`-paged content) should not be able to repaint the terminal's
default fg/bg from under the user. OSC 4 (palette entries) has similar
concerns; we apply the same "query-only" policy to 10/11/12.

## Invariants

Given a TerminalGrid with `setDefaultFg(fg)` and `setDefaultBg(bg)`
called with known colors, and a response-capturing lambda wired via
`setResponseCallback`:

- `handleOsc("10;?")` → capture equals `ESC ] 10 ; rgb:HH/HH/HH ESC \`
  where each HH is `fg.<ch>() * 0x0101` printed as `%04x`.
- `handleOsc("11;?")` → capture equals the analogous 11-prefixed form
  using `bg`.
- `handleOsc("12;?")` → capture equals the 12-prefixed form using
  `fg` (cursor color fallback).
- `handleOsc("10;#ff0000")` → capture is empty.
- `handleOsc("10;rgb:ff/00/00")` → capture is empty.
- `handleOsc("13;?")` → capture is empty (only 10/11/12 respond via
  this branch).

## Scope

### In scope
- Query-form response shape (OSC number, `rgb:` prefix, 16-bit
  hex components, ST terminator).
- OSC 12 cursor-color fallback to fg.
- Set-form silent drop (no response) for 10/11/12.
- Non-handled OSC numbers (13, 14, …) producing no response via this
  branch.

### Out of scope
- OSC 4 (palette entry) — separate command, separate handler.
- OSC 104/110/111/112 (reset) — separate command, separate handler.
- The PTY wire-level round-trip — this spec tests the grid's
  callback-facing API, not PTY plumbing.

## Regression history

- **0.7.0** — introduced alongside shell integration expansion. Target
  apps were delta, neovim's `background` auto-detection, and bat.
  This test locks (a) the 16-bit response shape so apps parsing
  strictly don't break, and (b) the "set form drops silently" policy
  so a future "helpfully accept set requests" refactor doesn't open
  a theme-injection channel.
