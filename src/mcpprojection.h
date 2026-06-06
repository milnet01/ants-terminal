#pragma once

#include <QString>

class QJsonArray;

// ANTS-1720 — `fields=` top-level response projection for high-volume
// MCP read tools. Pure (Qt6::Core only) so the dispatch layer
// (ClaudeIntegration) and the feature test share one implementation —
// mirrors the focusedtest / modelrecommender extraction pattern.
namespace mcp {

// Allowlist: the 11 read tools that accept `fields=` (grew past the original
// 7 via ANTS-1855 read_log, ANTS-2021 read_region, ANTS-1637 codebase_index,
// ANTS-1735 model_switch_stats). Mostly overlaps
// ClaudeIntegration::isEtagSupportedTool, but `read_log` is projection-only
// (NOT etag-able) — so this is no longer a strict subset of the etag set.
// Chosen for payload size (see docs/specs/ANTS-1720 / ROADMAP).
bool isFieldProjectionTool(const QString &toolName);

// Return a compact JSON object carrying only the named top-level fields
// of `responseText`. Contract:
//   - `fields` empty                    -> responseText returned unchanged.
//   - responseText not a JSON object    -> responseText returned unchanged.
//   - field present in source           -> copied verbatim.
//   - field absent / non-string / empty -> omitted (never an error).
//   - all fields unknown                -> "{}".
// The etag is NOT auto-preserved: a caller that wants the etag for a
// follow-up ANTS-1499 304 round-trip lists "etag" in `fields`. The
// dispatch computes the etag on the unfiltered body before projecting,
// so the etag a narrowed call returns equals a full call's etag.
QString projectFields(const QString &responseText, const QJsonArray &fields);

}  // namespace mcp
