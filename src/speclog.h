// ANTS-1963 — spec_log MCP tool: pure markdown transforms for the three
// recurring spec mutations (flip Status, append a cold-eyes loop entry,
// append an INV bullet). Qt6::Core-only, lives in ants_core_lib so it is
// unit-testable without RemoteControl / MainWindow (mirrors readlog.h /
// feedbackfile.h). The thin cmdSpecLog wrapper (remotecontrol.cpp)
// handles id/path routing + PathValidation + the caller_cwd contract +
// the atomic write, then calls into the helpers here.
//
// Contract: docs/standards/specs.md (Status § 5.6, INV bullet form
// § 3.5, cold-eyes loop log § 5.7). Spec: docs/specs/ANTS-1963.md (§ 2.2).

#pragma once

#include <QString>

namespace SpecLog {

// Result of a pure transform. `code`/`error` carry only the STRUCTURAL
// refusals computed from the content string (`unrecognised_format` for a
// missing section/line, `bad_args` for a duplicate INV-N); the handler
// owns all arg/cwd/path/file refusals. On success, `content` is the new
// full file text.
struct EditResult {
    bool    ok = false;
    QString content;
    int     line = -1;   // 1-based line the edit landed on (success only)
    QString code;
    QString error;
};

// op:"set_status" — replace the first `**Status:**` field's WHOLE extent
// (its opener plus any continuation lines) with one new line. ANTS-3785.
// `unrecognised_format` when the spec has no Status line.
EditResult setStatus(const QString &content, const QString &newStatus);

// op:"append_loop" — append a `- **<label>** — <body>` bullet at the end
// of the first `## …` section whose heading contains "Cold-eyes loop log"
// (case-insensitive). Creates the section at EOF when absent (repaired,
// not refused).
EditResult appendLoop(const QString &content, const QString &label,
                      const QString &body);

// op:"append_inv" — append a `- **<invId>** — <body>.` bullet (plus
// ` *Test:* <test>.` when test non-empty) at the end of the first `## …`
// section whose heading contains "Invariants" (case-insensitive). Never
// renumbers. `bad_args` when `invId` already appears as an INV bullet;
// `unrecognised_format` when there is no Invariants section.
EditResult appendInv(const QString &content, const QString &invId,
                     const QString &body, const QString &test);

}  // namespace SpecLog
