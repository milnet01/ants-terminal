// ANTS-4501 § 2.2 — the one seam every path reads "today" through.
// Spec: docs/specs/ANTS-4501-roadmap-report.md
//
// This exists because two of that spec's invariants cannot fail without it.
// INV-5 asserts that writing to an already-shipped item does NOT move its
// `shipped` date, and INV-6 that reopening clears it — both need a *later*
// day than the one the item shipped on. Read straight from
// QDate::currentDate(), a test's two writes land on the same date, the
// assertion holds, and the clause passes against exactly the broken build it
// was written to catch.
//
// It lives in ants_roadmapparse_lib (Qt6::Core only) rather than beside either
// caller because BOTH sides need it: the stamping paths in the log verb and
// the report builder in the query verb. § 2.2 is explicit that a seam wired
// into stamping alone leaves the report's own bucket boundaries on the real
// calendar, so INV-1 and INV-8 would still flake at every period boundary.
#pragma once

#include <QDate>

namespace RoadmapClock {

// The local calendar date, formatted YYYY-MM-DD by callers to satisfy the
// item table's GLOB CHECK. Local and not UTC: a user asking "what did I close
// today?" means their day (§ 2.2).
QDate today();

// Test-only override, modelled on RemoteControl::setRoadmapHistoryCapForTest().
// An invalid QDate clears it and restores the real clock — which is what a
// fixture's teardown passes, so one test cannot leak its date into the next.
// Never reachable from a request: nothing in the verb dispatch calls it.
void setTodayForTest(const QDate &d);

// True while an override is in force. Exists so a test can assert its own
// teardown worked rather than trusting it.
bool todayIsOverridden();

}  // namespace RoadmapClock
