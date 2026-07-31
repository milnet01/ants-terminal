// ANTS-3764: the roadmap markdown reader, lifted out of
// src/roadmapdialog.cpp so a headless caller can share it.
//
// It was three per-format readers plus a dispatcher with internal linkage in
// an anonymous namespace, returning a record nested inside a QDialog subclass
// — reachable only from ants_dialogs_lib. The roadmap migration (ANTS-3757)
// needs the same reader from ants_core_lib, and the two alternatives were both
// bad: link Widgets into a headless bulk import, or write a SECOND parser for
// the same three formats, whose disagreements would be silent and would be
// about the corpus itself.
//
// Qt6::Core only; lives in ants_core_lib so RoadmapDialog, the remotecontrol
// handlers, the feature tests and the migration all share one implementation.
// Mirrors the call ANTS-2126 already made in the other direction for the
// pass-headings WRITER (src/passheadingwrite.{h,cpp}).
//
// RoadmapDialog keeps its widget code and calls in: RoadmapDialog::BulletRecord
// is an alias for RoadmapParse::BulletRecord and RoadmapDialog::parseBullets()
// forwards here, so no existing call site changed.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace RoadmapParse {

// The four status markers roadmap-format.md § 3.3 defines. They are the
// FORMAT's vocabulary rather than the dialog's, so they live with the parser
// and RoadmapDialog uses them from here — one definition, which is the same
// argument ANTS-3764 makes for one reader. The dialog keeps kStatusLabels,
// which is display text and genuinely its own.
inline constexpr const char *kEmojiDone       = "✅";
inline constexpr const char *kEmojiPlanned    = "📋";
inline constexpr const char *kEmojiInProgress = "🚧";
inline constexpr const char *kEmojiConsidered = "💭";

// Bullet record surfaced via the `roadmap-query` IPC verb (ANTS-1117).
// One entry per top-level status-emoji-prefixed bullet in document
// order; plain narration bullets without an emoji are omitted (they
// don't have stable IDs and so are out of contract). See
// `docs/specs/ANTS-1117.md` § Acceptance criteria.
struct BulletRecord {
    QString id;          // <PREFIX>-NNNN; empty if no `[<PREFIX>-NNNN]` token (ANTS-1405)
    QString status;      // "✅" | "🚧" | "📋" | "💭"
    QString headline;    // first **bold** chunk after the emoji (≤ 120 chars)
    QString headlineFull; // ANTS-2075 — untruncated headline for locator use
    QString kind;        // value from `Kind:` line; "" if absent
    QStringList lanes;   // values from `Lanes:` line; [] if absent
    QStringList evidence; // ANTS-3382 — file paths from `Evidence:` line; [] if absent
    // ANTS-1154 v2 card-renderer extensions. Additive — older
    // callers that only inspect id/status/headline/kind/lanes
    // see no behaviour change.
    QString layman;      // value from `Layman:` line; "" if absent
    QString body;        // full bullet body (post-emoji, pre-continuation-join)
    QString sectionHeading;  // text of the most recent ## or ### heading
    int sectionLevel = 0;    // 2 for `##`, 3 for `###`, 0 if no section
    QString sectionSlug;     // sectionHeading → lowercase, non-alnum→`-`
    // ANTS-1428 (adapter mode) — fields populated when the parser
    // engages the GFM-task-list branch. native parses leave them
    // at default (empty/false).
    QString anchor;        // ^prefix-NNNN caret anchor; "" if absent
    bool    synthetic = false;  // id was content-hash-derived, not from a token
    QString format;        // "ants-v1" (default) | "github-task-list"
    // ANTS-1438 — bold-ID token, populated when the GFM-adapter
    // matched a `**...**` prefix at the start of the head. Distinct
    // from `id` because id may be a synthetic content-hash when
    // boldId is empty; when boldId is non-empty, id == boldId.
    // Surfaced through the envelope as `bold_id` so callers can
    // correlate with commit-message prefixes explicitly.
    QString boldId;        // "FW W5 (cont.)", "Sh4", "Audit/FW X2", …
};
// The `[PROJ-NNNN]` token shape, as a bare regex fragment (no anchors, no
// capture group). ANTS-1784 — exported rather than file-static because
// roadmapdialog.cpp's CHANGELOG scanner shares it with the bullet parser
// "so they can't drift"; that contract predates this extraction and has to
// survive it.
QString idTokenPattern();

// Classify a roadmap document: "ants-v1" | "github-task-list" |
// "pass-headings". Answers "ants-v1" for input it does not recognise,
// INCLUDING an empty file — so a returned format is no evidence that anything
// was understood (ANTS-3757 § 2.3 relies on this and reports zero items).
QString detectRoadmapFormat(const QStringList &lines);

// Pure helper: parse `markdownText` into top-level status-emoji bullets.
// Dispatches on detectRoadmapFormat(). Result is read-only; used by the
// `roadmap-query` IPC verb to feed Claude a structured snapshot without
// re-burning the file content as tokens.
QVector<BulletRecord> parseBullets(const QString &markdownText);

}  // namespace RoadmapParse
