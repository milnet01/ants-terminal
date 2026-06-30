// ANTS-1961 / ANTS-1962 — shared pure module for the cross-session
// `*_Ants_MCP_Feedback.md` files. Qt6::Core-only, lives in
// ants_core_lib so it is unit-testable without RemoteControl /
// MainWindow (mirrors readlog.h). The thin cmdFeedbackQuery /
// cmdFeedbackLog wrappers (remotecontrol.cpp) handle path resolution +
// PathValidation + the suffix guard + the caller_cwd contract, then
// call into the helpers here.
//
// Format contract: docs/standards/mcp-feedback-files.md
// Read verb spec:  docs/specs/ANTS-1961.md (§ 2.2 is the canonical home
//                  declaring the full namespace surface).
// Write verb spec: docs/specs/ANTS-1962.md (§ 2).

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace FeedbackFile {

// ---- ANTS-1961: read side (the un-triaged delta parser) -------------

// A maintainer tracking-table row. Authored by the write side (ANTS-1962,
// renderTrackingBlock) and re-surfaced on the read side (ANTS-3371,
// ParseResult::trackingRows). Defined before ParseResult so the read
// parser can carry the structured rows.
struct TrackingRow {
    QString     item;
    QStringList ids;     // ANTS-NNNN strings; empty → renders `n/a`
    QString     status;  // an emoji from the machine-readable set
    QString     notes;   // optional; triggers the 4th column when any row has it
};

struct ParseResult {
    QString     delta;               // text from the first contributor heading
                                     // after the last maintainer heading to EOF
    bool        deltaPresent = false;
    int         deltaStartLine = -1; // 1-based line of the FIRST contributor
                                     // heading of the delta; -1 when empty
    int         deltaLineCount = 0;  // lines in the full (pre-cap) delta
    QStringList mappedIds;           // unique, sorted; ANTS-[0-9]+ from
                                     // maintainer-block bodies only
    int         maintainerBlockCount = 0;
    int         lastMaintainerLine = -1; // 1-based; -1 when none
    QVector<TrackingRow> trackingRows;   // ANTS-3371: every maintainer
                                         // tracking-table data row, in
                                         // document order (later rows
                                         // supersede earlier ones for the
                                         // same ID). Surfaced only when
                                         // feedback_query is called with
                                         // include_tracking.
};

// Parse the full file content per mcp-feedback-files.md § "The
// un-triaged delta (parser contract)": fenced regions skipped, only
// `^# `/`^## ` (one or two hashes) outside fences are boundaries,
// maintainer headings identified by the anchor regex.
ParseResult parse(const QString &fileContent);

// ---- ANTS-1962: write side (block renderers) ------------------------

struct Finding {
    QString title;
    QString what;
    QString repro;
    QString impact;
    QString suggestedFix;
};

// Render a contributor session block: a dated heading (`## ` default,
// `# ` when h1Heading) + optional note prose + one `### <title>`
// sub-block per finding (only provided field bullets emitted; the
// `**Proposed ID:**` line is always emitted blank).
QString renderFindingBlock(const QString &date, const QString &sessionLabel,
                           bool h1Heading, const QString &note,
                           const QVector<Finding> &findings);

// Render a maintainer tracking block: the fully-qualified watermark
// heading `## 📋 Ants Terminal roadmap tracking update (<date>,
// maintainer)` + optional note + the mapping table (+ optional sentinel
// breadcrumb). A 4th `Notes` column is added iff any row carries notes.
QString renderTrackingBlock(const QString &date, const QString &note,
                            const QVector<TrackingRow> &rows, bool sentinel);

// Render the file skeleton for an absent file: the marker comment + a
// derived `# <projectTitle> …` H1 + the contributor-pointer blockquote.
QString skeleton(const QString &projectTitle);

}  // namespace FeedbackFile
