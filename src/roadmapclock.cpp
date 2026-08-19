// ANTS-4501 § 2.2 — see roadmapclock.h for why this seam exists at all.
#include "roadmapclock.h"

namespace RoadmapClock {
namespace {

// Invalid = no override, which is the shipped state. A QDate default-constructs
// invalid, so the override is off until something sets it and off again the
// moment a test clears it — no separate bool to keep in step.
QDate g_override;

}  // namespace

QDate today() {
    return g_override.isValid() ? g_override : QDate::currentDate();
}

void setTodayForTest(const QDate &d) {
    g_override = d;
}

bool todayIsOverridden() {
    return g_override.isValid();
}

}  // namespace RoadmapClock
