# ANTS-1117 — Claude Code → running-Ants GUI IPC (v1) — review loop log

Record for [`../specs/ANTS-1117-claude-code-running-ants-gui.md`](../specs/ANTS-1117-claude-code-running-ants-gui.md) § Cold-eyes review pass 1 (2026-04-30).

## Cold-eyes loop log

Findings folded back in:

- 17-1 Resolved: removed unverified `kitten @` claim; wire-path now
  defers to "whatever remotecontrol.{h,cpp} exposes" with
  implementation-time citation.
- 17-2 Resolved: INV-7 split into server-side refusal (config-off,
  listener not bound) vs. transport-layer error (out of contract).
- 17-3 Resolved: unified `{ok, data}` / `{ok, error, code}` shape
  documented; INVs reference it.
- 17-4 Resolved: INV-2 cites `roadmap-format.md` § 3.5.1 (stable
  IDs) and INV-3 cites § 3.3 (status emojis) directly. (Earlier
  draft mis-cited § 3.1 / § 1.2; corrected during the full-doc
  cross-check pass.)
- 17-5 Resolved: new INV-10 (rate-limit / cache) + INV-11 (perf
  ceiling) added.
- 17-6 Resolved: open question 2 rewritten — the surface is
  determined by what's in tree, not picked from menu.
