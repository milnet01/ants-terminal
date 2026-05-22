#pragma once

#include <QString>

// Regex-safety helpers shared across the codebase (ANTS-1665). Extracted from
// auditengine into ants_core_lib so widget-side consumers (terminalwidget's
// search / highlight / trigger-rule patterns) can harden user-supplied regexes
// without linking the audit lib. AuditEngine::isCatastrophicRegex /
// hardenUserRegex now forward here, preserving the single source of truth
// established by ANTS-1123.
namespace ants::regex {

// Conservative catastrophic-backtracking shape detector. Matches
// quantifier-under-quantifier (`(.+)+`) and alternation-under-quantifier
// (`(a|b)+`) groups. Errs toward rejecting safe-but-suspicious patterns rather
// than admitting adversarial ones.
bool isCatastrophicRegex(const QString &pattern);

// Prefix a user-supplied pattern with PCRE2's inline `(*LIMIT_MATCH=100000)` so
// a slow match aborts in milliseconds instead of hanging. Empty in → empty out;
// patterns already starting with `(*LIMIT_` pass through unchanged so an author
// can specify their own budget.
QString hardenUserRegex(const QString &pattern);

}  // namespace ants::regex
