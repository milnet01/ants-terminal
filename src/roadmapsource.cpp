// ANTS-3793 — see roadmapsource.h for the seam's shape and why it exists.

#include "roadmapsource.h"

#include "roadmapindex.h"    // RoadmapIndex::uniqueSlug() — the stateful slugger
#include "roadmaprender.h"   // bulletText() — ANTS-3808 § 2.4's export

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace RoadmapSource {

namespace {

// § 4 — detectRoadmapFormat() takes a QStringList while this seam holds a
// QString, so a naive markdown.split('\n') of a 3 MiB roadmap materialises
// another ~6 MiB plus ~34k QString headers for lines nothing reads. The
// detector stops at 300 NON-BLANK lines (and returns at the format marker long
// before that on a rendered file), so it is handed exactly that many.
//
// Blank lines are kept but not counted, which is the detector's own rule
// (`if (ln.trimmed().isEmpty()) continue;` sits above `if (++seen >= 300)`), so
// the prefix and the whole file classify identically.
QStringList detectionPrefix(const QString &markdown) {
    constexpr int kDetectorLineCap = 300;
    QStringList lines;
    int nonBlank = 0;
    qsizetype start = 0;
    while (nonBlank < kDetectorLineCap) {
        const qsizetype nl = markdown.indexOf(QLatin1Char('\n'), start);
        const QString line = (nl < 0) ? markdown.mid(start)
                                      : markdown.mid(start, nl - start);
        lines.append(line);
        if (!line.trimmed().isEmpty())
            ++nonBlank;
        if (nl < 0)
            break;
        start = nl + 1;
    }
    return lines;
}

// One item's record, built by the ONE rule § 2.1.1 makes normative: what
// parseBullets() would assign if it parsed this item's rendered bullet, under
// the section heading that item's section renders.
//
// Nothing is copied from a column that the head line also carries — not `id`,
// not `status`, not `idToken`. That is not fastidiousness: `rxId` takes the
// first `[<PREFIX>-NNNN]` anywhere in the body, an item with an empty `id`
// column renders no bracket at all, and `status` is a lifecycle WORD in the
// store and an emoji on the record. Copying any of the three fails INV-2 on
// bullets this corpus actually contains.
//
// Appends nothing for a `dropped` item, and that is the whole of § 2.1.2's
// exclusion: emojiFor() returns an empty string for `dropped` by design, so
// bulletText() emits a head line with no status marker and parseAntsV1Bullet()
// declines it exactly as a document walk would skip it.
void appendRecord(QVector<BulletRecord> &out, const RoadmapStore::ItemWrite &it,
                  const QString &heading, int level, const QString &slug) {
    auto rec = RoadmapParse::parseAntsV1Bullet(RoadmapRender::bulletText(it));
    if (!rec)
        return;
    rec->sectionHeading = heading;
    rec->sectionLevel   = level;
    // The parser assigns a slug only under a real heading, so a bullet above
    // the first one — every preamble item, under § 2.8's level-0 synthetic root
    // — keeps an empty slug and level 0. Copying SectionRow::title there is one
    // of INV-2's named break clauses.
    if (!heading.isEmpty())
        rec->sectionSlug = slug;
    // firstLine / lastLine stay 0: a store has no lines to number, and no walk
    // can invent them. INV-2's one declared field difference.
    out.append(*rec);
}

} // namespace

std::unique_ptr<RoadmapStore> storeFor(const QString &defaultPath,
                                       ReadError *why, QString *error) {
    if (why)
        *why = ReadError::None;
    if (error)
        error->clear();

    // Absent is the common case and it is NOT an error: most machines running
    // this code have never migrated anything, and a refusal here would break
    // every unmigrated project on the day the store shipped.
    if (!QFileInfo::exists(defaultPath))
        return nullptr;

    auto store = std::make_unique<RoadmapStore>(
        defaultPath, RoadmapStore::kDefaultHistoryCapBytes,
        // § 2.2's sixth rule — named, never defaulted. Interactive is a 5 s
        // busy deadline and SQLite's 2 MiB page cache; Bulk's 30 s is right for
        // a corpus load and reads as a freeze on a verb call.
        RoadmapStore::Access::Interactive);

    QString openErr;
    if (!store->open(&openErr)) {
        // Present and unopenable is the case a silent fallback would hide: a
        // corrupted store that quietly serves markdown is a store nobody
        // notices is corrupt.
        if (why)
            *why = ReadError::StoreFailed;
        if (error)
            *error = openErr.isEmpty()
                         ? QStringLiteral("could not open roadmap store %1").arg(defaultPath)
                         : openErr;
        return nullptr;
    }
    return store;
}

std::optional<qint64> migratedProject(RoadmapStore &store,
                                      const QString &projectRoot,
                                      const QString &markdown,
                                      QString *error, ReadError *why) {
    if (why)
        *why = ReadError::None;
    if (error)
        error->clear();

    // ANTS-3756 INV-8 keys a project on its CANONICAL root, so the lookup has
    // to canonicalise too. A reader passing the caller's raw path would miss on
    // every symlinked or non-normalised root and report "not migrated" — the
    // silent fallback INV-1 exists to forbid, arriving through the one door the
    // invariant does not watch.
    const QString canonical = QFileInfo(projectRoot).canonicalFilePath();
    if (canonical.isEmpty()) {
        // Empty is what canonicalFilePath() returns for a path that does not
        // resolve. Refusing beats querying: '' would match no row and read as
        // "not migrated", which is the same silent fallback one step later.
        if (why)
            *why = ReadError::SourceUnrecognised;
        if (error)
            *error = QStringLiteral("project root does not resolve: %1").arg(projectRoot);
        return std::nullopt;
    }

    QString sqlError;
    const auto row = store.readProjectByRoot(canonical, &sqlError);
    if (!row) {
        if (!sqlError.isEmpty()) {
            if (why)
                *why = ReadError::StoreFailed;
            if (error)
                *error = sqlError;
            return std::nullopt;
        }
        return std::nullopt;   // no project row — markdown, and not an error
    }

    // § 2.2's fourth and fifth rules. The migration reads all three dialects, so
    // a project row existing does not imply this path can serve it — and the
    // distinction is `sawSignal`, not the returned dialect, because
    // detectRoadmapFormat() answers "ants-v1" for input it does not recognise.
    bool sawSignal = false;
    const QString format =
        RoadmapParse::detectRoadmapFormat(detectionPrefix(markdown), &sawSignal);
    if (!sawSignal) {
        // A migrated project whose ROADMAP.md is absent, empty or mangled. Not
        // StoreFailed: the store is fine and the FILE is not, and the two send
        // the user to different places.
        if (why)
            *why = ReadError::SourceUnrecognised;
        if (error)
            *error = QStringLiteral(
                         "project %1 has a store row but its roadmap text is "
                         "unrecognisable").arg(canonical);
        return std::nullopt;
    }
    // ANTS-3815 § 2.4 — the stored dialect is the second witness, and it costs
    // no extra query: readProjectByRoot() above already returned it. '' is a
    // pre-bump row and means "not recorded", so it takes the version-1 path
    // unchanged; treating it as authoritative would turn every project migrated
    // before the bump into a refusal, the empty string matching no dialect a
    // file can produce.
    if (!row->sourceFormat.isEmpty() && row->sourceFormat != format) {
        // Same code as the unrecognisable-file case above, so the MESSAGE has to
        // separate them — ANTS-3793 chose a distinct code precisely so the store
        // and the file send the user to different places, and a second cause
        // under one code loses that. This one has a remedy and says so.
        if (why)
            *why = ReadError::SourceUnrecognised;
        if (error)
            *error = QStringLiteral(
                         "project %1: store records format '%2' but its roadmap now reads "
                         "as '%3'; re-run the migration to record the new format")
                         .arg(canonical, row->sourceFormat, format);
        return std::nullopt;
    }

    if (format != QStringLiteral("ants-v1"))
        return std::nullopt;   // legitimately markdown-served (§ 5)

    return row->projectId;
}

std::optional<QVector<BulletRecord>>
bulletsFromStore(RoadmapStore &store, qint64 projectId, bool includeArchive,
                 ReadError *why, QString *error) {
    if (why)
        *why = ReadError::None;
    if (error)
        error->clear();

    const auto fail = [&](ReadError e, const QString &msg) {
        if (why)
            *why = e;
        if (error && error->isEmpty())
            *error = msg;
        return std::optional<QVector<BulletRecord>>{};
    };

    // Step 0 — the item keys, for the ceiling gate and for the unfiled tail.
    // ItemRef carries a headline and four scalars rather than a body, which is
    // what makes the pre-read count cheap.
    const auto refs = store.listItems(projectId, error);
    if (!refs)
        return fail(ReadError::StoreFailed,
                    QStringLiteral("could not list items for project %1").arg(projectId));

    // INV-3 — tested BEFORE any readItem() runs, so a project over the ceiling
    // never materialises the bodies the ceiling exists to bound. `>` and not
    // `>=`: exactly kItemCeiling is accepted.
    if (refs->size() > kItemCeiling)
        return fail(ReadError::TooLarge,
                    QStringLiteral("project %1 holds %2 items, over the %3-item read ceiling")
                        .arg(projectId).arg(refs->size()).arg(kItemCeiling));

    // Then every item body in ONE query. This was a readItem() per item until
    // INV-3's p95 case measured it: 83.4 ms of a 101.5 ms read on a
    // 1,839-item project, against a 50 ms budget, with the whole
    // render-and-parse half costing 8.6 ms (2026-08-04). § 4 named the batched
    // reader as the remedy in advance and said to build it only if the budget
    // red — it did, so ANTS-3816's first half lands here.
    auto itemOf = store.readItems(projectId, error);
    if (!itemOf)
        return fail(ReadError::StoreFailed,
                    QStringLiteral("could not read items for project %1").arg(projectId));

    QVector<RoadmapStore::ItemRef> unfiled;
    for (const RoadmapStore::ItemRef &ref : *refs) {
        if (ref.sectionId == 0)
            unfiled.append(ref);
    }

    // Step 1 — sections, scoped by the caller's archive flag. SectionRow::
    // sourcePath is nullopt for the live roadmap and set for an archive, which
    // is the same distinction ANTS-3758's render routes files by.
    const auto sections = store.listSections(projectId, error);
    if (!sections)
        return fail(ReadError::StoreFailed,
                    QStringLiteral("could not list sections for project %1").arg(projectId));

    QVector<RoadmapStore::SectionRow> ordered;
    ordered.reserve(sections->size());
    for (const RoadmapStore::SectionRow &s : *sections) {
        if (includeArchive || !s.sourcePath)
            ordered.append(s);
    }
    // Step 2 — a C++ comparator and not an ORDER BY, because QString::compare()
    // is UTF-16 code-unit order while SQLite's BINARY collation is UTF-8 byte
    // order, and the two disagree on the supplementary-plane characters an
    // emoji heading slug reaches. This is render()'s own sort.
    std::sort(ordered.begin(), ordered.end(), sectionOrderLess);

    QVector<BulletRecord> out;
    // Step 4 — accumulated across the WHOLE walk and never per section: the
    // parser's uniqueSlug() is stateful, appending -2/-3 to a repeated heading
    // text, and a stateless per-section slug diverges on the first duplicated
    // title.
    QSet<QString> seenSlugs;

    for (const RoadmapStore::SectionRow &s : ordered) {
        QString heading;
        int level = 0;
        QString slug;
        // § 2.8's level-0 synthetic root emits no heading line at all, so the
        // parser attributes its bullets to no section.
        if (s.level != 0) {
            heading = s.title;
            level   = s.level;
            slug    = RoadmapIndex::uniqueSlug(seenSlugs, s.title);
        }

        // Step 3 — SectionRow carries no section_id (ANTS-3817), so the id
        // comes back through the slug lookup, exactly as render() does it.
        const auto sectionId = store.findSection(projectId, s.slug, error);
        if (!sectionId)
            return fail(ReadError::StoreFailed,
                        QStringLiteral("section '%1' vanished between reads").arg(s.slug));
        const auto elements = store.listElements(*sectionId, error);
        if (!elements)
            return fail(ReadError::StoreFailed,
                        QStringLiteral("could not list elements of section '%1'").arg(s.slug));

        // Step 5 — position order, which listElements() supplies as its own
        // contract (UNIQUE (section_id, position) makes it total). `narration`
        // and `table` elements are skipped: they are not bullets, and
        // parseBullets() returns no records for them either.
        for (const RoadmapStore::ElementRow &e : *elements) {
            if (e.kind != QLatin1String("item"))
                continue;
            const auto it = itemOf->constFind(e.itemPk);
            if (it == itemOf->constEnd())
                continue;
            appendRecord(out, *it, heading, level, slug);
        }
    }

    // Step 6 — unfiled items last. They have no element row and therefore no
    // position, so a deterministic order is the only answer that is both total
    // and reproducible. They are returned under BOTH values of includeArchive:
    // an unfiled item has no section, therefore no sourcePath for the flag to
    // test, and hiding it from the live read would hide it from the only read
    // likely to surface the fault ANTS-3758's INV-4 refuses the render over.
    std::sort(unfiled.begin(), unfiled.end(),
              [](const RoadmapStore::ItemRef &a, const RoadmapStore::ItemRef &b) {
                  return a.idFold.compare(b.idFold) < 0;
              });
    for (const RoadmapStore::ItemRef &ref : unfiled)
        appendRecord(out, itemOf->value(ref.itemPk), QString(), 0, QString());

    return out;
}

std::optional<QVector<BulletRecord>>
bulletsFor(RoadmapStore &store, const QString &projectRoot,
           const QString &markdown, bool includeArchive, ReadError *why,
           QString *error) {
    if (why)
        *why = ReadError::None;
    if (error)
        error->clear();

    ReadError markerWhy = ReadError::None;
    const auto projectId =
        migratedProject(store, projectRoot, markdown, error, &markerWhy);
    if (!projectId) {
        // None here is the unmigrated project — the caller parses `markdown`
        // itself. Anything else is a refusal that must never fall back.
        if (why)
            *why = markerWhy;
        return std::nullopt;
    }
    return bulletsFromStore(store, *projectId, includeArchive, why, error);
}

QHash<QString, QString> legendByEmoji(const QString &legendText) {
    QHash<QString, QString> out;
    if (legendText.isEmpty())
        return out;
    const QJsonObject legend =
        QJsonDocument::fromJson(legendText.toUtf8()).object();
    for (auto it = legend.constBegin(); it != legend.constEnd(); ++it) {
        // roadmaprender.cpp's own word→emoji map, exported rather than
        // duplicated: a second table here is a correspondence someone has to
        // keep true by hand. `dropped` returns empty and is skipped, which is
        // right — § 3.11 gives it no glyph and no rendered bullet carries it.
        const QString emoji = RoadmapRender::emojiFor(it.key());
        if (emoji.isEmpty())
            continue;
        out.insert(emoji, it.value().toString());
    }
    return out;
}

} // namespace RoadmapSource
