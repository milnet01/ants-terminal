# Ants Terminal — OSC 133 HMAC shell integration

This directory ships the shell-side hook scripts needed to authenticate
[OSC 133](https://gitlab.freedesktop.org/Per_Bothner/specifications/blob/master/proposals/semantic-prompts.md)
shell-integration markers against the Ants Terminal HMAC verifier added in
0.7.0.

## Threat model

OSC 133 is the de-facto cross-terminal protocol for shell integration:
the shell wraps each prompt in `OSC 133 ; A` (prompt start), `B` (command
start), `C` (output start), `D ; <exit-code>` (command end). Ants's
command-block UI uses these markers to group output, render per-block
status stripes, fold output, jump prompt-to-prompt, and fire the
`command_finished` plugin event.

Because the markers are byte sequences in the terminal stream, **any
process running inside the terminal can emit them** — including untrusted
content (`cat malicious.txt`, a malicious TUI, a curl-to-shell pipeline
that ignores its supposed output format). A rogue process can spoof prompt
regions, exit codes, and the command-finished plugin event, polluting the
UI and potentially tricking the user about which command actually ran.

The HMAC verifier (in `src/terminalgrid.cpp`) closes that gap: when the
terminal is launched with `ANTS_OSC133_KEY` set in its environment, every
OSC 133 marker must carry a matching `ahmac=<hex>` parameter computed as
`HMAC-SHA256(key, "<marker>|<promptId>[|<exitCode>]")`. Markers without a
matching HMAC are dropped (no UI side-effects) and a forgery counter
increments, surfaced as a status-bar warning the first time per 5-second
cooldown window.

The user's actual shell knows the key (via these hook scripts). Untrusted
inner processes don't, so they cannot mint a valid HMAC.

## Setup (bash)

```bash
# 1. Generate a per-machine secret. Run ONCE; keep it secret.
echo "export ANTS_OSC133_KEY=\"$(openssl rand -hex 32)\"" >> ~/.profile

# 2. Source the hook in ~/.bashrc (typically the last line):
echo '[ -f /usr/share/ants-terminal/shell-integration/ants-osc133.bash ] && \
    source /usr/share/ants-terminal/shell-integration/ants-osc133.bash' \
    >> ~/.bashrc

# 3. Re-login (so the new env var is in the session that launches ants-terminal).
```

## Setup (zsh)

```zsh
# 1. Generate the secret. Same key as bash if you use both shells.
echo "export ANTS_OSC133_KEY=\"$(openssl rand -hex 32)\"" >> ~/.zshenv

# 2. Source the hook in ~/.zshrc:
echo '[ -f /usr/share/ants-terminal/shell-integration/ants-osc133.zsh ] && \
    source /usr/share/ants-terminal/shell-integration/ants-osc133.zsh' \
    >> ~/.zshrc
```

## Verifying it works

After re-login + opening a new tab in Ants:

```bash
# This should print nothing visibly — the OSC sequences are consumed by
# the terminal — but the command-block UI should still show grouped output
# and a per-block status stripe.
ls /etc

# Now try to forge a marker. The terminal should drop it AND show a
# warning at the bottom of the window:
printf '\033]133;A\007'
```

If you see `⚠ OSC 133 forgery detected (count: N)` in the status bar, the
verifier is working. If you don't see the warning AND the spoofed marker
silently created a new (empty) prompt block, the verifier isn't active —
likely `ANTS_OSC133_KEY` isn't set in the terminal's environment. Check
with `echo $ANTS_OSC133_KEY` inside an Ants tab.

## Caveats

- **No backward compatibility risk**: the hook is a strict no-op when
  `ANTS_OSC133_KEY` is unset. Without the env var, OSC 133 behaves as
  before (unverified, permissive — the legacy mode).

- **Per-machine key**: the key lives in your `~/.profile` /
  `~/.zshenv`. Same machine + same user = same key everywhere. If you ssh
  to a remote box and want shell integration there too, you need to set
  `ANTS_OSC133_KEY` on the remote *and* in the local terminal — the
  local terminal verifies, but the remote shell needs the key to sign.

- **Replay scope**: the HMAC binds to a per-prompt UUID (regenerated on
  every PROMPT_COMMAND firing), so a captured HMAC for prompt N can't be
  replayed against prompt N+1. Within the same prompt, A/B/C/D each have
  distinct HMACs because the marker letter is part of the message.

- **openssl dependency**: the hook uses `openssl dgst -sha256 -hmac` for
  HMAC computation. If openssl isn't on PATH, the hook silently degrades
  to a no-op. Install via `zypper in openssl` / `apt install openssl` /
  `brew install openssl`.

- **TERM_PROGRAM gate**: the hook only activates when `TERM_PROGRAM` is
  `ants-terminal`. Inside tmux / screen / a different terminal, the hook
  no-ops so it never emits OSC 133 markers a non-Ants terminal might
  not understand.

## Protocol details

Canonical message form (HMAC input, before hex-encoding):

| Marker | Message            |
|--------|--------------------|
| A      | `A\|<promptId>`    |
| B      | `B\|<promptId>`    |
| C      | `C\|<promptId>`    |
| D      | `D\|<promptId>\|<exitCode>` |

Wire form (what the shell emits):

```
ESC ] 133 ; <marker> [ ; <exitCode> ] ; aid=<promptId> ; ahmac=<hex-sha256>  BEL
```

The `<exitCode>` field is positional and only appears for D, matching the
legacy OSC 133 D form (`OSC 133 ; D ; <exit> ST`). `aid=` and `ahmac=`
are appended as key=value params. Order of key=value params is not
significant; unknown keys are ignored for forward compatibility.

Hex case is not significant — the verifier accepts both `abcd...` and
`ABCD...` so shells using `printf %X` work alongside openssl's `-hex`.
