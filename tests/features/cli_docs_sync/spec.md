# Feature: the CLI's documentation lists every option

## Invariants

**INV-1 — every long option is documented everywhere.** Each long option
`src/main.cpp` defines — through a `QCommandLineOption` or the
`--export-roadmaps` pre-parse — appears in the man page
(`packaging/linux/ants-terminal.1`) and in the bash, zsh and fish completions
(`packaging/completions/`).

## Rationale

The three completion files each say "keep the three in sync" with the man page
and `main.cpp`, and nothing checked it: `--e2e`, `--export-roadmaps` and the
nine `--remote*` options shipped in none of them (audit 2026-09-26, RC-51).

## Test surface

`test_cli_docs_sync.cpp` reads `main.cpp`, collects the option names, and
checks each one in the four files. The man page escapes `-` as `\-`, so its
text is un-escaped before searching.
