// ANTS-1286 — process-lifetime tool-availability cache for the audit
// pipeline. PATH-hash-keyed; cache resets when $PATH changes. Lives
// in ants_core_lib (Qt6::Core only). See docs/specs/ANTS-1286-audit-tool-detection-cache-session.md.

#pragma once

#include <QString>
#include <QtGlobal>

namespace ToolDetectionEngine {

// True if `tool` resolves on PATH (cached). Empty tool → false.
bool exists(const QString &tool);

// ANTS-5110 — a miss is re-probed once this old, so a tool installed while
// the terminal runs is found without a restart. A hit never expires.
inline constexpr qint64 kDefaultMissTtlMs = 30'000;

// Resolved absolute path or empty string (cached). A miss is re-probed
// after kDefaultMissTtlMs, or setMissTtlMs's value.
QString resolve(const QString &tool);

// Test / diagnostic accessors.
void clearCache();
int  cacheSize();
QString currentPathHash();
// How old a cached miss may be before it is re-probed; negative = default.
void setMissTtlMs(qint64 ms);

}  // namespace ToolDetectionEngine
