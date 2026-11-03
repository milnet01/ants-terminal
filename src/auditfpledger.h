// ANTS-1708 — fingerprint-keyed false-positive ledger for static-audit
// findings.
//
// The line-grain `.audit_suppress` (keyed by Finding::dedupKey, which hashes
// file:line:checkId:title) breaks the moment code shifts lines — the same
// drift that produced the 43 "spec↔code line-number" findings. This ledger
// keys on a *line-independent* content fingerprint so a learned false-positive
// stays suppressed across edits above it.
//
// Distinct from:
//   - `.audit_suppress`        — line-grain, per-finding (auditdialog owns).
//   - `baseline.json`          — "everything at commit X" (auditdialog owns).
//   - `.ants_review_falsepos.jsonl` — prose-grain, for the AI-review skills
//                                (falseposledger; the sibling this mirrors).
//
// Storage:  <projectRoot>/.audit_cache/learned-fp.jsonl  (JSONL, 0600).
// Spec:     tests/features/audit_fp_ledger/spec.md
//
// Pure (Qt6::Core, no Finding dependency) so it lives in ants_core_lib next
// to falseposledger. The Finding-aware suppression filter that consumes it
// lives in AuditEngine (ants_audit_lib).

#pragma once

#include <QList>
#include <QSet>
#include <QString>

#include <cstdint>
#include <optional>

namespace ants {
namespace auditfp {

struct Entry {
    QString fingerprint;  // 16 lowercase hex chars (line-independent)
    QString rule;         // audit rule / check id the finding came from
    QString reason;       // optional human note
    QString timestamp;    // ISO-8601; stamped on append

    bool isValid() const;  // 16-hex fingerprint present
};

// Line-INDEPENDENT content fingerprint. Strips a leading
// `<path>:<line>[:<col>]:` location prefix from `message` before hashing, so
// the fingerprint survives the line shifts that invalidate the line-grain
// dedupKey. Returns 16 lowercase hex chars. The same (file, checkId,
// location-stripped message) always maps to the same fingerprint regardless
// of which line the finding currently sits on.
QString computeFingerprint(const QString &file,
                           const QString &checkId,
                           const QString &message);

// Reads <projectPath>/.audit_cache/learned-fp.jsonl. Returns an empty list on
// a missing / non-regular file or any I/O error; malformed lines are skipped
// individually (a torn append at EOF costs one entry, never the whole file).
// A later row for a fingerprint supersedes the earlier one, in its place.
QList<Entry> loadEntries(const QString &projectPath);

QSet<QString> fingerprintSet(const QList<Entry> &entries);

// Append one learned-FP entry. mkpath's `.audit_cache`, sets 0600 on first
// write, and stamps `timestamp` when the caller left it empty. Dedups: if the
// fingerprint is already in the ledger this is a no-op that still returns
// true. Returns false only on a real write failure or an invalid entry.
bool appendEntry(const QString &projectPath, Entry e);

// ANTS-5603 — what recordEntries should do with one entry.
struct RecordRequest {
    Entry entry;
    bool replaceReason = false;  // a different reason supersedes the stored one
};

enum class RecordOutcome : std::uint8_t { Recorded, AlreadyRecorded, ReasonReplaced };

struct RecordResult {
    RecordOutcome outcome = RecordOutcome::Recorded;
    Entry entry;  // what the ledger holds for this fingerprint afterwards
};

// Applies `requests` in order, under one lock, with one read and one write, so
// a repeat inside the batch behaves as it would across calls. With `dryRun` it
// reports the same results and touches nothing. Returns nullopt on a write
// failure or an invalid entry, having written nothing for the invalid case.
std::optional<QList<RecordResult>> recordEntries(
    const QString &projectPath, const QList<RecordRequest> &requests,
    bool dryRun = false);

}  // namespace auditfp
}  // namespace ants
