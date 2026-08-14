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

// Mode hint matches the MCP tool surface — "auto" + the explicit
// languages. Unknown extensions fall through to a byte-count-only
// envelope (still useful for orientation).
//
// ANTS-2150 — `Generic` is one shared brace-family regex set (Rust, Go,
// JS/TS, Java, C#, Kotlin, Swift, Scala, PHP). It is auto-detected from
// the file extension (pickModeByExt); the response `language` field still
// reports the precise language name (rust / go / typescript …) so the map
// stays useful. One mode covers many languages because their top-level
// declaration syntax overlaps enough that a keyword/method/arrow regex
// trio extracts the 90% case — the same Karpathy §2 bet the C++/Py set makes.
enum class Mode {
    Auto,
    Cpp,
    Py,
    Md,
    Json,
    Generic,
    // ANTS-3800 — GLSL / Vulkan shader stages. Extracted through the Cpp path
    // (the declaration grammar is the same shape: `<type> <name>(<args>)` at
    // file scope followed by `{`), but reported as its own language so the
    // three verbs that name a language set finally agree. find_definition and
    // find_caller have advertised `glsl` since ANTS-3558 while this verb
    // answered {language:"unknown"} with no symbols at all — which also broke
    // read_region's symbol mode, since it resolves through this outline.
    Glsl,
};

Mode parseMode(const QString &s);

// True for a GLSL / Vulkan shader-stage extension (lowercase, no dot).
// ANTS-4096 — exported so CodebaseIndex::isIndexableSuffix keys on the same
// set ANTS-3558 (find_definition) and ANTS-3800 (this verb) already share,
// rather than a fourth copy that can drift out of step with them.
bool isGlslExt(const QString &ext);

// Compute the outline for a single file. `absPath` MUST be a
// canonical filesystem path under the project root (the caller is
// responsible for the path-escape check; ANTS-1249-INV-1 lives at
// the call site, not here).
//
// On success the returned object is the spec § 2 response shape
// (ok/path/language/header_doc/symbols/truncated/total_bytes/
//  total_lines). Errors return {ok:false, error, code} mirrored
// from the IPC convention.
// ANTS-4384 — `withSizes` adds `bytes` + `lines` to each symbol: its extent
// from its start line to the line before the next symbol at the SAME OR
// HIGHER level (EOF for the last). Level is heading depth in md mode and
// uniform elsewhere, so a `##` section's size includes its `###` children —
// without that rule the reply answers "how long is this paragraph" rather
// than "where do I split this file", which is the question it exists for.
// Opt-in: the default envelope stays byte-identical, and the only cost when
// asked for is a per-line offset table.
QJsonObject compute(const QString &absPath,
                    Mode mode,
                    bool includeDocComment,
                    int maxSymbols,
                    bool withSizes = false);

}  // namespace FileOutline

#endif
