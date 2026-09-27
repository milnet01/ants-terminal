#pragma once

// ANTS-5485 — retiring obsolete MCP verbs: mark, measure, remove.
//
// A verb in the table below still works. tools/list prefixes its description
// with a deprecation line naming what to use instead, each call's reply carries
// a `deprecated` object saying the same, and each call appends one line to
// deprecated-calls.jsonl beside the roadmap store. After a release the verbs
// with no line are removed; a verb that was still called is looked at first.
//
// The record is its own file because the per-process mcpd-usage counters are
// deleted when their process exits, so they cannot say what was called across
// a week of reconnects.
//
// Reload story: the table is compiled in, so a change reaches sessions with an
// ants-mcpd rebuild and an MCP reconnect; the terminal is not relaunched.

#include <QString>

namespace mcp {

// What to use instead of `verb`, or an empty string when `verb` is not
// deprecated. Every entry names a replacement, a verb or a skill.
QString deprecatedReplacement(const QString &verb);

// The line tools/list puts in front of a deprecated verb's description.
// Empty for a verb that is not deprecated.
QString deprecationPrefix(const QString &verb);

// `responseText` with a top-level `deprecated` object added:
// {replacement, tracking:"ANTS-5485"}. Returned unchanged when the verb is not
// deprecated or the text is not a JSON object.
QString withDeprecationAdvisory(const QString &responseText, const QString &verb);

// Appends {at, verb, caller_cwd} to deprecated-calls.jsonl, owner-only. A
// failure to write is not an error for the call itself.
void recordDeprecatedCall(const QString &verb, const QString &callerCwd);

// Where recordDeprecatedCall writes.
QString deprecatedCallsPath();

}  // namespace mcp
