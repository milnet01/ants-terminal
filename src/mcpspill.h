#ifndef ANTS_MCPSPILL_H
#define ANTS_MCPSPILL_H

// ANTS-2094 — proactive server-side result offload (observation masking).
//
// When an offload-eligible MCP read verb returns a body over the byte
// threshold, the dispatcher spills the full body to a content-addressed
// cache file under ~/.cache/ants-terminal/mcp-spill/<sha256>.json and
// returns a small {head, handle, bytes, ...} envelope instead. The agent
// re-reads the full body (byte-paged) via the `read_spill` verb only when
// it needs more than the head. See docs/specs/ANTS-2094.md.
//
// Pure Qt6::Core (no widgets) so it links into ants_core_lib. The cache is
// global and content-addressed — never project-scoped — so it cannot
// "shadow" across a project relocation (docs/standards/mcp-caches.md).

#include <QString>

class QJsonObject;

namespace mcp {

// Eviction caps (INV-7): whichever binds first. Oldest-mtime first, never
// dropping the handle written this call.
constexpr int     kSpillMaxFiles = 64;
constexpr qint64  kSpillMaxBytes = 64LL * 1024 * 1024;   // 64 MiB

// Config (set from the GUI thread on load / Settings Apply, read on every
// dispatch). setOffloadConfig clamps threshold to [4096, 1048576] and head
// to [256, 16384] (INV-12) before publishing.
void setOffloadConfig(bool enabled, int thresholdBytes, int headBytes);
bool offloadDefault();
int  offloadThresholdBytes();
int  offloadHeadBytes();

// Resolve whether offload is requested for this call: a per-call `offload`
// bool wins (true OR false), else the session default (mirrors ANTS-2085's
// `compact` resolution).
bool offloadRequested(const QJsonObject &args);

// Spill `body` and return the head+pointer envelope JSON string. On ANY
// write failure (dir unwritable, disk full, commit() false) returns `body`
// unchanged — fail-open (INV-11). Caller must already have checked the
// threshold + head guard (INV-1).
QString offloadBody(const QString &toolName, const QString &body);

// read_spill byte-paging slice (INV-5/INV-6).
struct SpillSlice {
    bool    ok = false;
    QString code;          // bad_args / not_found when !ok
    QString content;       // UTF-8 char-boundary slice [offset, offset+n)
    qint64  offset = 0;
    qint64  bytes = 0;     // == content UTF-8 length
    qint64  totalBytes = 0;
    bool    truncated = false;
};
// handle must be a bare 64-hex sha256 (validated by the caller). maxBytes<=0
// → default 512 KiB; clamped to the 4 MiB ceiling.
SpillSlice readSpill(const QString &handle, qint64 offset, qint64 maxBytes);

// Session-start sweep: drop spill files with mtime older than 24 h (INV-7).
void spillSweep();

}  // namespace mcp

#endif  // ANTS_MCPSPILL_H
