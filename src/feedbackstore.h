// ANTS-4513 — the feedback corpus in the roadmap store.
//
// Spec: docs/specs/ANTS-4513-feedback-corpus-store.md. The store row is the
// record; the *_Ants_MCP_Feedback.md file is a read-only export rewritten from
// it after every write. feedback_log and feedback_query call this module; it
// owns import on first touch (§ 4.3), the locked write path (§ 4.4), drift
// (§ 4.5) and the export file. The text edits themselves stay in
// FeedbackFile, unchanged.
#pragma once

#include "roadmapstore.h"

#include <QString>
#include <QVector>
#include <functional>

namespace FeedbackStore {

// § 4.2 — the export_path key: the canonical path of the file's directory
// joined with its name, so a file not yet created keys as it will once it
// exists.
QString exportKey(const QString &filePath);

// § 4.1 — the finding rows for a document: FeedbackFile::parse's per-finding
// list, numbered. Classifies nothing itself.
QVector<RoadmapStore::FeedbackFindingRow> rowsFor(const QString &content);

struct Result {
    QString code;              // "" on success; else a refusal code
    QString message;
    bool    found    = false;  // a row exists for this path (perhaps just imported)
    bool    imported = false;  // this call imported the file
    bool    created  = false;  // write: the row was created from createContent
    bool    changed  = false;  // write: content changed (or would, on a dry run)
    bool    drift    = false;  // read: the file no longer hashes to export_sha256
    QString content;           // read: stored text; write: the text after the edit
    qint64  reporterId = 0;    // read: the row, for its feedback_finding rows
    qint64  storeBytes = -1;   // reimport confirm_required: both versions' sizes
    qint64  fileBytes  = -1;
};

// A read: the stored text, importing the file on first touch. found:false
// when there is neither a row nor a file.
Result load(RoadmapStore &store, const QString &filePath);

// An op's text edit. `created` is true when `in` is the createContent of a
// reporter this call is creating. Return false to refuse; the op keeps its own
// refusal, and the write answers code "edit_refused".
using Edit = std::function<bool(const QString &in, bool created, QString *out)>;

// § 4.4. A non-empty createContent lets a path with no row and no file be
// created (append_finding); otherwise that answers code "not_found".
Result write(RoadmapStore &store, const QString &filePath,
             const QString &createContent, bool dryRun, const Edit &edit);

// § 4.5 — replace the stored text with the file's. Refuses confirm_required
// without `confirm`.
Result reimport(RoadmapStore &store, const QString &filePath, bool confirm);

// Test seam: make the export write fail, as a full disk would.
void setForceExportFailForTest(bool on);

}  // namespace FeedbackStore
