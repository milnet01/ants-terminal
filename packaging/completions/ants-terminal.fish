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
