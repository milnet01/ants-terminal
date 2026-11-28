// ANTS-4932 § 2.1 / § 2.5 — how ants-mcpd finds the terminal it forwards the
// terminal-scoped verbs to, and the two uid checks it makes before sending.
//
// The picker takes $ANTS_MCP_SOCKET when set, else the newest mcp-* socket in
// ConfigPaths::antsRuntimeDir() (ANTS-5236), preferring one whose pid is
// alive (ANTS-1322) and, among several, one a listener answers (ANTS-6643).
// Every candidate must be a socket owned by the expected uid; a foreign-owned
// one is skipped.

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
// `whyNot` (optional) receives the reason when false. ANTS-6222 — every
// socket handler in the terminal makes its peer check through this.
bool peerUidIs(int fd, uid_t expectedUid, QString *whyNot = nullptr);

// ANTS-5236 § 2.2 — remove terminal MCP sockets left by dead processes in
// the runtime directory. Skips `self`, a live
// pid, and anything safeToUnlinkLocalSocket refuses. Returns how many it
// removed.
int reapStaleTerminalSockets(pid_t self);

}  // namespace mcpd
