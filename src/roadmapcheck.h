#pragma once

// ANTS-3810 § 2.2 — whole-store relationship acyclicity. The first member of
// the health-check family ANTS-3794 owns; that id adds the rest and the
// scheduling. Not placed beside ANTS-3793's reader seam: a graph check and a
// reader seam share nothing but a library.
//
// Contract: docs/specs/ANTS-3810-round-trip-oracle-and-acyclicity.md § 2.2.
// It reads through RoadmapStore's typed surface only — db() is private
// (ANTS-3819).

#include "roadmapstore.h"

#include <QPair>
#include <QString>
#include <QVector>

#include <optional>

namespace RoadmapCheck {

// One cycle, in path order, closing implicitly: `path` [A, B, C] means
// A → B → C → A. Rotated so path[0] is the smallest element, so the same
// cycle has one representation whatever order the walk reached it in.
struct RelationshipCycle {
    QString type;                             // one of the four acyclic types
    QVector<QPair<QString, QString>> path;    // (export_slug, id_fold), in order
};

struct AcyclicityReport {
    QVector<RelationshipCycle> cycles;  // empty ⇒ clean
    // ANY cross-project edge that does not resolve to an item in THIS store —
    // far project absent, or far project present but dst_id_fold matching no
    // item. One counter for both: the caller's response is the same (this
    // store cannot see the whole graph).
    int unresolvedEdges = 0;
    // True when ANY type hit kMaxCyclesPerType — "at least that many for some
    // type", not a floor of cycles.size().
    bool truncated = false;
};

inline constexpr int kMaxCyclesPerType = 64;

// Reports; never refuses, never writes. An engaged report with an empty
// `cycles` is a CLEAN store; `nullopt` with `*error` set is a FAILED check,
// and the two must not be collapsed.
std::optional<AcyclicityReport> findRelationshipCycles(RoadmapStore &store,
                                                       QString *error = nullptr);

}  // namespace RoadmapCheck
