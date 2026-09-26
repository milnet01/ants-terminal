# fish completion for ants-terminal(1).
#
# Source of truth: this file. Installed by CMake to
#   ${CMAKE_INSTALL_DATAROOTDIR}/fish/vendor_completions.d/ants-terminal.fish
# which fish auto-sources for system-wide installs (XDG fish completion path).
#
# CLI surface mirrored here is the same surface enumerated in the manpage and
# parsed in src/main.cpp via QCommandLineParser. Keep the three in sync.

complete -c ants-terminal -s h -l help     -d 'Display a short usage summary and exit'
complete -c ants-terminal -s v -l version  -d 'Print the program version and exit'
complete -c ants-terminal      -l quake    -d 'Run in Quake / drop-down mode'
complete -c ants-terminal      -l dropdown -d 'Run in Quake / drop-down mode (alias of --quake)'
complete -c ants-terminal      -l new-plugin -r -d 'Scaffold a new Lua plugin and exit' -x
complete -c ants-terminal      -l export-roadmaps -r -a '(__fish_complete_directories)' -d 'Export every roadmap-store project as JSONL and exit'
complete -c ants-terminal      -l e2e      -d 'End-to-end test mode, for the tools/e2e harness'
complete -c ants-terminal      -l remote   -x -d 'Send a remote-control command and exit'
complete -c ants-terminal      -l remote-socket  -r -F -d 'Override the remote-control socket path'
complete -c ants-terminal      -l remote-tab     -x -d 'Target tab index, 0-based'
complete -c ants-terminal      -l remote-text    -x -d 'Text for send-text'
complete -c ants-terminal      -l remote-cwd     -r -a '(__fish_complete_directories)' -d 'Working directory for new-tab or launch'
complete -c ants-terminal      -l remote-command -x -d 'Command for new-tab or launch to run'
complete -c ants-terminal      -l remote-title   -x -d 'Title for set-title'
complete -c ants-terminal      -l remote-lines   -x -d 'Trailing lines for get-text'
complete -c ants-terminal      -l remote-json    -x -d 'Send a raw JSON command object'
