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

namespace RoadmapMigrateVerb {

struct Request {
    QString projectRoot;      // already canonical (§ 2.3's precondition)
    QString projectName, exportSlug;
    // The caller's single stamp. run() never reads a clock — ANTS-3765
    // § 2.1's "the clock is a PARAMETER, not a call", held one layer out, so
    // a test can pin `changed_at` by passing it.
    QString changedAt;
    bool    dryRun = false;
};

// Returns the success envelope, or a refusal carrying `code`. Opens its OWN
// Access::Bulk store at `storePath` for the duration of the call and releases
// it before returning (§ 2.2).
QJsonObject run(const QString &storePath, const Request &req);

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
