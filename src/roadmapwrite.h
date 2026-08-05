// ANTS-3809 — the write half: mutate the store, render, then commit.
// Spec: docs/specs/ANTS-3809-roadmap-write-half.md
//
// ANTS-3793 moved the consumers' READS onto the store; their writes stayed on
// the markdown splice, so on a migrated project the read served the store, the
// write edited the file, and the next read served a store that never saw the
// edit. This is the other half: on a migrated project every `roadmap_log` op
// becomes mutate-the-store, render, commit, and no op grows a markdown writer
// of its own (INV-2).
//
// Qt6::Core + Qt6::Sql only, in ants_roadmapstore_lib beside roadmaprender.cpp,
// whose render() this drives. The callers are in ants_core_lib, which already
// has a PRIVATE edge here (ANTS-3793 § 4).

#pragma once

#include "roadmaprender.h"
#include "roadmapstore.h"

#include <QString>
#include <functional>

namespace RoadmapWrite {

// Every FAILING value below reaches an MCP envelope, so each names a `code`
// (docs/standards/mcp-error-codes.md): render_gate_unmet, render_failed,
// store_failed, and the taxonomy's existing write_failed. A caller must be able
// to branch on `code` alone, and all four have genuinely different remedies —
// fill in the missing Layman lines, fix the store's contents, fix the store
// file, re-run the render.
enum class Result {
    Ok,
    GateUnmet,     // the render's INV-5 gate  → `render_gate_unmet`
    RenderFailed,  // the render could not express the mutated store
                   //                          → `render_failed`
    StoreFailed,   // begin/commit/rollback, or the mutation itself, failed
                   //                          → `store_failed`
    PublishFailed, // committed, but the file write did not land
                   //                          → `write_failed` (existing)
};

// begin → mutate → dry render → commit → real render.
//
// One function, because the ordering is INV-1 and eight call sites would each
// get a chance to write it differently. And the ordering is not the obvious
// one: RoadmapRender::render() writes and commits its files itself, so a
// sequence that rendered before committing the store would leave the FILE ahead
// of the store when the store commit then failed — the wrong direction, because
// roadmap-data-model.md INV-3 makes the store primary. Options::dryRun is what
// makes the right sequence expressible: validate with a dry render, commit the
// store, publish with a real one.
//
// `mutate` performs the op's store writes and returns false on failure (setting
// *error). It is a callback rather than a shape this header enumerates because
// the eight ops have nothing in common but the transaction around them.
//
// Under `dryRun` the sequence stops after the dry render and rolls back — so a
// caller's `dry_run:true` commits nothing on EITHER path, which is what it
// already means on the markdown path (INV-7).
//
// `liveRoadmapPath` is resolved by the CALLER, from the same .ants/project.json
// `roadmap` override its markdown path already reads. RoadmapRender::Options
// requires it for the reason its own comment gives: this library does not link
// projectsettings.cpp and cannot read that file. Passed through; nothing is
// resolved here.
//
// `outcome` receives the render's own Outcome — the LAST render that ran, so
// the dry one under `dryRun` and the publishing one otherwise. It is the whole
// render result and not a bare gateFailures list because three separate
// obligations need the rest of it: GateUnmet's envelope needs `gateFailures`,
// `dry_run`'s preview envelope needs `filesWritten` / `itemsRendered` (the
// markdown path returns a would-be result today and this must not regress —
// INV-7), and PublishFailed's message needs `committed` + `filesWritten` to
// tell a total failure from ANTS-3758 § 2.7's partial commit. Left untouched
// when no render ran.
//
// The one window is PublishFailed: the store is committed and the file write
// did not land, so the store is ahead of the file. That is deliberate rather
// than a gap — the store is primary, a stale-behind file is what ANTS-3794's
// health check is for and what re-running the render fixes, whereas rolling the
// store back to match a file that may itself be half-written (ANTS-3758 § 2.7's
// partial commit) would discard the user's edit to match an artefact.
Result commitAndRender(RoadmapStore &store, qint64 projectId,
                       const QString &projectRoot,
                       const QString &liveRoadmapPath, bool dryRun,
                       const std::function<bool(QString *)> &mutate,
                       RoadmapRender::Outcome *outcome,
                       QString *error = nullptr);

} // namespace RoadmapWrite
