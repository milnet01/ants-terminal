// ANTS-1287: heading-index engine for ROADMAP.md slice queries.
// Pure (Qt6::Core-only); lives in ants_core_lib. See
// docs/specs/ANTS-1287.md.

#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace RoadmapIndex {

struct Section {
    QString slug;
    QString headingText;
    int level     = 0;   // 2 for `##`, 3 for `###`
    int lineStart = -1;  // 0-indexed line of the heading itself
    int lineEnd   = -1;  // 0-indexed, exclusive end of the section
};

// Walk every line of `markdown`; emit one Section per `##` / `###`
// heading. H1 and H4 are not indexed (INV-3 — parity with parseBullets,
// which only tracks H2/H3 in currentSectionSlug). lineEnd[i] is the
// first subsequent heading line with level <= level[i], or
// total_lines for the last section.
QVector<Section> buildIndex(const QString &markdown);

// Linear lookup by slug. Returns nullptr if no match.
const Section *findBySlug(const QVector<Section> &index, const QString &slug);

// Returns the substring covering lines [section.lineStart,
// section.lineEnd), joined with '\n'. First line is the heading.
QString sliceSection(const QString &markdown, const Section &section);

// ANTS-4819 — the slugs a `section=` read must cover: this section's own,
// plus every section nested inside it. Nesting is [lineStart, lineEnd]
// containment, the same relation `rollupCounts` below walks, so a caller
// that reads a section and a caller that reads the index agree by
// construction rather than by two rules that happen to coincide.
//
// The markdown backend never needed it: `sliceSection` returns the whole
// span, so a parent's slice already carries its children's bullets. The
// store backend holds records keyed on one slug each, and filtering on the
// section's own slug answered nothing for every parent heading while
// `section_index`, rolling those same children up, promised their bullets.
QStringList descendantSlugs(const QVector<Section> &index,
                            const Section &section);

// Canonical home for the slug helpers — moved out of
// roadmapdialog.cpp so the index and parseBullets share a single
// implementation (INV-4).
int     headingLevel(const QString &raw, QString *text = nullptr);
QString slugifyHeading(const QString &heading);
QString uniqueSlug(QSet<QString> &seen, const QString &heading);

// ANTS-1688 — shared canonical-ID predicate. True iff `id` matches a
// `[A-Za-z][A-Za-z0-9_-]*-<digits>` token (e.g. ANTS-1688, VEST-0042)
// — the only ID shape roadmap_log allocates. Synthetic content-hash
// nonces (10-char base36 like `35ra39wbn1`), Obsidian `^anchor`
// tokens, and legacy hyphen-less bold IDs (`Sh4`) are NOT canonical.
// The duplicate-ID detector keys on this so anchors/hashes can't
// masquerade as ID collisions (ANTS-1784 single-source ID-token regex).
bool    isCanonicalId(const QString &id);

// ANTS-1442 — per-section tally used by roadmap_query's section_index
// mode. Same shape as the `{active,shipped,total}` triplet the verb
// emits.
//
// ANTS-1622 — `*WithId` parallels mirror the default bullets[] filter
// predicate: a bullet whose `id` is empty (narrator prose, rollup, or
// legacy GFM-task-list line without an [PROJ-NNNN] token) doesn't
// contribute. Section_index emits both shapes side-by-side so callers
// reading `active_count` AND `active_count_id_only` see at-a-glance
// when a section's actionable bullets would survive the default
// bullets[] predicate vs when they wouldn't (the legacy-format
// disagreement case the bug report hit).
struct SectionCounts {
    int active        = 0;
    int shipped       = 0;
    int total         = 0;
    int activeWithId  = 0;
    int shippedWithId = 0;
    int totalWithId   = 0;

    // ANTS-1693 — field-wise accumulate so the generic `rollupCounts`
    // tree-walk (below) can bubble a child section's tally into its
    // ancestors without a bespoke `addInto`.
    SectionCounts &operator+=(const SectionCounts &o) {
        active        += o.active;
        shipped       += o.shipped;
        total         += o.total;
        activeWithId  += o.activeWithId;
        shippedWithId += o.shippedWithId;
        totalWithId   += o.totalWithId;
        return *this;
    }
};

// ANTS-1442 — given direct per-slug tallies (bullets keyed by their
// immediate containing heading slug) and the section index, return a
// tally per slug where each entry sums self + descendant sections.
// `direct` carries only sections whose immediate bullets contributed
// at least one count; the returned hash includes every section in
// `index` (level-2 parents pick up their level-3 children even when
// the level-2 heading has no direct bullets). Pure / Qt-Core-only.
// ANTS-1813 — implemented as a single linear stack pass over the
// section list (ANTS-1783), NOT the original O(n²) all-pairs
// containment scan; the header previously mis-described the cost.
//
// ANTS-1693 — generic over the count shape. The tree-walk (the
// load-bearing part) is identical whatever the tally struct; only the
// per-field sum differs, so `Counts` need only supply
// `Counts &operator+=(const Counts &)`. roadmap_query passes
// RoadmapIndex::SectionCounts; RoadmapDialog passes its own
// per-lifecycle chip tally — one algorithm, so the dialog's parent
// chips can no longer drift from the MCP's rollup.
template <class Counts>
QHash<QString, Counts> rollupCounts(
    const QVector<Section> &index,
    const QHash<QString, Counts> &direct) {
    QHash<QString, Counts> out;
    out.reserve(index.size());

    // Sections form a tree by [lineStart, lineEnd] containment; walking
    // them in lineStart order with a stack of open ancestors lets each
    // section's direct counts bubble up to every ancestor in amortised
    // O(1). A copy is sorted first so we don't depend on buildIndex's
    // emission order.
    QVector<const Section *> ordered;
    ordered.reserve(index.size());
    for (const auto &s : index) ordered.append(&s);
    std::sort(ordered.begin(), ordered.end(),
              [](const Section *a, const Section *b) {
                  if (a->lineStart != b->lineStart)
                      return a->lineStart < b->lineStart;
                  // Wider span first on a tie so a parent is pushed
                  // before a same-start child.
                  return a->lineEnd > b->lineEnd;
              });

    struct Frame { const Section *sec{}; Counts agg{}; };
    QVector<Frame> stack;
    auto closeFrame = [&](const Frame &f) {
        out.insert(f.sec->slug, f.agg);
        if (!stack.isEmpty()) stack.last().agg += f.agg;
    };

    for (const Section *s : ordered) {
        // Pop ancestors that end at or before this section begins —
        // their subtree is complete. lineEnd is EXCLUSIVE, so a sibling
        // whose lineStart equals the previous section's lineEnd (e.g.
        // [10,50) then [50,100)) is NOT nested: pop on `<=`, not `<`.
        while (!stack.isEmpty() &&
               stack.last().sec->lineEnd <= s->lineStart) {
            closeFrame(stack.takeLast());
        }
        // Seed with the section's own direct counts (covers "self").
        Counts seed{};
        const auto it = direct.constFind(s->slug);
        if (it != direct.cend()) seed = it.value();
        stack.append(Frame{s, seed});
    }
    while (!stack.isEmpty()) closeFrame(stack.takeLast());
    return out;
}

}  // namespace RoadmapIndex
