#pragma once

#include <QString>

class QJsonArray;
class QJsonObject;

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

// ANTS-2081 + ANTS-2086 — for a large successful read response, append
// presentation-only nudges: `next_call_hint` (reuse the issued etag to
// 304 a repeat read) and `leaner_call_hint` (the cheaper mode on this
// verb). Pure so the dispatch layer and the feature test share one
// implementation (mirrors projectFields). Lives here rather than inline
// in ClaudeIntegration so the per-verb tool-name literals don't displace
// the source-string-match anchors the WiringContract tests use against
// claudeintegration.cpp. Returns `responseText` unchanged on: a 304
// (`etagUnchanged`), a fields=-narrowed call, a body under the byte
// threshold, a refusal (`ok:false`), or an unparseable body.
QString appendReadHints(const QString &toolName, const QJsonObject &args,
                        const QString &responseText, bool etagUnchanged);

}  // namespace mcp
