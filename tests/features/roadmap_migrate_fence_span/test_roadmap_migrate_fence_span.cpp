// Feature-conformance test for ANTS-4403 — a multi-backtick inline span must
// not open a fence in the migration walk.
// Contract: tests/features/roadmap_migrate_fence_span/spec.md (INV-1..5).
//
// Behavioural, against ants_core_lib, driving inline documents: every case is
// one line of prose away from its neighbour, so the case under test is visible
// beside its assertion.

#include <gtest/gtest.h>

#include "roadmapmigrate.h"

#include <QString>
#include <QStringList>

using RoadmapMigrate::Discovery;
using RoadmapMigrate::MigrationPlan;
using RoadmapMigrate::PlannedItem;

namespace {

// planFrom over one inline ants-v1 document. Discovery is a plain struct and
// planFrom is the pure half, so no file and no temp dir is needed.
MigrationPlan planText(const QString &markdown) {
    RoadmapMigrate::Source src;
    src.path     = QStringLiteral("ROADMAP.md");
    src.markdown = markdown;
    src.format   = QStringLiteral("ants-v1");
    Discovery disc;
    disc.sources.append(src);
    return RoadmapMigrate::planFrom(disc, QStringLiteral("fence-span"),
                                    QStringLiteral("fence-span"));
}

bool hasItem(const MigrationPlan &plan, const char *id) {
    const QString want = QString::fromUtf8(id);
    for (const PlannedItem &it : plan.items)
        if (it.id == want) return true;
    return false;
}

}  // namespace

// INV-1 — a backtick run followed by a backtick is an inline span, not a fence
// opener, so the bullets after it are still planned. Before the fix the walk
// masked everything below the span line and ANTS-4403b vanished silently.
TEST(RoadmapMigrateFenceSpan, MultiBacktickSpanDoesNotOpenAFence) {
    const auto plan = planText(QStringLiteral(R"MD(## Work

- 📋 [ANTS-4403a] **Before the span.**
  Body.
- 📋 [ANTS-4403b] **After the span.**
  Why their specs are not simply retagged: every spec there writes patterns in
  ```` ```python ```` because that is what `ruff format` formats and CI gates.
- 📋 [ANTS-4403c] **Last.**
  Body.
)MD"));

    EXPECT_TRUE(hasItem(plan, "ANTS-4403a"));
    EXPECT_TRUE(hasItem(plan, "ANTS-4403b"));
    EXPECT_TRUE(hasItem(plan, "ANTS-4403c"))
        << "a multi-backtick inline span opened a fence and masked the rest of "
           "the document";
    EXPECT_EQ(plan.items.size(), 3);
}

// INV-2 — a REAL fence still masks. The fix must not buy INV-1 by weakening the
// thing fences are for.
TEST(RoadmapMigrateFenceSpan, RealFenceStillMasksItsContents) {
    const auto plan = planText(QStringLiteral(R"MD(## Work

- 📋 [ANTS-4403a] **Real.**
  Body.

```
- 📋 [ANTS-4403x] **Inside a fence — a sample, not a bullet.**
```

- 📋 [ANTS-4403c] **After the fence.**
  Body.
)MD"));

    EXPECT_TRUE(hasItem(plan, "ANTS-4403a"));
    EXPECT_FALSE(hasItem(plan, "ANTS-4403x"))
        << "a bullet inside a fenced code block was planned as a real item";
    EXPECT_TRUE(hasItem(plan, "ANTS-4403c"));
    EXPECT_EQ(plan.items.size(), 2);
}

// INV-3 — the loss is silent, so the guard is a COUNT: adding the span line
// changes no item. This is the shape the real defect took on ROADMAP.md, where
// the plan was well-formed and simply 481 items short.
TEST(RoadmapMigrateFenceSpan, SpanLineChangesNoItemCount) {
    const QString withoutSpan = QStringLiteral(R"MD(## Work

- 📋 [ANTS-4403a] **One.**
  Ordinary body prose.
- 📋 [ANTS-4403b] **Two.**
  Body.
- 📋 [ANTS-4403c] **Three.**
  Body.
)MD");
    const QString withSpan = QStringLiteral(R"MD(## Work

- 📋 [ANTS-4403a] **One.**
  ```` ```python ```` is how a doc quotes fence syntax.
- 📋 [ANTS-4403b] **Two.**
  Body.
- 📋 [ANTS-4403c] **Three.**
  Body.
)MD");

    const auto a = planText(withoutSpan);
    const auto b = planText(withSpan);
    EXPECT_EQ(a.items.size(), b.items.size());
    EXPECT_EQ(a.sections.size(), b.sections.size());
}

// INV-4 — a fence indented past the top-level three-space allowance, but within
// its enclosing list item's content column, still masks (ANTS-3638). ROADMAP.md
// carries fences at indent 5 and 6 under bullets, so a fix that simply tightened
// the indent rule would trade one silent loss for another.
TEST(RoadmapMigrateFenceSpan, IndentedFenceUnderABulletStillMasks) {
    const auto plan = planText(QStringLiteral(R"MD(## Work

- 📋 [ANTS-4403a] **Carries an indented sample.**
  Body.

     ```
     - 📋 [ANTS-4403x] **Sample text, not a bullet.**
     ```

- 📋 [ANTS-4403c] **After.**
  Body.
)MD"));

    EXPECT_TRUE(hasItem(plan, "ANTS-4403a"));
    EXPECT_FALSE(hasItem(plan, "ANTS-4403x"))
        << "an indented fence under a list item stopped masking";
    EXPECT_TRUE(hasItem(plan, "ANTS-4403c"));
}

// INV-5 — a fence is closed only by a line opening with the SAME fence
// character. The hand-rolled walk paired any delimiter with any other.
TEST(RoadmapMigrateFenceSpan, BacktickFenceIsNotClosedByATildeLine) {
    const auto plan = planText(QStringLiteral(R"MD(## Work

- 📋 [ANTS-4403a] **Before.**
  Body.

```
~~~
- 📋 [ANTS-4403x] **Still inside the backtick fence.**
```

- 📋 [ANTS-4403c] **After.**
  Body.
)MD"));

    EXPECT_TRUE(hasItem(plan, "ANTS-4403a"));
    EXPECT_FALSE(hasItem(plan, "ANTS-4403x"))
        << "a ~~~ line closed a ``` fence";
    EXPECT_TRUE(hasItem(plan, "ANTS-4403c"));
}
