# shellcheck shell=bash
# shellcheck disable=SC2034  # ANTS_NUDGE_TOOL is a sourced-lib output read cross-file by ants-bash-veto.sh (${ANTS_NUDGE_TOOL:-…}); shellcheck can't see the consumer (ANTS-2145).
# ants-terminal hook pack — shared helpers (sourced, not executed).
# See docs/specs/ANTS-1252.md.
#
# Two surfaces:
#   ants_in_project_or_exit    — INV-9 per-project gate (sentinel-only).
#   ants_validate_session_id   — INV-2 sessionId regex validator.

# ANTS-1252-INV-9 — per-project gate. Walk up from $PWD looking for
# `.ants-project`, bail at $HOME (or `/`). NO `git rev-parse` fork:
# stat-only ascent stays under 1 ms typically. Reject sane-toplevel
# roots belt-and-braces (`/`, `/home`, `/root`, `/tmp`, `$HOME`).
ants_in_project_or_exit() {
    local dir="${PWD}"
    local home="${HOME:-/nonexistent}"
    local hops=0
    while [ -n "$dir" ] && [ "$dir" != "/" ] && [ "$dir" != "$home" ]; do
        case "$dir" in
            /|/home|/root|/tmp) exit 0 ;;
        esac
        if [ -e "$dir/.ants-project" ]; then
            ANTS_PROJECT_ROOT="$dir"
            export ANTS_PROJECT_ROOT
            return 0
        fi
        # Cap ascent; prevents pathological symlink loops.
        hops=$((hops + 1))
        if [ "$hops" -gt 32 ]; then exit 0; fi
        dir="$(dirname "$dir")"
    done
    exit 0
}

# ANTS-1252-INV-2 — sessionId validator. Reject anything outside
# `^[a-zA-Z0-9_-]{1,64}$` BEFORE any filesystem interpolation.
# Caller passes "$1" (the candidate id); we echo it back on success
# or `exit 0` silently on rejection so PreCompact / drift-check can't
# be coerced into path traversal via a hostile transcript.
ants_validate_session_id() {
    local sid="$1"
    case "$sid" in
        "" ) exit 0 ;;
    esac
    # POSIX-ish bracket pattern; bash glob is good enough here. Length
    # bound enforced by the second test.
    if ! printf '%s' "$sid" | LC_ALL=C grep -Eq '^[a-zA-Z0-9_-]{1,64}$'; then
        exit 0
    fi
    printf '%s' "$sid"
}

# ANTS-2141 — soft-warn search-routing helpers. All filesystem ops are
# best-effort / guarded so the veto stays FAIL-OPEN: a broken cache must never
# silence the nudge or change the exit code (the veto runs `set -u` only).
# See docs/specs/ANTS-2141.md §2.2-§2.5.

# Cache dir for the throttle stamp + tally. Derives from $HOME so tests
# redirect everything via HOME=$tmp.
ants_grep_nudge_dir() {
    printf '%s' "${HOME:-/tmp}/.cache/ants-terminal/grep-nudge"
}

# ANTS-2141-INV-6 — pure-ish predicate: return 0 = warn-eligible raw source
# search, 1 = not. MUST `return`, never `exit`, so the harness can call it
# in-process. On a match it also sets ANTS_NUDGE_TOOL (the §2.4 tally bucket)
# as a side output. Operates on the jq-decoded command string, never raw JSON
# (ANTS-1252 INV-7). See §2.2.
ants_is_source_search() {
    local cmd="$1"
    ANTS_NUDGE_TOOL=""
    # Bypass override (ANTS-1252 INV-12): scan the WHOLE command.
    case "$cmd" in *"# ants-bypass"*) return 1 ;; esac

    # pre = everything before the first pipe (excludes "X | grep").
    local pre="${cmd%%|*}"
    pre="${pre#"${pre%%[![:space:]]*}"}"                 # ltrim
    # Strip leading VAR=val assignments (repeat).
    while [[ "$pre" =~ ^[A-Za-z_][A-Za-z0-9_]*=[^[:space:]]*[[:space:]]+ ]]; do
        pre="${pre#*[[:space:]]}"
        pre="${pre#"${pre%%[![:space:]]*}"}"
    done
    # Strip a single leading "cd ... &&".
    if [[ "$pre" =~ ^cd[[:space:]] ]] && [[ "$pre" == *"&&"* ]]; then
        pre="${pre#*&&}"
        pre="${pre#"${pre%%[![:space:]]*}"}"
    fi

    # EXEMPT (substring tests on pre): help/version flags.
    case " $pre " in
        *" --help"*|*" --version"*|*" -h "*|*" -V "*) return 1 ;;
    esac
    # EXEMPT: non-source-root path substrings + .log.
    case "$pre" in
        */tmp*|*/var*|*/etc*|*/usr*|*/proc*|*/sys*|*build/*|*build-*|*.git/*|*node_modules*|*.log*)
            return 1 ;;
    esac

    # Eligibility — inspect the leading command token(s).
    local first second
    read -r first second _ <<<"$pre"
    case "$first" in
        rg)            ANTS_NUDGE_TOOL=rg;       return 0 ;;
        ag)            ANTS_NUDGE_TOOL=ag;       return 0 ;;
        ack|ack-grep)  ANTS_NUDGE_TOOL=ack;      return 0 ;;
        grep|egrep|fgrep)
            case " $pre " in
                *" -r"*|*" -R"*|*" --recursive"*) ANTS_NUDGE_TOOL="grep"; return 0 ;;
            esac
            ;;
        git)
            if [ "$second" = "grep" ]; then ANTS_NUDGE_TOOL=git-grep; return 0; fi
            ;;
        find)
            # First path operand must be a project SOURCE location.
            local op="${second#./}"
            case "$op" in
                ""|.|src|src/*|tests|tests/*|include|include/*) : ;;
                *) return 1 ;;
            esac
            # Need a name/path primary, and no mutating/exec primary.
            case " $pre " in
                *" -name "*|*" -iname "*|*" -path "*|*" -ipath "*) : ;;
                *) return 1 ;;
            esac
            # Mutating/exec primary → not a search we route (spec §2.2). The
            # shorter globs subsume the longer ones (" -execdir"/" -okdir"/
            # " -fprintf" each begin with " -exec"/" -ok"/" -fprint"), so listing
            # both would be shellcheck SC2221/SC2222 dead patterns (ANTS-2143).
            case " $pre " in
                *" -delete"*|*" -exec"*|*" -ok"*|*" -fprint"*|*" -fls"*)
                    return 1 ;;
            esac
            ANTS_NUDGE_TOOL="find"
            return 0
            ;;
    esac
    return 1
}

# ANTS-2023-INV-5 — pure-ish predicate: return 0 = a code-file read-dump worth
# nudging toward file_outline/read_region, 1 = not. MUST `return`, never `exit`
# (in-process testable, like ants_is_source_search). On a match it sets
# ANTS_NUDGE_TOOL (the §2.3 tally bucket) to the matched leading token. Operates
# on the jq-decoded command string, never raw JSON (ANTS-1252 INV-7). The two
# predicates' leading-token sets ({cat,head,tail,bat} vs the search tools) are
# disjoint, so no command matches both. See docs/specs/ANTS-2023.md §2.2.
ants_is_source_read() {
    local cmd="$1"
    ANTS_NUDGE_TOOL=""
    # Bypass override (ANTS-1252 INV-12): scan the WHOLE command.
    case "$cmd" in *"# ants-bypass"*) return 1 ;; esac

    # A redirect / heredoc / PIPE anywhere means this is not a pure read-dump
    # (writing, feeding input, or processing — not dumping a file to be read).
    # Scanned $cmd-wide — only ever narrows to no-warn (ANTS-2023 §2.2). Unlike
    # the grep class (where a leading grep with a trailing pipe still warns), a
    # piped cat is a processing pipeline, so any '|' disqualifies.
    case "$cmd" in *">"*|*"<<"*|*"|"*) return 1 ;; esac

    # Strip leading VAR=val / cd … && prefixes (no pipe survives the check above).
    local pre="${cmd%%|*}"
    pre="${pre#"${pre%%[![:space:]]*}"}"                 # ltrim
    # Strip leading VAR=val assignments (repeat).
    while [[ "$pre" =~ ^[A-Za-z_][A-Za-z0-9_]*=[^[:space:]]*[[:space:]]+ ]]; do
        pre="${pre#*[[:space:]]}"
        pre="${pre#"${pre%%[![:space:]]*}"}"
    done
    # Strip a single leading "cd ... &&".
    if [[ "$pre" =~ ^cd[[:space:]] ]] && [[ "$pre" == *"&&"* ]]; then
        pre="${pre#*&&}"
        pre="${pre#"${pre%%[![:space:]]*}"}"
    fi

    # EXEMPT (substring tests on pre): help/version flags.
    case " $pre " in
        *" --help"*|*" --version"*|*" -h "*|*" -V "*) return 1 ;;
    esac
    # EXEMPT: non-source-root path substrings + .log (mirrors ants_is_source_search).
    case "$pre" in
        */tmp*|*/var*|*/etc*|*/usr*|*/proc*|*/sys*|*build/*|*build-*|*.git/*|*node_modules*|*.log*)
            return 1 ;;
    esac

    # Split pre into words WITHOUT glob expansion (read -a splits on IFS only;
    # `for tok in $pre` would expand a literal *.cpp). Element 0 is the command.
    local parts
    read -r -a parts <<<"$pre"
    case "${parts[0]:-}" in
        cat|head|tail|bat) : ;;
        *) return 1 ;;
    esac

    # The first operand not beginning with '-' must be a code source file:
    # under src/|tests/|include/ (with or without ./) OR a code extension.
    # New logic vs ants_is_source_search (which only reads first/second). A
    # separated option-argument (e.g. `-n 50`) is mis-read as the operand — an
    # accepted blind spot (ANTS-2023 §2.2 (d)).
    local i op=""
    for ((i = 1; i < ${#parts[@]}; i++)); do
        case "${parts[i]}" in
            -*) continue ;;          # skip flags
            *) op="${parts[i]}"; break ;;
        esac
    done
    [ -z "$op" ] && return 1
    op="${op#./}"
    case "$op" in
        src/*|tests/*|include/*|*.cpp|*.cc|*.cxx|*.c|*.h|*.hpp|*.hh|*.lua)
            ANTS_NUDGE_TOOL="${parts[0]}"
            return 0 ;;
    esac
    return 1
}

# ANTS-2141-INV-8 — best-effort tally append + cap (≤500 lines, kept-last-250).
# Never fails the caller. <warned> is the literal "true"/"false". See §2.4.
ants_grep_nudge_record() {
    local tool="$1" warned="$2"
    local dir; dir="$(ants_grep_nudge_dir)"
    local f="$dir/count.jsonl"
    mkdir -p "$dir" 2>/dev/null || return 0
    local n=0
    [ -f "$f" ] && n="$(wc -l <"$f" 2>/dev/null || echo 0)"
    if [ "${n:-0}" -ge 500 ] 2>/dev/null; then
        if tail -n 250 "$f" 2>/dev/null >"$f.tmp"; then
            mv -f "$f.tmp" "$f" 2>/dev/null || rm -f "$f.tmp" 2>/dev/null
        else
            rm -f "$f.tmp" 2>/dev/null
        fi
    fi
    local ts
    ts="$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo '')"
    printf '{"ts":"%s","tool":"%s","warned":%s}\n' "$ts" "$tool" "$warned" >>"$f" 2>/dev/null || true
    return 0
}

# ANTS-2141-INV-7/INV-9 — throttle. return 0 = a warn fired within the window
# (=> SUPPRESS this one); 1 = expired / I/O error / PPID missing (=> EMIT, and
# stamp). FAIL-OPEN: any failure returns 1. See §2.3.
ants_grep_nudge_throttled() {
    local window="${ANTS_GREP_NUDGE_THROTTLE_SEC:-600}"
    case "$window" in ''|*[!0-9]*) window=600 ;; esac
    # Throttle key: $PPID by default (one stamp per Claude process), overridable
    # via ANTS_GREP_NUDGE_KEY (the seam for a future session-id key — §2.3 /
    # ANTS-2141 §6 — and for deterministic tests). Sanitised to a safe filename.
    local key="${ANTS_GREP_NUDGE_KEY:-${PPID:-}}"
    key="${key//[^a-zA-Z0-9_-]/}"
    case "$key" in ''|0) return 1 ;; esac
    local dir; dir="$(ants_grep_nudge_dir)"
    local stamp="$dir/$key.stamp"
    mkdir -p "$dir" 2>/dev/null || return 1
    if [ -f "$stamp" ]; then
        local mtime now
        mtime="$(stat -c %Y "$stamp" 2>/dev/null || echo '')"
        now="$(date -u +%s 2>/dev/null || echo '')"
        if [ -n "$mtime" ] && [ -n "$now" ]; then
            if [ "$(( now - mtime ))" -lt "$window" ] 2>/dev/null; then
                return 0
            fi
        fi
    fi
    touch "$stamp" 2>/dev/null || true
    return 1
}
