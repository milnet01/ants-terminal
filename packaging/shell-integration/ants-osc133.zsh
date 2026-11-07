# Ants Terminal — OSC 133 HMAC-signed shell integration (zsh)
#
# Source this file from ~/.zshrc:
#
#     [ -f /usr/share/ants-terminal/shell-integration/ants-osc133.zsh ] && \
#         source /usr/share/ants-terminal/shell-integration/ants-osc133.zsh
#
# Then put a per-user secret in the key file, readable only by you
# (the welcome dialog's shell-integration button does this for you):
#
#     mkdir -p ~/.config/ants-terminal
#     (umask 077; openssl rand -hex 32 > ~/.config/ants-terminal/osc133.key)
#
# Generate the key ONCE per machine and keep it secret. Anyone who knows the
# key can forge OSC 133 markers in your Ants Terminal session. The terminal
# reads the file for each new tab, and this hook when it is sourced
# (ANTS-6441). An exported $ANTS_OSC133_KEY still works where there is no
# key file; this hook un-exports it, so programs the shell runs do not
# inherit it. The key stops forged markers in printed output (cat, remote
# output); a program you run as yourself can read the key file.
#
# This hook does nothing (preserves vanilla shell behaviour) when:
#   - there is no key (no key file and no $ANTS_OSC133_KEY), OR
#   - the parent terminal isn't Ants (TERM_PROGRAM != "ants-terminal"), OR
#   - neither ants-osc133-sign nor python3 is on PATH (silently degrades)
#
# Signing goes through ants-osc133-sign, which reads the key from stdin, so
# the key never appears in a process's argv (ANTS-5428). Where the helper is
# not installed (Flatpak, AppImage) it falls back to python3, which also
# reads the key from stdin (ANTS-6445).
#
# zsh has both preexec and precmd, so we get the cleanest separation of
# A (precmd, before PS1), B (preexec, after Enter), C (preexec, just
# before the command runs), D (precmd, on the next prompt).

# ANTS-6441 — the key: the key file's first line, else $ANTS_OSC133_KEY. The
# terminal applies the same rule (src/osc133keyfile.h); change them together.
# Neither variable stays exported, so no program this shell runs inherits it.
__ants_osc133_keyfile="${XDG_CONFIG_HOME:-${HOME:-}/.config}/ants-terminal/osc133.key"
typeset -g __ants_osc133_key=""
[ -r "$__ants_osc133_keyfile" ] && { read -r __ants_osc133_key < "$__ants_osc133_keyfile" || true; }
[ -z "$__ants_osc133_key" ] && __ants_osc133_key="${ANTS_OSC133_KEY:-}"
typeset -g +x __ants_osc133_key
[[ -n ${ANTS_OSC133_KEY+x} ]] && typeset -g +x ANTS_OSC133_KEY
unset __ants_osc133_keyfile

# Bail early if the verifier wouldn't run anyway.
[ -z "$__ants_osc133_key" ] && return 0
[ "${TERM_PROGRAM:-}" != "ants-terminal" ] && return 0
if command -v ants-osc133-sign >/dev/null 2>&1; then
    __ants_osc133_signer=helper
elif command -v python3 >/dev/null 2>&1; then
    __ants_osc133_signer=python
else
    return 0
fi

typeset -g __ants_osc133_py='import hmac, hashlib, os, sys
print(hmac.new(sys.stdin.buffer.read(), os.fsencode(sys.argv[1]),
               hashlib.sha256).hexdigest())'

# Audit TL-28 — kept across a re-source, or the open block's D is skipped.
typeset -g __ants_osc133_promptid="${__ants_osc133_promptid:-}"

__ants_osc133_hmac() {
    local marker="$1"
    local extra="${2:-}"
    local msg
    if [[ "$marker" == "D" && -n "$extra" ]]; then
        msg="${marker}|${__ants_osc133_promptid}|${extra}"
    else
        msg="${marker}|${__ants_osc133_promptid}"
    fi
    if [ "$__ants_osc133_signer" = helper ]; then
        # printf is a builtin: the key reaches no argv.
        printf '%s' "$__ants_osc133_key" | ants-osc133-sign "$msg" 2>/dev/null
        return
    fi
    # Fallback: python3 reads the key from stdin too. -I stops it importing
    # a planted hmac.py from the current directory.
    printf '%s' "$__ants_osc133_key" | python3 -I -c "$__ants_osc133_py" "$msg" 2>/dev/null
}

__ants_osc133_emit() {
    local marker="$1"
    local extra="${2:-}"
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
