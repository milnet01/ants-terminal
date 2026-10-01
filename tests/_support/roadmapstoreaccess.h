// ANTS-3819 — tests' route to RoadmapStore's private raw handle. Schema,
// upgrade and concurrency tests assert the tables themselves, which is the
// job raw SQL is for; routing them through the typed surface would make them
// assert the surface instead.
//
//   #include "../../_support/roadmapstoreaccess.h"
//   QSqlQuery q(RoadmapStoreTestAccess::db(store));
#pragma once

#include "roadmapstore.h"

struct RoadmapStoreTestAccess {
    static QSqlDatabase &db(RoadmapStore &store) { return store.db(); }
};
