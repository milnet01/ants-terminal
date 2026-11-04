# Feature test — warn Claude about instruction-like terminal text (ANTS-5184)

Terminal output can carry text written to steer an AI agent: a web page,
a README or a log line saying "ignore previous instructions". When
Claude reads that text through an MCP verb, nothing tells it the text
might be an attempt to give it orders.

The terminal-reading verbs now scan the text they read and add a warning
naming what they found. **It is a signal, not a block:** the text is
sent unchanged, and an ordinary sentence can trip it.

## Invariants

- **INV-1** — `InjectionHint::scan(text)` returns the sorted, distinct
  kinds of instruction-like phrase in `text`, matched case-insensitively:
  - `ignore_instructions` — "ignore / disregard / forget / override",
    optionally "all / any / the / your", then "previous / prior / above /
    earlier / preceding / system", then "instructions / prompt(s) / rules /
    directions / messages / context".
  - `new_instructions` — "new / updated / real instructions:", or
    "you are now a / an".
  - `chat_template` — a chat-template marker: `<|im_start|>`,
    `<|im_end|>`, `[INST]`, `[/INST]`, `<<SYS>>`, `<system>`,
    `</system>`.
  - `conceal_from_user` — "do not / don't / never" then "tell / inform /
    show" then "the user".

  Text with none of these returns an empty list.
- **INV-2** — `RemoteControl::redactForClaude` returns those kinds in
  `ClaudeText::injection`, scanned from the text it was given, whether or
  not secret redaction is switched on.
- **INV-3** — `get_text`, `recent_errors`, `last_selection`,
  `get_last_command` and `get_scrollback`'s `since_cursor` envelope carry
  `possible_prompt_injection: [<kind>, …]` when the list is not empty.
  `get_scrollback`'s plain-text reply starts with a
  `<possible prompt injection: <kinds>>` line instead.

## Reload

The phrase list is code in `src/injectionhint.h`. These verbs need a
tab's state, so only the terminal serves them: a changed list reaches a
running terminal on its next relaunch. There is no switch to turn the
warning off; it adds a few bytes to a reply only when a phrase matches.

## What this does not cover

- A phrase worded differently from the list is not flagged.
- The scan reads the text before the byte cap trims it, so a phrase in
  the trimmed head can raise the warning on a reply that no longer holds
  it.
