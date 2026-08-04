// ANTS-3793 — the read seam: one reader function, two backends.
// Spec: docs/specs/ANTS-3793-roadmap-consumer-cutover.md
//
// The store has been primary since ANTS-3756 and ANTS-3758 can write markdown
// back out of it, but until this file nothing READ it: every consumer parsed
// ROADMAP.md, so the store was a write-only copy that drifted the moment anyone
// edited the file.
//
// The cutover is not 26 rewrites. Every consumer read of bullet records already
// went through RoadmapParse::parseBullets(), so this adds a sibling PRODUCER of
// the same record type — sourced from the store — plus a resolver that picks
// between them. Each of the 26 sites swaps one call for its owner's wrapper.
//
// Qt6::Core + Qt6::Sql only, in ants_roadmapstore_lib, beside the render whose
// bulletText() § 2.1.1 is defined in terms of.

#pragma once

#include "roadmapparse.h"
#include "roadmapstore.h"

#include <QString>
#include <QVector>
#include <memory>
#include <optional>

namespace RoadmapSource {

using RoadmapParse::BulletRecord;

// Why the ceiling needs its own channel rather than an *error string:
// INV-3's refusal has to reach an MCP envelope as the code `too_large`, and
// RoadmapDialog — which is not a verb and emits no envelope — has to tell
// "too big, tell the user" apart from "the store is broken". A caller
// branching on error TEXT is a caller that breaks on a reworded message,
// which mcp-error-codes.md exists to forbid.
enum class ReadError {
    None,
    StoreFailed,        // the store file exists and will not open
    SourceUnrecognised, // migrated, but its ROADMAP.md is absent, empty
                        // or unparseable — the STORE is fine, and
                        // reporting this as StoreFailed would send the
                        // user to fix the wrong file (§ 2.2's table)
    TooLarge,
};

// INV-3 — the whole-project read ceiling, in items. Derived in § 4: ~4.5 KiB
// of records per item against a 16 MiB budget is ~3,678, rounded down. It is a
// runaway guard at ~2× the largest real project, not a working limit, and it is
// deliberately a COUNT rather than a byte total — the gate has to decide before
// materialising, which is the thing it exists to guard.
inline constexpr int kItemCeiling = 3500;

// Stat `defaultPath()`, and construct-and-open only if it is there. Returns an
// owned open store, or nullptr with `*why` set to None (no store on this
// machine — parse markdown) or StoreFailed (present, unopenable).
//
// It is a free function and not a wrapper member for one reason: INV-1's two
// unmigrated-project cases assert exactly this decision, and a member of
// RemoteControl or RoadmapDialog is not reachable from § 6's bundle. The
// wrapper still OWNS the decision — it is the only caller — but the decision is
// testable on its own.
std::unique_ptr<RoadmapStore> storeFor(const QString &defaultPath,
                                       ReadError *why,
                                       QString *error = nullptr);

// The records a consumer would have got from parsing this project's rendered
// markdown, sourced from the store instead. Document order (§ 2.1.3), in the
// SAME shape RoadmapParse::parseBullets returns.
//
// Two outcomes only, and NEITHER is a fallback: engaged with `*why` set to
// None, or nullopt with `*why` set to the reason (and `*error` carrying the
// human-readable detail). Reaching this function already means the project is
// migrated, so there is no "parse markdown instead" answer available to it.
// `why` is REQUIRED rather than defaulted, because a caller that drops it
// cannot tell TooLarge from StoreFailed, and it is written on EVERY path so a
// caller may branch on it first.
//
// `includeArchive` mirrors RoadmapDialog::loadMarkdown()'s flag of the same
// name and is REQUIRED for the reason § 2.1.2 gives: without it the store path
// spans archives the markdown path was told to skip, and the dialog's history
// toggle would stop mattering the moment a project migrated.
std::optional<QVector<BulletRecord>>
bulletsFromStore(RoadmapStore &store, qint64 projectId,
                 bool includeArchive, ReadError *why,
                 QString *error = nullptr);

// § 2.2's dispatch. Three outcomes, not two:
//   engaged               → migrated; read the store at this projectId
//   nullopt, *error empty → not migrated; parse markdown as today
//   nullopt, *error set   → REFUSE; never fall back (INV-1)
//
// `markdown` is REQUIRED and is the project's live roadmap text: § 2.2's
// ants-v1 gate runs detectRoadmapFormat() over it, and no store column records
// a source format. § 4 prices that retained read; ANTS-3815 is the column that
// would remove it.
//
// `why` is defaulted here and required on the two functions above, and the
// asymmetry is deliberate rather than an oversight. This function's refusals
// come in two kinds that send the user to different places — a roadmap that no
// longer looks like what the store says it is (SourceUnrecognised) and a store
// query that failed outright (StoreFailed) — so bulletsFor() has to be able to
// tell them apart to honour § 2.2's table. A caller content with "engaged or
// not" may still drop it.
std::optional<qint64> migratedProject(RoadmapStore &store,
                                      const QString &projectRoot,
                                      const QString &markdown,
                                      QString *error = nullptr,
                                      ReadError *why = nullptr);

// The library seam the two owner wrappers call — the two above are its halves,
// exposed because the tests drive them separately.
//
// Its three outcomes are the dispatch's, one layer on:
//   engaged               → the store's records (migrated project)
//   nullopt, *why == None → NOT migrated. The caller parses `markdown`
//                           itself, exactly as it does today. This is the
//                           common case and it is not an error.
//   nullopt, *why != None → REFUSE and surface it. Never parse.
//
// `markdown` is the caller's existing text, used by the gate above and by the
// caller on the unmigrated path. It is never re-read from disk here.
std::optional<QVector<BulletRecord>>
bulletsFor(RoadmapStore &store, const QString &projectRoot,
           const QString &markdown, bool includeArchive, ReadError *why,
           QString *error = nullptr);

} // namespace RoadmapSource
