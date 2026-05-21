#pragma once

#include <QString>

class QJsonArray;

// ANTS-1720 — `fields=` top-level response projection for high-volume
// MCP read tools. Pure (Qt6::Core only) so the dispatch layer
// (ClaudeIntegration) and the feature test share one implementation —
// mirrors the focusedtest / modelrecommender extraction pattern.
namespace mcp {

// Allowlist: the seven read tools that accept `fields=`. A subset of
// ClaudeIntegration::isEtagSupportedTool (all seven are also etag-able),
// chosen for payload size (see docs/specs/ANTS-1720 / ROADMAP).
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
