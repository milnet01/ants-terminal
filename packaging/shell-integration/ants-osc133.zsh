# Ants Terminal — OSC 133 HMAC-signed shell integration (zsh)
#
# Source this file from ~/.zshrc:
#
#     [ -f /usr/share/ants-terminal/shell-integration/ants-osc133.zsh ] && \
#         source /usr/share/ants-terminal/shell-integration/ants-osc133.zsh
#
# Then export a per-user secret in your login shell config (e.g. ~/.zshenv):
#
#     export ANTS_OSC133_KEY="$(openssl rand -hex 32)"
#
# Generate the key ONCE per machine and keep it secret. Anyone who knows the
# key can forge OSC 133 markers in your Ants Terminal session. The key is
# read by the terminal at process start (not per-tab), so set it before
# launching ants-terminal.
#
# This hook does nothing (preserves vanilla shell behaviour) when:
#   - ANTS_OSC133_KEY is unset, OR
#   - the parent terminal isn't Ants (TERM_PROGRAM != "ants-terminal"), OR
#   - openssl isn't on PATH (silently degrades)
#
# zsh has both preexec and precmd, so we get the cleanest separation of
# A (precmd, before PS1), B (preexec, after Enter), C (preexec, just
# before the command runs), D (precmd, on the next prompt).

[ -z "${ANTS_OSC133_KEY:-}" ] && return 0
[ "${TERM_PROGRAM:-}" != "ants-terminal" ] && return 0
command -v openssl >/dev/null 2>&1 || return 0

typeset -g __ants_osc133_promptid=""

__ants_osc133_hmac() {
    local marker="$1"
    local extra="$2"
    local msg
    if [[ "$marker" == "D" && -n "$extra" ]]; then
        msg="${marker}|${__ants_osc133_promptid}|${extra}"
    else
        msg="${marker}|${__ants_osc133_promptid}"
    fi
    print -nr -- "$msg" \
        | openssl dgst -sha256 -hmac "$ANTS_OSC133_KEY" -hex 2>/dev/null \
        | awk '{print $NF}'
}

__ants_osc133_emit() {
    local marker="$1"
    local extra="$2"
    local hmac
    hmac="$(__ants_osc133_hmac "$marker" "$extra")"
    [ -z "$hmac" ] && return 0
    if [[ "$marker" == "D" && -n "$extra" ]]; then
        printf '\033]133;%s;%s;aid=%s;ahmac=%s\007' \
            "$marker" "$extra" "$__ants_osc133_promptid" "$hmac"
    else
        printf '\033]133;%s;aid=%s;ahmac=%s\007' \
            "$marker" "$__ants_osc133_promptid" "$hmac"
    fi
}

__ants_osc133_newid() {
    if [ -r /proc/sys/kernel/random/uuid ]; then
        __ants_osc133_promptid="$(<"/proc/sys/kernel/random/uuid")"
    else
        __ants_osc133_promptid="${RANDOM}${RANDOM}${RANDOM}${RANDOM}"
    fi
}

__ants_osc133_precmd() {
    local last_exit=$?
    if [ -n "$__ants_osc133_promptid" ]; then
        __ants_osc133_emit "D" "$last_exit"
    fi
    __ants_osc133_newid
    __ants_osc133_emit "A"
    return $last_exit
}

__ants_osc133_preexec() {
    __ants_osc133_emit "B"
    __ants_osc133_emit "C"
}

# autoload + add-zsh-hook is the idiomatic way to chain into precmd /
# preexec without clobbering existing user hooks.
autoload -Uz add-zsh-hook
add-zsh-hook precmd  __ants_osc133_precmd
add-zsh-hook preexec __ants_osc133_preexec
