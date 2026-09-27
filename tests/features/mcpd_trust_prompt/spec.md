# mcpd_trust_prompt — ants-mcpd asks the terminal to prompt for trust

Contract: [`docs/specs/ANTS-5464-mcpd-trust-prompt.md`](../../../docs/specs/ANTS-5464-mcpd-trust-prompt.md)
§ 3. Each invariant there names the case below that checks it.

| Case | Invariant |
|---|---|
| `Inv1TerminalDecidesFromItsOwnRead` | INV-1 |
| `Inv2TerminalWordAloneTrustsNothing` | INV-2 |
| `Inv3GrantRunsTheGatesInTheSameCall` | INV-3 |
| `Inv4NoTerminalFallsBackWithoutWaiting` | INV-4 |
| `Inv5DenialIsAskedOncePerSha` | INV-5 |
| `Inv6TimeoutIsHeadlessAndNotCached` | INV-6 |
| `Inv7MethodIsNotATool` | INV-7 |
| `Inv8McpdAnswersWhileAPromptIsPending` | INV-8 |
| `Inv9TerminalAnswersWhileItsHandlerIsOpen` | INV-9 |

The ants-mcpd cases run the built binary as a child process against
`StubTerminal` (`../standalone_mcp_server/stub_terminal.h`), with
`XDG_CONFIG_HOME` pointed at a fresh directory so the trust file starts
empty. INV-1, INV-6 and INV-9 run in-process.
