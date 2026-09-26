// ANTS-3765 — the roadmap migration load half: write the plan ANTS-3757
// produced into the store ANTS-3756 ships.
//
// Contract: docs/specs/ANTS-3765-roadmap-migration-load.md. Its § 2.1
// declarations are the single statement of shape and the types below are those
// declarations; every rule is stated in one of its sections and is cited here
// rather than restated.
//
// This lives in ants_roadmapstore_lib and not ants_core_lib, which is the seam
// made mechanical: the read half is in core precisely because it needs no
// database, and this half needs Qt6::Sql and the store. It never opens a
// ROADMAP.md — reading, parsing and planning are ANTS-3757's entirely.
//
// The whole surface is one function. One plan, one project, one transaction.

#pragma once

#include "roadmapmigrate.h"
#include "roadmapstore.h"

#include <QString>
#include <QVector>

namespace RoadmapMigrateLoad {

// What one plan's load did. A value, not a log: § 2.11 requires every outcome
// to be assertable by a test rather than read out of stderr.
struct Outcome {
    bool    ok = false;          // false ⇒ NOTHING for this project was committed
    QString error;               // set iff !ok; the first failure, not a list
    qint64  projectId = 0;

    int     itemsInserted = 0;
    int     itemsUpdated = 0;    // matched an existing row, at least one field changed
    // ANTS-4065 § 2.6 — the same count restricted to that spec's GOVERNED
    // columns (status, headline, kind, source, layman, body, lanes, evidence).
    // Both figures are reported because INV-6 reads this one and `itemsUpdated`
    // cannot serve: `extras` is deliberately excluded from the governed set
    // (the render emits none, so `source_kind` is one-shot by construction), and
    // an excluded column moving still bumps the row-level counter — which would
    // make the acceptance test fail for reasons unrelated to the contract.
    int     itemsUpdatedGoverned = 0;
    int     itemsUnchanged = 0;  // matched, nothing to write (§ 2.6)

    // ANTS-5258 — per plan item, WHICH stored row (if any) it claimed, so a
    // caller can preview a bulk id assignment instead of discovering it after
    // the rewrite. Parallel to the plan's items, one entry each, uncapped:
    // it is one small record per bullet and the convert's envelope does its
    // own capping.
    //
    // Why the MATCHED arm is the one worth previewing, which inverts the
    // obvious reading: a freshly allocated id collides with nothing, while a
    // wrong match writes an EXISTING id into the file and every prior citation
    // of that id then resolves to the wrong work. It is not reviewable
    // afterwards, because the output is well-formed either way.
    //
    // `matchedOrigin` is deliberately ABSENT. Both match passes in
    // matchItems() require `idFromMigration` on the candidate, so every
    // matched row is migration-allocated BY CONSTRUCTION — a field for it
    // could only ever hold one value, and a constant dressed as data invites a
    // reader to believe it was checked.
    struct ItemMatch {
        bool    matched = false;
        QString matchedId;         // the stored id claimed; empty when !matched
        QString matchedHeadline;   // the headline the match was made ON
        // Several stored rows satisfied the key and § 2.6.1 paired them BY
        // ORDER. The pairing is reproducible but rests on order alone, so this
        // is the one arm a human should actually check.
        bool    ambiguous = false;
        // ANTS-5329 — on an ambiguous match, the folded id of EVERY stored
        // row that satisfied the key at that point, the claimed one first.
        // Empty otherwise. A reviewer can then see what the order chose from.
        QStringList candidateIds;
    };
    QVector<ItemMatch> itemMatches;
    // ANTS-4479 (ANTS-3855 § 2.4) — WHICH items `itemsUpdated` counted, and
    // which columns moved. A dry run reporting `items_updated: 3` named no ids
    // and no fields, so a caller could not tell a reconciliation of real drift
    // from a lossy re-parse flattening good rows — and the only confirmation
    // available was to back the file up and diff afterwards.
    //
    // Capped at kMaxUpdatedItems where the entries are COLLECTED, so nothing
    // unbounded accumulates here; `itemsUpdated` stays the true total, so a
    // capped list can never read as the complete one. `id` is the STORED id,
    // which is the plan's when it carries one and the row's when it does not.
    struct UpdatedItem {
        QString     id;
        QStringList fields;      // store column names, in write order
        // ANTS-4522 — columns the plan differed on where the write was
        // DECLINED ("defaulted does not overwrite", "empty does not
        // overwrite"). Each also raises a `field_conflict` note, and both are
        // filled from the same branches of the same loop, so the plan and the
        // notes cannot disagree — the defect this closes was a notes[] entry
        // naming a column `fields` omitted.
        //
        // Separate from `fields` on purpose: `fields` means "columns this
        // write moves". Folding the two together, as the report suggested,
        // would have the preview claim a change that is not going to happen.
        QStringList fieldsSuppressed;
    };
    QVector<UpdatedItem> updatedItems;
    // ANTS-4522 — entries the cap dropped. Replaces deriving truncation from
    // `itemsUpdated > updatedItems.size()`, which stopped holding the moment a
    // suppression-only item could enter the array without being an update.
    int     updatedItemsDropped = 0;
    int     itemsOrphaned = 0;   // in the store, absent from source (§ 2.7)
    int     idsAllocated = 0;    // § 2.8
    // INSERTED-or-UPDATED rows, not attempted ones: `sectionsWritten` counts a
    // section created or whose title/level/intro/parent/source_path changed
    // (`source_path` by ANTS-3782 § 2.2 — a section whose only change is its
    // provenance DID change, and a re-run reporting it unchanged would make the
    // one column that spec adds the one column this outcome cannot see),
    // `elementsWritten` every element row re-inserted by § 2.6's rebuild (so it
    // is non-zero even on an unchanged re-run), `historyRows` the rows § 2.9
    // appended. INV-13 compares all three between a dry run and the real one.
    int     sectionsWritten = 0, elementsWritten = 0, historyRows = 0;
    // ANTS-4490 (ANTS-3855 § 2.4) — `sectionsWritten`'s partner. `0 written`
    // alone is illegible: it is the proof of an idempotent re-run and reads as
    // a counter that never moved. Every HEADED plan section takes one branch or
    // the other, so the two sum to the plan's headed-section count on every run.
    int     sectionsUnchanged = 0;
    // ANTS-5355 — the level-0 section carrying the title and preamble is not a
    // heading, and roadmap_query's section_index does not list it. Counting it
    // in the two figures above made them disagree with that index (Groundwork:
    // sections_written 2 against one listed section), so it is reported here.
    bool    preambleWritten = false;

    // ANTS-4483 — the ids the render's INV-5 Layman gate would refuse once this
    // migration has landed. Measured INSIDE the transaction, which is the whole
    // point: after load() returns, a dry run has already rolled back, so a
    // check there would answer about the state the migration REPLACES rather
    // than the one it is previewing — the one question this exists to answer.
    //
    // The gate is the render's own, reached by a scopeless dry render, and not
    // a second copy of the predicate. A duplicate would be free to drift from
    // the rule it reports on, and reporting the wrong rule confidently is worse
    // than not reporting.
    //
    // `gateChecked` false means NOBODY LOOKED — the render could not run — and
    // is not the same answer as an empty `gateFailures`, which means the
    // project renders. Same distinction `externalEditsChecked` draws in
    // RoadmapRender::Outcome, for the same reason.
    QStringList gateFailures;
    bool        gateChecked = false;

    // Every note the plan carried, plus the ones only the load can raise
    // (§ 2.11's codes). Never a SUBSET of the plan's notes — a plan note is
    // never dropped — so one report covers the whole migration of one project.
    QVector<RoadmapMigrate::Note> notes;
};

// The clock is a PARAMETER, not a call. `history.changed_at` CHECKs a full
// ISO-8601 Z timestamp, so a load that read the clock itself would produce a
// different store on every run and INV-2's re-run comparison could not be
// written. The caller stamps once per migration, not once per row.
struct Options {
    // Required, and validated BEFORE the transaction opens: `history.changed_at`
    // CHECKs this exact shape, so an ill-formed stamp would otherwise surface
    // as a rolled-back project at the first re-run update rather than as a
    // refusal. A malformed value refuses with the `bad_options` note and writes
    // nothing.
    QString changedAt;           // "YYYY-MM-DDTHH:MM:SSZ"
    // Passed to registerProject(), WHICH CANONICALISES IT (ANTS-3756 INV-8) —
    // the caller supplies the root it was given, not a pre-canonicalised path.
    // A root that cannot be canonicalised is that method's refusal, not this
    // one's, and it aborts the load like any other write failure.
    QString projectRoot;
    // ANTS-3771 § 2.3 — the project's DECLARED id format. Only `prefix` is read
    // here, and it is inserted ABOVE all three of § 2.8 step 1's existing terms.
    // Without that, one declaring project takes its declared prefix from
    // roadmap_log and a sniffed or directory-derived one from a migration
    // allocation — two id families in one store, which is the outcome
    // UNIQUE (project_id, id_fold) cannot even detect.
    //
    // Supplied by the caller rather than loaded here: ProjectSettings is in
    // ants_core_lib and this library deliberately does not link it.
    RoadmapParse::IdFormat idFormat;
    bool    dryRun = false;      // plan the writes, roll back instead of commit
    // ANTS-4491 § 4.3 — run inside a transaction the CALLER already opened,
    // rather than opening one here. `RoadmapStore::begin()` refuses to nest, so
    // without this a load cannot run inside `RoadmapWrite::commitAndRender()`'s
    // `mutate()` step — which is exactly what the dialect convert must do,
    // because INV-4 requires its id allocation and its commit to be one
    // transaction.
    //
    // When true this load performs NO begin, NO commit and NO rollback: a
    // failure returns the refusal and leaves the transaction open for the owner
    // to roll back. Rolling back here would tear down writes the owner made
    // before calling, and commit is the owner's to decide because it owns the
    // steps that follow. `dryRun` is therefore refused alongside it — a dry run
    // IS the rollback, which a borrower cannot perform.
    bool    borrowTransaction = false;
};

// One plan, one project, one transaction (§ 2.5). `store` must be open on an
// Access::Bulk connection (§ 2.2); a load on an Interactive one is REFUSED
// rather than run slowly (INV-12).
[[nodiscard]] Outcome load(RoadmapStore &store, const RoadmapMigrate::MigrationPlan &plan,
                           const Options &opts);

}  // namespace RoadmapMigrateLoad
