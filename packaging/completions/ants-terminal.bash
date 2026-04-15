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

    # --new-plugin takes a freeform NAME argument; no completions to offer.
    case "${prev}" in
        --new-plugin)
            COMPREPLY=()
            return 0
            ;;
    esac

    local opts="-h --help -v --version --quake --dropdown --new-plugin"
    COMPREPLY=( $(compgen -W "${opts}" -- "${cur}") )
    return 0
}

complete -F _ants_terminal ants-terminal
