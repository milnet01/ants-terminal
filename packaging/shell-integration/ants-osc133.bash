# Ants Terminal — OSC 133 HMAC-signed shell integration (bash)
#
# Source this file from ~/.bashrc (typically the last line):
#
#     [ -f /usr/share/ants-terminal/shell-integration/ants-osc133.bash ] && \
#         source /usr/share/ants-terminal/shell-integration/ants-osc133.bash
#
# Then export a per-user secret in your login shell config (e.g. ~/.profile):
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
# Without this hook installed, Ants's command-block UI still works — but
# any process inside the terminal can spoof OSC 133 markers and pollute it.
# The HMAC verifier (terminalgrid.cpp) drops unsigned markers when
# ANTS_OSC133_KEY is set in the terminal's own environment.

# Bail early if the verifier wouldn't run anyway.
[ -z "${ANTS_OSC133_KEY:-}" ] && return 0
[ "${TERM_PROGRAM:-}" != "ants-terminal" ] && return 0
command -v openssl >/dev/null 2>&1 || return 0

# Per-prompt id. Bumped at each PS1 firing so a captured HMAC for prompt N
# can't be replayed against prompt N+1 (different message → different HMAC).
__ants_osc133_promptid=""

# HMAC helper. Args: <marker> [<exit-code-for-D>]
# Echoes the canonical message's hex HMAC-SHA256.
__ants_osc133_hmac() {
    local marker="$1"
    local extra="$2"
    local msg
    if [ "$marker" = "D" ] && [ -n "$extra" ]; then
        msg="${marker}|${__ants_osc133_promptid}|${extra}"
    else
        msg="${marker}|${__ants_osc133_promptid}"
    fi
    # `openssl dgst -sha256 -hmac` accepts the key as a CLI arg. The key is
    # already in the env; we never echo it. -binary | xxd would also work
    # but xxd isn't always installed; openssl can hex-encode itself with
    # the default text output ("(stdin)= <hex>").
    printf '%s' "$msg" \
        | openssl dgst -sha256 -hmac "$ANTS_OSC133_KEY" -hex 2>/dev/null \
        | awk '{print $NF}'
}

# Emit OSC 133 ; <marker> [ ; <extra> ] ; aid=<id> ; ahmac=<hex>  ST(=BEL)
__ants_osc133_emit() {
    local marker="$1"
    local extra="$2"
    local hmac
    hmac="$(__ants_osc133_hmac "$marker" "$extra")"
    [ -z "$hmac" ] && return 0
    if [ "$marker" = "D" ] && [ -n "$extra" ]; then
        printf '\033]133;%s;%s;aid=%s;ahmac=%s\007' \
            "$marker" "$extra" "$__ants_osc133_promptid" "$hmac"
    else
        printf '\033]133;%s;aid=%s;ahmac=%s\007' \
            "$marker" "$__ants_osc133_promptid" "$hmac"
    fi
}

# Generate a new prompt id. /proc/sys/kernel/random/uuid is fast and
# universally available on Linux. Fallback to $RANDOM × 4 (60-bit entropy)
# on any platform where it isn't readable.
__ants_osc133_newid() {
    if [ -r /proc/sys/kernel/random/uuid ]; then
        __ants_osc133_promptid="$(cat /proc/sys/kernel/random/uuid)"
    else
        __ants_osc133_promptid="${RANDOM}${RANDOM}${RANDOM}${RANDOM}"
    fi
}

# precmd: fires before PS1 renders. Capture last-command's exit code, emit
# D (closing the previous block), then A (opening the next one).
__ants_osc133_precmd() {
    local last_exit=$?
    if [ -n "$__ants_osc133_promptid" ]; then
        __ants_osc133_emit "D" "$last_exit"
    fi
    __ants_osc133_newid
    __ants_osc133_emit "A"
    return $last_exit
}

# Bash uses PROMPT_COMMAND for the precmd analogue. We chain in front so we
# don't clobber the user's existing PROMPT_COMMAND. PS1 is decorated with
# the B and C markers — B closes the prompt, C opens the command-output
# region (since bash has no preexec, B and C effectively coincide).
case "$PROMPT_COMMAND" in
    *__ants_osc133_precmd*) ;;
    *) PROMPT_COMMAND="__ants_osc133_precmd${PROMPT_COMMAND:+; $PROMPT_COMMAND}" ;;
esac

# Inject B (end-of-prompt) before the cursor and C (start-of-output) right
# after Enter is read. PS1 holds B; PS0 (bash 4.4+) holds C.
__ants_osc133_b='\[$(__ants_osc133_emit B)\]'
__ants_osc133_c='$(__ants_osc133_emit C)'
case "$PS1" in
    *__ants_osc133_emit*) ;;
    *) PS1="${PS1}${__ants_osc133_b}" ;;
esac
case "$PS0" in
    *__ants_osc133_emit*) ;;
    *) PS0="${PS0}${__ants_osc133_c}" ;;
esac
