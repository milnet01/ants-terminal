# Ants Terminal — OSC 133 HMAC-signed shell integration (bash)
#
# Source this file from ~/.bashrc (typically the last line):
#
#     [ -f /usr/share/ants-terminal/shell-integration/ants-osc133.bash ] && \
#         source /usr/share/ants-terminal/shell-integration/ants-osc133.bash
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
# Without this hook installed, Ants's command-block UI still works — but
# any process inside the terminal can spoof OSC 133 markers and pollute it.
# The HMAC verifier (terminalgrid.cpp) drops unsigned markers when the
# terminal has a key.

# ANTS-6441 — the key: the key file's first line, else $ANTS_OSC133_KEY. The
# terminal applies the same rule (src/osc133keyfile.h); change them together.
# Neither variable stays exported, so no program this shell runs inherits it.
__ants_osc133_keyfile="${XDG_CONFIG_HOME:-${HOME:-}/.config}/ants-terminal/osc133.key"
__ants_osc133_key=""
[ -r "$__ants_osc133_keyfile" ] && { read -r __ants_osc133_key < "$__ants_osc133_keyfile" || true; }
[ -z "$__ants_osc133_key" ] && __ants_osc133_key="${ANTS_OSC133_KEY:-}"
export -n __ants_osc133_key ANTS_OSC133_KEY
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

# Per-prompt id. Bumped at each PS1 firing so a captured HMAC for prompt N
# can't be replayed against prompt N+1 (different message → different HMAC).
# Audit TL-28 — kept across a re-source, or the open block's D is skipped.
__ants_osc133_promptid="${__ants_osc133_promptid:-}"

__ants_osc133_py='import hmac, hashlib, os, sys
print(hmac.new(sys.stdin.buffer.read(), os.fsencode(sys.argv[1]),
               hashlib.sha256).hexdigest())'

# HMAC helper. Args: <marker> [<exit-code-for-D>]
# Echoes the canonical message's hex HMAC-SHA256.
__ants_osc133_hmac() {
    local marker="$1"
    local extra="${2:-}"
    local msg
    if [ "$marker" = "D" ] && [ -n "$extra" ]; then
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

# Emit OSC 133 ; <marker> [ ; <extra> ] ; aid=<id> ; ahmac=<hex>  ST(=BEL)
__ants_osc133_emit() {
    local marker="$1"
    local extra="${2:-}"
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

# Audit TL-27 — every expansion of a variable that may be unset uses a default,
# so a shell running under `set -u` sources this without errors.
# Bash uses PROMPT_COMMAND for the precmd analogue. We chain in front so we
# don't clobber the user's existing PROMPT_COMMAND. PS1 is decorated with
# the B and C markers — B closes the prompt, C opens the command-output
# region (since bash has no preexec, B and C effectively coincide).
case "${PROMPT_COMMAND:-}" in
    *__ants_osc133_precmd*) ;;
    *) PROMPT_COMMAND="__ants_osc133_precmd${PROMPT_COMMAND:+; $PROMPT_COMMAND}" ;;
esac

# Inject B (end-of-prompt) before the cursor and C (start-of-output) right
# after Enter is read. PS1 holds B; PS0 (bash 4.4+) holds C.
__ants_osc133_b='\[$(__ants_osc133_emit B)\]'
__ants_osc133_c='$(__ants_osc133_emit C)'
case "${PS1:-}" in
    *__ants_osc133_emit*) ;;
    *) PS1="${PS1:-}${__ants_osc133_b}" ;;
esac
case "${PS0:-}" in
    *__ants_osc133_emit*) ;;
    *) PS0="${PS0:-}${__ants_osc133_c}" ;;
esac
