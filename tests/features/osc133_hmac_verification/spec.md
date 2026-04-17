# Feature: OSC 133 HMAC verification rejects forged shell-integration markers

## Contract

When the `TerminalGrid` is constructed with `$ANTS_OSC133_KEY` set
(or the test backdoor `setOsc133KeyForTest()` populates `m_osc133Key`),
every OSC 133 marker (`A`, `B`, `C`, `D`) MUST carry a matching
`ahmac=<hex>` parameter to take effect.

| Scenario                                          | Effect on `m_promptRegions` | Effect on `osc133ForgeryCount()` |
|---------------------------------------------------|-----------------------------|----------------------------------|
| Verifier OFF, marker has no HMAC                  | applied (legacy)            | unchanged                        |
| Verifier OFF, marker has any HMAC                 | applied (HMAC ignored)      | unchanged                        |
| Verifier ON, marker has no HMAC                   | DROPPED                     | +1                               |
| Verifier ON, marker has correct HMAC              | applied                     | unchanged                        |
| Verifier ON, marker has wrong HMAC                | DROPPED                     | +1                               |
| Verifier ON, marker has uppercase-hex correct HMAC| applied                     | unchanged                        |
| Verifier ON, D marker with wrong exit-code+correct-HMAC-for-different-exit | DROPPED | +1 |

The HMAC message form is canonical:

| Marker | Message form              |
|--------|---------------------------|
| A      | `A\|<promptId>`           |
| B      | `B\|<promptId>`           |
| C      | `C\|<promptId>`           |
| D      | `D\|<promptId>\|<exitCode>` |

## Rationale

OSC 133 is the de-facto cross-terminal protocol for shell integration.
Any process running inside the terminal can emit these markers — the
threat model includes a malicious TUI, content displayed by `cat
malicious.txt`, or anything that writes to stdout. A rogue process can
spoof prompt regions, exit codes, and the `command_finished` plugin
event, polluting the command-block UI and potentially misleading the
user about which command actually completed.

The HMAC verifier is a 0.7.0 ROADMAP item (§0.7.0 → 🔒 Security):

> **Shell-side HMAC verification** for OSC 133 markers — protects the
> command-block UI from forged markers written by an inner TTY process.
> Key passed via `ANTS_OSC133_KEY` env; shell hook computes HMAC over
> `(prompt_id, command, exit_code)`.

The user's actual shell knows `$ANTS_OSC133_KEY` (via the hook scripts
shipped under `packaging/shell-integration/`). Untrusted inner processes
do not, so they cannot mint a valid HMAC.

## Scope

### In scope
- Constant-time HMAC compare (the verifier uses a constant-time hex
  compare; lifting that into a unit test is hard without timing
  oracles, but we do verify the case-insensitive accept of uppercase
  hex which is part of the same comparison code path).
- All four marker letters (A/B/C/D) under verifier OFF and verifier ON.
- Exit-code binding for D markers: a D HMAC computed for exit code 0
  must NOT verify against a D wire form claiming exit code 1.
- promptId binding: an A HMAC computed for prompt 'p1' must NOT verify
  against an A wire form claiming prompt 'p2'.
- The forgery counter increments on every rejected marker (not
  throttled — only the callback firing is).
- Backward compatibility: when `ANTS_OSC133_KEY` is not set, the
  pre-0.7.0 OSC 133 form (`OSC 133 ; A` with no `aid=`/`ahmac=`)
  continues to work unchanged.

### Out of scope
- Throttling of the forgery callback (the 5-second cooldown is timing-
  based and not deterministically testable in a fast feature test).
- Shell-side hook script behaviour (covered by manual
  `packaging/shell-integration/README.md` walkthrough; bash/zsh hook
  semantics depend on the shell, openssl, and the user's PROMPT_COMMAND
  / preexec config).
- The status-bar message rendering (covered by inspection — connecting
  the forgeryDetected signal to showStatusMessage at
  `src/mainwindow.cpp:1297`).

## Test strategy

The test feeds VtParser → TerminalGrid with crafted OSC 133 byte
streams via the public processAction() pipeline (same path the PTY
uses). It exercises three configurations of the verifier in turn,
asserting m_promptRegions size and osc133ForgeryCount() after each
case. HMAC computation in the test mirrors the production form using
QMessageAuthenticationCode (same algorithm, same key, same canonical
message — if these drift, the test catches it).

The test does NOT instantiate TerminalWidget — it uses TerminalGrid +
VtParser only, the same approach as scrollback_redraw and
sgr_attribute_reset. Headless, fast, no Qt GUI dependencies.

A small source-grep tail check asserts the `ANTS_OSC133_KEY` env var is
still the only knob that activates the verifier in production code —
guards against accidental policy drift.
