// ANTS-3761 — the roadmap export: record emission, the id sort, and the write
// path. Spec: docs/specs/ANTS-3761-roadmap-export-format.md §§ 2.3–2.6.
//
// The export is the DURABLE RECORD (roadmap-data-model.md § 1): the store is
// untracked and the published render is lossy, so this file is the only
// complete copy that survives a lost disk. INV-1 therefore states the round
// trip — export, rebuild, re-export, byte-identical — which is why the reader
// lives here beside the writer rather than in the migration spec: they are two
// halves of one contract and neither is testable alone.
#pragma once

#include <QString>

class QIODevice;
class RoadmapStore;

namespace RoadmapExport {

// ANTS-3781 § 2.3 — the JSONL RECORD shape, which is not the store's table
// shape. Held apart from the store's own table-version constant (ANTS-3796
// § 2.4 handed the split here) so a store-side bump does not invalidate every
// export ever written — an export is the store's only rebuild path, and
// refusing one leaves no recovery route. Equal to 1 today because both started
// there; they move independently now.
//
// This comment names the store's constant DESCRIPTIVELY rather than by spelling
// it: ANTS-3781 INV-6 asserts zero occurrences of that token anywhere in either
// export translation unit, comments included, and a comment is exactly where a
// re-weld would hide.
//
// `inline` because a namespace-scope `constexpr` in a header has internal
// linkage, giving every TU its own object; `inline constexpr` is the C++17
// idiom for a header constant with one identity.
inline constexpr int kExportSchemaVersion = 1;

// Stream one project's records into `out`, in the order § 2.4 pins. Streaming
// is INV-12: the peak RSS delta across this call stays under 4 MiB however
// large the export, so the two unbounded tables — `item.body` and `history` —
// are read one row at a time rather than collected.
//
// Reads inside one deferred transaction (§ 2.6): the export spans many
// statements, and a commit landing mid-run would tear the file into half
// pre-change and half post-change.
//
// The caller chooses the connection profile. ANTS-3756 § 2.5 assigns the
// export `Access::Bulk` — a 30 s deadline, because an export knows it may
// queue behind a long transaction — but that is fixed when the store is
// constructed, so this function documents the requirement rather than
// enforcing it.
bool writeProject(RoadmapStore &store, const QString &exportSlug, QIODevice *out,
                  QString *error = nullptr);

// The full write path (§ 2.6): acquire ConfigWriteLock, write a temp file,
// rename(2) into place, all inside the lock's scope. On a failed acquire this
// ABORTS and reports — the model's § 9 makes a silent backup failure worse
// than no backup, because it stops anyone checking (INV-9).
bool exportProject(RoadmapStore &store, const QString &exportSlug,
                   const QString &destPath, QString *error = nullptr);

// The rebuild half of INV-1: read one project's export from `in` into `store`,
// which must be open and must not already hold that project. Single pass — the
// § 2.4 record order guarantees every reference is declared before it is used,
// which is the reason `section` precedes `element` and `item` precedes both.
bool rebuildProject(RoadmapStore &store, QIODevice *in, QString *error = nullptr);

}  // namespace RoadmapExport
