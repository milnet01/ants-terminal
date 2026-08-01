// ANTS-3765 — the roadmap migration load half.
// Spec: docs/specs/ANTS-3765-roadmap-migration-load.md
#include "roadmapmigrateload.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

using RoadmapMigrate::MigrationPlan;
using RoadmapMigrate::Note;
using RoadmapMigrate::PlannedItem;
using RoadmapMigrate::PlannedSection;

namespace {

// The same SHAPE `history.changed_at` CHECKs, and deliberately no stricter: the
// DDL's GLOB accepts a 13th month too, and a validator that refused one here
// would refuse a stamp the store would have taken.
bool isIsoZStamp(const QString &s) {
    static const QRegularExpression rx(
        QStringLiteral(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$)"));
    return rx.match(s).hasMatch();
}

QString canonList(const QStringList &v) {
    return RoadmapStore::canonicalJson(QJsonArray::fromStringList(v));
}

// A DEFAULT-CONSTRUCTED QString is null, and QSqlQuery binds a null QString as
// SQL NULL — while `section.slug` and `section.title` are NOT NULL. The
// synthetic root section, which ANTS-3757 § 2.1 gives an empty slug and title
// for content above the first heading, therefore arrives as two nulls: the
// insert is refused outright, and `findSection()` could never match it anyway
// because `slug = NULL` is never true, so a re-run would try to create it
// again — and SQLite's UNIQUE treats NULLs as distinct, so it would succeed.
// An empty string is what the store must hold, so the null is normalised here,
// at the boundary that knows empty is a real value rather than an absence.
//
// Found by running the ten-project corpus, not by a test: every invariant test
// names its sections, and no named slug is ever null.
QString notNull(const QString &s) {
    return s.isNull() ? QString::fromUtf8("") : s;
}

// One field the plan is authoritative for, paired with what the store holds.
// `planEmpty` is not `planText.isEmpty()`: an empty lane list renders as `[]`,
// which is a non-empty string and an absent value.
struct Field {
    QString column, planText, storedText;
    bool    planEmpty = false, storedEmpty = false;
};

QVector<Field> fieldsOf(const PlannedItem &it, const RoadmapStore::ItemWrite &cur) {
    const auto plain = [](const QString &column, const QString &p, const QString &s) {
        return Field{column, p, s, p.isEmpty(), s.isEmpty()};
    };
    const auto list = [](const QString &column, const QStringList &p, const QStringList &s) {
        return Field{column, canonList(p), canonList(s), p.isEmpty(), s.isEmpty()};
    };
    // § 2.6 — "the plan is authoritative only for the fields it carries".
    // priority, resolution, milestone, visibility and the three dates are NOT
    // here, and their absence is the whole invariant: a source file cannot
    // express them, so a re-run must never clear one a human set through
    // roadmap_log. INV-3.
    return {
        plain(QStringLiteral("status"), it.status, cur.status),
        plain(QStringLiteral("headline"), it.headline, cur.headline),
        plain(QStringLiteral("kind"), it.kind, cur.kind),
        plain(QStringLiteral("source"), it.source, cur.source),
        plain(QStringLiteral("layman"), it.layman, cur.layman),
        plain(QStringLiteral("body"), it.body, cur.body),
        list(QStringLiteral("lanes"), it.lanes, cur.lanes),
        list(QStringLiteral("evidence"), it.evidence, cur.evidence),
        Field{QStringLiteral("extras"), RoadmapStore::canonicalJson(it.extras),
              RoadmapStore::canonicalJson(cur.extras), it.extras.isEmpty(),
              cur.extras.isEmpty()},
    };
}

// The plan records provenance for the fields it makes a decision about —
// status, id, kind, source (ANTS-3757 § 2.7–2.9). For the rest it carries the
// author's own text through unchanged, which is what `asserted` means, and that
// is the same value the plan itself uses for a field it read from source.
QString provenanceFor(const PlannedItem &it, const QString &column) {
    const QString p = it.provenance.value(column).toString();
    return p.isEmpty() ? QStringLiteral("asserted") : p;
}

struct Loader {
    Loader(RoadmapStore &store_, const MigrationPlan &plan_,
           const RoadmapMigrateLoad::Options &opts_, RoadmapMigrateLoad::Outcome &out_)
        : store(store_), plan(plan_), opts(opts_), out(out_) {}

    RoadmapStore &store;
    const MigrationPlan &plan;
    const RoadmapMigrateLoad::Options &opts;
    RoadmapMigrateLoad::Outcome &out;
    QString err;

    qint64 projectId = 0;
    QHash<QString, qint64> sectionIds;          // slug -> section_id
    QSet<qint64> clearedSections;               // the sections step 2 emptied
    QVector<RoadmapStore::ItemRef> existing;
    QVector<qint64> matchPk;                    // parallel to plan.items; 0 = new
    QSet<qint64> consumed;                      // stored items a plan item claimed

    // § 2.8's counter, resolved on the first allocation and not before: a plan
    // with nothing to allocate must not touch id_prefix at all.
    bool    prefixResolved = false;
    QString prefix;
    qint64  highWater = 0;

    void note(const char *code, const QString &detail) {
        // § 2.11 — line is ALWAYS 0 on a load note. A plan note is keyed to the
        // source line it came from; a load note is about a store row, and the
        // load never opens the source file. A plausible-looking line would be
        // worse than none.
        out.notes.push_back(Note{QString::fromLatin1(code), detail, 0});
    }

    bool fail(const QString &message) {
        err = message;
        return false;
    }

    bool run();
    bool resolveSections();
    bool matchItems();
    bool updateMatched();
    bool rebuildElements();
    bool writeTail();

    bool allocateId(QString *allocated);
    bool applyPlanFields(const PlannedItem &it, qint64 itemPk, bool *changed);
    bool recordHistory(qint64 itemPk, const QString &field, const QString &oldValue,
                       const QString &newValue, int *seq);
    RoadmapStore::ItemWrite itemWriteFor(const PlannedItem &it, const QString &id,
                                         qint64 sectionId);
};

bool Loader::run() {
    const auto pid = store.registerProject(opts.projectRoot, plan.projectName,
                                           plan.exportSlug, &err);
    if (!pid)
        return fail(err);
    projectId = *pid;
    out.projectId = projectId;

    if (!resolveSections() || !matchItems() || !updateMatched() ||
        !rebuildElements() || !writeTail())
        return false;
    return true;
}

bool Loader::resolveSections() {
    // Parents before children, by LEVEL rather than by document order: a
    // parentSlug must always resolve to a row that exists, and sorting on the
    // property that defines the hierarchy makes that true whatever order the
    // plan lists them in. The synthetic root is level 0 and therefore first.
    QVector<const PlannedSection *> ordered;
    ordered.reserve(plan.sections.size());
    for (const PlannedSection &s : plan.sections)
        ordered.push_back(&s);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const PlannedSection *a, const PlannedSection *b) {
                         return a->level < b->level;
                     });

    for (const PlannedSection *s : ordered) {
        std::optional<qint64> parentId;
        if (!s->parentSlug.isEmpty()) {
            if (const auto it = sectionIds.constFind(s->parentSlug);
                it != sectionIds.constEnd())
                parentId = *it;
            else
                // Not in this plan: it may still be in the store from an
                // earlier run, since § 2.6 retains a section the plan stopped
                // carrying. Unresolvable ⇒ top level, which is the shape the
                // store can actually hold.
                parentId = store.findSection(projectId, notNull(s->parentSlug), &err);
        }

        err.clear();
        const auto found = store.findSection(projectId, notNull(s->slug), &err);
        if (!err.isEmpty())
            return fail(err);

        if (!found) {
            const auto sid = store.addSection(projectId, notNull(s->slug),
                                              notNull(s->title), s->level, parentId, &err);
            if (!sid)
                return fail(err);
            if (!s->intro.isEmpty() && !store.setSectionIntro(*sid, s->intro, &err))
                return fail(err);
            sectionIds.insert(s->slug, *sid);
            ++out.sectionsWritten;
            continue;
        }

        // A re-run: written only if it DIFFERS, which is what makes
        // sectionsWritten a count of changes rather than of attempts.
        const auto cur = store.readSection(*found, &err);
        if (!cur)
            return fail(err.isEmpty() ? QStringLiteral("section %1 vanished").arg(*found)
                                      : err);
        bool changed = false;
        if (cur->title != s->title || cur->level != s->level || cur->parentId != parentId) {
            if (!store.updateSection(*found, notNull(s->title), s->level, parentId, &err))
                return fail(err);
            changed = true;
        }
        if (cur->intro != s->intro) {
            if (!store.setSectionIntro(*found, s->intro, &err))
                return fail(err);
            changed = true;
        }
        sectionIds.insert(s->slug, *found);
        if (changed)
            ++out.sectionsWritten;
    }
    return true;
}

bool Loader::matchItems() {
    const auto items = store.listItems(projectId, &err);
    if (!items)
        return fail(err);
    existing = *items;

    QHash<QString, qint64> byIdFold;
    for (const RoadmapStore::ItemRef &r : existing)
        byIdFold.insert(r.idFold, r.itemPk);

    matchPk = QVector<qint64>(plan.items.size(), 0);
    for (qsizetype i = 0; i < plan.items.size(); ++i) {
        const PlannedItem &it = plan.items.at(i);

        if (!it.id.isEmpty()) {
            // § 2.6 — the store's own identity, case-folded within a project.
            // Never headline, never position: both change while an item stays
            // itself, and matching on either would delete and re-create it,
            // destroying its history rows.
            const qint64 pk = byIdFold.value(it.id.toLower(), 0);
            if (pk && !consumed.contains(pk)) {
                matchPk[i] = pk;
                consumed.insert(pk);
            }
            continue;
        }

        // § 2.6.1 — an id-less item cannot be matched by id, and ~40% of the
        // corpus is id-less. Its natural key is the weakest thing that is still
        // safe: same section, migration-allocated id, byte-identical headline.
        const qint64 sectionId = sectionIds.value(it.sectionSlug, 0);
        QVector<qint64> candidates;
        for (const RoadmapStore::ItemRef &r : existing) {
            if (r.sectionId == sectionId && r.idFromMigration &&
                r.headline == it.headline && !consumed.contains(r.itemPk))
                candidates.push_back(r.itemPk);
        }
        if (candidates.size() == 1) {
            matchPk[i] = candidates.first();
            consumed.insert(candidates.first());
        } else if (candidates.size() > 1) {
            // Never guessed: picking one of two would silently move history
            // onto the wrong item.
            note("ambiguous_rematch", it.headline);
        }
    }
    return true;
}

bool Loader::applyPlanFields(const PlannedItem &it, qint64 itemPk, bool *changed) {
    const auto cur = store.readItem(itemPk, &err);
    if (!cur)
        return fail(err.isEmpty() ? QStringLiteral("no such item %1").arg(itemPk) : err);

    int seq = 0;
    bool seqPrimed = false;
    *changed = false;

    for (const Field &f : fieldsOf(it, *cur)) {
        if (f.planText == f.storedText)
            continue;

        const QString prov = provenanceFor(it, f.column);

        if (f.planEmpty && !f.storedEmpty) {
            // "Empty does not overwrite" — with one exception. `body` is the
            // only field whose emptiness in source is meaningful (a bullet
            // whose continuation lines were deleted), and it is cleared to SQL
            // NULL rather than '' so one logical state has one representation.
            if (f.column != QLatin1String("body")) {
                note("field_conflict", QStringLiteral("%1: %2").arg(it.id.isEmpty()
                                                                       ? cur->id
                                                                       : it.id,
                                                                   f.column));
                continue;
            }
            if (!store.clearItemField(itemPk, f.column, prov, &err))
                return fail(err);
        } else if (!store.setItemField(itemPk, f.column, f.planText, prov, &err)) {
            return fail(err);
        }

        if (!seqPrimed) {
            // § 2.9 — continue from the stored maximum for this (item, stamp),
            // never restart at 0: two runs given the same stamp would otherwise
            // collide on UNIQUE (item_pk, changed_at, seq) and abort the whole
            // project, and a caller stamping two runs identically is not a
            // misuse. nullopt means no row yet, so the first row is seq 0.
            err.clear();
            const auto storedSeq = store.maxHistorySeq(itemPk, opts.changedAt, &err);
            if (!err.isEmpty())
                return fail(err);
            seq = storedSeq.value_or(-1) + 1;
            seqPrimed = true;
        }
        if (!recordHistory(itemPk, f.column, f.storedText, f.planText, &seq))
            return false;
        *changed = true;
    }
    return true;
}

bool Loader::recordHistory(qint64 itemPk, const QString &field, const QString &oldValue,
                           const QString &newValue, int *seq) {
    QString hErr;
    if (store.appendHistory(itemPk, opts.changedAt, *seq, field, oldValue, newValue,
                            &hErr)) {
        ++*seq;
        ++out.historyRows;
        return true;
    }

    // § 2.5's ONE exception, and how the loader tells it from any other failure
    // has to be mechanical: appendHistory() reports both the same way, and
    // string-matching an error message is exactly the invention this avoids.
    //
    // The discriminator RE-EVALUATES the store's own predicate rather than
    // comparing historyBytes() against the cap, which is what the spec said and
    // what implementing it disproved: appendHistory() refuses when
    // stored + incoming > cap, so at the moment of refusal the STORED bytes are
    // still below the cap in every case except an exact landing — the first
    // refusal on an empty history compares 0 against the cap and reads as "some
    // other failure", which would abort the project the exception exists to
    // save. Both terms are public and all three strings are in hand, so the
    // predicate is reproducible exactly, with no error text parsed.
    const qint64 incoming = field.size() + oldValue.size() + newValue.size();
    if (store.historyBytes() + incoming > store.historyCapBytes()) {
        note("history_capped", QStringLiteral("%1: %2").arg(itemPk).arg(field));
        return true;
    }
    return fail(hErr);
}

bool Loader::allocateId(QString *allocated) {
    if (!prefixResolved) {
        // § 2.8 step 1. The store's own row wins: it is what the project
        // already allocates under, and deriving the prefix afresh from a source
        // file that carries no ids would let a renamed directory start a second
        // counter.
        err.clear();
        const auto storedPrefix = store.idPrefixFor(projectId, &err);
        if (!err.isEmpty())
            return fail(err);
        if (storedPrefix) {
            prefix = *storedPrefix;
        } else {
            // Failing that, the most frequent prefix among the plan's PARSED
            // ids — ties by first appearance in document order. A prefix is the
            // text before an id's FINAL '-'; an id with no '-' (`Sh4`, the
            // legacy bold form) contributes none.
            QHash<QString, int> freq;
            QStringList order;
            for (const PlannedItem &p : plan.items) {
                if (p.idOrigin != QLatin1String("parsed"))
                    continue;
                const int cut = p.id.lastIndexOf(QLatin1Char('-'));
                if (cut <= 0)
                    continue;
                const QString pre = p.id.left(cut);
                if (!freq.contains(pre))
                    order << pre;
                ++freq[pre];
            }
            for (const QString &candidate : std::as_const(order)) {
                if (prefix.isEmpty() || freq.value(candidate) > freq.value(prefix))
                    prefix = candidate;
            }
            if (prefix.isEmpty()) {
                // And failing THAT — a project with no id-bearing items at all,
                // which is the first-run case for several of the corpus's ten —
                // the same fallback roadmap_log uses: the uppercased first four
                // characters of the project root's leaf directory.
                const QString leaf = QFileInfo(QDir::cleanPath(opts.projectRoot)).fileName();
                prefix = leaf.left(4).toUpper();
            }
        }

        // § 2.8 step 2. The stored high-water when there is one; otherwise the
        // maximum numeric suffix among the plan's parsed ids carrying this
        // prefix. Quarantined and synthesised ids are excluded from both: a
        // PASS-43-5 comes from a heading, not from the counter, and would
        // otherwise set the high-water to 5.
        err.clear();
        const auto storedHigh = store.idHighWater(projectId, prefix, &err);
        if (!err.isEmpty())
            return fail(err);
        if (storedHigh) {
            highWater = *storedHigh;
        } else {
            highWater = 0;   // step 4 — no parsed id yields an integer suffix
            for (const PlannedItem &p : plan.items) {
                if (p.idOrigin != QLatin1String("parsed"))
                    continue;
                const int cut = p.id.lastIndexOf(QLatin1Char('-'));
                if (cut <= 0 || p.id.left(cut) != prefix)
                    continue;
                bool isInt = false;
                const qint64 n = p.id.mid(cut + 1).toLongLong(&isInt);
                if (isInt && n > highWater)
                    highWater = n;
            }
        }
        prefixResolved = true;
    }

    ++highWater;
    // Zero-padded to four digits, and wider once the counter passes them —
    // roadmap-format.md § 3.5.1's own form. Left unstated an implementer
    // invents a width, and the choice is permanent: it is baked into every
    // stored id and every citation of it thereafter.
    *allocated = QStringLiteral("%1-%2").arg(
        prefix, QString::number(highWater).rightJustified(4, QLatin1Char('0')));
    ++out.idsAllocated;
    note("id_allocated", *allocated);
    return true;
}

RoadmapStore::ItemWrite Loader::itemWriteFor(const PlannedItem &it, const QString &id,
                                             qint64 sectionId) {
    RoadmapStore::ItemWrite w;
    w.projectId = projectId;
    w.id = id;
    // An allocated id is neither parsed from source nor quarantined, and
    // `id_origin` CHECKs those three values — so `synthesised` is the only one
    // of them that is true of it. The fact § 2.6.1's re-match keys on is
    // provenance.id == "migrated", which the plan already carries, not this.
    w.idOrigin = it.idOrigin.isEmpty() ? QStringLiteral("synthesised") : it.idOrigin;
    w.status = it.status;
    w.headline = it.headline;
    w.kind = it.kind;
    w.source = it.source;
    w.layman = it.layman;
    w.body = it.body;
    w.lanes = it.lanes;
    w.evidence = it.evidence;
    w.extras = it.extras;
    w.provenance = it.provenance;
    w.sectionId = sectionId;
    w.position = it.position;
    return w;
}

bool Loader::updateMatched() {
    for (qsizetype i = 0; i < plan.items.size(); ++i) {
        if (!matchPk.at(i))
            continue;
        bool changed = false;
        if (!applyPlanFields(plan.items.at(i), matchPk.at(i), &changed))
            return false;
        changed ? ++out.itemsUpdated : ++out.itemsUnchanged;
    }
    return true;
}

bool Loader::rebuildElements() {
    // Rebuilt, not diffed: `element` has UNIQUE (section_id, position) and
    // SQLite enforces it as each row is written, so an in-place shift fails at
    // the first row whose new position is still held by a row that has not
    // moved yet.
    for (const PlannedSection &s : plan.sections)
        clearedSections.insert(sectionIds.value(s.slug, 0));
    clearedSections.remove(0);

    // Step 1, and it must precede the delete: once the element rows are gone
    // the store has no other record of where an item was filed — there is no
    // item.section column. Only the rows that need it are read: an orphan in a
    // cleared section needs its position for ordering, and a MATCHED item
    // whose stored section the plan no longer carries needs unfiling, because
    // step 2 will not clear that section and fileItem() refuses a double file.
    struct Orphan {
        qint64 itemPk = 0, sectionId = 0;
        int    position = 0;
    };
    QVector<Orphan> orphans;
    for (const RoadmapStore::ItemRef &r : existing) {
        const bool matched = consumed.contains(r.itemPk);
        const bool cleared = clearedSections.contains(r.sectionId);

        if (matched) {
            if (!cleared && r.sectionId != 0 && !store.unfileItem(r.itemPk, &err))
                return fail(err);
            continue;
        }

        ++out.itemsOrphaned;
        note("orphaned_item", r.idFold);
        if (!cleared && r.sectionId != 0)
            continue;    // its element row is not deleted; it stays where it is

        Orphan o;
        o.itemPk = r.itemPk;
        o.sectionId = r.sectionId;
        if (r.sectionId != 0) {
            const auto cur = store.readItem(r.itemPk, &err);
            if (!cur)
                return fail(err.isEmpty() ? QStringLiteral("no such item %1").arg(r.itemPk)
                                          : err);
            o.position = cur->position;
        }
        orphans.push_back(o);
    }

    // Step 2 — per section the plan carries, never project-wide. A section
    // retained because the plan no longer mentions it also holds narration and
    // table rows the plan cannot supply, and nothing re-inserts them.
    for (const qint64 sid : std::as_const(clearedSections)) {
        if (!store.clearSectionElements(sid, &err))
            return fail(err);
    }

    // Step 3 — re-insert from the plan, all three element kinds in one pass per
    // section. New items via putItem(), which files as it inserts; matched ones
    // via fileItem(), without which every matched item would end the
    // transaction unfiled.
    QHash<qint64, int> nextPosition;
    for (qsizetype i = 0; i < plan.items.size(); ++i) {
        const PlannedItem &it = plan.items.at(i);
        const qint64 sid = sectionIds.value(it.sectionSlug, 0);
        if (!sid)
            return fail(QStringLiteral("item %1 names an unknown section '%2'")
                            .arg(it.id.isEmpty() ? it.headline : it.id, it.sectionSlug));

        if (const qint64 pk = matchPk.at(i)) {
            if (!store.fileItem(pk, sid, it.position, &err))
                return fail(err);
        } else {
            QString id = it.id;
            if (id.isEmpty() && !allocateId(&id))
                return false;
            RoadmapStore::ItemWrite w = itemWriteFor(it, id, sid);
            if (!store.putItem(w, &err))
                return fail(err);
            ++out.itemsInserted;
        }
        ++out.elementsWritten;
        nextPosition[sid] = std::max(nextPosition.value(sid, 0), it.position + 1);
    }
    for (const RoadmapMigrate::PlannedElement &e : plan.elements) {
        const qint64 sid = sectionIds.value(e.sectionSlug, 0);
        if (!sid)
            return fail(QStringLiteral("element names an unknown section '%1'")
                            .arg(e.sectionSlug));
        if (!store.addElement(sid, e.position, e.kind, e.payload, &err))
            return fail(err);
        ++out.elementsWritten;
        nextPosition[sid] = std::max(nextPosition.value(sid, 0), e.position + 1);
    }

    // Step 4 — re-file every orphan whose element row step 2 deleted, at the
    // end of that same section AFTER every element the plan placed there:
    // position is one sequence per section shared by items, narration and
    // tables alike, so "after the last item" can land on a row a narration
    // already holds. Ascending captured position, ties by item_pk — any total
    // order satisfies INV-4, but an unstated one makes the re-run
    // non-deterministic.
    std::stable_sort(orphans.begin(), orphans.end(), [](const Orphan &a, const Orphan &b) {
        return a.position != b.position ? a.position < b.position : a.itemPk < b.itemPk;
    });
    qint64 rootSection = 0;
    for (const Orphan &o : std::as_const(orphans)) {
        qint64 sid = o.sectionId;
        if (sid == 0) {
            // § 2.7 — the orphan's section is gone from the store entirely.
            // Nothing in this spec produces that state; a store rebuilt from an
            // export that omitted the section does. It is handled rather than
            // assumed away because the loader cannot tell which tool wrote the
            // store it opened. The synthetic root is the same row the plan uses
            // for content above the first heading.
            if (!rootSection) {
                const QString rootSlug = QString::fromUtf8("");
                if (const auto found = store.findSection(projectId, rootSlug, &err))
                    rootSection = *found;
                else if (!err.isEmpty())
                    return fail(err);
                else if (const auto made = store.addSection(projectId, rootSlug, rootSlug,
                                                            0, std::nullopt, &err))
                    rootSection = *made;
                else
                    return fail(err);
            }
            sid = rootSection;
        }
        const int position = nextPosition.value(sid, 0);
        if (!store.fileItem(o.itemPk, sid, position, &err))
            return fail(err);
        nextPosition[sid] = position + 1;
        ++out.elementsWritten;
    }
    return true;
}

bool Loader::writeTail() {
    if (plan.legend && !store.setLegend(projectId, plan.legend->entries, &err))
        return fail(err);

    // Only when this run allocated: a plan with nothing to allocate must leave
    // id_prefix untouched, which is what makes INV-6's first-run leg — no row
    // at all after a failed load — an assertion about the rollback rather than
    // about this branch.
    if (out.idsAllocated > 0 &&
        !store.raiseIdHighWater(projectId, prefix, highWater, &err))
        return fail(err);
    return true;
}

}  // namespace

namespace RoadmapMigrateLoad {

Outcome load(RoadmapStore &store, const MigrationPlan &plan, const Options &opts) {
    Outcome out;
    out.notes = plan.notes;

    // A refusal reports zero counts, because nothing was committed: `ok == false`
    // means NOTHING for this project landed, and a count surviving from a
    // partial pass would say otherwise.
    const auto refuse = [&](const char *code, const QString &message) {
        Outcome refused;
        refused.notes = plan.notes;
        refused.notes.push_back(Note{QString::fromLatin1(code), message, 0});
        refused.error = message;
        return refused;
    };

    // § 2.1 — validated BEFORE the transaction opens, so an ill-formed stamp
    // surfaces as a refusal rather than as a rolled-back project at the first
    // re-run update.
    if (!isIsoZStamp(opts.changedAt))
        return refuse("bad_options",
                      QStringLiteral("changedAt is not an ISO-8601 Z stamp: '%1'")
                          .arg(opts.changedAt));

    // § 2.2 / INV-12 — refused, not run slowly. A 5 s deadline against a
    // migration-sized transaction fails *sometimes*, which is the worst
    // available behaviour: it passes locally and fails on a loaded machine.
    if (store.access() != RoadmapStore::Access::Bulk)
        return refuse("project_refused",
                      QStringLiteral("a migration load needs an Access::Bulk store"));
    if (!store.isOpen())
        return refuse("project_refused", QStringLiteral("store is not open"));

    QString err;
    if (!store.begin(&err))
        return refuse("project_refused", err);

    Loader loader{store, plan, opts, out};
    if (!loader.run()) {
        // Every failure but § 2.5's one stated exception aborts the project.
        // The rollback's own error cannot displace the first one: the first is
        // what went wrong, and the rollback is only how it was undone.
        QString rollbackErr;
        store.rollback(&rollbackErr);
        return refuse("project_refused", loader.err);
    }

    // A dry run takes the identical path and rolls back at the end, so it
    // exercises every constraint the real one does rather than a cheaper
    // approximation of them (INV-13).
    if (opts.dryRun) {
        if (!store.rollback(&err))
            return refuse("project_refused", err);
    } else if (!store.commit(&err)) {
        store.rollback(nullptr);
        return refuse("project_refused", err);
    }

    out.ok = true;
    return out;
}

}  // namespace RoadmapMigrateLoad
