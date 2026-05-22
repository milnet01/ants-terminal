#pragma once

#include <QString>

// Regex-safety helpers shared across the codebase (ANTS-1665). Extracted from
// auditengine into ants_core_lib so widget-side consumers (terminalwidget's
// search / highlight / trigger-rule patterns) can harden user-supplied regexes
// without linking the audit lib. AuditEngine::isCatastrophicRegex /
// hardenUserRegex now forward here, preserving the single source of truth
// established by ANTS-1123.
namespace ants::regex {

// BEST-EFFORT catastrophic-backtracking shape detector (ANTS-1758).
// Recognises exactly two grouped shapes: quantifier-under-quantifier
// (`(.+)+`) and alternation-under-quantifier (`(a|b)+`). It does NOT
// catch every ReDoS shape — notably ungrouped consecutive quantified
// atoms (`a+a+`, `\d+\d+`) and some multi-level nesting slip through.
// Comprehensive static ReDoS detection is intractable with a regex, so
// this is a heuristic warning pre-filter only; the actual runtime
// backstop is `hardenUserRegex`'s `(*LIMIT_MATCH)` prefix, which aborts
// a slow match in milliseconds regardless of shape. Treat a `false`
// result as "no obvious red flag", not "proven safe".
bool isCatastrophicRegex(const QString &pattern);

// Prefix a user-supplied pattern with PCRE2's inline `(*LIMIT_MATCH=100000)` so
// a slow match aborts in milliseconds instead of hanging. Empty in → empty out;
// patterns already starting with `(*LIMIT_` pass through unchanged so an author
// can specify their own budget.
QString hardenUserRegex(const QString &pattern);

}  // namespace ants::regex
