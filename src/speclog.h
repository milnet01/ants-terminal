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
#include <QStringList>

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
    // ANTS-4114 — set_status only: the value being REPLACED, space-joined
    // across its continuations. The verb imposes no Status vocabulary (it
    // writes the caller's string verbatim), so the project's own vocabulary
    // is only knowable from the value already in the file. Empty for the
    // append ops.
    QString previousValue;
    // ANTS-4353/4364 — append_loop only. `rowShape` is "table" or "bullet",
    // so a caller can see which form the section actually holds rather than
    // assuming. `rowOrder` is the direction INFERRED from the existing rows —
    // "oldest_first", "newest_first", or "ambiguous" when fewer than two rows
    // carry a loop number. Loop logs run in opposite directions across specs
    // in one corpus, and a row inserted at the wrong end reads as a different
    // loop's result — which a checker validating only per-row tallies passes.
    QString rowShape;
    QString rowOrder;
};

// op:"set_status" — replace the first `**Status:**` field's WHOLE extent
// (its opener plus any continuation lines) with one new line. ANTS-3785.
// `unrecognised_format` when the spec has no Status line.
EditResult setStatus(const QString &content, const QString &newStatus);

// op:"append_loop" — append a `- **<label>** — <body>` bullet at the end
// of the first `## …` section whose heading contains "Cold-eyes loop log"
// (case-insensitive). Creates the section at EOF when absent (repaired,
// not refused).
// ANTS-4364 — `cells` is the TABLE form: one string per column, ordered to
// match the table's own header. Where the section holds a table and `cells`
// is empty the call REFUSES rather than writing a bullet into it — writing
// bullet form into a table was the original defect, and it is silent.
// Where the section holds bullets (or does not exist yet), `label` + `body`
// render the bullet as before and `cells` is ignored.
//
// The empty-section case deliberately still writes a BULLET rather than
// synthesising a table: the column set belongs to the project's format
// standard, and inventing one here would encode another repo's choice.
EditResult appendLoop(const QString &content, const QString &label,
                      const QString &body,
                      const QStringList &cells = QStringList());

// op:"append_inv" — append a `- **<invId>** — <body>.` bullet (plus
// ` *Test:* <test>.` when test non-empty) at the end of the first `## …`
// section whose heading contains "Invariants" (case-insensitive). Never
// renumbers. `bad_args` when `invId` already appears as an INV bullet;
// `unrecognised_format` when there is no Invariants section.
EditResult appendInv(const QString &content, const QString &invId,
                     const QString &body, const QString &test);

}  // namespace SpecLog
