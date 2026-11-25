# Feature: the initialize reply names the tools to load in one call (ANTS-5736)

A session spends one ToolSearch call per batch of deferred Ants verbs it
loads. ants-mcpd runs in the session's folder, so on each `initialize` it
reads that project's newest Claude Code transcripts, counts the Ants tools
each one loaded with a ToolSearch `select:` query, and sends the most used
as one ready-made load line in the reply's `instructions`.

Code: `src/mcploadhint.{h,cpp}`; wired in `src/mcpdmain.cpp` through
`ClaudeIntegration::setInstructionsProvider()`.

## Invariants

- **INV-1 — the transcript folder.** `transcriptDirFor()` turns every
  character of the project path that is not a letter or digit into `-`,
  under `$CLAUDE_CONFIG_DIR/projects/`, or `~/.claude/projects/` when the
  variable is unset. *Test:* `TranscriptDirFollowsClaudeCodesSlug`.
- **INV-2 — what counts.** A tool counts once per transcript that loaded
  it, however often. Only a name ending in `__<verb>` for a verb the server
  registers counts, so a retired verb and another server's tool are left
  out. Only the newest `maxFiles` transcripts are read. Ties rank by name.
  At most `maxNames` are returned. *Test:* `RanksToolsBySessionsThatLoadedThem`.
- **INV-3 — the reply.** With a provider returning text, `initialize`
  carries it as `instructions`; with none, or empty text, the key is
  absent. *Test:* `InitializeCarriesTheHintOnlyWhenThereIsOne`.

## Would break this

- Counting every `select:` occurrence rather than once per transcript:
  Claude Code writes each tool call more than once, so one session would
  outrank several.
- Setting a provider in the terminal's own MCP server: its folder is the
  terminal's, not the session's.
