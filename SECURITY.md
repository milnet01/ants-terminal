# Security Policy

We take the security of Ants Terminal seriously. Terminal emulators are
trust boundaries — every byte read from a PTY is potentially
attacker-controllable, and a bug in the parser, the renderer, or the
plugin sandbox can expose the user to remote code execution, data
exfiltration, or privilege escalation. This document describes how to
report vulnerabilities responsibly and what you can expect from us.

## Supported Versions

Security fixes are back-ported to the **latest released minor**. Until
the 1.0 stability pledge lands (see [ROADMAP.md](ROADMAP.md)), the
active `0.x` release line is the only one receiving security updates.

| Version | Supported                |
|---------|--------------------------|
| 0.7.x   | ✅ Yes — current branch  |
| 0.6.x   | ❌ Please upgrade        |
| < 0.6   | ❌ Please upgrade        |

Users on older point releases should upgrade to the latest `0.7.y`
before filing a report — the issue may already be fixed upstream. See
[CHANGELOG.md](CHANGELOG.md) for what shipped in each release.

## Reporting a Vulnerability

**Please do not file security issues as public GitHub issues.**

Use one of:

1. **GitHub Security Advisory** (preferred). Open a private advisory
   at <https://github.com/milnet01/ants-terminal/security/advisories/new>.
   GitHub's advisory flow keeps the report private while we
   investigate, and gives us a structured place to coordinate a fix
   and a CVE request.

2. **Email**. Send a signed, encrypted email to the maintainers. If
   you need a PGP key, request one on the advisory form above and
   we'll respond out-of-band.

In your report, please include:

- A clear description of the vulnerability, including the component
  (VT parser, grid, renderer, plugin sandbox, audit dialog, etc.).
- A reproducer — ideally a minimal sequence of bytes or a plugin
  payload that triggers the issue.
- The version you tested (`ants-terminal --version`) and your OS /
  compositor / shell.
- Your assessment of impact (denial of service, RCE, information
  leak, sandbox escape) and any caveats.
- Whether you plan to publicly disclose and on what timeline. We
  aim to accommodate reasonable timelines but will push back on
  anything under 30 days for a high-impact issue.

## Disclosure Timeline

From the time we acknowledge a report:

| Time     | Action                                                        |
|----------|---------------------------------------------------------------|
| 48 hours | Acknowledgement that the report was received                  |
| 7 days   | Initial triage — confirm reproduction, rate severity          |
| 30 days  | Target for a fix in a released patch version                  |
| 90 days  | Public disclosure (on advisory publish or the reporter's date) |

"Released patch version" means the fix has shipped in a tagged
release and CI is green on both lanes. If we need longer than 30 days
(complex threading / parser / multi-component fixes), we will say so
early and agree a revised timeline with the reporter.

## Severity Rubric

We classify findings roughly as:

- **Critical**: remote code execution, arbitrary file read/write
  outside the plugin sandbox, privilege escalation.
- **High**: denial of service against the host process, plugin
  sandbox escape with no further mitigating factor, silent data
  exfiltration through clipboard / OSC 52 / OSC 8.
- **Medium**: UI spoofing (homograph-class issues), information
  disclosure that requires a non-trivial chain, bypasses of an
  existing defense (the paste-confirmation dialog, the image
  byte-budget, the URI scheme allowlist).
- **Low**: local-only issues, theoretical weaknesses without a
  demonstrated reproducer, documentation gaps in security-relevant
  behavior.

Severity is a judgment call — we're happy to discuss the rating on
the advisory.

## What's In Scope

- The VT parser (`src/vtparser.cpp`) — handling of CSI, OSC, DCS, APC.
- The terminal grid (`src/terminalgrid.cpp`) — alt-screen swap, scrollback
  reflow, image budgeting, OSC 8 / OSC 52 / OSC 133 handling.
- The terminal widget (`src/terminalwidget.cpp`) — input handling,
  paste confirmation, trigger actions, rendering pipeline.
- The plugin sandbox (`src/luaengine.cpp`, `src/pluginmanager.cpp`) —
  permission gating, instruction-count timeout, heap budget, manifest
  validation.
- The audit dialog (`src/auditdialog.cpp`) — subprocess invocation,
  JSONL suppression parsing, SARIF output.
- PTY handling (`src/ptyhandler.cpp`) — file-descriptor lifetime,
  signal delivery, environment sanitization.
- Clipboard / OSC 52 quotas, OSC 8 URI allowlist, homograph warnings,
  image dimension and byte caps, multi-line paste confirmation —
  bypasses of any existing defense are in scope.

## What's Out of Scope

- Issues in upstream dependencies (Qt, Lua, libutil) — report those
  upstream. We'll take bugs in **how we use** the dependencies.
- Local attackers with the ability to write to the user's
  `~/.config/ants-terminal/config.json`, `~/.ssh/`, or the plugin
  directory. Those are trust boundaries under the user's control.
- Social-engineering attacks that depend on the user running an
  untrusted plugin — the capability model is designed to limit blast
  radius, but "I granted everything and ran a hostile plugin" is not
  a vulnerability we can fix at the terminal layer.
- Issues that only manifest when debug builds, sanitizers, or
  the `ANTS_PLUGIN_DEV=1` environment flag are enabled.
- Theoretical vulnerabilities without a reproducer.

## Hardening We Already Do

For context on the existing defense posture:

- **Image-bomb defenses** (0.6.11) — Sixel / Kitty / iTerm2 inline
  image decoders cap total in-memory bytes per terminal at 256 MB and
  per-image dimensions at 4096×4096.
- **URI scheme allowlist** (0.6.14) — both OSC 8 explicit hyperlinks
  and `make_hyperlink` trigger rules are restricted to
  http/https/ftp/file/mailto.
- **Plugin permissions** (0.6.0 manifest v2) — permissions are
  declarative in `manifest.json`, user-granted per-plugin on first
  load, persisted in `config.plugin_grants`, and auditable /
  revocable in Settings → Plugins (0.6.11). Un-granted permissions
  are absent from the plugin's `ants.*` table — no nil stubs.
- **Plugin sandbox** — per-plugin `lua_State`, 10 MB heap budget,
  10M-instruction hook, dangerous globals removed.
- **OSC 52 quotas** (0.6.0) — 32 writes/minute + 1 MB/minute per
  terminal, plus the 1 MB per-string cap.
- **Multi-line paste confirmation** (0.6.0) — dialog for
  `\n` / `sudo ` / `curl … | sh` / control chars, independent of
  bracketed-paste mode.
- **Compiler / linker hardening** — `-fstack-protector-strong`,
  `-fstack-clash-protection`, `-fcf-protection`, `_FORTIFY_SOURCE=3`,
  `_GLIBCXX_ASSERTIONS`, `-Wl,-z,relro,-z,now,-z,noexecstack`, PIE.
- **ASan + UBSan** on every CI push (the `build-asan` job).
- **Audit dialog** — a project-level static-analysis runner that
  enforces the rule pack against every commit via CTest.

## Acknowledgements

We maintain a public list of researchers who have responsibly
disclosed vulnerabilities. If you would prefer to remain anonymous,
say so in your report.

---

*This policy will be revised as the project matures. Last updated for
the 0.7.59 release (supported-versions table refreshed; the substantive
disclosure policy is unchanged from 0.6.16).*
