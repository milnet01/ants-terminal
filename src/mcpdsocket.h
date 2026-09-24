// ANTS-4932 § 2.1 / § 2.5 — how ants-mcpd finds the terminal it forwards the
// terminal-scoped verbs to, and the two uid checks it makes before sending.
//
// The picker is tools/mcp-bridge.py's pick_socket() in C++: $ANTS_MCP_SOCKET
// when set, else the newest /tmp/ants-terminal-mcp-* socket, preferring one
// whose pid is alive (ANTS-1322). Every candidate must be a socket owned by
// the expected uid; a foreign-owned one is skipped, as the bridge skips it.

#pragma once

#include <QString>

#include <sys/types.h>

namespace mcpd {

// The socket path to forward to, or empty when there is no acceptable one.
// `whyNot` (optional) receives the reason when empty.
QString pickTerminalSocket(uid_t expectedUid, QString *whyNot = nullptr);

// INV-11 — the socket file at `path` is a socket owned by `expectedUid`.
bool socketOwnedBy(const QString &path, uid_t expectedUid);

// INV-11 — the peer on the connected socket `fd` runs as `expectedUid`
// (SO_PEERCRED). False when the credential cannot be read: fail closed.
bool peerUidIs(int fd, uid_t expectedUid);

}  // namespace mcpd
