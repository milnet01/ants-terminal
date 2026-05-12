// ANTS-1249 — file_outline regex scanner. Reads a file line-by-line
// via QFile::readLine() and extracts a structured outline (header doc
// + symbol list) for C++ / Python / Markdown. Used by the
// `file_outline` MCP tool and IPC verb so Claude can orient on a
// 5 000-line file at ~1 K tokens instead of a 30 K full Read.
//
// No clangd / no tree-sitter — a 6-regex set covers the 90% case
// per Karpathy §2. PCRE2 JIT + possessive quantifiers + 1024-byte
// per-line cap bound the worst case (INV-8).
//
// See docs/specs/ANTS-1249.md.

#ifndef ANTS_FILEOUTLINE_H
#define ANTS_FILEOUTLINE_H

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace FileOutline {

// Mode hint matches the MCP tool surface — "auto" + the 4 explicit
// languages. Unknown extensions fall through to a byte-count-only
// envelope (still useful for orientation).
enum class Mode {
    Auto,
    Cpp,
    Py,
    Md,
    Json,
};

Mode parseMode(const QString &s);

// Compute the outline for a single file. `absPath` MUST be a
// canonical filesystem path under the project root (the caller is
// responsible for the path-escape check; ANTS-1249-INV-1 lives at
// the call site, not here).
//
// On success the returned object is the spec § 2 response shape
// (ok/path/language/header_doc/symbols/truncated/total_bytes/
//  total_lines). Errors return {ok:false, error, code} mirrored
// from the IPC convention.
QJsonObject compute(const QString &absPath,
                    Mode mode,
                    bool includeDocComment,
                    int maxSymbols);

}  // namespace FileOutline

#endif
