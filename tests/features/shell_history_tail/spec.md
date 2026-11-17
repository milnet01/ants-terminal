# Feature: autocomplete reads shell history off the GUI thread, from the file's tail

ANTS-5685. Autocomplete suggests from the shell's history file. A tab used
to read that whole file, then parse it, on the GUI thread, on its first
output.

## Invariants

**INV-1 — only the tail is read.** `shellhistory::readTail(path, maxBytes)`
returns at most `maxBytes` bytes from the end of the file. When it cuts the
file, it drops the partial line at the cut. A file within the cap comes back
whole. A file that cannot be opened gives an empty result.

**INV-2 — parsing is unchanged.** `shellhistory::parse` returns each command
once, newest first. It strips zsh's extended prefix (`: <time>:0;`), drops a
zsh line with no `;`, and strips fish's `- cmd: `.

**INV-3 — zsh, then bash, then fish.** `shellhistory::load(home)` uses the
first of `.zsh_history`, `.bash_history` and `.local/share/fish/fish_history`
under `home` that yields a command.

**INV-4 — the widget does not read the file itself.** Source-scrape of
`TerminalWidget::loadHistory`: it starts a `QThread::create` worker that
calls `shellhistory::load`, and calls neither `readAll` nor `QFile`.

Test: `test_shell_history_tail.cpp`, suite `ShellHistoryTail`, in the
`test_vt` bundle.
