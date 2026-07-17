# ANTS-2049 — e2e harness driver (sourced by smoke.sh, not executed directly).
#
# launch_e2e / call_e2e / teardown_e2e drive a throwaway `--e2e` Ants instance
# over its own RemoteControl socket, with an isolated profile (all three XDG
# dirs) so it never touches the user's socket, config, session, or caches
# (INV-3). Teardown reaps every spawned instance + temp dir via an EXIT trap
# (INV-6). NO `cmake --build` anywhere — the harness runs an already-built
# binary (INV-7).
#
# The binary defaults to build-fast/ants-terminal (INV-7 — never build/, so a
# harness run can't relink the tree the user's instance was built from);
# override with ANTS_E2E_BIN (the ctest wiring points it at the built target).

ANTS_E2E_BIN="${ANTS_E2E_BIN:-build-fast/ants-terminal}"

# Per-tag state: a tag names one isolated instance (default "main"); a second
# tag (e.g. "gate") gives case 5 its own socket/profile.
declare -A _E2E_SOCK _E2E_DIR _E2E_ART _E2E_PID
_E2E_TAGS=()

# launch_e2e [tag] [--no-e2e] [--no-artifact-dir]
#   Spawns an isolated instance and blocks until its socket answers tab-list
#   (or ~10 s). --no-e2e omits the flag (case 5 gate test); --no-artifact-dir
#   omits ANTS_E2E_ARTIFACT_DIR (case 7 grab bad_args test).
launch_e2e() {
    local tag="main" e2eflag="--e2e" set_art=1 a
    for a in "$@"; do
        case "$a" in
            --no-e2e)          e2eflag="" ;;
            --no-artifact-dir) set_art=0 ;;
            --*)               echo "launch_e2e: unknown flag $a" >&2; return 2 ;;
            *)                 tag="$a" ;;
        esac
    done

    local d; d="$(mktemp -d "${TMPDIR:-/tmp}/ants-e2e-${tag}.XXXXXX")" || return 1
    mkdir -p "$d/cfg/ants-terminal" "$d/data" "$d/cache" "$d/art"
    # Open the RemoteControl socket via config (remote_control_enabled defaults
    # false, so a throwaway profile's socket would never open). This is what
    # lets the gate case (--no-e2e) still be *reachable* to prove the inject
    # gate is --e2e, not mere socket-open: config opens the socket, only --e2e
    # sets m_e2eMode.
    printf '{"remote_control_enabled": true}\n' \
        > "$d/cfg/ants-terminal/config.json"
    chmod 600 "$d/cfg/ants-terminal/config.json"
    _E2E_SOCK[$tag]="$d/e2e.sock"
    _E2E_DIR[$tag]="$d"
    _E2E_ART[$tag]="$d/art"
    _E2E_TAGS+=("$tag")

    local art_env=()
    [[ "$set_art" == 1 ]] && art_env=(ANTS_E2E_ARTIFACT_DIR="$d/art")

    env ANTS_REMOTE_SOCKET="$d/e2e.sock" \
        XDG_CONFIG_HOME="$d/cfg" \
        XDG_DATA_HOME="$d/data" \
        XDG_CACHE_HOME="$d/cache" \
        "${art_env[@]}" \
        "$ANTS_E2E_BIN" $e2eflag >/dev/null 2>&1 &
    _E2E_PID[$tag]=$!

    # Readiness: the socket file existing is not enough — the server must
    # accept and answer. Poll tab-list (socket-dispatchable) until ok or ~10 s.
    local i
    for i in $(seq 1 40); do
        if call_e2e '{"cmd":"tab-list"}' "$tag" 2>/dev/null | grep -q '"ok":true'; then
            return 0
        fi
        kill -0 "${_E2E_PID[$tag]}" 2>/dev/null \
            || { echo "launch_e2e[$tag]: instance exited before ready" >&2; return 1; }
        sleep 0.25
    done
    echo "launch_e2e[$tag]: socket not ready after ~10s" >&2
    return 1
}

# call_e2e '<json>' [tag=main]
#   Sends a raw JSON verb to the tag's socket, echoes the reply. Exit code is
#   runClient's: 0 ok / 2 verb refusal / 1 transport. A refusal is expected
#   (the caller parses ok/code), so this does not treat exit 2 as fatal.
call_e2e() {
    local json="$1" tag="${2:-main}"
    ANTS_REMOTE_SOCKET="${_E2E_SOCK[$tag]}" "$ANTS_E2E_BIN" --remote-json "$json"
}

# e2e_art [tag=main] — echo the tag's artifact dir (for building grab paths).
e2e_art() { echo "${_E2E_ART[${1:-main}]}"; }

teardown_e2e() {
    (( ${#_E2E_TAGS[@]} == 0 )) && return 0
    local tag
    for tag in "${_E2E_TAGS[@]}"; do
        [[ -n "${_E2E_PID[$tag]:-}" ]] && kill "${_E2E_PID[$tag]}" 2>/dev/null
    done
    for tag in "${_E2E_TAGS[@]}"; do
        [[ -n "${_E2E_PID[$tag]:-}" ]] && wait "${_E2E_PID[$tag]}" 2>/dev/null
        [[ -n "${_E2E_DIR[$tag]:-}"  ]] && rm -rf "${_E2E_DIR[$tag]}"
    done
    _E2E_TAGS=()
}
trap teardown_e2e EXIT
