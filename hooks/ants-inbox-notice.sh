#!/usr/bin/env bash
# ants-terminal SessionStart + UserPromptSubmit hook — unread-mail notice.
# ANTS-5553: a session_message sits unread until the session asks for its
# inbox, and nothing prompts it to. This prints one line when mail waits:
#   [ants:inbox] 2 unread messages from other sessions — read them with ...
# and nothing otherwise, so it costs no tokens on a quiet inbox.
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
cwd=""
if command -v jq >/dev/null 2>&1; then
    cwd="$(printf '%s' "$input" | jq -r '.cwd // empty' 2>/dev/null || true)"
fi
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
    ''|0) exit 0 ;;
esac

if [ "$count" = "1" ]; then
    noun="message"
else
    noun="messages"
fi
printf '[ants:inbox] %s unread %s from other sessions — read with session_message (op:"inbox"), then ack each.\n' \
    "$count" "$noun"
exit 0
