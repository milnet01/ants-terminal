#!/usr/bin/env bash
# tools/install-hooks.sh — install the ants-terminal hook pack into
# ~/.claude/settings.json. See docs/specs/ANTS-1252.md § 2.
#
# Hardening (loop 2 cold-eyes):
#  - INV-5: lstat ~/.claude/settings.json; abort if it's a symlink.
#  - INV-6: sentinel-key fence (`ants_hooks_pack_v1`) — NOT text-fence
#    comments (jq strips JSON comments).
#  - INV-8: write tmpfile, validate with `jq empty`, ONLY then rename.
#  - cp --no-dereference for the backup (don't follow malicious link).
#  - Idempotent: re-runs detect sentinel + identical scripts → no-op.
#
# Flags:
#   --dry-run         List planned changes without touching disk.
#   --uninstall       Remove the sentinel key + ants-* hook entries.
#   --target <path>   Override settings.json target (default
#                     ~/.claude/settings.json). Test-only.
#   --hooks-dir <path> Override install dir for hook scripts (default
#                     ~/.claude/hooks/). Test-only.
#   -h | --help       Print usage.

set -euo pipefail

usage() {
    cat <<'EOF'
tools/install-hooks.sh — install ants-terminal Claude Code hook pack.

Usage: tools/install-hooks.sh [--dry-run] [--uninstall]
                              [--target <settings.json>]
                              [--hooks-dir <dir>]
EOF
}

dry_run=0
uninstall=0
target="${HOME}/.claude/settings.json"
hooks_dir="${HOME}/.claude/hooks"
src_dir="$(cd "$(dirname "$0")/.." && pwd)/hooks"

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) dry_run=1; shift ;;
        --uninstall) uninstall=1; shift ;;
        --target) target="$2"; shift 2 ;;
        --hooks-dir) hooks_dir="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown flag: $1" >&2; usage >&2; exit 2 ;;
    esac
done

command -v jq >/dev/null 2>&1 || { echo "jq required" >&2; exit 1; }

# ANTS-1252-INV-5 — lstat target. Symlinks abort hard.
if [ -L "$target" ]; then
    echo "settings.json is a symlink — resolve manually before installing" >&2
    echo "  target: $target" >&2
    exit 1
fi

mkdir -p "$(dirname "$target")"
mkdir -p "$hooks_dir"

# Build the hook entries we want to splice in. Schema mirrors the
# Claude Code hook config docs (event → matcher → command).
hook_entries() {
    cat <<EOF
{
  "ants_hooks_pack_v1": true,
  "hooks": {
    "SessionStart": [
      {"matcher": "*", "hooks": [{"type": "command", "command": "$hooks_dir/ants-session-preamble.sh"}]}
    ],
    "PreToolUse": [
      {"matcher": "Bash", "hooks": [{"type": "command", "command": "$hooks_dir/ants-bash-veto.sh"}]},
      {"matcher": "Read", "hooks": [{"type": "command", "command": "$hooks_dir/ants-read-roadmap-veto.sh"}]}
    ],
    "Stop": [
      {"matcher": "*", "hooks": [{"type": "command", "command": "$hooks_dir/ants-drift-check.sh"}]}
    ],
    "PreCompact": [
      {"matcher": "*", "hooks": [{"type": "command", "command": "$hooks_dir/ants-precompact-snapshot.sh"}]}
    ]
  }
}
EOF
}

# Read current settings (or {}). jq treats absent file as a parse
# error so we synthesise the empty object.
read_settings() {
    if [ -f "$target" ]; then
        jq '.' "$target"
    else
        echo '{}'
    fi
}

splice_install() {
    local current
    current="$(read_settings)"
    # Merge: shallow per top-level key; deep-merge under "hooks" so
    # other extensions can coexist with us. jq's `*` operator does a
    # recursive merge with right-side wins.
    jq --argjson pack "$(hook_entries)" '. * $pack' <<<"$current"
}

splice_uninstall() {
    local current
    current="$(read_settings)"
    jq '
        del(.ants_hooks_pack_v1)
        | if .hooks then
            .hooks |= (
                (.SessionStart? // [] | map(select(.hooks // [] | all(.command // "" | test("/ants-[^/]+\\.sh$") | not))) ) as $ss |
                (.PreToolUse?  // [] | map(select(.hooks // [] | all(.command // "" | test("/ants-[^/]+\\.sh$") | not))) ) as $pre |
                (.Stop?        // [] | map(select(.hooks // [] | all(.command // "" | test("/ants-[^/]+\\.sh$") | not))) ) as $stop |
                (.PreCompact?  // [] | map(select(.hooks // [] | all(.command // "" | test("/ants-[^/]+\\.sh$") | not))) ) as $pc |
                (if ($ss | length) > 0 then .SessionStart = $ss else del(.SessionStart) end)
                | (if ($pre | length) > 0 then .PreToolUse = $pre else del(.PreToolUse) end)
                | (if ($stop | length) > 0 then .Stop = $stop else del(.Stop) end)
                | (if ($pc | length) > 0 then .PreCompact = $pc else del(.PreCompact) end)
            )
            | if (.hooks | length) == 0 then del(.hooks) else . end
        else . end
    ' <<<"$current"
}

# Idempotency check: if sentinel is set AND every hook script in the
# install dir matches the source, we're a no-op.
already_installed() {
    [ -f "$target" ] || return 1
    local sentinel
    sentinel="$(jq -r '.ants_hooks_pack_v1 // false' "$target")"
    [ "$sentinel" = "true" ] || return 1
    local f
    for f in "$src_dir"/ants-*.sh "$src_dir"/_common.sh; do
        local base="${f##*/}"
        local installed="$hooks_dir/$base"
        [ -e "$installed" ] || return 1
        if ! cmp -s "$f" "$installed"; then return 1; fi
    done
    return 0
}

# ANTS-1252-INV-8 — atomic-rename: write tmpfile in same dir, jq empty
# validates parseability, only then mv -f.
atomic_write_settings() {
    local content="$1"
    local tmp
    tmp="$(mktemp "${target}.XXXXXX")"
    printf '%s\n' "$content" > "$tmp"
    if ! jq empty "$tmp" >/dev/null 2>&1; then
        rm -f "$tmp"
        echo "ERROR: would have written invalid JSON; aborting" >&2
        return 1
    fi
    chmod 0600 "$tmp"
    mv -f "$tmp" "$target"
}

if [ "$uninstall" -eq 1 ]; then
    new_content="$(splice_uninstall)"
    if [ "$dry_run" -eq 1 ]; then
        echo "[dry-run] would write to: $target"
        echo "[dry-run] removed sentinel + ants-* entries"
        echo "$new_content"
        echo "[dry-run] would unlink hook scripts in: $hooks_dir/ants-*.sh"
        exit 0
    fi
    if [ -f "$target" ]; then
        cp --no-dereference -- "$target" "${target}.ants-backup" 2>/dev/null || true
    fi
    atomic_write_settings "$new_content"
    rm -f "$hooks_dir"/ants-*.sh "$hooks_dir/_common.sh" 2>/dev/null || true
    echo "uninstalled ants_hooks_pack_v1 from $target"
    exit 0
fi

if already_installed; then
    if [ "$dry_run" -eq 1 ]; then
        echo "[dry-run] already installed at $target — no changes"
    else
        echo "ants_hooks_pack_v1 already installed; no changes"
    fi
    exit 0
fi

new_content="$(splice_install)"

if [ "$dry_run" -eq 1 ]; then
    echo "[dry-run] would write to: $target"
    echo "$new_content"
    echo "[dry-run] would copy:"
    for f in "$src_dir"/ants-*.sh "$src_dir"/_common.sh; do
        echo "  $f → $hooks_dir/${f##*/}"
    done
    exit 0
fi

# Backup with --no-dereference (don't follow link target).
if [ -f "$target" ]; then
    cp --no-dereference -- "$target" "${target}.ants-backup"
fi

atomic_write_settings "$new_content"

# Copy hook scripts: skip if target is a symlink (don't follow the
# link to write through it) or contents already match.
for f in "$src_dir"/ants-*.sh "$src_dir"/_common.sh; do
    base="${f##*/}"
    dst="$hooks_dir/$base"
    if [ -L "$dst" ]; then
        echo "skip $base (target is a symlink)" >&2
        continue
    fi
    if [ -f "$dst" ] && cmp -s "$f" "$dst"; then
        continue
    fi
    cp -f -- "$f" "$dst"
    chmod 0755 "$dst"
done

echo "installed ants_hooks_pack_v1:"
echo "  settings: $target"
echo "  hooks:    $hooks_dir/"
