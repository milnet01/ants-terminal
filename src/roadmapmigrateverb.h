// ANTS-3855 — the testable seam behind the `roadmap_migrate` MCP verb.
// Contract: docs/specs/ANTS-3855-roadmap-migrate-verb.md, § 2.1.1.
//
// The handler (RemoteControl::cmdRoadmapMigrate, in
// src/remotecontrol_roadmap_migrate.cpp) resolves the caller's root, reads the
// clock once, and resolves RoadmapStore::defaultPath(). Everything that happens
// to a store lives below, in a free function.
//
// SEPARATE TRANSLATION UNIT from that handler, and that is a LINK-TIME
// requirement rather than tidiness. A static archive is pulled in at object
// granularity, so a seam sharing an object with the handler would drag
// RemoteControl -> ants::resolveCallerCwdRoot -> MainWindow into every bundle
// that linked it — and test_core links ants_core_lib ALONE, with no
// ants_chrome_lib (CMakeLists.txt's `_ants_subset_linked_libs`). Measured
// 2026-08-06: the one-TU shape failed to link test_core with ~20 undefined
// MainWindow / ClaudeIntegration / AuditEngine symbols. A seam that cannot be
// linked without the thing it exists to be tested apart from is not a seam.
//
// `storePath` is a PARAMETER and not RoadmapStore::defaultPath() for one
// reason: without it this verb has no test at all. defaultPath() resolves
// under XDG_DATA_HOME with no override, so a test driving a handler that
// called it directly would migrate into the developer's real store — which is
// exactly how ANTS-3856's leaked fixture row got there. Being a free function
// taking a path, the signature also enforces § 2.2's other rule: run() cannot
// reach RemoteControl's process-owned Access::Interactive connection, which
// RoadmapMigrateLoad::load() refuses outright (ANTS-3765 INV-12).
//
// Same shape, and for the same reason, as RoadmapSource::storeFor()
// (ANTS-3793 § 2.2): the handler still OWNS which store is migrated — it is
// the only caller — but everything that happens to it is testable on its own.

#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

class RoadmapStore;   // ANTS-4491 — loadInOpenTransaction() takes the caller's
                      // OPEN store by reference; the definition is not needed here.

namespace RoadmapMigrateVerb {

struct Request {
    QString projectRoot;      // already canonical (§ 2.3's precondition)
    QString projectName, exportSlug;
    // The caller's single stamp. run() never reads a clock — ANTS-3765
    // § 2.1's "the clock is a PARAMETER, not a call", held one layer out, so
    // a test can pin `changed_at` by passing it.
    QString changedAt;
    bool    dryRun = false;
    // ANTS-4559 — § 2.4's row bound for notes[], forwarded verbatim from the
    // verb's `max_notes` argument. run() applies the [1, 2000] clamp, so a
    // test driving this seam directly is bounded identically to a live call.
    int     maxNotes = 200;
    // ANTS-4499 — the pre-migration snapshot of the machine-global store.
    //
    // On by default, because the session that most needs a backup is the one
    // that did not know to ask for it. `backup:false` is the caller saying
    // otherwise in so many words: a FAILED snapshot refuses the migration
    // (INV-8), since rewriting hundreds of rows in a shared store with no way
    // back is the thing this exists to prevent, and a warning nobody reads is
    // not protection.
    //
    // Empty `backupTo` means RoadmapStore::defaultSnapshotPath(). A dry run
    // takes none at all (INV-7): it commits nothing, so there is nothing to
    // protect and a rolling snapshot spent on a preview would overwrite the one
    // taken before the last real run.
    bool    backup = true;
    QString backupTo;
};

// Returns the success envelope, or a refusal carrying `code`. Opens its OWN
// Access::Bulk store at `storePath` for the duration of the call and releases
// it before returning (§ 2.2).
QJsonObject run(const QString &storePath, const Request &req);

// ANTS-4491 § 4.3 — the migration half of a dialect convert, run inside a
// transaction the CALLER has already opened.
//
// It lives here, and not in the convert's own TU, because INV-1 is a real
// contract: `findRoadmaps`, `planFrom` and `load` have exactly one production
// call site under src/, and this file is it. A second caller open-coding the
// three would be a second migration implementation, which is the drift that
// invariant exists to prevent — so the convert asks the seam instead.
//
// It takes the caller's OPEN store rather than a path, which is the whole
// point: `RoadmapStore::begin()` refuses to nest, and ANTS-4491 INV-4 requires
// the id allocation and the commit to be one transaction. `run()` above cannot
// serve that — it opens its own connection and owns its own transaction.
//
// The store must be Access::Bulk and already in a transaction; the load refuses
// otherwise. On failure NOTHING is rolled back here: the caller owns the
// transaction and may have written before calling.
// ANTS-5252 — one row per bullet the convert will write, so a bulk id
// assignment can be REVIEWED before it lands rather than counted after.
//
// Asked for by Vestige, who put the case better than the aggregate does: a
// convert is a one-way bulk rewrite of a version-controlled PUBLIC file, and it
// moves a counter that other documents — CHANGELOG entries, specs, commit
// bodies — cite by id. An aggregate count cannot be checked against anything.
// A per-bullet list can be read against the file.
//
// The asymmetry that makes `inFile` the field to scan: a NEWLY ASSIGNED id is
// visible and fixable, a CHANGED one is invisible and permanent. Both Vestige
// and the ~/.claude session reached that independently.
//
// Nothing here is derived — every field is already decided at the point the
// plan is built.
struct PlannedId {
    QString id;
    // "parsed" | "synthesised" | "quarantined" | "absent".
    //
    // What the FILE says, which is what this report can defend. "absent" is
    // this struct's own and means the bullet carries no id in the file: the
    // convert will either issue a fresh one OR match an existing store item by
    // headline (INV-6), and which of the two happens is decided by the load,
    // after this row is built. It is deliberately NOT called "allocated" —
    // that would assert an allocation this row cannot know happened, on the
    // one field a reviewer is trusting. `ids.allocated_ids[]` names the ids
    // actually issued.
    QString origin;
    // Adopted from a bold prose lead-in rather than read from a bracket.
    bool    inferred = false;
    // Does this id appear in the roadmap TODAY? True for anything the reader
    // took from the file, false for one this convert invents. Vestige's
    // "did anything change identity" question.
    bool    inFile   = false;
    // Whether the item carries a Layman line. The convert no longer refuses
    // over this (ANTS-5256) but the first ordinary edit to the item will, so
    // the reviewer should see it here rather than discover it later.
    bool    hasLayman = false;
    int     firstLine = 0;   // 1-based, in the source roadmap

    // ANTS-5258 — what the LOAD did with this bullet, filled after the load
    // runs. `origin` above says what the FILE holds; these say what became of
    // it, which is the half INV-10 deliberately refused to guess at.
    //
    // The matched arm is the one to review. A freshly allocated id collides
    // with nothing; a wrong match writes an EXISTING id into the file and
    // every prior citation of that id resolves to the wrong work — and the
    // output is well-formed either way, so it is not reviewable afterwards.
    bool    matched = false;
    QString matchedId;        // empty unless matched
    QString matchedHeadline;  // the headline the match was made ON
    // Several stored rows satisfied the key and were paired BY ORDER.
    // Reproducible, but resting on order alone — check these by hand.
    bool    ambiguous = false;
};

struct InTransactionLoad {
    bool        ok = false;
    QString     error;
    qint64      idsAllocated  = 0;
    QStringList allocatedIds;    // capped by the caller's own budget
    int         bulletsTotal = 0;
    int         idsParsed    = 0;
    // ANTS-5252 — capped by the caller's budget like allocatedIds. `bulletsTotal`
    // carries the true count either way, so a truncated list is always
    // detectable rather than merely suspected.
    QVector<PlannedId> plannedIds;
    // ANTS-5326 — store items the source no longer carries. The TRUE count,
    // and their ids case-folded as the loader notes them (the store's id_fold),
    // capped by the caller's budget.
    int         itemsOrphaned = 0;
    QStringList orphanedIdFolds;
};
InTransactionLoad loadInOpenTransaction(RoadmapStore &store,
                                        const QString &projectRoot,
                                        const QString &name,
                                        const QString &exportSlug,
                                        const QString &changedAt,
                                        int maxEchoedIds);

// ANTS-4617 — the inverse, and the catalogue had none. Migrating a scratch copy
// to test a destructive render in isolation is the careful instinct, and it left
// a permanent row that `roadmap_query mode:"report" scope:"all"` sums into
// machine-wide figures forever. Distinct from ANTS-4600's `transient_root`
// guard, which stops a scratchpad under the SYSTEM TEMP DIR being registered at
// all: the root that prompted this was not under the temp dir, so that guard
// does not fire and the row is legitimate at write time.
//
// Keyed by root OR export_slug, because the two callers know different things:
// a session in the project has the root, and someone pruning a project whose
// files are already gone has only the slug the store lists.
//
// The GUARD is the whole design. Deregistering a LIVE project is data loss with
// no undo — the store is primary and the file is a render — so it refuses
// unless the root no longer exists on disk, or the caller passes `confirm`.
// Absent-root is the case this was filed for and needs no ceremony; anything
// else must be asked for in so many words.
struct DeregisterRequest {
    QString projectRoot;   // already canonical; may be empty if slug is given
    QString exportSlug;    // alternative key
    bool    confirm = false;
    bool    dryRun  = false;
    // ANTS-5086 — called with the STORED root of the row about to be deleted,
    // after it is found and before anything is deleted, on a real run only.
    // Returning false refuses `roadmap_busy`. The row's root, not the caller's:
    // keying on export_slug can name another project (ANTS-2132 § 2.10).
    std::function<bool(const QString &rowRoot)> holdRoot;
};
QJsonObject deregister(const QString &storePath, const DeregisterRequest &req);

// § 2.1 — the DERIVED DEFAULT for `export_slug`: lowercase, every run of
// non-[a-z0-9] to a single `-`, leading/trailing `-` stripped. Ants_Terminal →
// ants-terminal. Declared here so the handler shares the one definition rather
// than carrying a second copy of the rule.
//
// Slugification applies to the derived default ONLY. A caller-supplied slug is
// validated verbatim by run() and never rewritten: silently reshaping an
// argument the caller chose would put a value in the store that differs from
// the one they passed, and they have an export path keyed on it.
QString defaultExportSlug(const QString &leafDirName);

// ANTS-4600, § 2.5 step 0b — is `canonicalRoot` a TRANSIENT location, one no
// durable project may be registered from? Declared here on defaultExportSlug()'s
// pattern: the handler is the caller, the policy belongs to the verb, and
// test_core can link this TU but not RemoteControl's (see the note above).
//
// `canonicalRoot` must already be canonical, as Request::projectRoot is.
bool isTransientRoot(const QString &canonicalRoot);

// ANTS-4740 — the ants-v1 skeleton op:"init" writes for a project that has no
// roadmap at all. Here, beside the two helpers above, for the reason this whole
// seam exists: a test can link it without dragging RemoteControl and MainWindow
// in behind it, and the file this returns is the one thing about the bootstrap
// that has to be exactly right — a mis-formed skeleton migrates cleanly and
// silently drops fields.
//
// Carries ONE section, because roadmap_log's `section` argument needs a slug to
// target and its op:"create_section" needs an `after_section`: a sectionless
// skeleton would be unappendable by either route, which is the state this op
// exists to escape.
QString initSkeleton(const QString &projectName);

}  // namespace RoadmapMigrateVerb
