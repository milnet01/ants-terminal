// ANTS-1286 — process-lifetime tool-availability cache for the audit
// pipeline. PATH-hash-keyed; cache resets when $PATH changes. Lives
// in ants_core_lib (Qt6::Core only). See docs/specs/ANTS-1286.md.

#pragma once

#include <QString>

namespace ToolDetectionEngine {

// True if `tool` resolves on PATH (cached). Empty tool → false.
bool exists(const QString &tool);

// Resolved absolute path or empty string (cached).
QString resolve(const QString &tool);

// Test / diagnostic accessors.
void clearCache();
int  cacheSize();
QString currentPathHash();

}  // namespace ToolDetectionEngine
