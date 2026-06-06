// ANTS-1993 — structural gate for the terminal scroll-scanner's Claude
// Code permission-prompt detection. Terminal output is fully attacker-
// controlled when you run a hostile program, so this is defense-in-depth,
// not a trust boundary: the goal is that an *incidental* string in normal
// output (a log line mentioning "always allow", a doc snippet with
// "Read"/"Write") can no longer manufacture a phantom "Add to allowlist"
// button pre-filled with an attacker-chosen rule.
//
// A genuine CC permission prompt is a multi-line bordered widget: a
// footer/question anchor PLUS a selection UI (numbered Yes/No options or
// the navigation hint footer). A single hostile line carries at most one
// of those, so requiring co-occurrence raises the bar from "any one
// string" to "reproduce the whole prompt shape". Pure (Qt6::Core-only)
// so it unit-tests without instantiating TerminalWidget.

#ifndef ANTS_CLAUDEPROMPTDETECT_H
#define ANTS_CLAUDEPROMPTDETECT_H

#include <QStringList>

namespace ClaudePromptDetect {

// Returns true iff `recentLines` (the last ~12 on-screen terminal lines)
// exhibit the structural signature of a real Claude Code permission
// prompt: an anchor phrase AND corroborating prompt structure.
//
//   anchor    : "Tab to accept" | "Do you want to proceed"
//               | "allow access to" | "always allow"
//   strong    : "Tab to accept" | "Ctrl+e to explain"  (CC-specific
//               footer fragments; stand alone as corroboration)
//   selection : a numbered "N. Yes"/"N. No" option line, a "❯" cursor,
//               an "Esc to cancel" navigation hint, or a "y · yes"/"y/n"
//               choice line
//
// Detection := hasAnchor && (hasStrong || hasSelection). The weak anchors
// ("always allow", "allow access to") therefore never fire alone.
bool isPermissionPromptStructure(const QStringList &recentLines);

}  // namespace ClaudePromptDetect

#endif  // ANTS_CLAUDEPROMPTDETECT_H
