#!/usr/bin/env bash
# ants-terminal SessionStart + UserPromptSubmit + PostToolUse hook —
# unread-mail notice.
# ANTS-5553: a session_message sits unread until the session asks for its
# inbox, and nothing prompts it to. This announces mail when it waits:
#   [ants:inbox] 2 unread messages from other sessions — read them with ...
# and prints nothing otherwise, so it costs no tokens on a quiet inbox.
#
# ANTS-5619: the plain line reached only Claude's context, so the user never
# saw it, and mail arriving mid-session waited for the user's next prompt.
# When the payload names its event the hook now answers in JSON: a
# `systemMessage` the user sees, plus `additionalContext` telling Claude to
# read the mail before anything else. On PostToolUse it speaks only when the
# count has RISEN since this session was last told, and asks ants-mcpd at most
# once per ANTS_INBOX_INTERVAL seconds (default 30). A payload with no event
# gets the plain line, as before.
#
# Unlike the rest of the pack it is NOT gated on `.ants-project`: the store
# mailbox serves every registered project. ants-mcpd decides which project the
# cwd belongs to, and an unregistered one refuses, which prints nothing.
#
# ants-mcpd is found via $ANTS_MCPD, then PATH, then the `ants-mcpd.path` file
# install-hooks.sh writes beside this script. Fail-open: no binary, a failed
# call or an unparseable reply all print nothing and exit 0.

set -u

input="$(cat 2>/dev/null || true)"

event=""
cwd=""
sid=""
if command -v jq >/dev/null 2>&1; then
    event="$(printf '%s' "$input" | jq -r '.hook_event_name // empty' 2>/dev/null || true)"
    cwd="$(printf '%s' "$input" | jq -r '.cwd // empty' 2>/dev/null || true)"
    sid="$(printf '%s' "$input" | jq -r '.session_id // empty' 2>/dev/null || true)"
fi
# A session id names a state file, so only a safe one is used.
case "$sid" in
    *[!A-Za-z0-9-]*) sid="" ;;
esac

# The per-session record of the count this session was last told about.
state=""
if [ -n "$sid" ]; then
    state_dir="${XDG_RUNTIME_DIR:-$HOME/.cache}/ants-inbox"
    state="$state_dir/$sid"
fi

# PostToolUse fires on every tool call: throttle before spawning anything.
if [ "$event" = "PostToolUse" ]; then
    [ -n "$state" ] || exit 0
    interval="${ANTS_INBOX_INTERVAL:-30}"
    if [ -f "$state.checked" ]; then
        last="$(stat -c %Y "$state.checked" 2>/dev/null || echo 0)"
        now="$(date +%s)"
        [ $((now - last)) -lt "$interval" ] && exit 0
    fi
    mkdir -p "$state_dir" 2>/dev/null && : > "$state.checked" 2>/dev/null
fi

mcpd="${ANTS_MCPD:-}"
if [ -z "$mcpd" ]; then
    mcpd="$(command -v ants-mcpd 2>/dev/null || true)"
fi
if [ -z "$mcpd" ]; then
    pathfile="$(dirname "$0")/ants-mcpd.path"
    [ -r "$pathfile" ] && mcpd="$(head -n 1 "$pathfile")"
fi
[ -n "$mcpd" ] && [ -x "$mcpd" ] || exit 0

# The session's own cwd from the hook payload, else ours.
[ -n "$cwd" ] || cwd="$PWD"
# The store keys a project by its root and refuses a subdirectory, so ask
# about the enclosing repository's top level when there is one.
if command -v git >/dev/null 2>&1; then
    top="$(git -C "$cwd" rev-parse --show-toplevel 2>/dev/null || true)"
    [ -n "$top" ] && cwd="$top"
fi

# JSON-escape the two characters a path can carry that break a string.
esc="${cwd//\\/\\\\}"
esc="${esc//\"/\\\"}"

reply="$(timeout 2 "$mcpd" --call session_message \
    "{\"caller_cwd\":\"$esc\",\"op\":\"inbox\",\"limit\":1}" 2>/dev/null || true)"

count="$(printf '%s' "$reply" | grep -o '"unacked_count":[0-9]*' | head -n 1 | cut -d: -f2)"
case "$count" in
    '') exit 0 ;;
esac

told=0
if [ -n "$state" ] && [ -r "$state" ]; then
    told="$(head -n 1 "$state" 2>/dev/null || echo 0)"
    case "$told" in ''|*[!0-9]*) told=0 ;; esac
fi
remember() {
    [ -n "$state" ] || return 0
    mkdir -p "$state_dir" 2>/dev/null && printf '%s\n' "$1" > "$state" 2>/dev/null
}

if [ "$count" = 0 ]; then
    remember 0
    exit 0
fi
# Mid-session, speak only about mail that arrived since this session was told.
if [ "$event" = "PostToolUse" ] && [ "$count" -le "$told" ]; then
    remember "$count"
    exit 0
fi
remember "$count"

if [ "$count" = "1" ]; then
    noun="message"
else
    noun="messages"
fi

if [ -z "$event" ]; then
    printf '[ants:inbox] %s unread %s from other sessions — read with session_message (op:"inbox"), then ack each.\n' \
        "$count" "$noun"
    exit 0
fi

if [ "$event" = "PostToolUse" ]; then
    seen="New mail arrived while you were working: $count unread $noun"
else
    seen="$count unread $noun"
fi
visible="[ants:inbox] $seen from other sessions."
context="[ants:inbox] $seen from other sessions are waiting in this project's inbox. Read them now, before continuing other work: call session_message with op:\"inbox\", act on or answer each, then ack each with op:\"ack\". Tell the user what they asked for."
if command -v jq >/dev/null 2>&1; then
    jq -cn --arg ev "$event" --arg vis "$visible" --arg ctx "$context" \
        '{systemMessage: $vis, hookSpecificOutput: {hookEventName: $ev, additionalContext: $ctx}}'
else
    printf '%s\n' "$visible"
fi
exit 0
