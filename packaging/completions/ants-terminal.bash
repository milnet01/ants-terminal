# bash-completion for ants-terminal(1).
#
# Source of truth: this file. Installed by CMake to
#   ${CMAKE_INSTALL_DATAROOTDIR}/bash-completion/completions/ants-terminal
# which the bash-completion package sources on demand via its dynamic loader.
#
# CLI surface mirrored here is the same surface enumerated in the manpage and
# parsed in src/main.cpp via QCommandLineParser. Keep the three in sync.

_ants_terminal()
{
    local cur prev
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # Freeform arguments get no completions; paths get files or directories.
    case "${prev}" in
        --new-plugin|--remote|--remote-tab|--remote-text|--remote-command|\
        --remote-title|--remote-lines|--remote-json)
            COMPREPLY=()
            return 0
            ;;
        --export-roadmaps|--remote-cwd)
            COMPREPLY=( $(compgen -d -- "${cur}") )
            return 0
            ;;
        --remote-socket)
            COMPREPLY=( $(compgen -f -- "${cur}") )
            return 0
            ;;
    esac

    local opts="-h --help -v --version --quake --dropdown --new-plugin
        --export-roadmaps --e2e --remote --remote-socket --remote-tab
        --remote-text --remote-cwd --remote-command --remote-title
        --remote-lines --remote-json"
    COMPREPLY=( $(compgen -W "${opts}" -- "${cur}") )
    return 0
}

complete -F _ants_terminal ants-terminal
